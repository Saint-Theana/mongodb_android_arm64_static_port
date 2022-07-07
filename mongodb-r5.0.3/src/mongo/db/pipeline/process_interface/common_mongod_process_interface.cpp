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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/process_interface/common_mongod_process_interface.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog/list_indexes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/transaction_coordinator_curop.h"
#include "mongo/db/s/transaction_coordinator_worker_curop_repository.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/document_source_merge_cursors.h"

namespace mongo {

namespace {

class MongoDResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext* opCtx) override {
        // We're about to block. Check back in the session so that it's available to other
        // threads. Note that we may block on a request to _ourselves_, meaning that we may have to
        // wait for another thread which will use the same session. This step is necessary
        // to prevent deadlocks.

        Session* const session = OperationContextSession::get(opCtx);
        if (session) {
            if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
                txnParticipant.stashTransactionResources(opCtx);
            }

            MongoDOperationContextSession::checkIn(opCtx);
        }
        _yielded = (session != nullptr);
    }

    void unyield(OperationContext* opCtx) override {
        if (_yielded) {
            // This may block on a sub-operation on this node finishing. It's possible that while
            // blocked on the network layer, another shard could have responded, theoretically
            // unblocking this thread of execution. However, we must wait until the child operation
            // on this shard finishes so we can get the session back. This may limit the throughput
            // of the operation, but it's correct.
            MongoDOperationContextSession::checkOut(opCtx);

            if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
                // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code
                // which relies on this parameter does not distinguish/care about the difference so
                // we simply always pass 'aggregate'.
                txnParticipant.unstashTransactionResources(opCtx, "aggregate");
            }
        }
    }

private:
    bool _yielded = false;
};

// Returns true if the field names of 'keyPattern' are exactly those in 'uniqueKeyPaths', and each
// of the elements of 'keyPattern' is numeric, i.e. not "text", "$**", or any other special type of
// index.
bool keyPatternNamesExactPaths(const BSONObj& keyPattern,
                               const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

bool supportsUniqueKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const IndexCatalogEntry* index,
                       const std::set<FieldPath>& uniqueKeyPaths) {
    return (index->descriptor()->unique() && !index->descriptor()->isPartial() &&
            keyPatternNamesExactPaths(index->descriptor()->keyPattern(), uniqueKeyPaths) &&
            CollatorInterface::collatorsMatch(index->getCollator(), expCtx->getCollator()));
}

// In an operation across GetMore requests we need to check that ignore conflicts is set for each
// write to the RecordStore.
void setIgnoreConflictsWriteBehavior(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (expCtx->opCtx->recoveryUnit()->getPrepareConflictBehavior() !=
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites) {
        expCtx->opCtx->recoveryUnit()->abandonSnapshot();
        expCtx->opCtx->recoveryUnit()->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
    }
}

}  // namespace

std::unique_ptr<TransactionHistoryIteratorBase>
CommonMongodProcessInterface::createTransactionHistoryIterator(repl::OpTime time) const {
    bool permitYield = true;
    return std::unique_ptr<TransactionHistoryIteratorBase>(
        new TransactionHistoryIterator(time, permitYield));
}

