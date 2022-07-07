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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/snapshot_window_options_gen.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

using FeatureCompatibilityParams = ServerGlobalParams::FeatureCompatibility;

MONGO_FAIL_POINT_DEFINE(migrationCommitVersionError);
MONGO_FAIL_POINT_DEFINE(migrateCommitInvalidChunkQuery);
MONGO_FAIL_POINT_DEFINE(skipExpiringOldChunkHistory);

const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

/**
 * Append min, max and version information from chunk to the buffer for logChange purposes.
 */
void appendShortVersion(BufBuilder* out, const ChunkType& chunk) {
    BSONObjBuilder bb(*out);
    bb.append(ChunkType::min(), chunk.getMin());
    bb.append(ChunkType::max(), chunk.getMax());
    if (chunk.isVersionSet()) {
        chunk.getVersion().appendLegacyWithField(&bb, ChunkType::lastmod());
    }
    bb.done();
}

BSONArray buildMergeChunksTransactionUpdates(const std::vector<ChunkType>& chunksToMerge,
                                             const ChunkVersion& mergeVersion,
                                             const boost::optional<Timestamp>& validAfter) {
    BSONArrayBuilder updates;

    // Build an update operation to expand the first chunk into the newly merged chunk
    {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);  // no upsert
        op.append("ns", ChunkType::ConfigNS.ns());

        // expand first chunk into newly merged chunk
        ChunkType mergedChunk(chunksToMerge.front());
        mergedChunk.setMax(chunksToMerge.back().getMax());

        // fill in additional details for sending through transaction
        mergedChunk.setVersion(mergeVersion);

        invariant(validAfter);
        mergedChunk.setHistory({ChunkHistory(validAfter.get(), mergedChunk.getShard())});

        // add the new chunk information as the update object
        op.append("o", mergedChunk.toConfigBSON());

        // query object
        op.append("o2", BSON(ChunkType::name(mergedChunk.getName())));

        updates.append(op.obj());
    }

    // Build update operations to delete the rest of the chunks to be merged. Remember not
    // to delete the first chunk we're expanding
    for (size_t i = 1; i < chunksToMerge.size(); ++i) {
        BSONObjBuilder op;
        op.append("op", "d");
        op.append("ns", ChunkType::ConfigNS.ns());

        op.append("o", BSON(ChunkType::name(chunksToMerge[i].getName())));

        updates.append(op.obj());
    }

    return updates.arr();
}

BSONArray buildMergeChunksTransactionPrecond(const std::vector<ChunkType>& chunksToMerge,
                                             const ChunkVersion& collVersion) {
    BSONArrayBuilder preCond;

    const bool collHasTimestamp = (bool)collVersion.getTimestamp();
    for (const auto& chunk : chunksToMerge) {
        BSONObj query = BSON(ChunkType::min(chunk.getMin()) << ChunkType::max(chunk.getMax()));
        if (collHasTimestamp) {
            query = query.addFields(BSON(ChunkType::collectionUUID() << chunk.getCollectionUUID()));
        } else {
            query = query.addFields(BSON(ChunkType::ns(chunk.getNS().ns())));
        }

        const auto collectionIdentityMatchCondition = collHasTimestamp
            ? BSON(ChunkType::collectionUUID()
                   << chunk.getCollectionUUID() << ChunkType::shard(chunk.getShard().toString()))
            : BSON(ChunkType::epoch(collVersion.epoch())
                   << ChunkType::shard(chunk.getShard().toString()));

        BSONObjBuilder b;
        b.append("ns", ChunkType::ConfigNS.ns());
        b.append("q", BSON("query" << query << "orderby" << BSON(ChunkType::lastmod() << -1)));
        b.append("res", collectionIdentityMatchCondition);

        preCond.append(b.obj());
    }
    return preCond.arr();
}

/*
 * Check that the chunk still exists and return its metadata.
 */
StatusWith<ChunkType> getCurrentChunk(OperationContext* opCtx,
                                      const NamespaceStringOrUUID& nsOrUUID,
                                      const OID& epoch,
                                      const boost::optional<Timestamp>& timestamp,
                                      const ChunkType& requestedChunk) {
    uassert(4683300,
            "Config server rejecting commitChunkMigration request that does not have a "
            "ChunkVersion",
            requestedChunk.isVersionSet() && requestedChunk.getVersion().isSet() &&
                requestedChunk.getVersion().epoch().isSet());

    BSONObj chunkQuery = BSON(ChunkType::min() << requestedChunk.getMin() << ChunkType::max()
                                               << requestedChunk.getMax());

    if (nsOrUUID.uuid()) {
        chunkQuery = chunkQuery.addFields(BSON(ChunkType::collectionUUID << *nsOrUUID.uuid()));
    } else {
        chunkQuery = chunkQuery.addFields(BSON(ChunkType::ns() << nsOrUUID.nss()->ns()));
    }

    // Must use local read concern because we're going to perform subsequent writes.
    auto findResponseWith =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            chunkQuery,
            BSONObj(),
            1);

    if (!findResponseWith.isOK()) {
        return findResponseWith.getStatus();
    }

    if (findResponseWith.getValue().docs.empty()) {
        return {ErrorCodes::Error(40165),
                str::stream() << "Could not find the chunk (" << requestedChunk.toString()
                              << "). Cannot execute the migration commit with invalid chunks."};
    }

    return uassertStatusOK(
        ChunkType::fromConfigBSON(findResponseWith.getValue().docs.front(), epoch, timestamp));
}

BSONObj makeCommitChunkTransactionCommand(const NamespaceString& nss,
                                          const ChunkType& migratedChunk,
                                          const boost::optional<ChunkType>& controlChunk,
                                          StringData fromShard,
                                          StringData toShard) {
    // Update migratedChunk's version and shard.
    BSONArrayBuilder updates;
    {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);  // No upserting
        op.append("ns", ChunkType::ConfigNS.ns());

        auto chunkID = MONGO_unlikely(migrateCommitInvalidChunkQuery.shouldFail())
            ? OID::gen()
            : migratedChunk.getName();

        op.append("o", migratedChunk.toConfigBSON());

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), chunkID);
        q.done();

        updates.append(op.obj());
    }

    // If we have a controlChunk, update its chunk version.
    if (controlChunk) {
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", false);
        op.append("ns", ChunkType::ConfigNS.ns());

        op.append("o", controlChunk->toConfigBSON());

        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), controlChunk->getName());
        q.done();

        updates.append(op.obj());
    }

    // Do not give applyOps a write concern. If applyOps tries to wait for replication, it will
    // fail because of the GlobalWrite lock CommitChunkMigration already holds. Replication will
    // not be able to take the lock it requires. Include "alwaysUpsert" false since it defaults
    // to true and alwaysUpsert overrides the 'b' field.
    return BSON("applyOps" << updates.arr() << "alwaysUpsert" << false);
}

/**
 * Returns a chunk different from the one being migrated or 'none' if one doesn't exist.
 */
boost::optional<ChunkType> getControlChunkForMigrate(OperationContext* opCtx,
                                                     const NamespaceStringOrUUID& nsOrUUID,
                                                     const OID& epoch,
                                                     const boost::optional<Timestamp>& timestamp,
                                                     const ChunkType& migratedChunk,
                                                     const ShardId& fromShard) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BSONObjBuilder queryBuilder;
    if (nsOrUUID.uuid()) {
        queryBuilder << ChunkType::collectionUUID << *nsOrUUID.uuid();
    } else {
        queryBuilder << ChunkType::ns(nsOrUUID.nss()->ns());
    }
    queryBuilder << ChunkType::shard(fromShard.toString());
    queryBuilder << ChunkType::min(BSON("$ne" << migratedChunk.getMin()));

    auto status =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            queryBuilder.obj(),
                                            {},
                                            1);
    auto response = uassertStatusOK(status);
    if (response.docs.empty()) {
        return boost::none;
    }

    return uassertStatusOK(ChunkType::fromConfigBSON(response.docs.front(), epoch, timestamp));
}

