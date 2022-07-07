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

#include "mongo/db/catalog/index_catalog_impl.h"

#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_build_block.h"
#include "mongo/db/catalog/index_catalog_entry_impl.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl_set_member_in_standalone_mode.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_util.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(skipUnindexingDocumentWhenDeleted);
MONGO_FAIL_POINT_DEFINE(skipIndexNewRecords);

using std::endl;
using std::string;
using std::unique_ptr;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

const BSONObj IndexCatalogImpl::_idObj = BSON("_id" << 1);

// -------------

std::unique_ptr<IndexCatalog> IndexCatalogImpl::clone() const {
    return std::make_unique<IndexCatalogImpl>(*this);
}

Status IndexCatalogImpl::init(OperationContext* opCtx, Collection* collection) {
    vector<string> indexNames;
    collection->getAllIndexes(&indexNames);
    const bool replSetMemberInStandaloneMode =
        getReplSetMemberInStandaloneMode(opCtx->getServiceContext());

    boost::optional<Timestamp> recoveryTs = boost::none;
    if (auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        storageEngine->supportsRecoveryTimestamp()) {
        recoveryTs = storageEngine->getRecoveryTimestamp();
    }

    for (size_t i = 0; i < indexNames.size(); i++) {
        const string& indexName = indexNames[i];
        BSONObj spec = collection->getIndexSpec(indexName).getOwned();
        BSONObj keyPattern = spec.getObjectField("key");

        // TODO SERVER-51871: Delete this block once 5.0 becomes last-lts.
        if (spec.hasField(IndexDescriptor::kGeoHaystackBucketSize)) {
            LOGV2_OPTIONS(4670602,
                          {logv2::LogTag::kStartupWarnings},
                          "Found an existing geoHaystack index in the catalog. Support for "
                          "geoHaystack indexes has been removed. Instead create a 2d index. See "
                          "https://dochub.mongodb.org/core/4.4-deprecate-geoHaystack");
        }
        auto descriptor = std::make_unique<IndexDescriptor>(_getAccessMethodName(keyPattern), spec);

        // TTL indexes are not compatible with capped collections.
        if (spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName) &&
            !collection->isCapped()) {
            TTLCollectionCache::get(opCtx->getServiceContext())
                .registerTTLInfo(collection->uuid(), indexName);
        }

        bool ready = collection->isIndexReady(indexName);
        if (!ready) {
            auto buildUUID = collection->getIndexBuildUUID(indexName);
            invariant(buildUUID,
                      str::stream() << "collection: " << collection->ns() << "index:" << indexName);
            // We intentionally do not drop or rebuild unfinished two-phase index builds before
            // initializing the IndexCatalog when starting a replica set member in standalone mode.
            // This is because the index build cannot complete until it receives a replicated commit
            // or abort oplog entry.
            if (replSetMemberInStandaloneMode) {
                // Indicate that this index is "frozen". It is not ready but is not currently in
                // progress either. These indexes may be dropped.
                auto flags = CreateIndexEntryFlags::kInitFromDisk | CreateIndexEntryFlags::kFrozen;
                IndexCatalogEntry* entry =
                    createIndexEntry(opCtx, collection, std::move(descriptor), flags);
                fassert(31433, !entry->isReady(opCtx, collection));
            } else {
                // Initializing with unfinished indexes may occur during rollback or startup.
                auto flags = CreateIndexEntryFlags::kInitFromDisk;
                IndexCatalogEntry* entry =
                    createIndexEntry(opCtx, collection, std::move(descriptor), flags);
                fassert(4505500, !entry->isReady(opCtx, collection));
            }
        } else {
            auto flags = CreateIndexEntryFlags::kInitFromDisk | CreateIndexEntryFlags::kIsReady;
            IndexCatalogEntry* entry =
                createIndexEntry(opCtx, collection, std::move(descriptor), flags);
            fassert(17340, entry->isReady(opCtx, collection));

            // When initializing indexes from disk, we conservatively set the minimumVisibleSnapshot
            // to non _id indexes to the recovery timestamp. The _id index is left visible. It's
            // assumed if the collection is visible, it's _id is valid to be used.
            if (recoveryTs && !entry->descriptor()->isIdIndex()) {
                entry->setMinimumVisibleSnapshot(recoveryTs.get());
            }
        }
    }

    CollectionQueryInfo::get(collection).init(opCtx, collection);
    return Status::OK();
}

std::unique_ptr<IndexCatalog::IndexIterator> IndexCatalogImpl::getIndexIterator(
    OperationContext* const opCtx, const bool includeUnfinishedIndexes) const {
    if (!includeUnfinishedIndexes) {
        // If the caller only wants the ready indexes, we return an iterator over the catalog's
        // ready indexes vector. When the user advances this iterator, it will filter out any
        // indexes that were not ready at the OperationContext's read timestamp.
        return std::make_unique<ReadyIndexesIterator>(
            opCtx, _readyIndexes.begin(), _readyIndexes.end());
    }

    // If the caller wants all indexes, for simplicity of implementation, we copy the pointers to
    // a new vector. The vector's ownership is passed to the iterator. The query code path from an
    // external client is not expected to hit this case so the cost isn't paid by the important
    // code path.
    auto allIndexes = std::make_unique<std::vector<IndexCatalogEntry*>>();
    for (auto it = _readyIndexes.begin(); it != _readyIndexes.end(); ++it) {
        allIndexes->push_back(it->get());
    }

    for (auto it = _buildingIndexes.begin(); it != _buildingIndexes.end(); ++it) {
        allIndexes->push_back(it->get());
    }

    return std::make_unique<AllIndexesIterator>(opCtx, std::move(allIndexes));
}

string IndexCatalogImpl::_getAccessMethodName(const BSONObj& keyPattern) const {
    string pluginName = IndexNames::findPluginName(keyPattern);

    // This assert will be triggered when downgrading from a future version that
    // supports an index plugin unsupported by this version.
    uassert(17197,
            str::stream() << "Invalid index type '" << pluginName << "' "
                          << "in index " << keyPattern,
            IndexNames::isKnownName(pluginName));

    return pluginName;
}


// ---------------------------

StatusWith<BSONObj> IndexCatalogImpl::_validateAndFixIndexSpec(OperationContext* opCtx,
                                                               const CollectionPtr& collection,
                                                               const BSONObj& original) const {
    Status status = _isSpecOk(opCtx, collection, original);
    if (!status.isOK()) {
        return status;
    }

    auto swFixed = _fixIndexSpec(opCtx, collection, original);
    if (!swFixed.isOK()) {
        return swFixed;
    }

    // we double check with new index spec
    status = _isSpecOk(opCtx, collection, swFixed.getValue());
    if (!status.isOK()) {
        return status;
    }

    return swFixed;
}

Status IndexCatalogImpl::_isNonIDIndexAndNotAllowedToBuild(OperationContext* opCtx,
                                                           const BSONObj& spec) const {
    const BSONObj key = spec.getObjectField("key");
    invariant(!key.isEmpty());
    if (!IndexDescriptor::isIdIndexPattern(key)) {
        // Check whether the replica set member's config has {buildIndexes:false} set, which means
        // we are not allowed to build non-_id indexes on this server.
        if (!repl::ReplicationCoordinator::get(opCtx)->buildsIndexes()) {
            // We return an IndexAlreadyExists error so that the caller can catch it and silently
            // skip building it.
            return Status(ErrorCodes::IndexAlreadyExists,
                          "this replica set member's 'buildIndexes' setting is set to false");
        }
    }

    return Status::OK();
}

