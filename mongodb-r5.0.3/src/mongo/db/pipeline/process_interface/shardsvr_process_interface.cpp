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

#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"

#include <fmt/format.h>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/stale_shard_version_helpers.h"

namespace mongo {

using namespace fmt::literals;

ShardServerProcessInterface::ShardServerProcessInterface(
    OperationContext* opCtx, std::shared_ptr<executor::TaskExecutor> executor)
    : CommonMongodProcessInterface(executor) {
    _opIsVersioned = OperationShardingState::isOperationVersioned(opCtx);
}

bool ShardServerProcessInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    const auto cm =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    return cm.isSharded();
}

void ShardServerProcessInterface::checkRoutingInfoEpochOrThrow(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    ChunkVersion targetCollectionVersion) const {

    auto const shardId = ShardingState::get(expCtx->opCtx)->shardId();
    auto* catalogCache = Grid::get(expCtx->opCtx)->catalogCache();

    // Mark the cache entry routingInfo for the 'nss' and 'shardId' if the entry is staler than
    // 'targetCollectionVersion'.
    catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        nss, targetCollectionVersion, shardId);

    const auto routingInfo =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(expCtx->opCtx, nss));

    const auto foundVersion =
        routingInfo.isSharded() ? routingInfo.getVersion() : ChunkVersion::UNSHARDED();

    uassert(StaleEpochInfo(nss),
            str::stream() << "could not act as router for " << nss.ns() << ", wanted "
                          << targetCollectionVersion.toString() << ", but found "
                          << foundVersion.toString(),
            foundVersion.epoch() == targetCollectionVersion.epoch());
}

std::pair<std::vector<FieldPath>, bool>
ShardServerProcessInterface::collectDocumentKeyFieldsForHostedCollection(OperationContext* opCtx,
                                                                         const NamespaceString& nss,
                                                                         UUID uuid) const {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    auto* const catalogCache = Grid::get(opCtx)->catalogCache();
    auto swCM = catalogCache->getCollectionRoutingInfo(opCtx, nss);
    if (swCM.isOK()) {
        const auto& cm = swCM.getValue();
        if (cm.isSharded() && cm.uuidMatches(uuid)) {
            // Unpack the shard key. Collection is now sharded so the document key fields will never
            // change, mark as final.
            return {_shardKeyToDocumentKeyFields(cm.getShardKeyPattern().getKeyPatternFields()),
                    true};
        }
    } else if (swCM != ErrorCodes::NamespaceNotFound) {
        uassertStatusOK(std::move(swCM));
    }

    // An unsharded collection can still become sharded so is not final. If the uuid doesn't match
    // the one stored in the ScopedCollectionDescription, this implies that the collection has been
    // dropped and recreated as sharded. We don't know what the old document key fields might have
    // been in this case so we return just _id.
    return {{"_id"}, false};
}

Status ShardServerProcessInterface::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const NamespaceString& ns,
                                           std::vector<BSONObj>&& objs,
                                           const WriteConcernOptions& wc,
                                           boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest insertCommand(
        buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    insertCommand.setWriteConcern(wc.toBSON());

    cluster::write(expCtx->opCtx, insertCommand, &stats, &response, targetEpoch);

    return response.toStatus();
}

StatusWith<MongoProcessInterface::UpdateResult> ShardServerProcessInterface::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    BatchedObjects&& batch,
    const WriteConcernOptions& wc,
    UpsertType upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest updateCommand(buildUpdateOp(expCtx, ns, std::move(batch), upsert, multi));

    updateCommand.setWriteConcern(wc.toBSON());

    cluster::write(expCtx->opCtx, updateCommand, &stats, &response, targetEpoch);

    if (auto status = response.toStatus(); status != Status::OK()) {
        return status;
    }
    return {{response.getN(), response.getNModified()}};
}