// Helper function to find collection version and shard version.
StatusWith<ChunkVersion> getMaxChunkVersionFromQueryResponse(
    const CollectionType& coll, const StatusWith<Shard::QueryResponse>& queryResponse) {

    if (!queryResponse.isOK()) {
        return queryResponse.getStatus();
    }

    const auto& chunksVector = queryResponse.getValue().docs;
    if (chunksVector.empty()) {
        return {ErrorCodes::Error(50577),
                str::stream() << "Collection '" << coll.getNss().ns()
                              << "' no longer either exists, is sharded, or has chunks"};
    }

    const auto chunk = uassertStatusOK(
        ChunkType::fromConfigBSON(chunksVector.front(), coll.getEpoch(), coll.getTimestamp()));

    return chunk.getVersion();
}

// Helper function to get the collection version for nss. Always uses kLocalReadConcern.
StatusWith<ChunkVersion> getCollectionVersion(OperationContext* opCtx, const NamespaceString& nss) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1);
    if (!findCollResponse.isOK()) {
        return findCollResponse.getStatus();
    }

    if (findCollResponse.getValue().docs.empty()) {
        return {ErrorCodes::Error(5057701),
                str::stream() << "Collection '" << nss.ns() << "' no longer either exists"};
    }

    const CollectionType coll(findCollResponse.getValue().docs[0]);
    const auto chunksQuery = coll.getTimestamp() ? BSON(ChunkType::collectionUUID << coll.getUuid())
                                                 : BSON(ChunkType::ns(coll.getNss().ns()));
    return getMaxChunkVersionFromQueryResponse(
        coll,
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            chunksQuery,                     // Query all chunks for this namespace.
            BSON(ChunkType::lastmod << -1),  // Sort by version.
            1));                             // Limit 1.
}

// Helper function to get collection version and donor shard version following a merge/move/split
BSONObj getShardAndCollectionVersion(OperationContext* opCtx,
                                     const CollectionType& coll,
                                     const ShardId& fromShard) {
    BSONObjBuilder result;

    auto swCollectionVersion = getCollectionVersion(opCtx, coll.getNss());
    auto collectionVersion = uassertStatusOKWithContext(
        std::move(swCollectionVersion), "Couldn't retrieve collection version from config server");

    const auto chunksQuery = coll.getTimestamp()
        ? BSON(ChunkType::collectionUUID << coll.getUuid()
                                         << ChunkType::shard(fromShard.toString()))
        : BSON(ChunkType::ns(coll.getNss().ns()) << ChunkType::shard(fromShard.toString()));

    auto swDonorShardVersion = getMaxChunkVersionFromQueryResponse(
        coll,
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            chunksQuery,
            BSON(ChunkType::lastmod << -1),  // Sort by version.
            1));

    ChunkVersion shardVersion;

    if (!swDonorShardVersion.isOK()) {
        if (swDonorShardVersion.getStatus().code() == 50577) {
            // The query to find 'nss' chunks belonging to the donor shard didn't return any chunks,
            // meaning the last chunk for fromShard was donated. Gracefully handle the error.
            shardVersion =
                ChunkVersion(0, 0, collectionVersion.epoch(), collectionVersion.getTimestamp());
        } else {
            // Bubble up any other error
            uassertStatusOK(swDonorShardVersion);
        }
    } else {
        shardVersion = swDonorShardVersion.getValue();
    }

    uassert(4914701,
            str::stream() << "Aborting due to metadata corruption. Collection version '"
                          << collectionVersion.toString() << "' and shard version '"
                          << shardVersion.toString() << "'.",
            shardVersion.isOlderOrEqualThan(collectionVersion));

    collectionVersion.appendWithField(&result, "collectionVersion");
    shardVersion.appendWithField(&result, "shardVersion");

    return result.obj();
}

void bumpMajorVersionOneChunkPerShard(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      TxnNumber txnNumber,
                                      const std::vector<ShardId>& shardIds) {
    auto curCollectionVersion = uassertStatusOK(getCollectionVersion(opCtx, nss));
    ChunkVersion targetChunkVersion(curCollectionVersion.majorVersion() + 1,
                                    0,
                                    curCollectionVersion.epoch(),
                                    curCollectionVersion.getTimestamp());

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection does not exist",
            !findCollResponse.docs.empty());
    const CollectionType coll(findCollResponse.docs[0]);

    for (const auto& shardId : shardIds) {
        const auto query = [&] {
            if (coll.getTimestamp()) {
                return BSON(ChunkType::collectionUUID << coll.getUuid()
                                                      << ChunkType::shard(shardId.toString()));
            } else {
                return BSON(ChunkType::ns(coll.getNss().ns())
                            << ChunkType::shard(shardId.toString()));
            }
        }();

        BSONObjBuilder updateVersionClause;
        updateVersionClause.appendTimestamp(ChunkType::lastmod(), targetChunkVersion.toLong());

        auto request = BatchedCommandRequest::buildUpdateOp(
            ChunkType::ConfigNS,
            query,
            BSON("$set" << updateVersionClause.obj()),  // update
            false,                                      // upsert
            false                                       // multi
        );

        auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
            opCtx, ChunkType::ConfigNS, request, txnNumber);

        auto numDocsExpectedModified = 1;
        auto numDocsModified = res.getIntField("n");

        uassert(5030400,
                str::stream() << "Expected to match " << numDocsExpectedModified
                              << " docs, but only matched " << numDocsModified
                              << " for write request " << request.toString(),
                numDocsExpectedModified == numDocsModified);

        // There exists a constraint that a chunk version must be unique for a given namespace,
        // so the minor version is incremented for each chunk placed.
        targetChunkVersion.incMinor();
    }
}

NamespaceStringOrUUID getNsOrUUIDForChunkTargeting(const CollectionType& coll) {
    if (coll.getTimestamp()) {
        return {coll.getNss().db().toString(), coll.getUuid()};
    } else {
        return {coll.getNss()};
    }
}

std::vector<ShardId> getShardsOwningChunksForCollection(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1));
    uassert(
        ErrorCodes::Error(5514600), "Collection does not exist", !findCollResponse.docs.empty());
    const CollectionType coll(findCollResponse.docs[0]);
    const auto nsOrUUID = getNsOrUUIDForChunkTargeting(coll);

    DistinctCommandRequest distinctCmd(ChunkType::ConfigNS, ChunkType::shard.name());
    if (nsOrUUID.uuid()) {
        distinctCmd.setQuery(BSON(ChunkType::collectionUUID << *(nsOrUUID.uuid())));
    } else {
        distinctCmd.setQuery(BSON(ChunkType::ns(nsOrUUID.nss()->ns())));
    }

    const auto distinctResult = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        NamespaceString::kConfigDb.toString(),
        distinctCmd.toBSON({}),
        Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(distinctResult.commandStatus);

    const auto valuesElem = distinctResult.response.getField("values");
    std::vector<ShardId> shardIds;
    for (const auto& shard : valuesElem.Array()) {
        shardIds.emplace_back(shard.String());
    }
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find shardIds owning chunks for collection '" << nss.ns()
                          << ", but found none",
            !shardIds.empty());

    return shardIds;
}

}  // namespace