void IndexCatalogImpl::_logInternalState(OperationContext* opCtx,
                                         const CollectionPtr& collection,
                                         long long numIndexesInCollectionCatalogEntry,
                                         const std::vector<std::string>& indexNamesToDrop,
                                         bool haveIdIndex) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));

    LOGV2_ERROR(20365,
                "Internal Index Catalog state",
                "numIndexesTotal"_attr = numIndexesTotal(opCtx),
                "numIndexesInCollectionCatalogEntry"_attr = numIndexesInCollectionCatalogEntry,
                "readyIndexes_size"_attr = _readyIndexes.size(),
                "buildingIndexes_size"_attr = _buildingIndexes.size(),
                "indexNamesToDrop_size"_attr = indexNamesToDrop.size(),
                "haveIdIndex"_attr = haveIdIndex);

    // Report the ready indexes.
    for (const auto& entry : _readyIndexes) {
        const IndexDescriptor* desc = entry->descriptor();
        LOGV2_ERROR(20367,
                    "readyIndex",
                    "desc_indexName"_attr = desc->indexName(),
                    "desc_infoObj"_attr = redact(desc->infoObj()));
    }

    // Report the in-progress indexes.
    for (const auto& entry : _buildingIndexes) {
        const IndexDescriptor* desc = entry->descriptor();
        LOGV2_ERROR(20369,
                    "inprogIndex",
                    "desc_indexName"_attr = desc->indexName(),
                    "desc_infoObj"_attr = redact(desc->infoObj()));
    }

    LOGV2_ERROR(20370, "Internal Collection Catalog Entry state:");
    std::vector<std::string> allIndexes;
    std::vector<std::string> readyIndexes;

    collection->getAllIndexes(&allIndexes);
    collection->getReadyIndexes(&readyIndexes);

    for (const auto& index : allIndexes) {
        LOGV2_ERROR(20372,
                    "allIndexes",
                    "index"_attr = index,
                    "spec"_attr = redact(collection->getIndexSpec(index)));
    }

    for (const auto& index : readyIndexes) {
        LOGV2_ERROR(20374,
                    "readyIndexes",
                    "index"_attr = index,
                    "spec"_attr = redact(collection->getIndexSpec(index)));
    }

    for (const auto& indexNameToDrop : indexNamesToDrop) {
        LOGV2_ERROR(20376,
                    "indexNamesToDrop",
                    "index"_attr = indexNameToDrop,
                    "spec"_attr = redact(collection->getIndexSpec(indexNameToDrop)));
    }
}

StatusWith<BSONObj> IndexCatalogImpl::prepareSpecForCreate(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const BSONObj& original,
    const boost::optional<ResumeIndexInfo>& resumeInfo) const {
    auto swValidatedAndFixed = _validateAndFixIndexSpec(opCtx, collection, original);
    if (!swValidatedAndFixed.isOK()) {
        return swValidatedAndFixed.getStatus().withContext(
            str::stream() << "Error in specification " << original.toString());
    }

    auto validatedSpec = swValidatedAndFixed.getValue();

    // TODO SERVER-51871: Delete this block once 5.0 becomes last-lts.
    if (validatedSpec.hasField(IndexDescriptor::kGeoHaystackBucketSize)) {
        LOGV2_OPTIONS(4670601,
                      {logv2::LogTag::kStartupWarnings},
                      "Support for "
                      "geoHaystack indexes has been removed. Instead create a 2d index. See "
                      "https://dochub.mongodb.org/core/4.4-deprecate-geoHaystack");
    }

    // Check whether this is a non-_id index and there are any settings disallowing this server
    // from building non-_id indexes.
    Status status = _isNonIDIndexAndNotAllowedToBuild(opCtx, validatedSpec);
    if (!status.isOK()) {
        return status;
    }

    // First check against only the ready indexes for conflicts.
    status = _doesSpecConflictWithExisting(opCtx, collection, validatedSpec, false);
    if (!status.isOK()) {
        return status;
    }

    if (resumeInfo) {
        // Don't check against unfinished indexes if this index is being resumed, since it will
        // conflict with itself.
        return validatedSpec;
    }

    // Now we will check against all indexes, in-progress included.
    //
    // The index catalog cannot currently iterate over only in-progress indexes. So by previously
    // checking against only ready indexes without error, we know that any errors encountered
    // checking against all indexes occurred due to an in-progress index.
    status = _doesSpecConflictWithExisting(opCtx, collection, validatedSpec, true);
    if (!status.isOK()) {
        if (ErrorCodes::IndexAlreadyExists == status.code()) {
            // Callers need to be able to distinguish conflicts against ready indexes versus
            // in-progress indexes.
            return {ErrorCodes::IndexBuildAlreadyInProgress, status.reason()};
        }
        return status;
    }

    return validatedSpec;
}

std::vector<BSONObj> IndexCatalogImpl::removeExistingIndexesNoChecks(
    OperationContext* const opCtx,
    const CollectionPtr& collection,
    const std::vector<BSONObj>& indexSpecsToBuild) const {
    std::vector<BSONObj> result;
    // Filter out ready and in-progress index builds, and any non-_id indexes if 'buildIndexes' is
    // set to false in the replica set's config.
    for (const auto& spec : indexSpecsToBuild) {
        // returned to be built by the caller.
        if (ErrorCodes::OK != _isNonIDIndexAndNotAllowedToBuild(opCtx, spec)) {
            continue;
        }

        // _doesSpecConflictWithExisting currently does more work than we require here: we are only
        // interested in the index already exists error.
        if (ErrorCodes::IndexAlreadyExists ==
            _doesSpecConflictWithExisting(
                opCtx, collection, spec, true /*includeUnfinishedIndexes*/)) {
            continue;
        }

        result.push_back(spec);
    }
    return result;
}

std::vector<BSONObj> IndexCatalogImpl::removeExistingIndexes(
    OperationContext* const opCtx,
    const CollectionPtr& collection,
    const std::vector<BSONObj>& indexSpecsToBuild,
    const bool removeIndexBuildsToo) const {
    std::vector<BSONObj> result;
    for (const auto& spec : indexSpecsToBuild) {
        auto prepareResult = prepareSpecForCreate(opCtx, collection, spec);
        if (prepareResult == ErrorCodes::IndexAlreadyExists ||
            (removeIndexBuildsToo && prepareResult == ErrorCodes::IndexBuildAlreadyInProgress)) {
            continue;
        }
        uassertStatusOK(prepareResult);
        result.push_back(prepareResult.getValue());
    }
    return result;
}

IndexCatalogEntry* IndexCatalogImpl::createIndexEntry(OperationContext* opCtx,
                                                      Collection* collection,
                                                      std::unique_ptr<IndexDescriptor> descriptor,
                                                      CreateIndexEntryFlags flags) {
    Status status = _isSpecOk(opCtx, collection, descriptor->infoObj());
    if (!status.isOK()) {
        LOGV2_FATAL_NOTRACE(28782,
                            "Found an invalid index",
                            "descriptor"_attr = descriptor->infoObj(),
                            "namespace"_attr = collection->ns(),
                            "error"_attr = redact(status));
    }

    auto engine = opCtx->getServiceContext()->getStorageEngine();
    std::string ident = engine->getCatalog()->getIndexIdent(
        opCtx, collection->getCatalogId(), descriptor->indexName());

    bool isReadyIndex = CreateIndexEntryFlags::kIsReady & flags;
    bool frozen = CreateIndexEntryFlags::kFrozen & flags;
    invariant(!frozen || !isReadyIndex);

    auto* const descriptorPtr = descriptor.get();
    auto entry = std::make_shared<IndexCatalogEntryImpl>(
        opCtx, collection, ident, std::move(descriptor), frozen);

    IndexDescriptor* desc = entry->descriptor();
    const auto& collOptions = collection->getCollectionOptions();
    std::unique_ptr<SortedDataInterface> sdi =
        engine->getEngine()->getSortedDataInterface(opCtx, collOptions, ident, desc);

    std::unique_ptr<IndexAccessMethod> accessMethod =
        IndexAccessMethodFactory::get(opCtx)->make(entry.get(), std::move(sdi));

    entry->init(std::move(accessMethod));


    IndexCatalogEntry* save = entry.get();
    if (isReadyIndex) {
        _readyIndexes.add(std::move(entry));
    } else {
        _buildingIndexes.add(std::move(entry));
    }

    bool initFromDisk = CreateIndexEntryFlags::kInitFromDisk & flags;
    if (!initFromDisk && UncommittedCollections::getForTxn(opCtx, collection->ns()) == nullptr) {
        const std::string indexName = descriptorPtr->indexName();
        opCtx->recoveryUnit()->onRollback(
            [collectionDecorations = collection->getSharedDecorations(),
             indexName = std::move(indexName)] {
                CollectionIndexUsageTrackerDecoration::get(collectionDecorations)
                    .unregisterIndex(indexName);
            });
    }

    return save;
}

