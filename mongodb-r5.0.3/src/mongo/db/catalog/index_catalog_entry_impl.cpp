/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog_entry_impl.h"

#include <algorithm>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;

IndexCatalogEntryImpl::IndexCatalogEntryImpl(OperationContext* const opCtx,
                                             const CollectionPtr& collection,
                                             const std::string& ident,
                                             std::unique_ptr<IndexDescriptor> descriptor,
                                             bool isFrozen)
    : _ident(ident),
      _descriptor(std::move(descriptor)),
      _catalogId(collection->getCatalogId()),
      _ordering(Ordering::make(_descriptor->keyPattern())),
      _isReady(false),
      _isFrozen(isFrozen),
      _isDropped(false) {

    _descriptor->_entry = this;
    _isReady = collection->isIndexReady(_descriptor->indexName());

    {
        stdx::lock_guard<Latch> lk(_indexMultikeyPathsMutex);
        const bool isMultikey = _catalogIsMultikey(opCtx, collection, &_indexMultikeyPathsForRead);
        _isMultikeyForRead.store(isMultikey);
        _isMultikeyForWrite.store(isMultikey);
        _indexTracksMultikeyPathsInCatalog = !_indexMultikeyPathsForRead.empty();
    }

    auto nss = DurableCatalog::get(opCtx)->getEntry(_catalogId).nss;
    const BSONObj& collation = _descriptor->collation();
    if (!collation.isEmpty()) {
        auto statusWithCollator =
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation);

        // Index spec should have already been validated.
        invariant(statusWithCollator.getStatus());

        _collator = std::move(statusWithCollator.getValue());
    }

    if (_descriptor->isPartial()) {
        const BSONObj& filter = _descriptor->partialFilterExpression();

        _expCtxForFilter = make_intrusive<ExpressionContext>(
            opCtx, CollatorInterface::cloneCollator(_collator.get()), nss);

        // Parsing the partial filter expression is not expected to fail here since the
        // expression would have been successfully parsed upstream during index creation.
        _filterExpression =
            MatchExpressionParser::parseAndNormalize(filter,
                                                     _expCtxForFilter,
                                                     ExtensionsCallbackNoop(),
                                                     MatchExpressionParser::kBanAllSpecialFeatures);
        LOGV2_DEBUG(20350,
                    2,
                    "have filter expression for {namespace} {indexName} {filter}",
                    "namespace"_attr = nss,
                    "indexName"_attr = _descriptor->indexName(),
                    "filter"_attr = redact(filter));
    }
}

void IndexCatalogEntryImpl::init(std::unique_ptr<IndexAccessMethod> accessMethod) {
    invariant(!_accessMethod);
    _accessMethod = std::move(accessMethod);
}

bool IndexCatalogEntryImpl::isReady(OperationContext* opCtx,
                                    const CollectionPtr& collection) const {
    // For multi-document transactions, we can open a snapshot prior to checking the
    // minimumSnapshotVersion on a collection.  This means we are unprotected from reading
    // out-of-sync index catalog entries.  To fix this, we uassert if we detect that the
    // in-memory catalog is out-of-sync with the on-disk catalog.
    if (opCtx->inMultiDocumentTransaction()) {
        if (!isPresentInMySnapshot(opCtx) || isReadyInMySnapshot(opCtx) != _isReady) {
            uasserted(ErrorCodes::SnapshotUnavailable,
                      str::stream() << "Unable to read from a snapshot due to pending collection"
                                       " catalog changes; please retry the operation.");
        }
    }

    if (kDebugBuild)
        invariant(_isReady == isReadyInMySnapshot(opCtx));
    return _isReady;
}

bool IndexCatalogEntryImpl::isFrozen() const {
    invariant(!_isFrozen || !_isReady);
    return _isFrozen;
}

bool IndexCatalogEntryImpl::isMultikey() const {
    return _isMultikeyForRead.load();
}

MultikeyPaths IndexCatalogEntryImpl::getMultikeyPaths(OperationContext* opCtx) const {
    stdx::lock_guard<Latch> lk(_indexMultikeyPathsMutex);
    return _indexMultikeyPathsForRead;
}