StatusWith<BSONObj> ShardingCatalogManager::commitChunkSplit(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const OID& requestEpoch,
    const ChunkRange& range,
    const std::vector<BSONObj>& splitPoints,
    const std::string& shardName) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    // Get the max chunk version for this namespace.
    auto swCollVersion = getCollectionVersion(opCtx, nss);

    if (!swCollVersion.isOK()) {
        return swCollVersion.getStatus().withContext(
            str::stream() << "splitChunk cannot split chunk " << range.toString() << ".");
    }

    auto collVersion = swCollVersion.getValue();

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection does not exist",
            !findCollResponse.docs.empty());
    const CollectionType coll(findCollResponse.docs[0]);

    // Return an error if collection epoch does not match epoch of request.
    if (coll.getEpoch() != requestEpoch) {
        return {ErrorCodes::StaleEpoch,
                str::stream() << "splitChunk cannot split chunk " << range.toString()
                              << ". Epoch of collection '" << nss.ns() << "' has changed."
                              << " Current epoch: " << coll.getEpoch()
                              << ", cmd epoch: " << requestEpoch};
    }

    // Find the chunk history.
    const auto collNsOrUUID = getNsOrUUIDForChunkTargeting(coll);
    const auto origChunk = _findChunkOnConfig(
        opCtx, collNsOrUUID, coll.getEpoch(), coll.getTimestamp(), range.getMin());
    if (!origChunk.isOK()) {
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return origChunk.getStatus();
    }

    std::vector<ChunkType> newChunks;

    ChunkVersion currentMaxVersion = collVersion;

    auto startKey = range.getMin();
    auto newChunkBounds(splitPoints);
    newChunkBounds.push_back(range.getMax());

    auto shouldTakeOriginalChunkID = true;
    OID chunkID;

    BSONArrayBuilder updates;

    for (const auto& endKey : newChunkBounds) {
        // Verify the split points are all within the chunk
        if (endKey.woCompare(range.getMax()) != 0 && !range.containsKey(endKey)) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Split key " << endKey << " not contained within chunk "
                                  << range.toString()};
        }

        // Verify the split points came in increasing order
        if (endKey.woCompare(startKey) < 0) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream() << "Split keys must be specified in strictly increasing order. Key "
                              << endKey << " was specified after " << startKey << "."};
        }

        // Verify that splitPoints are not repeated
        if (endKey.woCompare(startKey) == 0) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Split on lower bound of chunk [" << startKey.toString()
                                  << ", " << endKey.toString() << "] is not allowed"};
        }

        // verify that splits don't use disallowed BSON object format
        Status shardKeyStorageStatus =
            ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(endKey);
        if (!shardKeyStorageStatus.isOK()) {
            return shardKeyStorageStatus;
        }

        // splits only update the 'minor' portion of version
        currentMaxVersion.incMinor();

        // First chunk takes ID of the original chunk and all other chunks get new IDs. This occurs
        // because we perform an update operation below (with upsert true). Keeping the original ID
        // ensures we overwrite the old chunk (before the split) without having to perform a delete.
        chunkID = shouldTakeOriginalChunkID ? origChunk.getValue().getName() : OID::gen();

        shouldTakeOriginalChunkID = false;

        // build an update operation against the chunks collection of the config database
        // with upsert true
        BSONObjBuilder op;
        op.append("op", "u");
        op.appendBool("b", true);
        op.append("ns", ChunkType::ConfigNS.ns());


        ChunkType newChunk = origChunk.getValue();
        newChunk.setName(chunkID);
        newChunk.setVersion(currentMaxVersion);
        newChunk.setMin(startKey);
        newChunk.setMax(endKey);

        op.append("o", newChunk.toConfigBSON());

        // add the chunk's _id as the query part of the update statement
        BSONObjBuilder q(op.subobjStart("o2"));
        q.append(ChunkType::name(), chunkID);
        q.done();

        updates.append(op.obj());

        // remember this chunk info for logging later
        newChunks.push_back(std::move(newChunk));

        startKey = endKey;
    }

    BSONArrayBuilder preCond;
    {
        BSONObjBuilder b;
        b.append("ns", ChunkType::ConfigNS.ns());

        BSONObj query =
            BSON(ChunkType::min() << range.getMin() << ChunkType::max() << range.getMax());
        if (origChunk.getValue().getVersion().getTimestamp()) {
            query = query.addFields(
                BSON(ChunkType::collectionUUID << origChunk.getValue().getCollectionUUID()));
        } else {
            query = query.addFields(BSON(ChunkType::ns(nss.ns())));
        }

        b.append("q", BSON("query" << query << "orderby" << BSON(ChunkType::lastmod() << -1)));

        const auto resultMustMatch = origChunk.getValue().getVersion().getTimestamp()
            ? BSON(ChunkType::collectionUUID()
                   << origChunk.getValue().getCollectionUUID() << ChunkType::shard(shardName))
            : BSON(ChunkType::epoch(requestEpoch) << ChunkType::shard(shardName));

        b.append("res", resultMustMatch);

        preCond.append(b.obj());
    }

    // apply the batch of updates to local metadata.
    const Status applyOpsStatus = Grid::get(opCtx)->catalogClient()->applyChunkOpsDeprecated(
        opCtx,
        updates.arr(),
        preCond.arr(),
        collNsOrUUID,
        nss,
        currentMaxVersion,
        WriteConcernOptions(),
        repl::ReadConcernLevel::kLocalReadConcern);
    if (!applyOpsStatus.isOK()) {
        return applyOpsStatus;
    }

    // The current implementation of the split chunk is not idempotent (SERVER-51805).
    // Best effort: in order to reduce the probability of having an error, try to execute the
    // getShardAndCollectionVersion as soon as the batch of updates is completed
    const auto shardAndCollVersion = getShardAndCollectionVersion(opCtx, coll, ShardId(shardName));

    // log changes
    BSONObjBuilder logDetail;
    {
        BSONObjBuilder b(logDetail.subobjStart("before"));
        b.append(ChunkType::min(), range.getMin());
        b.append(ChunkType::max(), range.getMax());
        collVersion.appendLegacyWithField(&b, ChunkType::lastmod());
    }

    if (newChunks.size() == 2) {
        appendShortVersion(&logDetail.subobjStart("left"), newChunks[0]);
        appendShortVersion(&logDetail.subobjStart("right"), newChunks[1]);
        logDetail.append("owningShard", shardName);

        ShardingLogging::get(opCtx)->logChange(
            opCtx, "split", nss.ns(), logDetail.obj(), WriteConcernOptions());
    } else {
        BSONObj beforeDetailObj = logDetail.obj();
        BSONObj firstDetailObj = beforeDetailObj.getOwned();
        const int newChunksSize = newChunks.size();

        for (int i = 0; i < newChunksSize; i++) {
            BSONObjBuilder chunkDetail;
            chunkDetail.appendElements(beforeDetailObj);
            chunkDetail.append("number", i + 1);
            chunkDetail.append("of", newChunksSize);
            appendShortVersion(&chunkDetail.subobjStart("chunk"), newChunks[i]);
            chunkDetail.append("owningShard", shardName);

            const auto status = ShardingLogging::get(opCtx)->logChangeChecked(
                opCtx, "multi-split", nss.ns(), chunkDetail.obj(), WriteConcernOptions());

            // Stop logging if the last log op failed because the primary stepped down
            if (status.code() == ErrorCodes::InterruptedDueToReplStateChange)
                break;
        }
    }

    return shardAndCollVersion;
}