StatusWith<BSONObj> IndexCatalogImpl::createIndexOnEmptyCollection(OperationContext* opCtx,
                                                                   Collection* collection,
                                                                   BSONObj spec) {
    invariant(collection->uuid() == collection->uuid());
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx,
                                                                               collection->ns());
    invariant(collection->isEmpty(opCtx),
              str::stream() << "Collection must be empty. Collection: " << collection->ns()
                            << " UUID: " << collection->uuid()
                            << " Count (from size storer): " << collection->numRecords(opCtx));

    StatusWith<BSONObj> statusWithSpec = prepareSpecForCreate(opCtx, collection, spec);
    Status status = statusWithSpec.getStatus();
    if (!status.isOK())
        return status;
    spec = statusWithSpec.getValue();

    // now going to touch disk
    boost::optional<UUID> buildUUID = boost::none;
    IndexBuildBlock indexBuildBlock(
        collection->ns(), spec, IndexBuildMethod::kForeground, buildUUID);
    status = indexBuildBlock.init(opCtx, collection);
    if (!status.isOK())
        return status;

    // sanity checks, etc...
    IndexCatalogEntry* entry = indexBuildBlock.getEntry(opCtx, collection);
    invariant(entry);
    IndexDescriptor* descriptor = entry->descriptor();
    invariant(descriptor);

    status = entry->accessMethod()->initializeAsEmpty(opCtx);
    if (!status.isOK())
        return status;
    indexBuildBlock.success(opCtx, collection);

    // sanity check
    invariant(collection->isIndexReady(descriptor->indexName()));


    return spec;
}

namespace {

constexpr int kMaxNumIndexesAllowed = 64;

// While technically recursive, only current possible with 2 levels.
Status _checkValidFilterExpressions(MatchExpression* expression, int level = 0) {
    if (!expression)
        return Status::OK();

    switch (expression->matchType()) {
        case MatchExpression::AND:
            if (level > 0)
                return Status(ErrorCodes::CannotCreateIndex,
                              "$and only supported in partialFilterExpression at top level");
            for (size_t i = 0; i < expression->numChildren(); i++) {
                Status status = _checkValidFilterExpressions(expression->getChild(i), level + 1);
                if (!status.isOK())
                    return status;
            }
            return Status::OK();
        case MatchExpression::EQ:
        case MatchExpression::LT:
        case MatchExpression::LTE:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::EXISTS:
        case MatchExpression::TYPE_OPERATOR:
            return Status::OK();
        default:
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "unsupported expression in partial index: "
                                        << expression->debugString());
    }
}

/**
 * Adjust the provided index spec BSONObj depending on the type of index obj describes.
 *
 * This is a no-op unless the object describes a TEXT or a GEO_2DSPHERE index.  TEXT and
 * GEO_2DSPHERE provide additional validation on the index spec, and tweak the index spec
 * object to conform to their expected format.
 */
StatusWith<BSONObj> adjustIndexSpecObject(const BSONObj& obj) {
    std::string pluginName = IndexNames::findPluginName(obj.getObjectField("key"));

    if (IndexNames::TEXT == pluginName) {
        return fts::FTSSpec::fixSpec(obj);
    }

    if (IndexNames::GEO_2DSPHERE == pluginName) {
        return S2AccessMethod::fixSpec(obj);
    }

    return obj;
}

}  // namespace

Status IndexCatalogImpl::_isSpecOk(OperationContext* opCtx,
                                   const CollectionPtr& collection,
                                   const BSONObj& spec) const {
    const NamespaceString& nss = collection->ns();

    BSONElement vElt = spec["v"];
    if (!vElt) {
        return {ErrorCodes::InternalError,
                str::stream()
                    << "An internal operation failed to specify the 'v' field, which is a required "
                       "property of an index specification: "
                    << spec};
    }

    if (!vElt.isNumber()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "non-numeric value for \"v\" field: " << vElt);
    }

    auto vEltAsInt = representAs<int>(vElt.number());
    if (!vEltAsInt) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Index version must be representable as a 32-bit integer, but got "
                              << vElt.toString(false, false)};
    }

    auto indexVersion = static_cast<IndexVersion>(*vEltAsInt);

    if (indexVersion >= IndexVersion::kV2) {
        auto status = index_key_validate::validateIndexSpecFieldNames(spec);
        if (!status.isOK()) {
            return status;
        }
    }

    if (!IndexDescriptor::isIndexVersionSupported(indexVersion)) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "this version of mongod cannot build new indexes "
                                    << "of version number " << static_cast<int>(indexVersion));
    }

    if (nss.isOplog())
        return Status(ErrorCodes::CannotCreateIndex, "cannot have an index on the oplog");

    // logical name of the index
    const BSONElement nameElem = spec["name"];
    if (nameElem.type() != String)
        return Status(ErrorCodes::CannotCreateIndex, "index name must be specified as a string");

    const StringData name = nameElem.valueStringData();
    if (name.find('\0') != std::string::npos)
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot contain NUL bytes");

    if (name.empty())
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot be empty");

    const BSONObj key = spec.getObjectField("key");
    const Status keyStatus = index_key_validate::validateKeyPattern(key, indexVersion);
    if (!keyStatus.isOK()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream()
                          << "bad index key pattern " << key << ": " << keyStatus.reason());
    }

    const string pluginName = IndexNames::findPluginName(key);
    std::unique_ptr<CollatorInterface> collator;
    BSONElement collationElement = spec.getField("collation");
    if (collationElement) {
        if (collationElement.type() != BSONType::Object) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "\"collation\" for an index must be a document");
        }
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(collationElement.Obj());
        if (!statusWithCollator.isOK()) {
            return statusWithCollator.getStatus();
        }
        collator = std::move(statusWithCollator.getValue());

        if (!collator) {
            return {ErrorCodes::InternalError,
                    str::stream() << "An internal operation specified the collation "
                                  << CollationSpec::kSimpleSpec
                                  << " explicitly, which should instead be implied by omitting the "
                                     "'collation' field from the index specification"};
        }

        if (static_cast<IndexVersion>(vElt.numberInt()) < IndexVersion::kV2) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "Index version " << vElt.fieldNameStringData() << "="
                                  << vElt.numberInt() << " does not support the '"
                                  << collationElement.fieldNameStringData() << "' option"};
        }

        if ((pluginName != IndexNames::BTREE) && (pluginName != IndexNames::GEO_2DSPHERE) &&
            (pluginName != IndexNames::HASHED) && (pluginName != IndexNames::WILDCARD)) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Index type '" << pluginName
                              << "' does not support collation: " << collator->getSpec().toBSON());
        }
    }

    const bool isSparse = spec["sparse"].trueValue();

    if (pluginName == IndexNames::WILDCARD) {
        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "Index type '" << pluginName
                                        << "' does not support the sparse option");
        }

        if (spec["unique"].trueValue()) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "Index type '" << pluginName
                                        << "' does not support the unique option");
        }

        if (spec.getField("expireAfterSeconds")) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Index type '" << pluginName << "' cannot be a TTL index");
        }
    }

    // Create an ExpressionContext, used to parse the match expression and to house the collator for
    // the remaining checks.
    boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContext(opCtx, std::move(collator), nss));

    // Ensure if there is a filter, its valid.
    BSONElement filterElement = spec.getField("partialFilterExpression");
    if (filterElement) {
        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "cannot mix \"partialFilterExpression\" and \"sparse\" options");
        }

        if (filterElement.type() != Object) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "\"partialFilterExpression\" for an index must be a document");
        }

        // Parsing the partial filter expression is not expected to fail here since the
        // expression would have been successfully parsed upstream during index creation.
        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filterElement.Obj(),
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kBanAllSpecialFeatures);
        if (!statusWithMatcher.isOK()) {
            return statusWithMatcher.getStatus();
        }
        const std::unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        Status status = _checkValidFilterExpressions(filterExpr.get());
        if (!status.isOK()) {
            return status;
        }
    }

    uassert(ErrorCodes::InvalidOptions,
            "Unique indexes are not supported on collections clustered by _id",
            !collection->isClustered() || !spec[IndexDescriptor::kUniqueFieldName].trueValue());

    if (IndexDescriptor::isIdIndexPattern(key)) {
        if (collection->isClustered()) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "cannot create an _id index on a collection already clustered by _id");
        }

        BSONElement uniqueElt = spec["unique"];
        if (uniqueElt && !uniqueElt.trueValue()) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be non-unique");
        }

        if (filterElement) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be a partial index");
        }

        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be sparse");
        }

        if (collationElement &&
            !CollatorInterface::collatorsMatch(expCtx->getCollator(),
                                               collection->getDefaultCollator())) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "_id index must have the collection default collation");
        }
    }

    // --- only storage engine checks allowed below this ----

    BSONElement storageEngineElement = spec.getField("storageEngine");
    if (storageEngineElement.eoo()) {
        return Status::OK();
    }
    if (storageEngineElement.type() != mongo::Object) {
        return Status(ErrorCodes::CannotCreateIndex,
                      "\"storageEngine\" options must be a document if present");
    }
    BSONObj storageEngineOptions = storageEngineElement.Obj();
    if (storageEngineOptions.isEmpty()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      "Empty \"storageEngine\" options are invalid. "
                      "Please remove the field or include valid options.");
    }
    Status storageEngineStatus = validateStorageOptions(
        opCtx->getServiceContext(), storageEngineOptions, [](const auto& x, const auto& y) {
            return x->validateIndexStorageOptions(y);
        });
    if (!storageEngineStatus.isOK()) {
        return storageEngineStatus;
    }

    return Status::OK();
}