BSONObj ShardServerProcessInterface::preparePipelineAndExplain(
    Pipeline* ownedPipeline, ExplainOptions::Verbosity verbosity) {
    auto firstStage = ownedPipeline->peekFront();
    // We don't want to send an internal stage to the shards.
    if (firstStage &&
        (typeid(*firstStage) == typeid(DocumentSourceMerge) ||
         typeid(*firstStage) == typeid(DocumentSourceMergeCursors) ||
         typeid(*firstStage) == typeid(DocumentSourceCursor))) {
        ownedPipeline->popFront();
    }
    return sharded_agg_helpers::targetShardsForExplain(ownedPipeline);
}

std::unique_ptr<ShardFilterer> ShardServerProcessInterface::getShardFilterer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    auto collectionFilter =
        CollectionShardingState::get(expCtx->opCtx, expCtx->ns)
            ->getOwnershipFilter(
                expCtx->opCtx,
                CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup);
    return std::make_unique<ShardFiltererImpl>(std::move(collectionFilter));
}

void ShardServerProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& destinationNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, destinationNs.db()));
    auto newCmdObj = CommonMongodProcessInterface::_convertRenameToInternalRename(
        opCtx, renameCommandObj, originalCollectionOptions, originalIndexes);
    BSONObjBuilder newCmdWithWriteConcernBuilder(std::move(newCmdObj));
    newCmdWithWriteConcernBuilder.append(WriteConcernOptions::kWriteConcernField,
                                         opCtx->getWriteConcern().toBSON());
    newCmdObj = newCmdWithWriteConcernBuilder.done();
    auto response = executeRawCommandAgainstDatabasePrimary(
        opCtx,
        // internalRenameIfOptionsAndIndexesMatch is adminOnly.
        NamespaceString::kAdminDb,
        cachedDbInfo,
        // Only unsharded collections can be renamed.
        _versionCommandIfAppropriate(newCmdObj, cachedDbInfo, ChunkVersion::UNSHARDED()),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kNoRetry);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << newCmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << newCmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << newCmdObj);
}

BSONObj ShardServerProcessInterface::getCollectionOptions(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db()));
    auto shard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cachedDbInfo.primaryId()));

    const BSONObj filterObj = BSON("name" << nss.coll());
    const BSONObj cmdObj = BSON("listCollections" << 1 << "filter" << filterObj);

    Shard::QueryResponse resultCollections;
    try {
        resultCollections = uassertStatusOK(
            shard->runExhaustiveCursorCommand(opCtx,
                                              ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                              nss.db().toString(),
                                              _versionCommandIfAppropriate(cmdObj, cachedDbInfo),
                                              Milliseconds(-1)));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return BSONObj{};
    }

    if (resultCollections.docs.empty()) {
        return BSONObj{};
    }

    for (const BSONObj& element : resultCollections.docs) {
        // Return first element which matches on name and has options.
        const BSONElement nameElement = element["name"];
        if (!nameElement || nameElement.valueStringDataSafe() != nss.coll()) {
            continue;
        }

        const BSONElement optionsElement = element["options"];
        if (optionsElement) {
            return optionsElement.Obj().getOwned();
        }

        invariant(resultCollections.docs.size() <= 1,
                  str::stream() << "Expected at most one collection with the name " << nss << ": "
                                << resultCollections.docs.size());
    }

    return BSONObj{};
}

std::list<BSONObj> ShardServerProcessInterface::getIndexSpecs(OperationContext* opCtx,
                                                              const NamespaceString& ns,
                                                              bool includeBuildUUIDs) {
    // Note that 'ns' must be an unsharded collection. The indexes for a sharded collection must be
    // read from a shard with a chunk instead of the primary shard.
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, ns.db()));
    auto shard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cachedDbInfo.primaryId()));
    auto cmdObj = BSON("listIndexes" << ns.coll());
    Shard::QueryResponse indexes;
    try {
        indexes = uassertStatusOK(
            shard->runExhaustiveCursorCommand(opCtx,
                                              ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                              ns.db().toString(),
                                              _versionCommandIfAppropriate(cmdObj, cachedDbInfo),
                                              Milliseconds(-1)));
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return std::list<BSONObj>();
    }
    return std::list<BSONObj>(indexes.docs.begin(), indexes.docs.end());
}