StatusWith<BSONObj> ShardingCatalogManager::commitChunkMerge(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const OID& requestEpoch,
    const std::vector<BSONObj>& chunkBoundaries,
    const std::string& shardName,
    const boost::optional<Timestamp>& validAfter) {
    // This method must never be called with empty chunks to merge
    invariant(!chunkBoundaries.empty());

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    if (!validAfter) {
        return {ErrorCodes::IllegalOperation, "chunk operation requires validAfter timestamp"};
    }

    // Get the max chunk version for this namespace.
    auto swCollVersion = getCollectionVersion(opCtx, nss);
    if (!swCollVersion.isOK()) {
        return swCollVersion.getStatus().withContext(str::stream()
                                                     << "mergeChunk cannot merge chunks.");
    }
    auto collVersion = swCollVersion.getValue();

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection does not exist",
            !findCollResponse.docs.empty());
    const CollectionType coll(findCollResponse.docs[0]);

    if (coll.getEpoch() != requestEpoch) {
        return {ErrorCodes::StaleEpoch,
                str::stream()
                    << "Epoch of collection does not match epoch of request. Collection epoch: "
                    << coll.getEpoch() << ", request epoch: " << requestEpoch};
    }

    // Check if the chunk(s) have already been merged. If so, return success.
    const auto collNsOrUUID = getNsOrUUIDForChunkTargeting(coll);
    auto minChunkOnDisk = uassertStatusOK(_findChunkOnConfig(
        opCtx, collNsOrUUID, coll.getEpoch(), coll.getTimestamp(), chunkBoundaries.front()));
    if (minChunkOnDisk.getMax().woCompare(chunkBoundaries.back()) == 0) {
        auto replyWithVersions = getShardAndCollectionVersion(opCtx, coll, ShardId(shardName));
        // Makes sure that the last thing we read in getCurrentChunk and
        // getShardAndCollectionVersion gets majority written before to return from this command,
        // otherwise next RoutingInfo cache refresh from the shard may not see those newest
        // information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return replyWithVersions;
    }

    // Build chunks to be merged
    std::vector<ChunkType> chunksToMerge;

    // Do not use the first chunk boundary as a max bound while building chunks
    for (size_t i = 1; i < chunkBoundaries.size(); ++i) {
        // Read the original chunk from disk to lookup that chunk's '_id' field.
        auto currentChunk = uassertStatusOK(_findChunkOnConfig(
            opCtx, collNsOrUUID, coll.getEpoch(), coll.getTimestamp(), chunkBoundaries[i - 1]));

        // Ensure the chunk boundaries are strictly increasing
        if (chunkBoundaries[i].woCompare(currentChunk.getMin()) <= 0) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream()
                    << "Chunk boundaries must be specified in strictly increasing order. Boundary "
                    << chunkBoundaries[i] << " was specified after " << currentChunk.getMin()
                    << "."};
        }

        chunksToMerge.push_back(std::move(currentChunk));
    }

    ChunkVersion mergeVersion = collVersion;
    mergeVersion.incMinor();

    auto updates = buildMergeChunksTransactionUpdates(chunksToMerge, mergeVersion, validAfter);
    auto preCond = buildMergeChunksTransactionPrecond(chunksToMerge, collVersion);

    // apply the batch of updates to local metadata
    const Status applyOpsStatus = Grid::get(opCtx)->catalogClient()->applyChunkOpsDeprecated(
        opCtx,
        updates,
        preCond,
        collNsOrUUID,
        nss,
        mergeVersion,
        WriteConcernOptions(),
        repl::ReadConcernLevel::kLocalReadConcern);
    if (!applyOpsStatus.isOK()) {
        return applyOpsStatus;
    }

    // log changes
    BSONObjBuilder logDetail;
    {
        BSONArrayBuilder b(logDetail.subarrayStart("merged"));
        for (const auto& chunkToMerge : chunksToMerge) {
            b.append(chunkToMerge.toConfigBSON());
        }
    }
    collVersion.appendLegacyWithField(&logDetail, "prevShardVersion");
    mergeVersion.appendLegacyWithField(&logDetail, "mergedVersion");
    logDetail.append("owningShard", shardName);

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "merge", nss.ns(), logDetail.obj(), WriteConcernOptions());

    return getShardAndCollectionVersion(opCtx, coll, ShardId(shardName));
}

StatusWith<BSONObj> ShardingCatalogManager::commitChunksMerge(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& requestCollectionUUID,
    const ChunkRange& chunkRange,
    const ShardId& shardId,
    const boost::optional<Timestamp>& validAfter) {
    if (!validAfter) {
        return {ErrorCodes::IllegalOperation, "chunk operation requires validAfter timestamp"};
    }

    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    // 1. Retrieve the initial collection version info to build up the logging info.
    auto swCollVersion = getCollectionVersion(opCtx, nss);
    if (!swCollVersion.isOK()) {
        return swCollVersion.getStatus().withContext(str::stream()
                                                     << "mergeChunk cannot merge chunks.");
    }

    // 2. Retrieve the list of chunks belonging to the requested shard + key range.
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1));

    if (findCollResponse.docs.empty()) {
        return {ErrorCodes::Error(5678601),
                str::stream() << "Collection '" << nss.ns() << "' no longer either exists"};
    }

    const CollectionType coll(findCollResponse.docs[0]);
    if (coll.getUuid() != requestCollectionUUID) {
        return {
            ErrorCodes::InvalidUUID,
            str::stream() << "UUID of collection does not match UUID of request. Colletion UUID: "
                          << coll.getUuid() << ", request UUID: " << requestCollectionUUID};
    }
    const auto shardChunksInRangeQuery = [&]() {
        BSONObjBuilder queryBuilder;
        if (coll.getTimestamp()) {
            queryBuilder << ChunkType::collectionUUID << coll.getUuid();
        } else {
            queryBuilder << ChunkType::ns(coll.getNss().ns());
        }
        queryBuilder << ChunkType::shard(shardId.toString());
        queryBuilder << ChunkType::min(BSON("$gte" << chunkRange.getMin()));
        queryBuilder << ChunkType::min(BSON("$lt" << chunkRange.getMax()));
        return queryBuilder.obj();
    }();

    const auto shardChunksInRangeResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            shardChunksInRangeQuery,
                                            BSON(ChunkType::min << 1),
                                            boost::none));

    // Check if the chunk(s) have already been merged. If so, return success.
    if (shardChunksInRangeResponse.docs.size() == 1) {
        auto chunk = uassertStatusOK(ChunkType::fromConfigBSON(
            shardChunksInRangeResponse.docs.back(), coll.getEpoch(), coll.getTimestamp()));
        uassert(
            ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, shard " << shardId
                          << " does not contain a sequence of chunks that exactly fills the range "
                          << chunkRange.toString(),
            chunk.getRange() == chunkRange);
        auto replyWithVersions = getShardAndCollectionVersion(opCtx, coll, shardId);
        // Makes sure that the last thing we read in getCurrentChunk and
        // getShardAndCollectionVersion gets majority written before to return from this command,
        // otherwise next RoutingInfo cache refresh from the shard may not see those newest
        // information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return replyWithVersions;
    }

    // 3. Prepare the data for the merge
    //    and ensure that the retrieved list of chunks covers the whole range.
    std::vector<ChunkType> chunksToMerge;
    for (const auto& chunkDoc : shardChunksInRangeResponse.docs) {
        auto chunk = uassertStatusOK(
            ChunkType::fromConfigBSON(chunkDoc, coll.getEpoch(), coll.getTimestamp()));
        if (chunksToMerge.empty()) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream()
                        << "could not merge chunks, shard " << shardId
                        << " does not contain a sequence of chunks that exactly fills the range "
                        << chunkRange.toString(),
                    chunk.getMin().woCompare(chunkRange.getMin()) == 0);
        } else {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream()
                        << "could not merge chunks, shard " << shardId
                        << " does not contain a sequence of chunks that exactly fills the range "
                        << chunkRange.toString(),
                    chunk.getMin().woCompare(chunksToMerge.back().getMax()) == 0);
        }
        chunksToMerge.push_back(std::move(chunk));
    }
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, shard " << shardId
                          << " does not contain a sequence of chunks that exactly fills the range "
                          << chunkRange.toString(),
            !chunksToMerge.empty() &&
                chunksToMerge.back().getMax().woCompare(chunkRange.getMax()) == 0);

    ChunkVersion initialVersion = swCollVersion.getValue();
    ChunkVersion mergeVersion = initialVersion;
    mergeVersion.incMinor();

    auto updates = buildMergeChunksTransactionUpdates(chunksToMerge, mergeVersion, validAfter);
    auto preCond = buildMergeChunksTransactionPrecond(chunksToMerge, initialVersion);

    // 4. apply the batch of updates to local metadata
    uassertStatusOK(Grid::get(opCtx)->catalogClient()->applyChunkOpsDeprecated(
        opCtx,
        updates,
        preCond,
        getNsOrUUIDForChunkTargeting(coll),
        nss,
        mergeVersion,
        WriteConcernOptions(),
        repl::ReadConcernLevel::kLocalReadConcern));

    // 5. log changes
    BSONObjBuilder logDetail;
    {
        BSONArrayBuilder b(logDetail.subarrayStart("merged"));
        for (const auto& chunkToMerge : chunksToMerge) {
            b.append(chunkToMerge.toConfigBSON());
        }
    }
    initialVersion.appendLegacyWithField(&logDetail, "prevShardVersion");
    mergeVersion.appendLegacyWithField(&logDetail, "mergedVersion");
    logDetail.append("owningShard", shardId);

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "merge", nss.ns(), logDetail.obj(), WriteConcernOptions());

    return getShardAndCollectionVersion(opCtx, coll, shardId);
}