std::vector<Document> CommonMongodProcessInterface::getIndexStats(OperationContext* opCtx,
                                                                  const NamespaceString& ns,
                                                                  StringData host,
                                                                  bool addShardName) {
    AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, ns);

    std::vector<Document> indexStats;
    if (!collection) {
        LOGV2_DEBUG(23881,
                    2,
                    "Collection not found on index stats retrieval: {ns_ns}",
                    "ns_ns"_attr = ns.ns());
        return indexStats;
    }

    auto indexStatsMap =
        CollectionIndexUsageTrackerDecoration::get(collection->getSharedDecorations())
            .getUsageStats();
    for (auto&& indexStatsMapIter : *indexStatsMap) {
        auto indexName = indexStatsMapIter.first;
        auto stats = indexStatsMapIter.second;
        MutableDocument doc;
        doc["name"] = Value(indexName);
        doc["key"] = Value(stats->indexKey);
        doc["host"] = Value(host);
        doc["accesses"]["ops"] = Value(stats->accesses.loadRelaxed());
        doc["accesses"]["since"] = Value(stats->trackerStartTime);

        if (addShardName)
            doc["shard"] = Value(getShardName(opCtx));

        // Retrieve the relevant index entry.
        auto idxCatalog = collection->getIndexCatalog();
        auto idx = idxCatalog->findIndexByName(opCtx,
                                               indexName,
                                               /* includeUnfinishedIndexes */ true);
        uassert(ErrorCodes::IndexNotFound,
                "Could not find entry in IndexCatalog for index " + indexName,
                idx);
        auto entry = idxCatalog->getEntry(idx);
        doc["spec"] = Value(idx->infoObj());

        // Not all indexes in the CollectionIndexUsageTracker may be visible or consistent with our
        // snapshot. For this reason, it is unsafe to check `isReady` on the entry, which
        // asserts that the index's in-memory state is consistent with our snapshot.
        if (!entry->isPresentInMySnapshot(opCtx)) {
            continue;
        }

        if (!entry->isReadyInMySnapshot(opCtx)) {
            doc["building"] = Value(true);
        }

        indexStats.push_back(doc.freeze());
    }
    return indexStats;
}

void CommonMongodProcessInterface::appendLatencyStats(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      bool includeHistograms,
                                                      BSONObjBuilder* builder) const {
    Top::get(opCtx->getServiceContext()).appendLatencyStats(nss, includeHistograms, builder);
}

Status CommonMongodProcessInterface::appendStorageStats(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const StorageStatsSpec& spec,
                                                        BSONObjBuilder* builder) const {
    return appendCollectionStorageStats(opCtx, nss, spec, builder);
}

Status CommonMongodProcessInterface::appendRecordCount(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       BSONObjBuilder* builder) const {
    return appendCollectionRecordCount(opCtx, nss, builder);
}

Status CommonMongodProcessInterface::appendQueryExecStats(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          BSONObjBuilder* builder) const {
    AutoGetCollectionForReadCommand collection(opCtx, nss);

    if (!collection.getDb()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Database [" << nss.db().toString() << "] not found."};
    }

    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection [" << nss.toString() << "] not found."};
    }

    auto collectionScanStats =
        CollectionIndexUsageTrackerDecoration::get(collection->getSharedDecorations())
            .getCollectionScanStats();

    dassert(collectionScanStats.collectionScans <=
            static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    dassert(collectionScanStats.collectionScansNonTailable <=
            static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    builder->append("queryExecStats",
                    BSON("collectionScans" << BSON(
                             "total" << static_cast<long long>(collectionScanStats.collectionScans)
                                     << "nonTailable"
                                     << static_cast<long long>(
                                            collectionScanStats.collectionScansNonTailable))));

    return Status::OK();
}

BSONObj CommonMongodProcessInterface::getCollectionOptions(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    AutoGetCollectionForReadCommand collection(opCtx, nss);
    BSONObj collectionOptions = {};
    if (!collection.getDb()) {
        return collectionOptions;
    }
    if (!collection) {
        return collectionOptions;
    }

    collectionOptions = collection->getCollectionOptions().toBSON();
    return collectionOptions;
}

std::unique_ptr<Pipeline, PipelineDeleter>
CommonMongodProcessInterface::attachCursorSourceToPipelineForLocalRead(Pipeline* ownedPipeline) {
    auto expCtx = ownedPipeline->getContext();
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));

    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceCursor*>(pipeline->getSources().front().get()));

    boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> autoColl;
    const NamespaceStringOrUUID nsOrUUID = expCtx->uuid
        ? NamespaceStringOrUUID{expCtx->ns.db().toString(), *expCtx->uuid}
        : expCtx->ns;
    autoColl.emplace(expCtx->opCtx,
                     nsOrUUID,
                     AutoGetCollectionViewMode::kViewsForbidden,
                     Date_t::max(),
                     AutoStatsTracker::LogMode::kUpdateTop);

    PipelineD::buildAndAttachInnerQueryExecutorToPipeline(
        autoColl->getCollection(), expCtx->ns, nullptr, pipeline.get());

    return pipeline;
}