Status IndexCatalogImpl::_doesSpecConflictWithExisting(OperationContext* opCtx,
                                                       const CollectionPtr& collection,
                                                       const BSONObj& spec,
                                                       const bool includeUnfinishedIndexes) const {
    const char* name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
    invariant(name[0]);

    const BSONObj key = spec.getObjectField(IndexDescriptor::kKeyPatternFieldName);

    {
        // Check whether an index with the specified candidate name already exists in the catalog.
        const IndexDescriptor* desc = findIndexByName(opCtx, name, includeUnfinishedIndexes);

        if (desc) {
            // Index already exists with same name. Check whether the options are the same as well.
            IndexDescriptor candidate(_getAccessMethodName(key), spec);
            auto indexComparison =
                candidate.compareIndexOptions(opCtx, collection->ns(), getEntry(desc));

            // Key pattern or another uniquely-identifying option differs. We can build this index,
            // but not with the specified (duplicate) name. User must specify another index name.
            if (indexComparison == IndexDescriptor::Comparison::kDifferent) {
                return Status(ErrorCodes::IndexKeySpecsConflict,
                              str::stream()
                                  << "An existing index has the same name as the "
                                     "requested index. When index names are not specified, they "
                                     "are auto generated and can cause conflicts. Please refer to "
                                     "our documentation. Requested index: "
                                  << spec << ", existing index: " << desc->infoObj());
            }

            // The candidate's key and uniquely-identifying options are equivalent to an existing
            // index, but some other options are not identical. Return a message to that effect.
            if (indexComparison == IndexDescriptor::Comparison::kEquivalent) {
                return Status(ErrorCodes::IndexOptionsConflict,
                              str::stream() << "An equivalent index already exists with the same "
                                               "name but different options. Requested index: "
                                            << spec << ", existing index: " << desc->infoObj());
            }

            // If we've reached this point, the requested index is identical to an existing index.
            invariant(indexComparison == IndexDescriptor::Comparison::kIdentical);

            // If an identical index exists, but it is frozen, return an error with a different
            // error code to the user, forcing the user to drop before recreating the index.
            auto entry = getEntry(desc);
            if (entry->isFrozen()) {
                return Status(ErrorCodes::CannotCreateIndex,
                              str::stream()
                                  << "An identical, unfinished index '" << name
                                  << "' already exists. Must drop before recreating. Spec: "
                                  << desc->infoObj());
            }

            // Index already exists with the same options, so there is no need to build a new one.
            // This is not an error condition.
            return Status(ErrorCodes::IndexAlreadyExists,
                          str::stream() << "Identical index already exists: " << name);
        }
    }

    {
        // No index with the candidate name exists. Check for an index with conflicting options.
        const IndexDescriptor* desc =
            findIndexByKeyPatternAndOptions(opCtx, key, spec, includeUnfinishedIndexes);

        if (desc) {
            LOGV2_DEBUG(20353,
                        2,
                        "Index already exists with a different name: {name}, spec: {spec}",
                        "Index already exists with a different name",
                        "name"_attr = desc->indexName(),
                        "spec"_attr = desc->infoObj());

            // Index already exists with a different name. Check whether the options are identical.
            // We will return an error in either case, but this check allows us to generate a more
            // informative error message.
            IndexDescriptor candidate(_getAccessMethodName(key), spec);
            auto indexComparison =
                candidate.compareIndexOptions(opCtx, collection->ns(), getEntry(desc));

            // The candidate's key and uniquely-identifying options are equivalent to an existing
            // index, but some other options are not identical. Return a message to that effect.
            if (indexComparison == IndexDescriptor::Comparison::kEquivalent)
                return Status(ErrorCodes::IndexOptionsConflict,
                              str::stream() << "An equivalent index already exists with a "
                                               "different name and options. Requested index: "
                                            << spec << ", existing index: " << desc->infoObj());

            // If we've reached this point, the requested index is identical to an existing index.
            invariant(indexComparison == IndexDescriptor::Comparison::kIdentical);

            // An identical index already exists with a different name. We cannot build this index.
            return Status(ErrorCodes::IndexOptionsConflict,
                          str::stream() << "Index already exists with a different name: "
                                        << desc->indexName());
        }
    }

    if (numIndexesTotal(opCtx) >= kMaxNumIndexesAllowed) {
        string s = str::stream() << "add index fails, too many indexes for " << collection->ns()
                                 << " key:" << key;
        LOGV2(20354,
              "Exceeded maximum number of indexes",
              "namespace"_attr = collection->ns(),
              "key"_attr = key,
              "maxNumIndexes"_attr = kMaxNumIndexesAllowed);
        return Status(ErrorCodes::CannotCreateIndex, s);
    }

    // Refuse to build text index if another text index exists or is in progress.
    // Collections should only have one text index.
    string pluginName = IndexNames::findPluginName(key);
    if (pluginName == IndexNames::TEXT) {
        vector<const IndexDescriptor*> textIndexes;
        findIndexByType(opCtx, IndexNames::TEXT, textIndexes, includeUnfinishedIndexes);
        if (textIndexes.size() > 0) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "only one text index per collection allowed, "
                                        << "found existing text index \""
                                        << textIndexes[0]->indexName() << "\"");
        }
    }
    return Status::OK();
}