StatusWith<BSONObj> ShardingCatalogManager::commitChunkMigration(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkType& migratedChunk,
    const OID& collectionEpoch,
    const ShardId& fromShard,
    const ShardId& toShard,
    const boost::optional<Timestamp>& validAfter) {

    // TODO(SERVER-53283): Remove the logic around fcvRegion to re-enable
    // the concurrent execution of moveChunk() and setFCV().
    FixedFCVRegion fcvRegion(opCtx);
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Cannot commit a chunk migration request "
            "while the cluster is being upgraded or downgraded",
            !fcvRegion->isUpgradingOrDowngrading());


    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Must hold the shard lock until the entire commit finishes to serialize with removeShard.
    Lock::SharedLock shardLock(opCtx->lockState(), _kShardMembershipLock);
    auto shardResult = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ShardType::ConfigNS,
                                            BSON(ShardType::name(toShard.toString())),
                                            {},
                                            boost::none));

    uassert(ErrorCodes::ShardNotFound,
            str::stream() << "Shard " << toShard << " does not exist",
            !shardResult.docs.empty());

    auto shard = uassertStatusOK(ShardType::fromBSON(shardResult.docs.front()));
    uassert(ErrorCodes::ShardNotFound,
            str::stream() << "Shard " << toShard << " is currently draining",
            !shard.getDraining());

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations.
    //
    // ConfigSvrCommitChunkMigration commands must be run serially because the new ChunkVersions
    // for migrated chunks are generated within the command and must be committed to the database
    // before another chunk commit generates new ChunkVersions in the same manner.
    //
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel.
    // (Note: This is not needed while we have a global lock, taken here only for consistency.)
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    if (!validAfter) {
        return {ErrorCodes::IllegalOperation, "chunk operation requires validAfter timestamp"};
    }

    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection does not exist",
            !findCollResponse.docs.empty());
    const CollectionType coll(findCollResponse.docs[0]);
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection is undergoing changes and chunks cannot be moved",
            coll.getAllowMigrations());

    const auto findChunkQuery = coll.getTimestamp()
        ? BSON(ChunkType::collectionUUID() << coll.getUuid())
        : BSON(ChunkType::ns(coll.getNss().ns()));

    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            findChunkQuery,
                                            BSON(ChunkType::lastmod << -1),
                                            1));
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max chunk version for collection '" << nss.ns()
                          << ", but found no chunks",
            !findResponse.docs.empty());

    const auto chunk = uassertStatusOK(
        ChunkType::fromConfigBSON(findResponse.docs[0], coll.getEpoch(), coll.getTimestamp()));
    const auto& currentCollectionVersion = chunk.getVersion();

    if (MONGO_unlikely(migrationCommitVersionError.shouldFail())) {
        uasserted(ErrorCodes::StaleEpoch,
                  "Failpoint 'migrationCommitVersionError' generated error");
    }

    // It is possible for a migration to end up running partly without the protection of the
    // distributed lock if the config primary stepped down since the start of the migration and
    // failed to recover the migration. Check that the collection has not been dropped and recreated
    // or had its shard key refined since the migration began, unbeknown to the shard when the
    // command was sent.
    if (currentCollectionVersion.epoch() != collectionEpoch) {
        return {ErrorCodes::StaleEpoch,
                str::stream() << "The epoch of collection '" << nss.ns()
                              << "' has changed since the migration began. The config server's "
                                 "collection version epoch is now '"
                              << currentCollectionVersion.epoch().toString()
                              << "', but the shard's is " << collectionEpoch.toString()
                              << "'. Aborting migration commit for chunk ("
                              << migratedChunk.getRange().toString() << ")."};
    }

    // Check if chunk still exists and which shard owns it
    const auto collNsOrUUID = getNsOrUUIDForChunkTargeting(coll);
    auto swCurrentChunk =
        getCurrentChunk(opCtx, collNsOrUUID, coll.getEpoch(), coll.getTimestamp(), migratedChunk);

    if (!swCurrentChunk.isOK()) {
        return swCurrentChunk.getStatus();
    }

    auto currentChunk = swCurrentChunk.getValue();

    if (currentChunk.getShard() == toShard) {
        // The commit was already done successfully
        auto replyWithVersions = getShardAndCollectionVersion(opCtx, coll, fromShard);
        // Makes sure that the last thing we read in getCurrentChunk and
        // getShardAndCollectionVersion gets majority written before to return from this command,
        // otherwise next RoutingInfo cache refresh from the shard may not see those newest
        // information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return replyWithVersions;
    }

    uassert(4914702,
            str::stream() << "Migrated  chunk " << migratedChunk.toString()
                          << " from ns: " << nss.ns() << " not owned by donor " << fromShard
                          << " neither by recipient " << toShard,
            currentChunk.getShard() == fromShard);

    if (migratedChunk.getVersion().epoch() != currentChunk.getVersion().epoch() ||
        migratedChunk.getVersion().isOlderThan(currentChunk.getVersion())) {
        return {ErrorCodes::ConflictingOperationInProgress,
                str::stream()
                    << "Rejecting migration request because the version of the requested chunk "
                    << migratedChunk.toConfigBSON()
                    << " is older than the version of the current chunk "
                    << currentChunk.toConfigBSON() << " on the shard " << fromShard.toString()};
    }

    auto controlChunk = getControlChunkForMigrate(
        opCtx, collNsOrUUID, coll.getEpoch(), coll.getTimestamp(), migratedChunk, fromShard);

    // Find the chunk history.
    const auto origChunk = uassertStatusOK(_findChunkOnConfig(
        opCtx, collNsOrUUID, coll.getEpoch(), coll.getTimestamp(), migratedChunk.getMin()));

    // Generate the new versions of migratedChunk and controlChunk. Migrating chunk's minor version
    // will be 0.
    ChunkType newMigratedChunk = origChunk;

    newMigratedChunk.setShard(toShard);
    newMigratedChunk.setVersion(ChunkVersion(currentCollectionVersion.majorVersion() + 1,
                                             0,
                                             currentCollectionVersion.epoch(),
                                             currentCollectionVersion.getTimestamp()));

    // Copy the complete history.
    auto newHistory = origChunk.getHistory();
    invariant(validAfter);

    // Drop old history. Keep at least 1 entry so ChunkInfo::getShardIdAt finds valid history for
    // any query younger than the history window.
    if (!MONGO_unlikely(skipExpiringOldChunkHistory.shouldFail())) {
        auto windowInSeconds = std::max(std::max(minSnapshotHistoryWindowInSeconds.load(),
                                                 gTransactionLifetimeLimitSeconds.load()),
                                        10);
        int entriesDeleted = 0;
        while (newHistory.size() > 1 &&
               newHistory.back().getValidAfter().getSecs() + windowInSeconds <
                   validAfter.get().getSecs()) {
            newHistory.pop_back();
            ++entriesDeleted;
        }

        logv2::DynamicAttributes attrs;
        attrs.add("entriesDeleted", entriesDeleted);
        if (!newHistory.empty()) {
            attrs.add("oldestEntryValidAfter", newHistory.back().getValidAfter());
        }

        LOGV2_DEBUG(4778500, 1, "Deleted old chunk history entries", attrs);
    }

    if (!newHistory.empty() && newHistory.front().getValidAfter() >= validAfter.get()) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "The chunk history for chunk with namespace " << nss.ns()
                              << " and min key " << migratedChunk.getMin()
                              << " is corrupted. The last validAfter "
                              << newHistory.back().getValidAfter().toString()
                              << " is greater or equal to the new validAfter "
                              << validAfter.get().toString()};
    }
    newHistory.emplace(newHistory.begin(), ChunkHistory(validAfter.get(), toShard));
    newMigratedChunk.setHistory(std::move(newHistory));

    // Control chunk's minor version will be 1 (if control chunk is present).
    boost::optional<ChunkType> newControlChunk = boost::none;
    if (controlChunk) {
        // Find the chunk history.
        auto origControlChunk = uassertStatusOK(_findChunkOnConfig(
            opCtx, collNsOrUUID, coll.getEpoch(), coll.getTimestamp(), controlChunk->getMin()));

        newControlChunk = std::move(origControlChunk);
        newControlChunk->setVersion(ChunkVersion(currentCollectionVersion.majorVersion() + 1,
                                                 1,
                                                 currentCollectionVersion.epoch(),
                                                 currentCollectionVersion.getTimestamp()));
    }

    auto command = makeCommitChunkTransactionCommand(
        nss, newMigratedChunk, newControlChunk, fromShard.toString(), toShard.toString());

    StatusWith<Shard::CommandResponse> applyOpsCommandResponse =
        configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.db().toString(),
            command,
            Shard::RetryPolicy::kIdempotent);

    if (!applyOpsCommandResponse.isOK()) {
        return applyOpsCommandResponse.getStatus();
    }

    if (!applyOpsCommandResponse.getValue().commandStatus.isOK()) {
        return applyOpsCommandResponse.getValue().commandStatus;
    }

    return getShardAndCollectionVersion(opCtx, coll, fromShard);
}