void ShardServerProcessInterface::createCollection(OperationContext* opCtx,
                                                   const std::string& dbName,
                                                   const BSONObj& cmdObj) {
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName));
    BSONObjBuilder finalCmdBuilder(cmdObj);
    finalCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());
    BSONObj finalCmdObj = finalCmdBuilder.obj();
    auto response = executeRawCommandAgainstDatabasePrimary(
        opCtx,
        dbName,
        cachedDbInfo,
        _versionCommandIfAppropriate(finalCmdObj, cachedDbInfo),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kIdempotent);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << finalCmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << finalCmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream()
                                   << "write concern failed while running command " << finalCmdObj);
}

void ShardServerProcessInterface::createIndexesOnEmptyCollection(
    OperationContext* opCtx, const NamespaceString& ns, const std::vector<BSONObj>& indexSpecs) {
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, ns.db()));
    BSONObjBuilder newCmdBuilder;
    newCmdBuilder.append("createIndexes", ns.coll());
    newCmdBuilder.append("indexes", indexSpecs);
    newCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                         opCtx->getWriteConcern().toBSON());
    auto cmdObj = newCmdBuilder.done();

    shardVersionRetry(
        opCtx,
        Grid::get(opCtx)->catalogCache(),
        ns,
        "copying index for empty collection {}"_format(ns.ns()),
        [&] {
            auto response = executeRawCommandAgainstDatabasePrimary(
                opCtx,
                ns.db(),
                cachedDbInfo,
                _versionCommandIfAppropriate(cmdObj, cachedDbInfo),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);

            uassertStatusOKWithContext(response.swResponse,
                                       str::stream() << "failed to run command " << cmdObj);
            auto result = response.swResponse.getValue().data;
            uassertStatusOKWithContext(getStatusFromCommandResult(result),
                                       str::stream() << "failed while running command " << cmdObj);
            uassertStatusOKWithContext(
                getWriteConcernStatusFromCommandResult(result),
                str::stream() << "write concern failed while running command " << cmdObj);
        });
}

void ShardServerProcessInterface::dropCollection(OperationContext* opCtx,
                                                 const NamespaceString& ns) {
    // Build and execute the dropCollection command against the primary shard of the given
    // database.
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, ns.db()));
    BSONObjBuilder newCmdBuilder;
    newCmdBuilder.append("drop", ns.coll());
    newCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                         opCtx->getWriteConcern().toBSON());
    auto cmdObj = newCmdBuilder.done();
    auto response = executeRawCommandAgainstDatabasePrimary(
        opCtx,
        ns.db(),
        cachedDbInfo,
        // Only unsharded collections can be dropped.
        _versionCommandIfAppropriate(cmdObj, cachedDbInfo, ChunkVersion::UNSHARDED()),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kIdempotent);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << cmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << cmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream()
                                   << "write concern failed while running command " << cmdObj);
}

std::unique_ptr<Pipeline, PipelineDeleter>
ShardServerProcessInterface::attachCursorSourceToPipeline(Pipeline* ownedPipeline,
                                                          bool allowTargetingShards) {
    return sharded_agg_helpers::attachCursorToPipeline(ownedPipeline, allowTargetingShards);
}

void ShardServerProcessInterface::setExpectedShardVersion(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> chunkVersion) {
    auto& oss = OperationShardingState::get(opCtx);
    if (oss.hasShardVersion(nss)) {
        invariant(oss.getShardVersion(nss) == chunkVersion);
    } else if (_opIsVersioned) {
        oss.initializeClientRoutingVersions(nss, chunkVersion, boost::none);
    }
}

BSONObj ShardServerProcessInterface::_versionCommandIfAppropriate(
    BSONObj cmdObj,
    const CachedDatabaseInfo& cachedDbInfo,
    boost::optional<ChunkVersion> shardVersion) {
    if (!_opIsVersioned) {
        return cmdObj;
    }
    return appendDbVersionIfPresent(
        shardVersion ? appendShardVersion(cmdObj, *shardVersion) : cmdObj, cachedDbInfo);
}

}  // namespace mongo