std::string CommonMongodProcessInterface::getShardName(OperationContext* opCtx) const {
    if (ShardingState::get(opCtx)->enabled()) {
        return ShardingState::get(opCtx)->shardId().toString();
    }

    return std::string();
}

std::vector<GenericCursor> CommonMongodProcessInterface::getIdleCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, CurrentOpUserMode userMode) const {
    return CursorManager::get(expCtx->opCtx)->getIdleCursors(expCtx->opCtx, userMode);
}

boost::optional<Document> CommonMongodProcessInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern,
    bool allowSpeculativeMajorityRead) {
    invariant(!readConcern);  // We don't currently support a read concern on mongod - it's only
                              // expected to be necessary on mongos.
    invariant(!allowSpeculativeMajorityRead);  // We don't expect 'allowSpeculativeMajorityRead' on
                                               // mongod - it's only expected to be necessary on
                                               // mongos.

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        // Be sure to do the lookup using the collection default collation
        auto foreignExpCtx = expCtx->copyWith(
            nss,
            collectionUUID,
            _getCollectionDefaultCollator(expCtx->opCtx, nss.db(), collectionUUID));
        // When looking up on a mongoD, we only ever want to read from the local collection. By
        // default, makePipeline will attach a cursor source which may read from remote if the
        // collection is sharded, so we configure it to not allow that here.
        MakePipelineOptions opts;
        opts.allowTargetingShards = false;
        pipeline = Pipeline::makePipeline({BSON("$match" << documentKey)}, foreignExpCtx, opts);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = pipeline->getNext();
    if (auto next = pipeline->getNext()) {
        uasserted(ErrorCodes::ChangeStreamFatalError,
                  str::stream() << "found more than one document with document key "
                                << documentKey.toString() << " [" << lookedUpDocument->toString()
                                << ", " << next->toString() << "]");
    }

    // Set the speculative read timestamp appropriately after we do a document lookup locally. We
    // set the speculative read timestamp based on the timestamp used by the transaction.
    repl::SpeculativeMajorityReadInfo& speculativeMajorityReadInfo =
        repl::SpeculativeMajorityReadInfo::get(expCtx->opCtx);
    if (speculativeMajorityReadInfo.isSpeculativeRead()) {
        // Speculative majority reads are required to use the 'kNoOverlap' read source.
        // Storage engine operations require at least Global IS.
        Lock::GlobalLock lk(expCtx->opCtx, MODE_IS);
        invariant(expCtx->opCtx->recoveryUnit()->getTimestampReadSource() ==
                  RecoveryUnit::ReadSource::kNoOverlap);
        boost::optional<Timestamp> readTs =
            expCtx->opCtx->recoveryUnit()->getPointInTimeReadTimestamp(expCtx->opCtx);
        invariant(readTs);
        speculativeMajorityReadInfo.setSpeculativeReadTimestampForward(*readTs);
    }

    return lookedUpDocument;
}

BackupCursorState CommonMongodProcessInterface::openBackupCursor(
    OperationContext* opCtx, const StorageEngine::BackupOptions& options) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        return backupCursorHooks->openBackupCursor(opCtx, options);
    } else {
        uasserted(50956, "Backup cursors are an enterprise only feature.");
    }
}

void CommonMongodProcessInterface::closeBackupCursor(OperationContext* opCtx,
                                                     const UUID& backupId) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        backupCursorHooks->closeBackupCursor(opCtx, backupId);
    } else {
        uasserted(50955, "Backup cursors are an enterprise only feature.");
    }
}

BackupCursorExtendState CommonMongodProcessInterface::extendBackupCursor(
    OperationContext* opCtx, const UUID& backupId, const Timestamp& extendTo) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        return backupCursorHooks->extendBackupCursor(opCtx, backupId, extendTo);
    } else {
        uasserted(51010, "Backup cursors are an enterprise only feature.");
    }
}