StatusWith<ChunkType> ShardingCatalogManager::_findChunkOnConfig(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    const OID& epoch,
    const boost::optional<Timestamp>& timestamp,
    const BSONObj& key) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    const auto query = [&]() {
        if (nsOrUUID.uuid()) {
            return BSON(ChunkType::collectionUUID << *(nsOrUUID.uuid()) << ChunkType::min(key));
        } else {
            return BSON(ChunkType::ns(nsOrUUID.nss()->ns()) << ChunkType::min(key));
        }
    }();

    auto findResponse =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            query,
                                            BSONObj(),
                                            1);

    if (!findResponse.isOK()) {
        return findResponse.getStatus();
    }

    const auto origChunks = std::move(findResponse.getValue().docs);
    if (origChunks.size() != 1) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Tried to find the chunk for namespace/uuid" << nsOrUUID.toString()
                              << " and min key " << key.toString() << ", but found no chunks"};
    }

    return ChunkType::fromConfigBSON(origChunks.front(), epoch, timestamp);
}

void ShardingCatalogManager::clearJumboFlag(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const OID& collectionEpoch,
                                            const ChunkRange& chunk) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations.
    //
    // ConfigSvrClearJumboFlag commands must be run serially because the new ChunkVersions
    // for the modified chunks are generated within the command and must be committed to the
    // database before another chunk operation generates new ChunkVersions in the same manner.
    //
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel.
    // (Note: This is not needed while we have a global lock, taken here only for consistency.)
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection does not exist",
            !findCollResponse.docs.empty());
    const CollectionType coll(findCollResponse.docs[0]);

    BSONObj targetChunkQuery =
        BSON(ChunkType::min(chunk.getMin()) << ChunkType::max(chunk.getMax()));
    if (coll.getTimestamp()) {
        targetChunkQuery =
            targetChunkQuery.addFields(BSON(ChunkType::collectionUUID << coll.getUuid()));
    } else {
        targetChunkQuery = targetChunkQuery.addFields(BSON(ChunkType::ns(coll.getNss().ns())));
    }

    auto targetChunkResult = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            targetChunkQuery,
                                            {},
                                            1));

    const auto targetChunkVector = std::move(targetChunkResult.docs);
    uassert(51262,
            str::stream() << "Unable to locate chunk " << chunk.toString()
                          << " from ns: " << nss.ns(),
            !targetChunkVector.empty());

    const auto targetChunk = uassertStatusOK(
        ChunkType::fromConfigBSON(targetChunkVector.front(), coll.getEpoch(), coll.getTimestamp()));

    if (!targetChunk.getJumbo()) {
        return;
    }

    const auto allChunksQuery = coll.getTimestamp()
        ? BSON(ChunkType::collectionUUID << coll.getUuid())
        : BSON(ChunkType::ns(coll.getNss().ns()));

    // Must use local read concern because we will perform subsequent writes.
    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
                                            allChunksQuery,
                                            BSON(ChunkType::lastmod << -1),
                                            1));

    const auto chunksVector = std::move(findResponse.docs);
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max chunk version for collection '" << nss.ns()
                          << ", but found no chunks",
            !chunksVector.empty());

    const auto highestVersionChunk = uassertStatusOK(
        ChunkType::fromConfigBSON(chunksVector.front(), coll.getEpoch(), coll.getTimestamp()));
    const auto currentCollectionVersion = highestVersionChunk.getVersion();

    // It is possible for a migration to end up running partly without the protection of the
    // distributed lock if the config primary stepped down since the start of the migration and
    // failed to recover the migration. Check that the collection has not been dropped and recreated
    // or had its shard key refined since the migration began, unbeknown to the shard when the
    // command was sent.
    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "The epoch of collection '" << nss.ns()
                          << "' has changed since the migration began. The config server's "
                             "collection version epoch is now '"
                          << currentCollectionVersion.epoch().toString() << "', but the shard's is "
                          << collectionEpoch.toString() << "'. Aborting clear jumbo on chunk ("
                          << chunk.toString() << ").",
            currentCollectionVersion.epoch() == collectionEpoch);

    ChunkVersion newVersion(currentCollectionVersion.majorVersion() + 1,
                            0,
                            currentCollectionVersion.epoch(),
                            currentCollectionVersion.getTimestamp());


    BSONObj chunkQuery(BSON(ChunkType::min(chunk.getMin()) << ChunkType::max(chunk.getMax())));
    if (coll.getTimestamp()) {
        chunkQuery = chunkQuery.addFields(BSON(ChunkType::collectionUUID << coll.getUuid()));
    } else {
        chunkQuery = chunkQuery.addFields(
            BSON(ChunkType::ns(coll.getNss().ns()) << ChunkType::epoch(collectionEpoch)));
    }

    BSONObjBuilder updateBuilder;
    updateBuilder.append("$unset", BSON(ChunkType::jumbo() << ""));

    BSONObjBuilder updateVersionClause(updateBuilder.subobjStart("$set"));
    newVersion.appendLegacyWithField(&updateVersionClause, ChunkType::lastmod());
    updateVersionClause.doneFast();

    auto chunkUpdate = updateBuilder.obj();

    auto didUpdate = uassertStatusOK(
        Grid::get(opCtx)->catalogClient()->updateConfigDocument(opCtx,
                                                                ChunkType::ConfigNS,
                                                                chunkQuery,
                                                                chunkUpdate,
                                                                false /* upsert */,
                                                                kNoWaitWriteConcern));

    uassert(51263,
            str::stream() << "failed to clear jumbo flag due to " << chunkQuery
                          << " not matching any existing chunks",
            didUpdate);
}