BSONObj IndexCatalogImpl::getDefaultIdIndexSpec(const CollectionPtr& collection) const {
    dassert(_idObj["_id"].type() == NumberInt);

    const auto indexVersion = IndexDescriptor::getDefaultIndexVersion();

    BSONObjBuilder b;
    b.append("v", static_cast<int>(indexVersion));
    b.append("name", "_id_");
    b.append("key", _idObj);
    if (collection->getDefaultCollator() && indexVersion >= IndexVersion::kV2) {
        // Creating an index with the "collation" option requires a v=2 index.
        b.append("collation", collection->getDefaultCollator()->getSpec().toBSON());
    }
    return b.obj();
}

void IndexCatalogImpl::dropAllIndexes(OperationContext* opCtx,
                                      Collection* collection,
                                      bool includingIdIndex,
                                      std::function<void(const IndexDescriptor*)> onDropFn) {
    uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot perform operation: an index build is currently running",
            !haveAnyIndexesInProgress());

    bool haveIdIndex = false;

    invariant(_buildingIndexes.size() == 0);
    vector<string> indexNamesToDrop;
    {
        int seen = 0;
        std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, true);
        while (ii->more()) {
            seen++;
            const IndexDescriptor* desc = ii->next()->descriptor();
            if (desc->isIdIndex() && includingIdIndex == false) {
                haveIdIndex = true;
                continue;
            }
            indexNamesToDrop.push_back(desc->indexName());
        }
        invariant(seen == numIndexesTotal(opCtx));
    }

    for (size_t i = 0; i < indexNamesToDrop.size(); i++) {
        string indexName = indexNamesToDrop[i];
        const IndexDescriptor* desc = findIndexByName(opCtx, indexName, true);
        invariant(desc);
        LOGV2_DEBUG(20355, 1, "\t dropAllIndexes dropping: {desc}", "desc"_attr = *desc);
        IndexCatalogEntry* entry = desc->getEntry();
        invariant(entry);

        // If the onDrop function creates an oplog entry, it should run first so that the drop is
        // timestamped at the same optime.
        if (onDropFn) {
            onDropFn(desc);
        }
        invariant(dropIndexEntry(opCtx, collection, entry).isOK());
    }

    // verify state is sane post cleaning

    long long numIndexesInCollectionCatalogEntry = collection->getTotalIndexCount();

    if (haveIdIndex) {
        fassert(17324, numIndexesTotal(opCtx) == 1);
        fassert(17325, numIndexesReady(opCtx) == 1);
        fassert(17326, numIndexesInCollectionCatalogEntry == 1);
        fassert(17336, _readyIndexes.size() == 1);
    } else {
        if (numIndexesTotal(opCtx) || numIndexesInCollectionCatalogEntry || _readyIndexes.size()) {
            _logInternalState(opCtx,
                              collection,
                              numIndexesInCollectionCatalogEntry,
                              indexNamesToDrop,
                              haveIdIndex);
        }
        fassert(17327, numIndexesTotal(opCtx) == 0);
        fassert(17328, numIndexesInCollectionCatalogEntry == 0);
        fassert(17337, _readyIndexes.size() == 0);
    }
}

void IndexCatalogImpl::dropAllIndexes(OperationContext* opCtx,
                                      Collection* collection,
                                      bool includingIdIndex) {
    dropAllIndexes(opCtx, collection, includingIdIndex, {});
}

Status IndexCatalogImpl::dropIndex(OperationContext* opCtx,
                                   Collection* collection,
                                   const IndexDescriptor* desc) {
    IndexCatalogEntry* entry = desc->getEntry();

    if (!entry)
        return Status(ErrorCodes::InternalError, "cannot find index to delete");

    if (!entry->isReady(opCtx, collection))
        return Status(ErrorCodes::InternalError, "cannot delete not ready index");

    return dropIndexEntry(opCtx, collection, entry);
}

Status IndexCatalogImpl::dropUnfinishedIndex(OperationContext* opCtx,
                                             Collection* collection,
                                             const IndexDescriptor* desc) {
    IndexCatalogEntry* entry = desc->getEntry();

    if (!entry)
        return Status(ErrorCodes::InternalError, "cannot find index to delete");

    if (entry->isReady(opCtx, collection))
        return Status(ErrorCodes::InternalError, "expected unfinished index, but it is ready");

    return dropIndexEntry(opCtx, collection, entry);
}

namespace {
class IndexRemoveChange final : public RecoveryUnit::Change {
public:
    IndexRemoveChange(IndexCatalogEntryContainer* entries,
                      std::shared_ptr<IndexCatalogEntry> entry,
                      SharedCollectionDecorations* collectionDecorations)
        : _entries(entries),
          _entry(std::move(entry)),
          _collectionDecorations(collectionDecorations) {}

    void commit(boost::optional<Timestamp> commitTime) final {
        _entry->setDropped();
    }

    void rollback() final {
        auto indexDescriptor = _entry->descriptor();

        // Refresh the CollectionIndexUsageTrackerDecoration's knowledge of what indices are
        // present as it is shared state across Collection copies.
        CollectionIndexUsageTrackerDecoration::get(_collectionDecorations)
            .registerIndex(indexDescriptor->indexName(), indexDescriptor->keyPattern());
    }

private:
    IndexCatalogEntryContainer* _entries;
    std::shared_ptr<IndexCatalogEntry> _entry;
    SharedCollectionDecorations* _collectionDecorations;
};
}  // namespace

Status IndexCatalogImpl::dropIndexEntry(OperationContext* opCtx,
                                        Collection* collection,
                                        IndexCatalogEntry* entry) {
    invariant(entry);

    // Pulling indexName out as it is needed post descriptor release.
    string indexName = entry->descriptor()->indexName();

    audit::logDropIndex(opCtx->getClient(), indexName, collection->ns());

    auto released = _readyIndexes.release(entry->descriptor());
    if (released) {
        invariant(released.get() == entry);
        opCtx->recoveryUnit()->registerChange(std::make_unique<IndexRemoveChange>(
            &_readyIndexes, std::move(released), collection->getSharedDecorations()));
    } else {
        released = _buildingIndexes.release(entry->descriptor());
        invariant(released.get() == entry);
        opCtx->recoveryUnit()->registerChange(std::make_unique<IndexRemoveChange>(
            &_buildingIndexes, std::move(released), collection->getSharedDecorations()));
    }

    CollectionQueryInfo::get(collection).rebuildIndexData(opCtx, collection);
    CollectionIndexUsageTrackerDecoration::get(collection->getSharedDecorations())
        .unregisterIndex(indexName);
    _deleteIndexFromDisk(opCtx, collection, indexName, entry->getSharedIdent());

    return Status::OK();
}

void IndexCatalogImpl::deleteIndexFromDisk(OperationContext* opCtx,
                                           Collection* collection,
                                           const string& indexName) {
    _deleteIndexFromDisk(opCtx, collection, indexName, nullptr);
}

void IndexCatalogImpl::_deleteIndexFromDisk(OperationContext* opCtx,
                                            Collection* collection,
                                            const string& indexName,
                                            std::shared_ptr<Ident> ident) {
    invariant(!findIndexByName(opCtx, indexName, true /* includeUnfinishedIndexes*/));
    catalog::removeIndex(opCtx, indexName, collection, std::move(ident));
}

void IndexCatalogImpl::setMultikeyPaths(OperationContext* const opCtx,
                                        const CollectionPtr& coll,
                                        const IndexDescriptor* desc,
                                        const KeyStringSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths) const {
    IndexCatalogEntry* entry = desc->getEntry();
    invariant(entry);
    entry->setMultikey(opCtx, coll, multikeyMetadataKeys, multikeyPaths);
};

// ---------------------------

bool IndexCatalogImpl::haveAnyIndexes() const {
    return _readyIndexes.size() > 0 || _buildingIndexes.size() > 0;
}