std::vector<BSONObj> CommonMongodProcessInterface::getMatchingPlanCacheEntryStats(
    OperationContext* opCtx, const NamespaceString& nss, const MatchExpression* matchExp) const {
    const auto serializer = [](const PlanCacheEntry& entry) {
        BSONObjBuilder out;
        Explain::planCacheEntryToBSON(entry, &out);
        return out.obj();
    };

    const auto predicate = [&matchExp](const BSONObj& obj) {
        return !matchExp ? true : matchExp->matchesBSON(obj);
    };

    AutoGetCollection collection(opCtx, nss, MODE_IS);
    uassert(
        50933, str::stream() << "collection '" << nss.toString() << "' does not exist", collection);

    const auto planCache = CollectionQueryInfo::get(collection.getCollection()).getPlanCache();
    invariant(planCache);

    return planCache->getMatchingStats(serializer, predicate);
}

bool CommonMongodProcessInterface::fieldsHaveSupportingUniqueIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& fieldPaths) const {
    auto* opCtx = expCtx->opCtx;
    // We purposefully avoid a helper like AutoGetCollection here because we don't want to check the
    // db version or do anything else. We simply want to protect against concurrent modifications to
    // the catalog.
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.db());
    auto collection =
        db ? CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss) : nullptr;
    if (!collection) {
        return fieldPaths == std::set<FieldPath>{"_id"};
    }

    auto indexIterator = collection->getIndexCatalog()->getIndexIterator(opCtx, false);
    while (indexIterator->more()) {
        const IndexCatalogEntry* entry = indexIterator->next();
        if (supportsUniqueKey(expCtx, entry, fieldPaths)) {
            return true;
        }
    }
    return false;
}

BSONObj CommonMongodProcessInterface::_reportCurrentOpForClient(
    OperationContext* opCtx,
    Client* client,
    CurrentOpTruncateMode truncateOps,
    CurrentOpBacktraceMode backtraceMode) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(opCtx,
                                    client,
                                    (truncateOps == CurrentOpTruncateMode::kTruncateOps),
                                    (backtraceMode == CurrentOpBacktraceMode::kIncludeBacktrace),
                                    &builder);

    OperationContext* clientOpCtx = client->getOperationContext();

    if (clientOpCtx) {
        if (auto txnParticipant = TransactionParticipant::get(clientOpCtx)) {
            txnParticipant.reportUnstashedState(clientOpCtx, &builder);
        }

        // Append lock stats before returning.
        if (auto lockerInfo = clientOpCtx->lockState()->getLockerInfo(
                CurOp::get(*clientOpCtx)->getLockStatsBase())) {
            fillLockerInfo(*lockerInfo, builder);
        }

        if (auto tcWorkerRepo = getTransactionCoordinatorWorkerCurOpRepository()) {
            tcWorkerRepo->reportState(clientOpCtx, &builder);
        }

        auto flowControlStats = clientOpCtx->lockState()->getFlowControlStats();
        flowControlStats.writeToBuilder(builder);
    }

    return builder.obj();
}

void CommonMongodProcessInterface::_reportCurrentOpsForTransactionCoordinators(
    OperationContext* opCtx, bool includeIdle, std::vector<BSONObj>* ops) const {
    reportCurrentOpsForTransactionCoordinators(opCtx, includeIdle, ops);
}

void CommonMongodProcessInterface::_reportCurrentOpsForPrimaryOnlyServices(
    OperationContext* opCtx,
    CurrentOpConnectionsMode connMode,
    CurrentOpSessionsMode sessionMode,
    std::vector<BSONObj>* ops) const {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    invariant(registry);

    registry->reportServiceInfoForCurrentOp(connMode, sessionMode, ops);
}