void ShardingCatalogManager::ensureChunkVersionIsGreaterThan(
    OperationContext* opCtx,
    const boost::optional<NamespaceString>& nss,
    const boost::optional<UUID>& collUuid,
    const BSONObj& minKey,
    const BSONObj& maxKey,
    const ChunkVersion& version) {
    auto earlyReturnBeforeDoingWriteGuard = makeGuard([&] {
        // Ensure waiting for writeConcern of the data read.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    });

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk operations.
    // TODO (SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel.
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    CollectionType coll;
    {
        auto findCollResponse = uassertStatusOK(configShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            CollectionType::ConfigNS,
            BSON(CollectionType::kEpochFieldName << version.epoch()),
            {} /* sort */,
            1));

        if (findCollResponse.docs.empty()) {
            LOGV2(5731600,
                  "ensureChunkVersionIsGreaterThan did not find a collection with epoch "
                  "{epoch} epoch; returning success.",
                  "epoch"_attr = version.epoch());
            return;
        }

        coll = CollectionType(findCollResponse.docs[0]);
        dassert(!collUuid || *collUuid == coll.getUuid());
    }

    const auto requestedChunkQuery = coll.getTimestamp()
        ? BSON(ChunkType::min(minKey)
               << ChunkType::max(maxKey) << ChunkType::collectionUUID() << *collUuid)
        : BSON(ChunkType::min(minKey) << ChunkType::max(maxKey) << ChunkType::ns(coll.getNss().ns())
                                      << ChunkType::epoch(version.epoch()));

    // Get the chunk matching the requested chunk.
    ChunkType matchingChunk;
    {
        const auto matchingChunksVector =
            uassertStatusOK(configShard->exhaustiveFindOnConfig(
                                opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                repl::ReadConcernLevel::kLocalReadConcern,
                                ChunkType::ConfigNS,
                                requestedChunkQuery,
                                BSONObj() /* sort */,
                                1 /* limit */))
                .docs;
        if (matchingChunksVector.empty()) {
            // This can happen in a number of cases, such as that the collection has been
            // dropped, its shard key has been refined, the chunk has been split, or the chunk
            // has been merged.
            LOGV2(23884,
                  "ensureChunkVersionIsGreaterThan did not find any chunks with minKey {minKey}, "
                  "maxKey {maxKey}, and epoch {epoch}. Returning success.",
                  "ensureChunkVersionIsGreaterThan did not find any matching chunks; returning "
                  "success",
                  "minKey"_attr = minKey,
                  "maxKey"_attr = maxKey,
                  "epoch"_attr = version.epoch());
            return;
        }

        matchingChunk = uassertStatusOK(ChunkType::fromConfigBSON(
            matchingChunksVector.front(), coll.getEpoch(), coll.getTimestamp()));

        if (version.isOlderThan(matchingChunk.getVersion())) {
            LOGV2(23885,
                  "ensureChunkVersionIsGreaterThan found that the chunk with minKey {minKey}, "
                  "maxKey "
                  "{maxKey}, and epoch {epoch} already has a higher version than {version}. "
                  "Current "
                  "chunk is {currentChunk}. Returning success.",
                  "ensureChunkVersionIsGreaterThan found that the chunk already has a higher "
                  "version; "
                  "returning success",
                  "minKey"_attr = minKey,
                  "maxKey"_attr = maxKey,
                  "epoch"_attr = version.epoch(),
                  "version"_attr = version,
                  "currentChunk"_attr = matchingChunk.toConfigBSON());
            return;
        }
    }

    // Get the chunk with the current collectionVersion for this epoch.
    ChunkType highestChunk;
    {
        const auto query = coll.getTimestamp() ? BSON(ChunkType::collectionUUID() << *collUuid)
                                               : BSON(ChunkType::epoch(version.epoch()));
        const auto highestChunksVector =
            uassertStatusOK(configShard->exhaustiveFindOnConfig(
                                opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                repl::ReadConcernLevel::kLocalReadConcern,
                                ChunkType::ConfigNS,
                                query,
                                BSON(ChunkType::lastmod << -1) /* sort */,
                                1 /* limit */))
                .docs;
        if (highestChunksVector.empty()) {
            LOGV2(23886,
                  "ensureChunkVersionIsGreaterThan did not find any chunks with epoch {epoch} "
                  "when "
                  "attempting to find the collectionVersion. The collection must have been "
                  "dropped "
                  "concurrently or had its shard key refined. Returning success.",
                  "ensureChunkVersionIsGreaterThan did not find any chunks with a matching epoch "
                  "when "
                  "attempting to find the collectionVersion. The collection must have been "
                  "dropped "
                  "concurrently or had its shard key refined. Returning success.",
                  "epoch"_attr = version.epoch());
            return;
        }
        highestChunk = uassertStatusOK(ChunkType::fromConfigBSON(
            highestChunksVector.front(), coll.getEpoch(), coll.getTimestamp()));
    }

    // Generate a new version for the chunk by incrementing the collectionVersion's major
    // version.
    auto newChunk = matchingChunk;
    newChunk.setVersion(ChunkVersion(
        highestChunk.getVersion().majorVersion() + 1, 0, coll.getEpoch(), coll.getTimestamp()));

    // Update the chunk, if it still exists, to have the bumped version.
    earlyReturnBeforeDoingWriteGuard.dismiss();
    auto didUpdate = uassertStatusOK(
        Grid::get(opCtx)->catalogClient()->updateConfigDocument(opCtx,
                                                                ChunkType::ConfigNS,
                                                                requestedChunkQuery,
                                                                newChunk.toConfigBSON(),
                                                                false /* upsert */,
                                                                kNoWaitWriteConcern));
    if (didUpdate) {
        LOGV2(23887,
              "ensureChunkVersionIsGreaterThan bumped the version of the chunk with minKey "
              "{minKey}, "
              "maxKey {maxKey}, and epoch {epoch}. Chunk is now {newChunk}",
              "ensureChunkVersionIsGreaterThan bumped the the chunk version",
              "minKey"_attr = minKey,
              "maxKey"_attr = maxKey,
              "epoch"_attr = version.epoch(),
              "newChunk"_attr = newChunk.toConfigBSON());
    } else {
        LOGV2(23888,
              "ensureChunkVersionIsGreaterThan did not find a chunk matching minKey {minKey}, "
              "maxKey {maxKey}, and epoch {epoch} when trying to bump its version. The "
              "collection "
              "must have been dropped concurrently or had its shard key refined. Returning "
              "success.",
              "ensureChunkVersionIsGreaterThan did not find a matching chunk when trying to bump "
              "its "
              "version. The collection must have been dropped concurrently or had its shard key "
              "refined. Returning success.",
              "minKey"_attr = minKey,
              "maxKey"_attr = maxKey,
              "epoch"_attr = version.epoch());
    }
}

void ShardingCatalogManager::bumpCollectionVersionAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const NamespaceString& nss,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {

    bumpMultipleCollectionVersionsAndChangeMetadataInTxn(
        opCtx, {nss}, std::move(changeMetadataFunc));
}

void ShardingCatalogManager::bumpMultipleCollectionVersionsAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const std::vector<NamespaceString>& collNames,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    using NssAndShardIds = std::pair<NamespaceString, std::vector<ShardId>>;
    std::vector<NssAndShardIds> nssAndShardIds;
    for (const auto& nss : collNames) {
        auto shardIds = getShardsOwningChunksForCollection(opCtx, nss);
        nssAndShardIds.emplace_back(nss, std::move(shardIds));
    }

    withTransaction(opCtx,
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    [&](OperationContext* opCtx, TxnNumber txnNumber) {
                        for (const auto& nssAndShardId : nssAndShardIds) {
                            bumpMajorVersionOneChunkPerShard(
                                opCtx, nssAndShardId.first, txnNumber, nssAndShardId.second);
                        }
                        changeMetadataFunc(opCtx, txnNumber);
                    });
}