// ---

void IndexCatalogEntryImpl::setMinimumVisibleSnapshot(Timestamp newMinimumVisibleSnapshot) {
    if (!_minVisibleSnapshot || (newMinimumVisibleSnapshot > _minVisibleSnapshot.get())) {
        _minVisibleSnapshot = newMinimumVisibleSnapshot;
    }
}

void IndexCatalogEntryImpl::setIsReady(bool newIsReady) {
    _isReady = newIsReady;
}

void IndexCatalogEntryImpl::setMultikey(OperationContext* opCtx,
                                        const CollectionPtr& collection,
                                        const KeyStringSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths) const {
    // An index can either track path-level multikey information in the catalog or as metadata keys
    // in the index itself, but not both.
    invariant(!(_indexTracksMultikeyPathsInCatalog && multikeyMetadataKeys.size() > 0));
    // If the index is already set as multikey and we don't have any path-level information to
    // update, then there's nothing more for us to do.
    bool hasNoPathLevelInfo = (!_indexTracksMultikeyPathsInCatalog && multikeyMetadataKeys.empty());
    if (hasNoPathLevelInfo && _isMultikeyForWrite.load()) {
        return;
    }

    if (_indexTracksMultikeyPathsInCatalog) {
        MultikeyPaths indexMultikeyPathsForWrite;
        [[maybe_unused]] const bool isMultikeyInCatalog =
            _catalogIsMultikey(opCtx, collection, &indexMultikeyPathsForWrite);
        invariant(multikeyPaths.size() == indexMultikeyPathsForWrite.size());

        bool newPathIsMultikey = false;
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            if (!std::includes(indexMultikeyPathsForWrite[i].begin(),
                               indexMultikeyPathsForWrite[i].end(),
                               multikeyPaths[i].begin(),
                               multikeyPaths[i].end())) {
                // If 'multikeyPaths' contains a new path component that causes this index to be
                // multikey, then we must update the index metadata in the CollectionCatalogEntry.
                newPathIsMultikey = true;
                break;
            }
        }

        if (!newPathIsMultikey) {
            // Otherwise, if all the path components in 'multikeyPaths' are already tracked in
            // '_indexMultikeyPaths', then there's nothing more for us to do.
            return;
        }
    }

    MultikeyPaths paths = _indexTracksMultikeyPathsInCatalog ? multikeyPaths : MultikeyPaths{};

    // On a primary, we can simply assign this write the same timestamp as the index creation,
    // insert, or update that caused this index to become multikey. This is because if two
    // operations concurrently try to change the index to be multikey, they will conflict and the
    // loser will simply get a higher timestamp and go into the oplog second with a later optime.
    //
    // On a secondary, writes must get the timestamp of their oplog entry, and the multikey change
    // must occur before the timestamp of the earliest write that makes the index multikey.
    // Secondaries only serialize writes by document, not by collection. If two inserts that both
    // make an index multikey are applied out of order, changing the index to multikey at the
    // insert timestamps would change the index to multikey at the later timestamp, which would be
    // wrong. To prevent this, rather than setting the index to be multikey here, we add the
    // necessary information to the OperationContext and do the write at the timestamp of the
    // beginning of the batch.
    //
    // One exception to this rule is for background indexes. Background indexes are built using
    // a different OperationContext and thus this information would be ignored. Background index
    // builds happen concurrently though and thus the multikey write can safely occur at the
    // current clock time. Once a background index is committed, if a future write makes
    // it multikey, that write will be marked as "isTrackingMultikeyPathInfo" on the applier's
    // OperationContext and we can safely defer that write to the end of the batch.
    if (MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo()) {
        MultikeyPathTracker::get(opCtx).addMultikeyPathInfo(
            {collection->ns(), _descriptor->indexName(), multikeyMetadataKeys, std::move(paths)});
        return;
    }

    // If multikeyMetadataKeys is non-empty, we must insert these keys into the index itself. We do
    // not have to account for potential dupes, since all metadata keys are indexed against a single
    // RecordId. An attempt to write a duplicate key will therefore be ignored.
    if (!multikeyMetadataKeys.empty()) {
        uassertStatusOK(accessMethod()->insertKeys(
            opCtx, collection, multikeyMetadataKeys, {}, {}, {}, nullptr));
    }

    // Mark the catalog as multikey, and record the multikey paths if applicable.
    if (opCtx->inMultiDocumentTransaction()) {
        auto status = _setMultikeyInMultiDocumentTransaction(opCtx, collection, paths);
        // Retry without side transaction.
        if (!status.isOK()) {
            _catalogSetMultikey(opCtx, collection, paths);
        }
    } else {
        _catalogSetMultikey(opCtx, collection, paths);
    }
}