bool IndexCatalogImpl::haveAnyIndexesInProgress() const {
    return _buildingIndexes.size() > 0;
}

int IndexCatalogImpl::numIndexesTotal(OperationContext* opCtx) const {
    return _readyIndexes.size() + _buildingIndexes.size();
}

int IndexCatalogImpl::numIndexesReady(OperationContext* opCtx) const {
    std::vector<const IndexDescriptor*> itIndexes;
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, /*includeUnfinished*/ false);
    while (ii->more()) {
        itIndexes.push_back(ii->next()->descriptor());
    }
    return itIndexes.size();
}

bool IndexCatalogImpl::haveIdIndex(OperationContext* opCtx) const {
    return findIdIndex(opCtx) != nullptr;
}

const IndexDescriptor* IndexCatalogImpl::findIdIndex(OperationContext* opCtx) const {
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, false);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (desc->isIdIndex())
            return desc;
    }
    return nullptr;
}

const IndexDescriptor* IndexCatalogImpl::findIndexByName(OperationContext* opCtx,
                                                         StringData name,
                                                         bool includeUnfinishedIndexes) const {
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (desc->indexName() == name)
            return desc;
    }
    return nullptr;
}

const IndexDescriptor* IndexCatalogImpl::findIndexByKeyPatternAndOptions(
    OperationContext* opCtx,
    const BSONObj& key,
    const BSONObj& indexSpec,
    bool includeUnfinishedIndexes) const {
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, includeUnfinishedIndexes);
    IndexDescriptor needle(_getAccessMethodName(key), indexSpec);
    while (ii->more()) {
        const auto* entry = ii->next();
        if (needle.compareIndexOptions(opCtx, {}, entry) !=
            IndexDescriptor::Comparison::kDifferent) {
            return entry->descriptor();
        }
    }
    return nullptr;
}  // namespace mongo

void IndexCatalogImpl::findIndexesByKeyPattern(OperationContext* opCtx,
                                               const BSONObj& key,
                                               bool includeUnfinishedIndexes,
                                               std::vector<const IndexDescriptor*>* matches) const {
    invariant(matches);
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (SimpleBSONObjComparator::kInstance.evaluate(desc->keyPattern() == key)) {
            matches->push_back(desc);
        }
    }
}

const IndexDescriptor* IndexCatalogImpl::findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                                   const BSONObj& shardKey,
                                                                   bool requireSingleKey) const {
    const IndexDescriptor* best = nullptr;

    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, false);
    while (ii->more()) {
        const IndexCatalogEntry* entry = ii->next();
        const IndexDescriptor* desc = entry->descriptor();
        bool hasSimpleCollation = desc->collation().isEmpty();

        if (desc->isPartial() || desc->isSparse())
            continue;

        if (!shardKey.isPrefixOf(desc->keyPattern(), SimpleBSONElementComparator::kInstance))
            continue;

        if (!entry->isMultikey() && hasSimpleCollation)
            return desc;

        if (!requireSingleKey && hasSimpleCollation)
            best = desc;
    }

    return best;
}

void IndexCatalogImpl::findIndexByType(OperationContext* opCtx,
                                       const string& type,
                                       vector<const IndexDescriptor*>& matches,
                                       bool includeUnfinishedIndexes) const {
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (IndexNames::findPluginName(desc->keyPattern()) == type) {
            matches.push_back(desc);
        }
    }
}

const IndexCatalogEntry* IndexCatalogImpl::getEntry(const IndexDescriptor* desc) const {
    const IndexCatalogEntry* entry = desc->getEntry();
    massert(17357, "cannot find index entry", entry);
    return entry;
}

std::shared_ptr<const IndexCatalogEntry> IndexCatalogImpl::getEntryShared(
    const IndexDescriptor* indexDescriptor) const {
    return indexDescriptor->getEntry()->shared_from_this();
}

std::vector<std::shared_ptr<const IndexCatalogEntry>> IndexCatalogImpl::getAllReadyEntriesShared()
    const {
    return _readyIndexes.getAllEntries();
}

const IndexDescriptor* IndexCatalogImpl::refreshEntry(OperationContext* opCtx,
                                                      Collection* collection,
                                                      const IndexDescriptor* oldDesc) {
    invariant(_buildingIndexes.size() == 0);

    const std::string indexName = oldDesc->indexName();
    invariant(collection->isIndexReady(indexName));

    // Delete the IndexCatalogEntry that owns this descriptor.  After deletion, 'oldDesc' is
    // invalid and should not be dereferenced. Also, invalidate the index from the
    // CollectionIndexUsageTrackerDecoration (shared state among Collection instances).
    auto oldEntry = _readyIndexes.release(oldDesc);
    invariant(oldEntry);
    opCtx->recoveryUnit()->registerChange(std::make_unique<IndexRemoveChange>(
        &_readyIndexes, std::move(oldEntry), collection->getSharedDecorations()));
    CollectionIndexUsageTrackerDecoration::get(collection->getSharedDecorations())
        .unregisterIndex(indexName);

    // Ask the CollectionCatalogEntry for the new index spec.
    BSONObj spec = collection->getIndexSpec(indexName).getOwned();
    BSONObj keyPattern = spec.getObjectField("key");

    // Re-register this index in the index catalog with the new spec. Also, add the new index
    // to the CollectionIndexUsageTrackerDecoration (shared state among Collection instances).
    auto newDesc = std::make_unique<IndexDescriptor>(_getAccessMethodName(keyPattern), spec);
    auto newEntry =
        createIndexEntry(opCtx, collection, std::move(newDesc), CreateIndexEntryFlags::kIsReady);
    invariant(newEntry->isReady(opCtx, collection));
    auto desc = newEntry->descriptor();
    CollectionIndexUsageTrackerDecoration::get(collection->getSharedDecorations())
        .registerIndex(desc->indexName(), desc->keyPattern());

    // Last rebuild index data for CollectionQueryInfo for this Collection.
    CollectionQueryInfo::get(collection).rebuildIndexData(opCtx, collection);

    opCtx->recoveryUnit()->onCommit([newEntry](auto commitTime) {
        if (commitTime) {
            newEntry->setMinimumVisibleSnapshot(*commitTime);
        }
    });

    // Return the new descriptor.
    return newEntry->descriptor();
}

// ---------------------------

Status IndexCatalogImpl::_indexKeys(OperationContext* opCtx,
                                    const CollectionPtr& coll,
                                    const IndexCatalogEntry* index,
                                    const KeyStringSet& keys,
                                    const KeyStringSet& multikeyMetadataKeys,
                                    const MultikeyPaths& multikeyPaths,
                                    const BSONObj& obj,
                                    RecordId loc,
                                    const InsertDeleteOptions& options,
                                    int64_t* keysInsertedOut) const {
    Status status = Status::OK();
    if (index->isHybridBuilding()) {
        // The side table interface accepts only records that meet the criteria for this partial
        // index.
        // For non-hybrid builds, the decision to use the filter for the partial index is left to
        // the IndexAccessMethod. See SERVER-28975 for details.
        if (auto filter = index->getFilterExpression()) {
            if (!filter->matchesBSON(obj)) {
                return Status::OK();
            }
        }

        int64_t inserted;
        status = index->indexBuildInterceptor()->sideWrite(opCtx,
                                                           keys,
                                                           multikeyMetadataKeys,
                                                           multikeyPaths,
                                                           loc,
                                                           IndexBuildInterceptor::Op::kInsert,
                                                           &inserted);
        if (keysInsertedOut) {
            *keysInsertedOut += inserted;
        }
    } else {
        int64_t numInserted;
        status = index->accessMethod()->insertKeysAndUpdateMultikeyPaths(
            opCtx,
            coll,
            keys,
            {multikeyMetadataKeys.begin(), multikeyMetadataKeys.end()},
            multikeyPaths,
            loc,
            options,
            nullptr,
            &numInserted);
        if (keysInsertedOut) {
            *keysInsertedOut += numInserted;
        }
    }

    return status;
}