void CommonMongodProcessInterface::_reportCurrentOpsForIdleSessions(
    OperationContext* opCtx, CurrentOpUserMode userMode, std::vector<BSONObj>* ops) const {
    auto sessionCatalog = SessionCatalog::get(opCtx);

    const bool authEnabled =
        AuthorizationSession::get(opCtx->getClient())->getAuthorizationManager().isAuthEnabled();

    // If the user is listing only their own ops, we use makeSessionFilterForAuthenticatedUsers to
    // create a pattern that will match against all authenticated usernames for the current client.
    // If the user is listing ops for all users, we create an empty pattern; constructing an
    // instance of SessionKiller::Matcher with this empty pattern will return all sessions.
    auto sessionFilter = (authEnabled && userMode == CurrentOpUserMode::kExcludeOthers
                              ? makeSessionFilterForAuthenticatedUsers(opCtx)
                              : KillAllSessionsByPatternSet{{}});

    sessionCatalog->scanSessions({std::move(sessionFilter)}, [&](const ObservableSession& session) {
        auto op = TransactionParticipant::get(session).reportStashedState(opCtx);
        if (!op.isEmpty()) {
            ops->emplace_back(op);
        }
    });
}

std::unique_ptr<CollatorInterface> CommonMongodProcessInterface::_getCollectionDefaultCollator(
    OperationContext* opCtx, StringData dbName, UUID collectionUUID) {
    auto it = _collatorCache.find(collectionUUID);
    if (it == _collatorCache.end()) {
        auto collator = [&]() -> std::unique_ptr<CollatorInterface> {
            AutoGetCollection autoColl(opCtx, {dbName.toString(), collectionUUID}, MODE_IS);
            if (!autoColl.getCollection()) {
                // This collection doesn't exist, so assume a nullptr default collation
                return nullptr;
            } else {
                auto defaultCollator = autoColl.getCollection()->getDefaultCollator();
                // Clone the collator so that we can safely use the pointer if the collection
                // disappears right after we release the lock.
                return defaultCollator ? defaultCollator->clone() : nullptr;
            }
        }();

        it = _collatorCache.emplace(collectionUUID, std::move(collator)).first;
    }

    auto& collator = it->second;
    return collator ? collator->clone() : nullptr;
}

std::unique_ptr<ResourceYielder> CommonMongodProcessInterface::getResourceYielder() const {
    return std::make_unique<MongoDResourceYielder>();
}


std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
CommonMongodProcessInterface::ensureFieldsUniqueOrResolveDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<std::set<FieldPath>> fieldPaths,
    boost::optional<ChunkVersion> targetCollectionVersion,
    const NamespaceString& outputNs) const {
    uassert(51123,
            "Unexpected target chunk version specified",
            !targetCollectionVersion || expCtx->fromMongos);

    if (!fieldPaths) {
        uassert(51124, "Expected fields to be provided from mongos", !expCtx->fromMongos);
        return {std::set<FieldPath>{"_id"}, targetCollectionVersion};
    }

    // Make sure the 'fields' array has a supporting index. Skip this check if the command is sent
    // from mongos since the 'fields' check would've happened already.
    if (!expCtx->fromMongos) {
        uassert(51183,
                "Cannot find index to verify that join fields will be unique",
                fieldsHaveSupportingUniqueIndex(expCtx, outputNs, *fieldPaths));
    }
    return {*fieldPaths, targetCollectionVersion};
}

write_ops::InsertCommandRequest CommonMongodProcessInterface::buildInsertOp(
    const NamespaceString& nss, std::vector<BSONObj>&& objs, bool bypassDocValidation) {
    write_ops::InsertCommandRequest insertOp(nss);
    insertOp.setDocuments(std::move(objs));
    insertOp.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(bypassDocValidation);
        return wcb;
    }());
    return insertOp;
}