void IndexCatalogEntryImpl::forceSetMultikey(OperationContext* const opCtx,
                                             const CollectionPtr& coll,
                                             bool isMultikey,
                                             const MultikeyPaths& multikeyPaths) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(coll->ns(), MODE_X));

    // Don't check _indexTracksMultikeyPathsInCatalog because the caller may be intentionally trying
    // to bypass this check. That is, pre-3.4 indexes may be 'stuck' in a state where they are not
    // tracking multikey paths in the catalog (i.e. the multikeyPaths field is absent), but the
    // caller wants to upgrade this index because it knows exactly which paths are multikey. We rely
    // on the following function to make sure this upgrade only takes place on index types that
    // currently support path-level multikey path tracking.
    coll->forceSetIndexIsMultikey(opCtx, _descriptor.get(), isMultikey, multikeyPaths);

    // The prior call to set the multikey metadata in the catalog does some validation and clean up
    // based on the inputs, so reset the multikey variables based on what is actually in the durable
    // catalog entry.
    {
        stdx::lock_guard<Latch> lk(_indexMultikeyPathsMutex);
        const bool isMultikeyInCatalog =
            _catalogIsMultikey(opCtx, coll, &_indexMultikeyPathsForRead);
        _isMultikeyForRead.store(isMultikeyInCatalog);
        _isMultikeyForWrite.store(isMultikeyInCatalog);
        _indexTracksMultikeyPathsInCatalog = !_indexMultikeyPathsForRead.empty();
    }

    // Since multikey metadata has changed, invalidate the query cache.
    CollectionQueryInfo::get(coll).clearQueryCacheForSetMultikey(coll);
}

Status IndexCatalogEntryImpl::_setMultikeyInMultiDocumentTransaction(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const MultikeyPaths& multikeyPaths) const {
    // If we are inside a multi-document transaction, we write the on-disk multikey update in a
    // separate transaction so that it will not generate prepare conflicts with other operations
    // that try to set the multikey flag. In general, it should always be safe to update the
    // multikey flag earlier than necessary, and so we are not concerned with the atomicity of the
    // multikey flag write and the parent transaction. We can do this write separately and commit it
    // before the parent transaction commits.
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

    // If the index is not visible within the side transaction, the index may have been created,
    // but not committed, in the parent transaction. Therefore, we abandon the side transaction
    // and set the multikey flag in the parent transaction.
    if (!isPresentInMySnapshot(opCtx)) {
        return {ErrorCodes::SnapshotUnavailable, "index not visible in side transaction"};
    }

    writeConflictRetry(
        opCtx, "set index multikey", collection->ns().ns(), [&] {
            WriteUnitOfWork wuow(opCtx);

            // If we have a prepare optime for recovery, then we always use that. This is safe since
            // the prepare timestamp is always <= the commit timestamp of a transaction, which
            // satisfies the correctness requirement for multikey writes i.e. they must occur at or
            // before the first write that set the multikey flag. This only occurs when
            // reconstructing prepared transactions, and not during replication recovery oplog
            // application.
            auto recoveryPrepareOpTime = txnParticipant.getPrepareOpTimeForRecovery();
            if (!recoveryPrepareOpTime.isNull()) {
                // We might replay a prepared transaction behind the oldest timestamp during initial
                // sync or behind the stable timestamp during rollback. During initial sync, we
                // may not have a stable timestamp. Therefore, we need to round up
                // the multi-key write timestamp to the max of the three so that we don't write
                // behind the oldest/stable timestamp. This code path is only hit during initial
                // sync/recovery when reconstructing prepared transactions and so we don't expect
                // the oldest/stable timestamp to advance concurrently.
                auto status = opCtx->recoveryUnit()->setTimestamp(std::max(
                    {recoveryPrepareOpTime.getTimestamp(),
                     opCtx->getServiceContext()->getStorageEngine()->getOldestTimestamp(),
                     opCtx->getServiceContext()->getStorageEngine()->getStableTimestamp()}));
                fassert(31164, status);
            } else {
                // If there is no recovery prepare OpTime, then this node must be a primary. We
                // write a noop oplog entry to get a properly ordered timestamp.
                invariant(opCtx->writesAreReplicated());

                auto msg = BSON("msg"
                                << "Setting index to multikey"
                                << "coll" << collection->ns().ns() << "index"
                                << _descriptor->indexName());
                opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(opCtx, msg);
            }

            _catalogSetMultikey(opCtx, collection, multikeyPaths);

            wuow.commit();
        });

    return Status::OK();
}