void ShardingCatalogManager::splitOrMarkJumbo(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const BSONObj& minKey) {
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss));
    auto chunk = cm.findIntersectingChunkWithSimpleCollation(minKey);

    try {
        const auto splitPoints = uassertStatusOK(shardutil::selectChunkSplitPoints(
            opCtx,
            chunk.getShardId(),
            nss,
            cm.getShardKeyPattern(),
            ChunkRange(chunk.getMin(), chunk.getMax()),
            Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes(),
            boost::none));

        if (splitPoints.empty()) {
            LOGV2(21873,
                  "Marking chunk {chunk} as jumbo",
                  "Marking chunk as jumbo",
                  "chunk"_attr = redact(chunk.toString()));
            chunk.markAsJumbo();

            auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk operations.
            // TODO (SERVER-25359): Replace with a collection-specific lock map to allow
            // splits/merges/ move chunks on different collections to proceed in parallel.
            Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

            const auto findCollResponse = uassertStatusOK(configShard->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                CollectionType::ConfigNS,
                BSON(CollectionType::kNssFieldName << nss.ns()),
                {},
                1));
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Collection does not exist",
                    !findCollResponse.docs.empty());
            const CollectionType coll(findCollResponse.docs[0]);

            const auto chunkQuery = [&]() {
                if (coll.getTimestamp()) {
                    return BSON(ChunkType::collectionUUID()
                                << coll.getUuid() << ChunkType::min(chunk.getMin()));
                } else {
                    return BSON(ChunkType::ns(nss.ns()) << ChunkType::min(chunk.getMin()));
                }
            }();
            auto status = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
                opCtx,
                ChunkType::ConfigNS,
                chunkQuery,
                BSON("$set" << BSON(ChunkType::jumbo(true))),
                false,
                ShardingCatalogClient::kMajorityWriteConcern);
            if (!status.isOK()) {
                LOGV2(21874,
                      "Couldn't mark chunk with namespace {namespace} and min key {minKey} as "
                      "jumbo due to {error}",
                      "Couldn't mark chunk as jumbo",
                      "namespace"_attr = redact(nss.ns()),
                      "minKey"_attr = redact(chunk.getMin()),
                      "error"_attr = redact(status.getStatus()));
            }

            return;
        }

        uassertStatusOK(
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  chunk.getShardId(),
                                                  nss,
                                                  cm.getShardKeyPattern(),
                                                  cm.getVersion(),
                                                  ChunkRange(chunk.getMin(), chunk.getMax()),
                                                  splitPoints));
    } catch (const DBException&) {
    }
}

void ShardingCatalogManager::setAllowMigrationsAndBumpOneChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& collectionUUID,
    bool allowMigrations) {
    std::set<ShardId> shardsIds;
    {
        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations
        Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

        const auto cm = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss));

        uassert(ErrorCodes::InvalidUUID,
                str::stream() << "Collection uuid " << collectionUUID
                              << " in the request does not match the current uuid " << cm.getUUID()
                              << " for ns " << nss,
                !collectionUUID || collectionUUID == cm.getUUID());

        cm.getAllShardIds(&shardsIds);
        withTransaction(
            opCtx, CollectionType::ConfigNS, [&](OperationContext* opCtx, TxnNumber txnNumber) {
                // Update the 'allowMigrations' field. An unset 'allowMigrations' field implies
                // 'true'. To ease backwards compatibility we omit 'allowMigrations' instead of
                // setting it explicitly to 'true'.
                const auto update = allowMigrations
                    ? BSON("$unset" << BSON(CollectionType::kAllowMigrationsFieldName << ""))
                    : BSON("$set" << BSON(CollectionType::kAllowMigrationsFieldName << false));

                BSONObj query = BSON(CollectionType::kNssFieldName << nss.ns());
                if (collectionUUID) {
                    query =
                        query.addFields(BSON(CollectionType::kUuidFieldName << *collectionUUID));
                }

                const auto res = writeToConfigDocumentInTxn(
                    opCtx,
                    CollectionType::ConfigNS,
                    BatchedCommandRequest::buildUpdateOp(CollectionType::ConfigNS,
                                                         query,
                                                         update /* update */,
                                                         false /* upsert */,
                                                         false /* multi */),
                    txnNumber);
                const auto numDocsModified = UpdateOp::parseResponse(res).getN();
                uassert(ErrorCodes::ConflictingOperationInProgress,
                        str::stream() << "Expected to match one doc for query " << query
                                      << " but matched " << numDocsModified,
                        numDocsModified == 1);

                // Bump the chunk version for one single chunk
                invariant(!shardsIds.empty());
                bumpMajorVersionOneChunkPerShard(opCtx, nss, txnNumber, {*shardsIds.begin()});
            });

        // From now on migrations are not allowed anymore, so it is not possible that new shards
        // will own chunks for this collection.
    }

    // Trigger a refresh on each shard containing chunks for this collection.
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    sharding_util::tellShardsToRefreshCollection(
        opCtx,
        {std::make_move_iterator(shardsIds.begin()), std::make_move_iterator(shardsIds.end())},
        nss,
        executor);
}

}  // namespace mongo