Status IndexCatalogImpl::_indexFilteredRecords(OperationContext* opCtx,
                                               const CollectionPtr& coll,
                                               const IndexCatalogEntry* index,
                                               const std::vector<BsonRecord>& bsonRecords,
                                               int64_t* keysInsertedOut) const {
    auto& executionCtx = StorageExecutionContext::get(opCtx);

    InsertDeleteOptions options;
    prepareInsertDeleteOptions(opCtx, coll->ns(), index->descriptor(), &options);

    for (auto bsonRecord : bsonRecords) {
        invariant(bsonRecord.id != RecordId());

        if (!bsonRecord.ts.isNull()) {
            Status status = opCtx->recoveryUnit()->setTimestamp(bsonRecord.ts);
            if (!status.isOK())
                return status;
        }

        auto keys = executionCtx.keys();
        auto multikeyMetadataKeys = executionCtx.multikeyMetadataKeys();
        auto multikeyPaths = executionCtx.multikeyPaths();

        index->accessMethod()->getKeys(executionCtx.pooledBufferBuilder(),
                                       *bsonRecord.docPtr,
                                       options.getKeysMode,
                                       IndexAccessMethod::GetKeysContext::kAddingKeys,
                                       keys.get(),
                                       multikeyMetadataKeys.get(),
                                       multikeyPaths.get(),
                                       bsonRecord.id,
                                       IndexAccessMethod::kNoopOnSuppressedErrorFn);

        Status status = _indexKeys(opCtx,
                                   coll,
                                   index,
                                   *keys,
                                   *multikeyMetadataKeys,
                                   *multikeyPaths,
                                   *bsonRecord.docPtr,
                                   bsonRecord.id,
                                   options,
                                   keysInsertedOut);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status IndexCatalogImpl::_indexRecords(OperationContext* opCtx,
                                       const CollectionPtr& coll,
                                       const IndexCatalogEntry* index,
                                       const std::vector<BsonRecord>& bsonRecords,
                                       int64_t* keysInsertedOut) const {
    if (MONGO_unlikely(skipIndexNewRecords.shouldFail())) {
        return Status::OK();
    }

    const MatchExpression* filter = index->getFilterExpression();
    if (!filter)
        return _indexFilteredRecords(opCtx, coll, index, bsonRecords, keysInsertedOut);

    std::vector<BsonRecord> filteredBsonRecords;
    for (auto bsonRecord : bsonRecords) {
        if (filter->matchesBSON(*(bsonRecord.docPtr)))
            filteredBsonRecords.push_back(bsonRecord);
    }

    return _indexFilteredRecords(opCtx, coll, index, filteredBsonRecords, keysInsertedOut);
}

Status IndexCatalogImpl::_updateRecord(OperationContext* const opCtx,
                                       const CollectionPtr& coll,
                                       const IndexCatalogEntry* index,
                                       const BSONObj& oldDoc,
                                       const BSONObj& newDoc,
                                       const RecordId& recordId,
                                       int64_t* const keysInsertedOut,
                                       int64_t* const keysDeletedOut) const {
    IndexAccessMethod* iam = index->accessMethod();

    InsertDeleteOptions options;
    prepareInsertDeleteOptions(opCtx, coll->ns(), index->descriptor(), &options);

    UpdateTicket updateTicket;

    iam->prepareUpdate(opCtx, index, oldDoc, newDoc, recordId, options, &updateTicket);

    int64_t keysInserted = 0;
    int64_t keysDeleted = 0;

    auto status = Status::OK();
    if (index->isHybridBuilding() || !index->isReady(opCtx, coll)) {
        bool logIfError = false;
        _unindexKeys(
            opCtx, coll, index, updateTicket.removed, oldDoc, recordId, logIfError, &keysDeleted);
        status = _indexKeys(opCtx,
                            coll,
                            index,
                            updateTicket.added,
                            updateTicket.newMultikeyMetadataKeys,
                            updateTicket.newMultikeyPaths,
                            newDoc,
                            recordId,
                            options,
                            &keysInserted);
    } else {
        status = iam->update(opCtx, coll, updateTicket, &keysInserted, &keysDeleted);
    }

    if (!status.isOK())
        return status;

    *keysInsertedOut += keysInserted;
    *keysDeletedOut += keysDeleted;

    return Status::OK();
}

void IndexCatalogImpl::_unindexKeys(OperationContext* opCtx,
                                    const CollectionPtr& collection,
                                    const IndexCatalogEntry* index,
                                    const KeyStringSet& keys,
                                    const BSONObj& obj,
                                    RecordId loc,
                                    bool logIfError,
                                    int64_t* const keysDeletedOut) const {
    InsertDeleteOptions options;
    prepareInsertDeleteOptions(opCtx, collection->ns(), index->descriptor(), &options);
    options.logIfError = logIfError;

    if (index->isHybridBuilding()) {
        // The side table interface accepts only records that meet the criteria for this partial
        // index.
        // For non-hybrid builds, the decision to use the filter for the partial index is left to
        // the IndexAccessMethod. See SERVER-28975 for details.
        if (auto filter = index->getFilterExpression()) {
            if (!filter->matchesBSON(obj)) {
                return;
            }
        }

        int64_t removed;
        fassert(31155,
                index->indexBuildInterceptor()->sideWrite(
                    opCtx, keys, {}, {}, loc, IndexBuildInterceptor::Op::kDelete, &removed));
        if (keysDeletedOut) {
            *keysDeletedOut += removed;
        }

        return;
    }

    // On WiredTiger, we do blind unindexing of records for efficiency.  However, when duplicates
    // are allowed in unique indexes, WiredTiger does not do blind unindexing, and instead confirms
    // that the recordid matches the element we are removing.
    //
    // We need to disable blind-deletes for in-progress indexes, in order to force recordid-matching
    // for unindex operations, since initial sync can build an index over a collection with
    // duplicates. See SERVER-17487 for more details.
    options.dupsAllowed = options.dupsAllowed || !index->isReady(opCtx, collection);

    int64_t removed;
    Status status = index->accessMethod()->removeKeys(opCtx, keys, loc, options, &removed);

    if (!status.isOK()) {
        LOGV2(20362,
              "Couldn't unindex record {obj} from collection {namespace}: {error}",
              "Couldn't unindex record",
              "record"_attr = redact(obj),
              "namespace"_attr = collection->ns(),
              "error"_attr = redact(status));
    }

    if (keysDeletedOut) {
        *keysDeletedOut += removed;
    }
}

void IndexCatalogImpl::_unindexRecord(OperationContext* opCtx,
                                      const CollectionPtr& collection,
                                      const IndexCatalogEntry* entry,
                                      const BSONObj& obj,
                                      const RecordId& loc,
                                      bool logIfError,
                                      int64_t* keysDeletedOut) const {
    auto& executionCtx = StorageExecutionContext::get(opCtx);

    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when removing a document since the index metadata isn't updated when keys are
    // deleted.
    auto keys = executionCtx.keys();
    entry->accessMethod()->getKeys(executionCtx.pooledBufferBuilder(),
                                   obj,
                                   IndexAccessMethod::GetKeysMode::kRelaxConstraintsUnfiltered,
                                   IndexAccessMethod::GetKeysContext::kRemovingKeys,
                                   keys.get(),
                                   nullptr,
                                   nullptr,
                                   loc,
                                   IndexAccessMethod::kNoopOnSuppressedErrorFn);

    // Tests can enable this failpoint to produce index corruption scenarios where an index has
    // extra keys.
    if (auto failpoint = skipUnindexingDocumentWhenDeleted.scoped();
        MONGO_unlikely(failpoint.isActive())) {
        auto indexName = failpoint.getData()["indexName"].valueStringDataSafe();
        if (indexName == entry->descriptor()->indexName()) {
            return;
        }
    }
    _unindexKeys(opCtx, collection, entry, *keys, obj, loc, logIfError, keysDeletedOut);
}

Status IndexCatalogImpl::indexRecords(OperationContext* opCtx,
                                      const CollectionPtr& coll,
                                      const std::vector<BsonRecord>& bsonRecords,
                                      int64_t* keysInsertedOut) const {
    if (keysInsertedOut) {
        *keysInsertedOut = 0;
    }

    for (auto&& it : _readyIndexes) {
        Status s = _indexRecords(opCtx, coll, it.get(), bsonRecords, keysInsertedOut);
        if (!s.isOK())
            return s;
    }

    for (auto&& it : _buildingIndexes) {
        Status s = _indexRecords(opCtx, coll, it.get(), bsonRecords, keysInsertedOut);
        if (!s.isOK())
            return s;
    }

    return Status::OK();
}

Status IndexCatalogImpl::updateRecord(OperationContext* const opCtx,
                                      const CollectionPtr& coll,
                                      const BSONObj& oldDoc,
                                      const BSONObj& newDoc,
                                      const RecordId& recordId,
                                      int64_t* const keysInsertedOut,
                                      int64_t* const keysDeletedOut) const {
    *keysInsertedOut = 0;
    *keysDeletedOut = 0;

    // Ready indexes go directly through the IndexAccessMethod.
    for (IndexCatalogEntryContainer::const_iterator it = _readyIndexes.begin();
         it != _readyIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();
        auto status = _updateRecord(
            opCtx, coll, entry, oldDoc, newDoc, recordId, keysInsertedOut, keysDeletedOut);
        if (!status.isOK())
            return status;
    }

    // Building indexes go through the interceptor.
    for (IndexCatalogEntryContainer::const_iterator it = _buildingIndexes.begin();
         it != _buildingIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();
        auto status = _updateRecord(
            opCtx, coll, entry, oldDoc, newDoc, recordId, keysInsertedOut, keysDeletedOut);
        if (!status.isOK())
            return status;
    }
    return Status::OK();
}

void IndexCatalogImpl::unindexRecord(OperationContext* opCtx,
                                     const CollectionPtr& collection,
                                     const BSONObj& obj,
                                     const RecordId& loc,
                                     bool noWarn,
                                     int64_t* keysDeletedOut) const {
    if (keysDeletedOut) {
        *keysDeletedOut = 0;
    }

    for (IndexCatalogEntryContainer::const_iterator it = _readyIndexes.begin();
         it != _readyIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();

        bool logIfError = !noWarn;
        _unindexRecord(opCtx, collection, entry, obj, loc, logIfError, keysDeletedOut);
    }

    for (IndexCatalogEntryContainer::const_iterator it = _buildingIndexes.begin();
         it != _buildingIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();

        // If it's a background index, we DO NOT want to log anything.
        bool logIfError = entry->isReady(opCtx, collection) ? !noWarn : false;
        _unindexRecord(opCtx, collection, entry, obj, loc, logIfError, keysDeletedOut);
    }
}

Status IndexCatalogImpl::compactIndexes(OperationContext* opCtx) const {
    for (IndexCatalogEntryContainer::const_iterator it = _readyIndexes.begin();
         it != _readyIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();

        LOGV2_DEBUG(20363,
                    1,
                    "compacting index: {entry_descriptor}",
                    "entry_descriptor"_attr = *(entry->descriptor()));
        Status status = entry->accessMethod()->compact(opCtx);
        if (!status.isOK()) {
            LOGV2_ERROR(20377,
                        "Failed to compact index",
                        "index"_attr = *(entry->descriptor()),
                        "error"_attr = redact(status));
            return status;
        }
    }
    return Status::OK();
}

std::string::size_type IndexCatalogImpl::getLongestIndexNameLength(OperationContext* opCtx) const {
    std::unique_ptr<IndexIterator> it = getIndexIterator(opCtx, true);
    std::string::size_type longestIndexNameLength = 0;
    while (it->more()) {
        auto thisLength = it->next()->descriptor()->indexName().length();
        if (thisLength > longestIndexNameLength)
            longestIndexNameLength = thisLength;
    }
    return longestIndexNameLength;
}

BSONObj IndexCatalogImpl::fixIndexKey(const BSONObj& key) const {
    if (IndexDescriptor::isIdIndexPattern(key)) {
        return _idObj;
    }
    if (key["_id"].type() == Bool && key.nFields() == 1) {
        return _idObj;
    }
    return key;
}

void IndexCatalogImpl::prepareInsertDeleteOptions(OperationContext* opCtx,
                                                  const NamespaceString& ns,
                                                  const IndexDescriptor* desc,
                                                  InsertDeleteOptions* options) const {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->shouldRelaxIndexConstraints(opCtx, ns)) {
        options->getKeysMode = IndexAccessMethod::GetKeysMode::kRelaxConstraints;
    } else {
        options->getKeysMode = IndexAccessMethod::GetKeysMode::kEnforceConstraints;
    }

    // Don't allow dups for Id key. Allow dups for non-unique keys or when constraints relaxed.
    if (desc->isIdIndex()) {
        options->dupsAllowed = false;
    } else {
        options->dupsAllowed = !desc->unique() ||
            options->getKeysMode == IndexAccessMethod::GetKeysMode::kRelaxConstraints;
    }
}

void IndexCatalogImpl::indexBuildSuccess(OperationContext* opCtx,
                                         Collection* coll,
                                         IndexCatalogEntry* index) {
    auto releasedEntry = _buildingIndexes.release(index->descriptor());
    invariant(releasedEntry.get() == index);
    _readyIndexes.add(std::move(releasedEntry));

    index->setIndexBuildInterceptor(nullptr);
    index->setIsReady(true);
}

StatusWith<BSONObj> IndexCatalogImpl::_fixIndexSpec(OperationContext* opCtx,
                                                    const CollectionPtr& collection,
                                                    const BSONObj& spec) const {
    auto statusWithSpec = adjustIndexSpecObject(spec);
    if (!statusWithSpec.isOK()) {
        return statusWithSpec;
    }
    BSONObj o = statusWithSpec.getValue();

    BSONObjBuilder b;

    // We've already verified in IndexCatalog::_isSpecOk() that the index version is present and
    // that it is representable as a 32-bit integer.
    auto vElt = o["v"];
    invariant(vElt);

    b.append("v", vElt.numberInt());

    if (o["unique"].trueValue())
        b.appendBool("unique", true);  // normalize to bool true in case was int 1 or something...

    if (o["hidden"].trueValue())
        b.appendBool("hidden", true);  // normalize to bool true in case was int 1 or something...

    BSONObj key = fixIndexKey(o["key"].Obj());
    b.append("key", key);

    string name = o["name"].String();
    if (IndexDescriptor::isIdIndexPattern(key)) {
        name = "_id_";
    }
    b.append("name", name);

    // During repair, if the 'ns' field exists in the index spec, do not remove it as repair can be
    // running on old data files from other mongod versions. Removing the 'ns' field during repair
    // would prevent the data files from starting up on the original mongod version as the 'ns'
    // field is required to be present in 3.6 and 4.0.
    if (storageGlobalParams.repair && o.hasField("ns")) {
        b.append("ns", o.getField("ns").String());
    }

    {
        BSONObjIterator i(o);
        while (i.more()) {
            BSONElement e = i.next();
            string s = e.fieldName();

            if (s == "_id") {
                // skip
            } else if (s == "dropDups" || s == "ns") {
                // dropDups is silently ignored and removed from the spec as of SERVER-14710.
                // ns is removed from the spec as of 4.4.
            } else if (s == "v" || s == "unique" || s == "key" || s == "name" || s == "hidden") {
                // covered above
            } else {
                b.append(e);
            }
        }
    }

    return b.obj();
}

}  // namespace mongo