std::shared_ptr<Ident> IndexCatalogEntryImpl::getSharedIdent() const {
    return {shared_from_this(), _accessMethod->getSortedDataInterface()};  // aliasing constructor
}

// ----

NamespaceString IndexCatalogEntryImpl::getNSSFromCatalog(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->getEntry(_catalogId).nss;
}

bool IndexCatalogEntryImpl::isReadyInMySnapshot(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->isIndexReady(opCtx, _catalogId, _descriptor->indexName());
}

bool IndexCatalogEntryImpl::isPresentInMySnapshot(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->isIndexPresent(opCtx, _catalogId, _descriptor->indexName());
}

bool IndexCatalogEntryImpl::_catalogIsMultikey(OperationContext* opCtx,
                                               const CollectionPtr& collection,
                                               MultikeyPaths* multikeyPaths) const {
    return collection->isIndexMultikey(opCtx, _descriptor->indexName(), multikeyPaths);
}

void IndexCatalogEntryImpl::_catalogSetMultikey(OperationContext* opCtx,
                                                const CollectionPtr& collection,
                                                const MultikeyPaths& multikeyPaths) const {
    // It's possible that the index type (e.g. ascending/descending index) supports tracking
    // path-level multikey information, but this particular index doesn't.
    // CollectionCatalogEntry::setIndexIsMultikey() requires that we discard the path-level
    // multikey information in order to avoid unintentionally setting path-level multikey
    // information on an index created before 3.4.
    auto indexMetadataHasChanged =
        collection->setIndexIsMultikey(opCtx, _descriptor->indexName(), multikeyPaths);

    // In the absence of using the storage engine to read from the catalog, we must set multikey
    // prior to the storage engine transaction committing.
    //
    // Moreover, there must not be an `onRollback` handler to reset this back to false. Given a long
    // enough pause in processing `onRollback` handlers, a later writer that successfully flipped
    // multikey can be undone. Alternatively, one could use a counter instead of a boolean to avoid
    // that problem.
    _isMultikeyForRead.store(true);
    if (_indexTracksMultikeyPathsInCatalog) {
        stdx::lock_guard<Latch> lk(_indexMultikeyPathsMutex);
        [[maybe_unused]] const bool isMultikeyInCatalog =
            _catalogIsMultikey(opCtx, collection, &_indexMultikeyPathsForRead);
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            _indexMultikeyPathsForRead[i].insert(multikeyPaths[i].begin(), multikeyPaths[i].end());
        }
    }
    if (indexMetadataHasChanged) {
        LOGV2_DEBUG(4718705,
                    1,
                    "Index set to multi key, clearing query plan cache",
                    "namespace"_attr = collection->ns(),
                    "keyPattern"_attr = _descriptor->keyPattern());
        CollectionQueryInfo::get(collection).clearQueryCacheForSetMultikey(collection);
    }

    opCtx->recoveryUnit()->onCommit([this](boost::optional<Timestamp>) {
        // Writers must attempt to flip multikey until it's confirmed a storage engine
        // transaction successfully commits. Only after this point may a writer optimize out
        // flipping multikey.
        _isMultikeyForWrite.store(true);
    });
}

}  // namespace mongo