write_ops::UpdateCommandRequest CommonMongodProcessInterface::buildUpdateOp(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    BatchedObjects&& batch,
    UpsertType upsert,
    bool multi) {
    write_ops::UpdateCommandRequest updateOp(nss);
    updateOp.setUpdates([&] {
        std::vector<write_ops::UpdateOpEntry> updateEntries;
        for (auto&& obj : batch) {
            updateEntries.push_back([&] {
                write_ops::UpdateOpEntry entry;
                auto&& [q, u, c] = obj;
                entry.setQ(std::move(q));
                entry.setU(std::move(u));
                entry.setC(std::move(c));
                entry.setUpsert(upsert != UpsertType::kNone);
                entry.setUpsertSupplied(
                    {{entry.getUpsert(), upsert == UpsertType::kInsertSuppliedDoc}});
                entry.setMulti(multi);
                return entry;
            }());
        }
        return updateEntries;
    }());
    updateOp.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(expCtx->bypassDocumentValidation);
        return wcb;
    }());
    auto [constants, letParams] =
        expCtx->variablesParseState.transitionalCompatibilitySerialize(expCtx->variables);
    updateOp.setLegacyRuntimeConstants(std::move(constants));
    if (!letParams.isEmpty()) {
        updateOp.setLet(std::move(letParams));
    }
    return updateOp;
}

BSONObj CommonMongodProcessInterface::_convertRenameToInternalRename(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {

    BSONObjBuilder newCmd;
    newCmd.append("internalRenameIfOptionsAndIndexesMatch", 1);
    newCmd.append("from", renameCommandObj["renameCollection"].String());
    newCmd.append("to", renameCommandObj["to"].String());
    newCmd.append("collectionOptions", originalCollectionOptions);
    BSONArrayBuilder indexArrayBuilder(newCmd.subarrayStart("indexes"));
    for (auto&& index : originalIndexes) {
        indexArrayBuilder.append(index);
    }
    indexArrayBuilder.done();
    return newCmd.obj();
}

void CommonMongodProcessInterface::writeRecordsToRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    RecordStore* rs,
    std::vector<Record>* records,
    const std::vector<Timestamp>& ts) const {
    tassert(5643012, "Attempted to write to record store with nullptr", records);
    setIgnoreConflictsWriteBehavior(expCtx);
    writeConflictRetry(expCtx->opCtx, "MPI::writeRecordsToRecordStore", expCtx->ns.ns(), [&] {
        AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
        WriteUnitOfWork wuow(expCtx->opCtx);
        auto writeResult = rs->insertRecords(expCtx->opCtx, records, ts);
        tassert(5643002,
                str::stream() << "Failed to write to disk because " << writeResult.reason(),
                writeResult.isOK());
        wuow.commit();
    });
}
std::unique_ptr<TemporaryRecordStore> CommonMongodProcessInterface::createTemporaryRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    expCtx->opCtx->recoveryUnit()->abandonSnapshot();
    expCtx->opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
    return expCtx->opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
        expCtx->opCtx);
}

Document CommonMongodProcessInterface::readRecordFromRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, RecordStore* rs, RecordId rID) const {
    RecordData possibleRecord;
    AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
    auto foundDoc = rs->findRecord(expCtx->opCtx, RecordId(rID), &possibleRecord);
    tassert(775101, str::stream() << "Could not find document id " << rID, foundDoc);
    return Document(possibleRecord.toBson());
}

void CommonMongodProcessInterface::deleteRecordFromRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, RecordStore* rs, RecordId rID) const {
    setIgnoreConflictsWriteBehavior(expCtx);
    writeConflictRetry(expCtx->opCtx, "MPI::deleteFromRecordStore", expCtx->ns.ns(), [&] {
        AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
        WriteUnitOfWork wuow(expCtx->opCtx);
        rs->deleteRecord(expCtx->opCtx, rID);
        wuow.commit();
    });
}

void CommonMongodProcessInterface::truncateRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, RecordStore* rs) const {
    setIgnoreConflictsWriteBehavior(expCtx);
    writeConflictRetry(expCtx->opCtx, "MPI::truncateRecordStore", expCtx->ns.ns(), [&] {
        AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
        WriteUnitOfWork wuow(expCtx->opCtx);
        auto status = rs->truncate(expCtx->opCtx);
        tassert(5643000, "Unable to clear record store", status.isOK());
        wuow.commit();
    });
}

void CommonMongodProcessInterface::deleteTemporaryRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<TemporaryRecordStore> rs) const {
    setIgnoreConflictsWriteBehavior(expCtx);
    AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
    rs->finalizeTemporaryTable(expCtx->opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
}

}  // namespace mongo
