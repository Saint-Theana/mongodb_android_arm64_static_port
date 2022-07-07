/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/balancer/balance_stats.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"
#include "mongo/s/request_types/flush_resharding_state_change_gen.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorAfterPreparingToDonate);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeCloning);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeBlockingWrites);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeDecisionPersisted);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeRemovingStateDoc);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeCompletion);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeStartingErrorFlow);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforePersistingStateTransition);
MONGO_FAIL_POINT_DEFINE(pauseBeforeTellDonorToRefresh);
MONGO_FAIL_POINT_DEFINE(pauseBeforeInsertCoordinatorDoc);

const std::string kReshardingCoordinatorActiveIndexName = "ReshardingCoordinatorActiveIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

bool shouldStopAttemptingToCreateIndex(Status status, const CancellationToken& token) {
    return status.isOK() || token.isCanceled();
}

Date_t getCurrentTime() {
    const auto svcCtx = cc().getServiceContext();
    return svcCtx->getFastClockSource()->now();
}

void assertNumDocsModifiedMatchesExpected(const BatchedCommandRequest& request,
                                          const BSONObj& response,
                                          int expected) {
    auto numDocsModified = response.getIntField("n");
    uassert(5030401,
            str::stream() << "Expected to match " << expected << " docs, but only matched "
                          << numDocsModified << " for write request " << request.toString(),
            expected == numDocsModified);
}

void appendShardEntriesToSetBuilder(const ReshardingCoordinatorDocument& coordinatorDoc,
                                    BSONObjBuilder& setBuilder) {
    BSONArrayBuilder donorShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kDonorShardsFieldName));
    for (const auto& donorShard : coordinatorDoc.getDonorShards()) {
        donorShards.append(donorShard.toBSON());
    }
    donorShards.doneFast();

    BSONArrayBuilder recipientShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kRecipientShardsFieldName));
    for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
        recipientShards.append(recipientShard.toBSON());
    }
    recipientShards.doneFast();
}

void unsetInitializingFields(BSONObjBuilder& updateBuilder) {
    BSONObjBuilder unsetBuilder(updateBuilder.subobjStart("$unset"));
    unsetBuilder.append(ReshardingCoordinatorDocument::kPresetReshardedChunksFieldName, "");
    unsetBuilder.append(ReshardingCoordinatorDocument::kZonesFieldName, "");
    unsetBuilder.doneFast();
}

void writeToCoordinatorStateNss(OperationContext* opCtx,
                                const ReshardingCoordinatorDocument& coordinatorDoc,
                                TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kInitializing:
                // Insert the new coordinator document.
                return BatchedCommandRequest::buildInsertOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    std::vector<BSONObj>{coordinatorDoc.toBSON()});
            case CoordinatorStateEnum::kDone:
                // Remove the coordinator document.
                return BatchedCommandRequest::buildDeleteOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    BSON("_id" << coordinatorDoc.getReshardingUUID()),  // query
                    false                                               // multi
                );
            default: {
                // Partially update the coordinator document.
                BSONObjBuilder updateBuilder;
                {
                    BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                    // Always update the state field.
                    setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                                      CoordinatorState_serializer(coordinatorDoc.getState()));

                    if (auto cloneTimestamp = coordinatorDoc.getCloneTimestamp()) {
                        // If the cloneTimestamp exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kCloneTimestampFieldName,
                                          *cloneTimestamp);
                    }

                    if (auto abortReason = coordinatorDoc.getAbortReason()) {
                        // If the abortReason exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kAbortReasonFieldName,
                                          *abortReason);
                    }

                    if (auto approxBytesToCopy = coordinatorDoc.getApproxBytesToCopy()) {
                        // If the approxBytesToCopy exists, include it in the update.
                        setBuilder.append(
                            ReshardingCoordinatorDocument::kApproxBytesToCopyFieldName,
                            *approxBytesToCopy);
                    }

                    if (auto approxDocumentsToCopy = coordinatorDoc.getApproxDocumentsToCopy()) {
                        // If the approxDocumentsToCopy exists, include it in the update.
                        setBuilder.append(
                            ReshardingCoordinatorDocument::kApproxDocumentsToCopyFieldName,
                            *approxDocumentsToCopy);
                    }

                    if (nextState == CoordinatorStateEnum::kPreparingToDonate) {
                        appendShardEntriesToSetBuilder(coordinatorDoc, setBuilder);
                        setBuilder.doneFast();
                        unsetInitializingFields(updateBuilder);
                    }
                }

                return BatchedCommandRequest::buildUpdateOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    BSON("_id" << coordinatorDoc.getReshardingUUID()),
                    updateBuilder.obj(),
                    false,  // upsert
                    false   // multi
                );
            }
        }
    }());

    auto expectedNumModified = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);
    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, NamespaceString::kConfigReshardingOperationsNamespace, request, txnNumber);

    if (expectedNumModified) {
        assertNumDocsModifiedMatchesExpected(request, res, *expectedNumModified);
    }
}

/**
 * Creates reshardingFields.recipientFields for the resharding operation. Note: these should not
 * change once the operation has begun.
 */
TypeCollectionRecipientFields constructRecipientFields(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    std::vector<DonorShardFetchTimestamp> donorShards;

    for (const auto& donor : coordinatorDoc.getDonorShards()) {
        DonorShardFetchTimestamp donorFetchTimestamp(donor.getId());
        donorFetchTimestamp.setMinFetchTimestamp(donor.getMutableState().getMinFetchTimestamp());
        donorShards.push_back(std::move(donorFetchTimestamp));
    }

    TypeCollectionRecipientFields recipientFields(
        std::move(donorShards),
        coordinatorDoc.getSourceUUID(),
        coordinatorDoc.getSourceNss(),
        resharding::gReshardingMinimumOperationDurationMillis.load());

    emplaceCloneTimestampIfExists(recipientFields, coordinatorDoc.getCloneTimestamp());
    emplaceApproxBytesToCopyIfExists(recipientFields,
                                     coordinatorDoc.getReshardingApproxCopySizeStruct());

    return recipientFields;
}

BSONObj createReshardingFieldsUpdateForOriginalNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<OID> newCollectionEpoch,
    boost::optional<Timestamp> newCollectionTimestamp) {
    auto nextState = coordinatorDoc.getState();
    switch (nextState) {
        case CoordinatorStateEnum::kInitializing: {
            // Append 'reshardingFields' to the config.collections entry for the original nss
            TypeCollectionReshardingFields originalEntryReshardingFields(
                coordinatorDoc.getReshardingUUID());
            originalEntryReshardingFields.setState(coordinatorDoc.getState());

            return BSON("$set" << BSON(CollectionType::kReshardingFieldsFieldName
                                       << originalEntryReshardingFields.toBSON()
                                       << CollectionType::kUpdatedAtFieldName
                                       << opCtx->getServiceContext()->getPreciseClockSource()->now()
                                       << CollectionType::kAllowMigrationsFieldName << false));
        }
        case CoordinatorStateEnum::kPreparingToDonate: {
            TypeCollectionDonorFields donorFields(
                coordinatorDoc.getTempReshardingNss(),
                coordinatorDoc.getReshardingKey(),
                extractShardIdsFromParticipantEntries(coordinatorDoc.getRecipientShards()));

            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
                {
                    setBuilder.append(CollectionType::kReshardingFieldsFieldName + "." +
                                          TypeCollectionReshardingFields::kStateFieldName,
                                      CoordinatorState_serializer(nextState));

                    setBuilder.append(CollectionType::kReshardingFieldsFieldName + "." +
                                          TypeCollectionReshardingFields::kDonorFieldsFieldName,
                                      donorFields.toBSON());

                    setBuilder.append(CollectionType::kUpdatedAtFieldName,
                                      opCtx->getServiceContext()->getPreciseClockSource()->now());
                }

                setBuilder.doneFast();
            }

            return updateBuilder.obj();
        }
        case CoordinatorStateEnum::kCommitting: {
            // Update the config.collections entry for the original nss to reflect the new sharded
            // collection. Set 'uuid' to the reshardingUUID, 'key' to the new shard key,
            // 'lastmodEpoch' to newCollectionEpoch, and 'timestamp' to newCollectionTimestamp. Also
            // update the 'state' field and add the 'recipientFields' to the 'reshardingFields'
            // section.
            auto recipientFields = constructRecipientFields(coordinatorDoc);
            BSONObj setFields =
                BSON("uuid" << coordinatorDoc.getReshardingUUID() << "key"
                            << coordinatorDoc.getReshardingKey().toBSON() << "lastmodEpoch"
                            << newCollectionEpoch.get() << "lastmod"
                            << opCtx->getServiceContext()->getPreciseClockSource()->now()
                            << "reshardingFields.state"
                            << CoordinatorState_serializer(coordinatorDoc.getState()).toString()
                            << "reshardingFields.recipientFields" << recipientFields.toBSON());
            if (newCollectionTimestamp.has_value()) {
                setFields = setFields.addFields(BSON("timestamp" << newCollectionTimestamp.get()));
            }

            return BSON("$set" << setFields);
        }
        case mongo::CoordinatorStateEnum::kDone:
            // Remove 'reshardingFields' from the config.collections entry
            return BSON(
                "$unset" << BSON(CollectionType::kReshardingFieldsFieldName
                                 << "" << CollectionType::kAllowMigrationsFieldName << "")
                         << "$set"
                         << BSON(CollectionType::kUpdatedAtFieldName
                                 << opCtx->getServiceContext()->getPreciseClockSource()->now()));
        default: {
            // Update the 'state' field, and 'abortReason' field if it exists, in the
            // 'reshardingFields' section.
            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                setBuilder.append("reshardingFields.state",
                                  CoordinatorState_serializer(nextState).toString());
                setBuilder.append("lastmod",
                                  opCtx->getServiceContext()->getPreciseClockSource()->now());

                if (auto abortReason = coordinatorDoc.getAbortReason()) {
                    // If the abortReason exists, include it in the update.
                    setBuilder.append("reshardingFields.abortReason", *abortReason);

                    auto abortStatus = getStatusFromAbortReason(coordinatorDoc);
                    setBuilder.append("reshardingFields.userCanceled",
                                      abortStatus == ErrorCodes::ReshardCollectionAborted);
                }

                setBuilder.doneFast();

                if (coordinatorDoc.getAbortReason()) {
                    updateBuilder.append("$unset",
                                         BSON(CollectionType::kAllowMigrationsFieldName << ""));
                }
            }

            return updateBuilder.obj();
        }
    }
}

void updateConfigCollectionsForOriginalNss(OperationContext* opCtx,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           boost::optional<OID> newCollectionEpoch,
                                           boost::optional<Timestamp> newCollectionTimestamp,
                                           TxnNumber txnNumber) {
    auto writeOp = createReshardingFieldsUpdateForOriginalNss(
        opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp);

    auto request = BatchedCommandRequest::buildUpdateOp(
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << coordinatorDoc.getSourceNss().ns()),  // query
        writeOp,
        false,  // upsert
        false   // multi
    );

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    assertNumDocsModifiedMatchesExpected(request, res, 1 /* expected */);
}

void writeToConfigCollectionsForTempNss(OperationContext* opCtx,
                                        const ReshardingCoordinatorDocument& coordinatorDoc,
                                        boost::optional<ChunkVersion> chunkVersion,
                                        boost::optional<const BSONObj&> collation,
                                        TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kPreparingToDonate: {
                // Insert new entry for the temporary nss into config.collections
                auto collType = resharding::createTempReshardingCollectionType(
                    opCtx, coordinatorDoc, chunkVersion.get(), collation.get());
                return BatchedCommandRequest::buildInsertOp(
                    CollectionType::ConfigNS, std::vector<BSONObj>{collType.toBSON()});
            }
            case CoordinatorStateEnum::kCloning: {
                // Update the 'state', 'donorShards', 'approxCopySize', and 'cloneTimestamp' fields
                // in the 'reshardingFields.recipient' section

                BSONArrayBuilder donorShardsBuilder;
                for (const auto& donor : coordinatorDoc.getDonorShards()) {
                    DonorShardFetchTimestamp donorShardFetchTimestamp(donor.getId());
                    donorShardFetchTimestamp.setMinFetchTimestamp(
                        donor.getMutableState().getMinFetchTimestamp());
                    donorShardsBuilder.append(donorShardFetchTimestamp.toBSON());
                }

                return BatchedCommandRequest::buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    BSON("$set" << BSON(
                             "reshardingFields.state"
                             << CoordinatorState_serializer(nextState).toString()
                             << "reshardingFields.recipientFields.approxDocumentsToCopy"
                             << coordinatorDoc.getApproxDocumentsToCopy().get()
                             << "reshardingFields.recipientFields.approxBytesToCopy"
                             << coordinatorDoc.getApproxBytesToCopy().get()
                             << "reshardingFields.recipientFields.cloneTimestamp"
                             << coordinatorDoc.getCloneTimestamp().get()
                             << "reshardingFields.recipientFields.donorShards"
                             << donorShardsBuilder.arr() << "lastmod"
                             << opCtx->getServiceContext()->getPreciseClockSource()->now())),
                    false,  // upsert
                    false   // multi
                );
            }
            case CoordinatorStateEnum::kCommitting:
                // Remove the entry for the temporary nss
                return BatchedCommandRequest::buildDeleteOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    false  // multi
                );
            default: {
                // Update the 'state' field, and 'abortReason' field if it exists, in the
                // 'reshardingFields' section.
                BSONObjBuilder updateBuilder;
                {
                    BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                    setBuilder.append("reshardingFields.state",
                                      CoordinatorState_serializer(nextState).toString());
                    setBuilder.append("lastmod",
                                      opCtx->getServiceContext()->getPreciseClockSource()->now());

                    if (auto abortReason = coordinatorDoc.getAbortReason()) {
                        setBuilder.append("reshardingFields.abortReason", *abortReason);

                        auto abortStatus = getStatusFromAbortReason(coordinatorDoc);
                        setBuilder.append("reshardingFields.userCanceled",
                                          abortStatus == ErrorCodes::ReshardCollectionAborted);
                    }
                }

                return BatchedCommandRequest::buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    updateBuilder.obj(),
                    true,  // upsert
                    false  // multi
                );
            }
        }
    }());

    auto expectedNumModified = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    if (expectedNumModified) {
        assertNumDocsModifiedMatchesExpected(request, res, *expectedNumModified);
    }
}

void insertChunkAndTagDocsForTempNss(OperationContext* opCtx,
                                     std::vector<ChunkType> initialChunks,
                                     std::vector<BSONObj> newZones,
                                     TxnNumber txnNumber) {
    // Insert new initial chunk documents for temp nss
    std::vector<BSONObj> initialChunksBSON(initialChunks.size());
    std::transform(initialChunks.begin(),
                   initialChunks.end(),
                   initialChunksBSON.begin(),
                   [](ChunkType chunk) { return chunk.toConfigBSON(); });

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(
        opCtx, ChunkType::ConfigNS, std::move(initialChunksBSON), txnNumber);

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(
        opCtx, TagsType::ConfigNS, newZones, txnNumber);
}

void removeChunkAndTagsDocs(OperationContext* opCtx,
                            const NamespaceString& ns,
                            const boost::optional<UUID>& collUUID,
                            TxnNumber txnNumber) {
    // Remove all chunk documents for the original nss. We do not know how many chunk docs currently
    // exist, so cannot pass a value for expectedNumModified
    const auto chunksQuery = [&]() {
        if (collUUID) {
            return BSON(ChunkType::collectionUUID() << *collUUID);
        } else {
            return BSON(ChunkType::ns(ns.ns()));
        }
    }();

    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        ChunkType::ConfigNS,
        BatchedCommandRequest::buildDeleteOp(ChunkType::ConfigNS,
                                             chunksQuery,
                                             true  // multi
                                             ),
        txnNumber);

    // Remove all tag documents for the original nss. We do not know how many tag docs currently
    // exist, so cannot pass a value for expectedNumModified
    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        TagsType::ConfigNS,
        BatchedCommandRequest::buildDeleteOp(TagsType::ConfigNS,
                                             BSON(ChunkType::ns(ns.ns())),  // query
                                             true                           // multi
                                             ),
        txnNumber);
}

void removeConfigMetadataForTempNss(OperationContext* opCtx,
                                    const ReshardingCoordinatorDocument& coordinatorDoc,
                                    TxnNumber txnNumber) {
    auto delCollEntryRequest = BatchedCommandRequest::buildDeleteOp(
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << coordinatorDoc.getTempReshardingNss().ns()),  // query
        false                                                                               // multi
    );

    (void)ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, delCollEntryRequest, txnNumber);

    boost::optional<UUID> reshardingTempUUID;
    if (serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
            ServerGlobalParams::FeatureCompatibility::Version::kVersion50)) {
        reshardingTempUUID = coordinatorDoc.getReshardingUUID();
    }

    removeChunkAndTagsDocs(
        opCtx, coordinatorDoc.getTempReshardingNss(), reshardingTempUUID, txnNumber);
}

void updateChunkAndTagsDocsForTempNss(OperationContext* opCtx,
                                      const ReshardingCoordinatorDocument& coordinatorDoc,
                                      OID newCollectionEpoch,
                                      boost::optional<Timestamp> newCollectionTimestamp,
                                      TxnNumber txnNumber) {
    // If the collection entry has a timestamp, this means the metadata has been upgraded to the 5.0
    // format in which case chunks are indexed by UUID and do not contain Epochs. Therefore, only
    // the update to config.collections is sufficient.
    if (!newCollectionTimestamp) {
        auto chunksRequest = BatchedCommandRequest::buildUpdateOp(
            ChunkType::ConfigNS,
            BSON(ChunkType::ns(coordinatorDoc.getTempReshardingNss().ns())),  // query
            BSON("$set" << BSON(ChunkType::ns << coordinatorDoc.getSourceNss().ns()
                                              << ChunkType::epoch
                                              << newCollectionEpoch)),  // update
            false,                                                      // upsert
            true                                                        // multi
        );

        auto chunksRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
            opCtx, ChunkType::ConfigNS, chunksRequest, txnNumber);
    }

    auto tagsRequest = BatchedCommandRequest::buildUpdateOp(
        TagsType::ConfigNS,
        BSON(TagsType::ns(coordinatorDoc.getTempReshardingNss().ns())),    // query
        BSON("$set" << BSON("ns" << coordinatorDoc.getSourceNss().ns())),  // update
        false,                                                             // upsert
        true                                                               // multi
    );

    // Update the 'ns' field to be the original collection namespace for all tags documents that
    // currently have 'ns' as the temporary collection namespace
    auto tagsRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, TagsType::ConfigNS, tagsRequest, txnNumber);
}

/**
 * Executes metadata changes in a transaction without bumping the collection version.
 */
void executeMetadataChangesInTxn(
    OperationContext* opCtx,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    ShardingCatalogManager::withTransaction(opCtx,
                                            NamespaceString::kConfigReshardingOperationsNamespace,
                                            [&](OperationContext* opCtx, TxnNumber txnNumber) {
                                                changeMetadataFunc(opCtx, txnNumber);
                                            });
}

BSONObj makeFlushRoutingTableCacheUpdatesCmd(const NamespaceString& nss) {
    auto cmd = _flushRoutingTableCacheUpdatesWithWriteConcern(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.db());
    return CommandHelpers::appendMajorityWriteConcern(cmd.toBSON({})).getOwned();
}

}  // namespace

namespace resharding {
CollectionType createTempReshardingCollectionType(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const ChunkVersion& chunkVersion,
    const BSONObj& collation) {
    CollectionType collType(coordinatorDoc.getTempReshardingNss(),
                            chunkVersion.epoch(),
                            chunkVersion.getTimestamp(),
                            opCtx->getServiceContext()->getPreciseClockSource()->now(),
                            coordinatorDoc.getReshardingUUID());
    collType.setKeyPattern(coordinatorDoc.getReshardingKey());
    collType.setDefaultCollation(collation);
    collType.setUnique(false);

    TypeCollectionReshardingFields tempEntryReshardingFields(coordinatorDoc.getReshardingUUID());
    tempEntryReshardingFields.setState(coordinatorDoc.getState());

    auto recipientFields = constructRecipientFields(coordinatorDoc);
    tempEntryReshardingFields.setRecipientFields(std::move(recipientFields));
    collType.setReshardingFields(std::move(tempEntryReshardingFields));
    collType.setAllowMigrations(false);
    return collType;
}

void writeDecisionPersistedState(OperationContext* opCtx,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 OID newCollectionEpoch,
                                 boost::optional<Timestamp> newCollectionTimestamp) {
    // No need to bump originalNss version because its epoch will be changed.
    executeMetadataChangesInTxn(opCtx, [&](OperationContext* opCtx, TxnNumber txnNumber) {
        // Update the config.reshardingOperations entry
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Remove the config.collections entry for the temporary collection
        writeToConfigCollectionsForTempNss(
            opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);

        // Update the config.collections entry for the original namespace to reflect the new
        // shard key, new epoch, and new UUID
        updateConfigCollectionsForOriginalNss(
            opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp, txnNumber);

        // Remove all chunk and tag documents associated with the original collection, then
        // update the chunk and tag docs currently associated with the temp nss to be associated
        // with the original nss

        boost::optional<UUID> collUUID;
        if (newCollectionTimestamp) {
            collUUID = coordinatorDoc.getSourceUUID();
        }

        removeChunkAndTagsDocs(opCtx, coordinatorDoc.getSourceNss(), collUUID, txnNumber);
        updateChunkAndTagsDocsForTempNss(
            opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp, txnNumber);
    });
}

void insertCoordDocAndChangeOrigCollEntry(OperationContext* opCtx,
                                          const ReshardingCoordinatorDocument& coordinatorDoc) {

    ShardingCatalogManager::get(opCtx)->bumpCollectionVersionAndChangeMetadataInTxn(
        opCtx, coordinatorDoc.getSourceNss(), [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Insert the coordinator document to config.reshardingOperations.
            invariant(coordinatorDoc.getActive());
            try {
                writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);
            } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
                auto extraInfo = ex.extraInfo<DuplicateKeyErrorInfo>();
                if (extraInfo->getKeyPattern().woCompare(BSON("active" << 1)) == 0) {
                    uasserted(ErrorCodes::ReshardCollectionInProgress,
                              str::stream()
                                  << "Only one resharding operation is allowed to be active at a "
                                     "time, aborting resharding op for "
                                  << coordinatorDoc.getSourceNss());
                }

                throw;
            }

            // Update the config.collections entry for the original collection to include
            // 'reshardingFields'
            updateConfigCollectionsForOriginalNss(
                opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);
        });
}

void writeParticipantShardsAndTempCollInfo(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc,
    std::vector<ChunkType> initialChunks,
    std::vector<BSONObj> zones) {
    ShardingCatalogManager::get(opCtx)->bumpCollectionVersionAndChangeMetadataInTxn(
        opCtx,
        updatedCoordinatorDoc.getSourceNss(),
        [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update on-disk state to reflect latest state transition.
            writeToCoordinatorStateNss(opCtx, updatedCoordinatorDoc, txnNumber);
            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);

            // Insert the config.collections entry for the temporary resharding collection. The
            // chunks all have the same epoch, so picking the last chunk here is arbitrary.
            auto chunkVersion = initialChunks.back().getVersion();
            writeToConfigCollectionsForTempNss(
                opCtx, updatedCoordinatorDoc, chunkVersion, CollationSpec::kSimpleSpec, txnNumber);

            insertChunkAndTagDocsForTempNss(opCtx, initialChunks, zones, txnNumber);
        });
}

void writeStateTransitionAndCatalogUpdatesThenBumpShardVersions(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
    // Run updates to config.reshardingOperations and config.collections in a transaction
    auto nextState = coordinatorDoc.getState();

    std::vector<NamespaceString> collNames = {coordinatorDoc.getSourceNss()};
    if (nextState < CoordinatorStateEnum::kCommitting) {
        collNames.emplace_back(coordinatorDoc.getTempReshardingNss());
    }

    ShardingCatalogManager::get(opCtx)->bumpMultipleCollectionVersionsAndChangeMetadataInTxn(
        opCtx, collNames, [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update the config.reshardingOperations entry
            writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

            // Update the config.collections entry for the original collection
            updateConfigCollectionsForOriginalNss(
                opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);

            // Update the config.collections entry for the temporary resharding collection. If we've
            // already successfully committed that the operation will succeed, we've removed the
            // entry for the temporary collection and updated the entry with original namespace to
            // have the new shard key, UUID, and epoch
            if (nextState < CoordinatorStateEnum::kCommitting) {
                writeToConfigCollectionsForTempNss(
                    opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);
            }
        });
}

void removeCoordinatorDocAndReshardingFields(OperationContext* opCtx,
                                             const ReshardingCoordinatorDocument& coordinatorDoc,
                                             boost::optional<Status> abortReason) {
    // If the coordinator needs to abort and isn't in kInitializing, additional collections need to
    // be cleaned up in the final transaction. Otherwise, cleanup for abort and success are the
    // same.
    const bool wasDecisionPersisted =
        coordinatorDoc.getState() == CoordinatorStateEnum::kCommitting;
    invariant((wasDecisionPersisted && !abortReason) || abortReason);

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kDone);
    emplaceTruncatedAbortReasonIfExists(updatedCoordinatorDoc, abortReason);

    ShardingCatalogManager::get(opCtx)->bumpCollectionVersionAndChangeMetadataInTxn(
        opCtx,
        updatedCoordinatorDoc.getSourceNss(),
        [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Remove entry for this resharding operation from config.reshardingOperations
            writeToCoordinatorStateNss(opCtx, updatedCoordinatorDoc, txnNumber);

            // Remove the resharding fields from the config.collections entry
            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);

            // Once the decision has been persisted, the coordinator would have modified the
            // config.chunks and config.collections entry. This means that the UUID of the
            // non-temp collection is now the UUID of what was previously the UUID of the temp
            // collection. So don't try to call remove as it will end up removing the metadata
            // for the real collection.
            if (!wasDecisionPersisted) {
                removeConfigMetadataForTempNss(opCtx, updatedCoordinatorDoc, txnNumber);
            }
        });
}
}  // namespace resharding

ChunkVersion ReshardingCoordinatorExternalState::calculateChunkVersionForInitialChunks(
    OperationContext* opCtx) {
    boost::optional<Timestamp> timestamp;
    if (serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
            ServerGlobalParams::FeatureCompatibility::Version::kVersion50)) {
        const auto now = VectorClock::get(opCtx)->getTime();
        timestamp = now.clusterTime().asTimestamp();
    }

    return ChunkVersion(1, 0, OID::gen(), timestamp);
}

std::vector<DonorShardEntry> constructDonorShardEntries(const std::set<ShardId>& donorShardIds) {
    std::vector<DonorShardEntry> donorShards;
    std::transform(donorShardIds.begin(),
                   donorShardIds.end(),
                   std::back_inserter(donorShards),
                   [](const ShardId& shardId) -> DonorShardEntry {
                       DonorShardContext donorCtx;
                       donorCtx.setState(DonorStateEnum::kUnused);
                       return DonorShardEntry{shardId, std::move(donorCtx)};
                   });
    return donorShards;
}

std::vector<RecipientShardEntry> constructRecipientShardEntries(
    const std::set<ShardId>& recipientShardIds) {
    std::vector<RecipientShardEntry> recipientShards;
    std::transform(recipientShardIds.begin(),
                   recipientShardIds.end(),
                   std::back_inserter(recipientShards),
                   [](const ShardId& shardId) -> RecipientShardEntry {
                       RecipientShardContext recipientCtx;
                       recipientCtx.setState(RecipientStateEnum::kUnused);
                       return RecipientShardEntry{shardId, std::move(recipientCtx)};
                   });
    return recipientShards;
}

ReshardingCoordinatorExternalState::ParticipantShardsAndChunks
ReshardingCoordinatorExternalStateImpl::calculateParticipantShardsAndChunks(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
            opCtx, coordinatorDoc.getSourceNss()));

    std::set<ShardId> donorShardIds;
    cm.getAllShardIds(&donorShardIds);

    std::set<ShardId> recipientShardIds;
    std::vector<ChunkType> initialChunks;

    // The database primary must always be a recipient to ensure it ends up with consistent
    // collection metadata.
    recipientShardIds.emplace(cm.dbPrimary());

    if (const auto& chunks = coordinatorDoc.getPresetReshardedChunks()) {
        auto version = calculateChunkVersionForInitialChunks(opCtx);

        // Use the provided shardIds from presetReshardedChunks to construct the
        // recipient list.
        for (const auto& reshardedChunk : *chunks) {
            recipientShardIds.emplace(reshardedChunk.getRecipientShardId());

            if (version.getTimestamp()) {
                initialChunks.emplace_back(
                    coordinatorDoc.getReshardingUUID(),
                    ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                    version,
                    reshardedChunk.getRecipientShardId());
            } else {
                initialChunks.emplace_back(
                    coordinatorDoc.getTempReshardingNss(),
                    ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                    version,
                    reshardedChunk.getRecipientShardId());
            }
            version.incMinor();
        }
    } else {
        int numInitialChunks = coordinatorDoc.getNumInitialChunks()
            ? *coordinatorDoc.getNumInitialChunks()
            : cm.numChunks();

        ShardKeyPattern shardKey(coordinatorDoc.getReshardingKey());
        const auto tempNs = coordinatorDoc.getTempReshardingNss();

        boost::optional<std::vector<mongo::TagsType>> parsedZones;
        auto rawBSONZones = coordinatorDoc.getZones();
        if (rawBSONZones && rawBSONZones->size() != 0) {
            parsedZones.emplace();
            parsedZones->reserve(rawBSONZones->size());

            for (const auto& zone : *rawBSONZones) {
                ChunkRange range(zone.getMin(), zone.getMax());
                TagsType tag(
                    coordinatorDoc.getTempReshardingNss(), zone.getZone().toString(), range);

                parsedZones->push_back(tag);
            }
        }

        auto initialSplitter = ReshardingSplitPolicy::make(opCtx,
                                                           coordinatorDoc.getSourceNss(),
                                                           tempNs,
                                                           shardKey,
                                                           numInitialChunks,
                                                           std::move(parsedZones));

        // Note: The resharding initial split policy doesn't care about what is the real primary
        // shard, so just pass in a random shard.
        const SplitPolicyParams splitParams{
            tempNs,
            coordinatorDoc.getReshardingUUID(),
            *donorShardIds.begin(),
            ChunkEntryFormat::getForVersionCallerGuaranteesFCVStability(
                ServerGlobalParams::FeatureCompatibility::Version::kVersion50)};
        auto splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
        initialChunks = std::move(splitResult.chunks);

        for (const auto& chunk : initialChunks) {
            recipientShardIds.insert(chunk.getShard());
        }
    }

    return {constructDonorShardEntries(donorShardIds),
            constructRecipientShardEntries(recipientShardIds),
            initialChunks};
}

void ReshardingCoordinatorExternalStateImpl::sendCommandToShards(
    OperationContext* opCtx,
    StringData dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    sharding_util::sendCommandToShards(opCtx, dbName, command, shardIds, executor);
}

ThreadPool::Limits ReshardingCoordinatorService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimit;
    threadPoolLimit.maxThreads = resharding::gReshardingCoordinatorServiceMaxThreadCount;
    return threadPoolLimit;
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingCoordinatorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<ReshardingCoordinator>(
        this,
        ReshardingCoordinatorDocument::parse(IDLParserErrorContext("ReshardingCoordinatorStateDoc"),
                                             std::move(initialState)),
        std::make_shared<ReshardingCoordinatorExternalStateImpl>());
}

ExecutorFuture<void> ReshardingCoordinatorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);
               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("active" << 1) << "name"
                                                 << kReshardingCoordinatorActiveIndexName
                                                 << "unique" << true))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([token](Status status) { return shouldStopAttemptingToCreateIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

void ReshardingCoordinatorService::abortAllReshardCollection(OperationContext* opCtx) {
    std::vector<SharedSemiFuture<void>> reshardingCoordinatorFutures;

    for (auto& instance : getAllInstances(opCtx)) {
        auto reshardingCoordinator =
            checked_pointer_cast<ReshardingCoordinatorService::ReshardingCoordinator>(instance);
        reshardingCoordinatorFutures.push_back(reshardingCoordinator->getCompletionFuture());
        reshardingCoordinator->abort();
    }

    for (auto&& future : reshardingCoordinatorFutures) {
        future.wait(opCtx);
    }
}

ReshardingCoordinatorService::ReshardingCoordinator::ReshardingCoordinator(
    const ReshardingCoordinatorService* coordinatorService,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    std::shared_ptr<ReshardingCoordinatorExternalState> externalState)
    : PrimaryOnlyService::TypedInstance<ReshardingCoordinator>(),
      _id(coordinatorDoc.getReshardingUUID().toBSON()),
      _coordinatorService(coordinatorService),
      _metadata(coordinatorDoc.getCommonReshardingMetadata()),
      _coordinatorDoc(coordinatorDoc),
      _markKilledExecutor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "ReshardingCoordinatorCancelableOpCtxPool";
          options.minThreads = 1;
          options.maxThreads = 1;
          return options;
      }())),
      _reshardingCoordinatorExternalState(externalState) {
    _reshardingCoordinatorObserver = std::make_shared<ReshardingCoordinatorObserver>();

    // If the coordinator is recovering from step-up, make sure to properly initialize the
    // promises to reflect the latest state of this resharding operation.
    if (coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        _reshardingCoordinatorObserver->onReshardingParticipantTransition(coordinatorDoc);
    }
}

void ReshardingCoordinatorService::ReshardingCoordinator::installCoordinatorDoc(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& doc) noexcept {
    invariant(doc.getReshardingUUID() == _coordinatorDoc.getReshardingUUID());

    BSONObjBuilder bob;
    bob.append("newState", CoordinatorState_serializer(doc.getState()));
    bob.append("oldState", CoordinatorState_serializer(_coordinatorDoc.getState()));
    bob.append("namespace", doc.getSourceNss().toString());
    bob.append("collectionUUID", doc.getSourceUUID().toString());
    bob.append("reshardingUUID", doc.getReshardingUUID().toString());
    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "resharding.coordinator.transition",
                                           doc.getSourceNss().toString(),
                                           bob.obj(),
                                           ShardingCatalogClient::kMajorityWriteConcern);

    LOGV2_INFO(5343001,
               "Transitioned resharding coordinator state",
               "newState"_attr = CoordinatorState_serializer(doc.getState()),
               "oldState"_attr = CoordinatorState_serializer(_coordinatorDoc.getState()),
               "namespace"_attr = doc.getSourceNss(),
               "collectionUUID"_attr = doc.getSourceUUID(),
               "reshardingUUID"_attr = doc.getReshardingUUID());

    _coordinatorDoc = doc;
}

void markCompleted(const Status& status) {
    auto metrics = ReshardingMetrics::get(cc().getServiceContext());
    auto metricsOperationStatus = [&] {
        if (status.isOK()) {
            return ReshardingOperationStatusEnum::kSuccess;
        } else if (status == ErrorCodes::ReshardCollectionAborted) {
            return ReshardingOperationStatusEnum::kCanceled;
        } else {
            return ReshardingOperationStatusEnum::kFailure;
        }
    }();

    metrics->onCompletion(
        ReshardingMetrics::Role::kCoordinator, metricsOperationStatus, getCurrentTime());
}

BSONObj createFlushReshardingStateChangeCommand(const NamespaceString& nss,
                                                const UUID& reshardingUUID) {
    _flushReshardingStateChange cmd(nss);
    cmd.setDbName(NamespaceString::kAdminDb);
    cmd.setReshardingUUID(reshardingUUID);
    return cmd.toBSON(
        BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));
}

BSONObj createShardsvrCommitReshardCollectionCmd(const NamespaceString& nss,
                                                 const UUID& reshardingUUID) {
    ShardsvrCommitReshardCollection cmd(nss);
    cmd.setDbName(NamespaceString::kAdminDb);
    cmd.setReshardingUUID(reshardingUUID);
    return cmd.toBSON(
        BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_tellAllParticipantsReshardingStarted(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this] {
                       // Ensure the flushes to create participant state machines don't get
                       // interrupted upon abort.
                       _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(),
                                                       _markKilledExecutor);
                   })
                   .then([this, executor] {
                       pauseBeforeTellDonorToRefresh.pauseWhileSet();
                       _establishAllDonorsAsParticipants(executor);
                   })
                   .then([this, executor] { _establishAllRecipientsAsParticipants(executor); })
                   .onCompletion([this](Status status) {
                       // Swap back to using operation contexts canceled upon abort until ready to
                       // persist the decision or unrecoverable error.
                       _cancelableOpCtxFactory.emplace(_ctHolder->getAbortToken(),
                                                       _markKilledExecutor);

                       return status;
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093702,
                  "Resharding coordinator encountered transient error while telling participants "
                  "to refresh",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken());
}

ExecutorFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::_initializeCoordinator(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor] { _insertCoordDocAndChangeOrigCollEntry(); })
                   .then([this, executor] { _calculateParticipantsAndChunksThenWriteToDisk(); });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093703,
                  "Resharding coordinator encountered transient error while initializing",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this, executor](Status status) {
            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<void>(**executor, status);
            }

            if (_coordinatorDoc.getState() < CoordinatorStateEnum::kPreparingToDonate) {
                return ExecutorFuture<void>(**executor, status);
            }

            // Regardless of error or non-error, guarantee that once the coordinator
            // completes its transition to kPreparingToDonate, participants are aware of
            // the resharding operation and their state machines are created.
            return _tellAllParticipantsReshardingStarted(executor);
        })
        .onError([this, executor](Status status) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<void>(**executor, status);
            }

            if (_ctHolder->isAborted()) {
                // If the abort cancellation token was triggered, implying that a user ran the abort
                // command, override status with a resharding abort error.
                //
                // Note for debugging purposes: Ensure the original error status is recorded in the
                // logs before replacing it.
                status = {ErrorCodes::ReshardCollectionAborted, "aborted"};
            }

            auto nss = _coordinatorDoc.getSourceNss();
            LOGV2(4956903,
                  "Resharding failed",
                  "namespace"_attr = nss.ns(),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            // Allow abort to continue except when stepped down.
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);

            if (_coordinatorDoc.getState() < CoordinatorStateEnum::kPreparingToDonate) {
                return _onAbortCoordinatorOnly(executor, status);
            } else {
                return _onAbortCoordinatorAndParticipants(executor, status);
            }
        });
}

ExecutorFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorService::ReshardingCoordinator::_runUntilReadyToCommit(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) noexcept {
    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kCloning) {
                           _tellAllRecipientsToRefresh(executor);
                       }
                   })
                   .then([this, executor] { return _awaitAllRecipientsFinishedCloning(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kApplying) {
                           _tellAllDonorsToRefresh(executor);
                       }
                   })
                   .then([this, executor] { return _awaitAllRecipientsFinishedApplying(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kBlockingWrites) {
                           _tellAllDonorsToRefresh(executor);
                       }
                   })
                   .then([this, executor] {
                       return _awaitAllRecipientsInStrictConsistency(executor);
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093704,
                  "Resharding coordinator encountered transient error",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<StatusWith<ReshardingCoordinatorDocument>>(
            [](const StatusWith<ReshardingCoordinatorDocument>& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this](auto passthroughFuture) {
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);
            return passthroughFuture;
        })
        .onError([this, executor](Status status) -> ExecutorFuture<ReshardingCoordinatorDocument> {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<ReshardingCoordinatorDocument>(**executor, status);
            }

            if (_ctHolder->isAborted()) {
                // If the abort cancellation token was triggered, implying that a user ran the abort
                // command, override status with a resharding abort error.
                status = {ErrorCodes::ReshardCollectionAborted, "aborted"};
            }

            auto nss = _coordinatorDoc.getSourceNss();
            LOGV2(4956902,
                  "Resharding failed",
                  "namespace"_attr = nss.ns(),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            invariant(_coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate);

            return _onAbortCoordinatorAndParticipants(executor, status)
                .onCompletion([](Status status) {
                    return StatusWith<ReshardingCoordinatorDocument>(status);
                });
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_commitAndFinishReshardOperation(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc) noexcept {
    return resharding::WithAutomaticRetry([this, executor, updatedCoordinatorDoc] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor, updatedCoordinatorDoc] {
                       return _commit(updatedCoordinatorDoc);
                   })
                   .then([this, executor] {
                       _tellAllParticipantsToCommit(_coordinatorDoc.getSourceNss(), executor);
                   })
                   .then([this] { _updateChunkImbalanceMetrics(_coordinatorDoc.getSourceNss()); })
                   .then([this, executor] { return _awaitAllParticipantShardsDone(executor); })
                   .then([this, self = shared_from_this(), executor] {
                       // Best-effort attempt to trigger a refresh on the participant shards so
                       // they see the collection metadata without reshardingFields and no longer
                       // throw ReshardCollectionInProgress. There is no guarantee this logic ever
                       // runs if the config server primary steps down after having removed the
                       // coordinator state document.
                       return _tellAllRecipientsToRefresh(executor);
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093705,
                  "Resharding coordinator encountered transient error while committing",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        .onError([this, executor](Status status) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return status;
            }

            LOGV2_FATAL(5277000,
                        "Unrecoverable error past the point resharding was guaranteed to succeed",
                        "error"_attr = redact(status));
        });
}

SemiFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    _ctHolder = std::make_unique<CoordinatorCancellationTokenHolder>(stepdownToken);
    _markKilledExecutor->startup();
    _cancelableOpCtxFactory.emplace(_ctHolder->getAbortToken(), _markKilledExecutor);

    return _initializeCoordinator(executor)
        .then([this, executor] { return _runUntilReadyToCommit(executor); })
        .then([this, executor](const ReshardingCoordinatorDocument& updatedCoordinatorDoc) {
            return _commitAndFinishReshardOperation(executor, updatedCoordinatorDoc);
        })
        .onCompletion([this, executor](Status status) {
            if (!_ctHolder->isSteppingOrShuttingDown() &&
                _coordinatorDoc.getState() != CoordinatorStateEnum::kUnused) {
                // Notify `ReshardingMetrics` as the operation is now complete for external
                // observers.
                markCompleted(status);
            }

            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            reshardingPauseCoordinatorBeforeCompletion.pauseWhileSetAndNotCanceled(
                opCtx.get(), _ctHolder->getStepdownToken());

            {
                auto lg = stdx::lock_guard(_fulfillmentMutex);
                if (status.isOK()) {
                    _completionPromise.emplaceValue();

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.emplaceValue();
                    }
                } else {
                    _completionPromise.setError(status);

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.setError(status);
                    }
                }
            }

            if (_criticalSectionTimeoutCbHandle) {
                (*executor)->cancel(*_criticalSectionTimeoutCbHandle);
            }

            return status;
        })
        .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this()](Status status) {
            // On stepdown or shutdown, the _scopedExecutor may have already been shut down.
            // Schedule cleanup work on the parent executor.
            if (_ctHolder->isSteppingOrShuttingDown()) {
                ReshardingMetrics::get(cc().getServiceContext())
                    ->onStepDown(ReshardingMetrics::Role::kCoordinator);
            }

            if (!status.isOK()) {
                {
                    auto lg = stdx::lock_guard(_fulfillmentMutex);
                    if (!_completionPromise.getFuture().isReady()) {
                        _completionPromise.setError(status);
                    }

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.setError(status);
                    }
                }
                _reshardingCoordinatorObserver->interrupt(status);
            }
        })
        .semi();
}

ExecutorFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::_onAbortCoordinatorOnly(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const Status& status) {
    if (_coordinatorDoc.getState() == CoordinatorStateEnum::kUnused) {
        // No work to be done.
        return ExecutorFuture<void>(**executor, status);
    }

    return resharding::WithAutomaticRetry([this, executor, status] {
               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

               // The temporary collection and its corresponding entries were never created. Only
               // the coordinator document and reshardingFields require cleanup.
               resharding::removeCoordinatorDocAndReshardingFields(
                   opCtx.get(), _coordinatorDoc, status);
               return status;
           })
        .onTransientError([](const Status& retryStatus) {
            LOGV2(5093706,
                  "Resharding coordinator encountered transient error while aborting",
                  "error"_attr = retryStatus);
        })
        .onUnrecoverableError([](const Status& retryStatus) {})
        .until<Status>([](const Status& retryStatus) { return retryStatus.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        // Return back original status.
        .then([status] { return status; });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_onAbortCoordinatorAndParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const Status& status) {
    // Participants should never be waited upon to complete the abort if they were never made aware
    // of the resharding operation (the coordinator flushing its state change to
    // kPreparingToDonate).
    invariant(_coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate);

    return resharding::WithAutomaticRetry([this, executor, status] {
               if (_coordinatorDoc.getState() != CoordinatorStateEnum::kAborting) {
                   // The coordinator only transitions into kAborting if there are participants to
                   // wait on before transitioning to kDone.
                   _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kAborting,
                                                               _coordinatorDoc,
                                                               boost::none,
                                                               boost::none,
                                                               status);
               }

               _tellAllParticipantsToAbort(executor,
                                           status == ErrorCodes::ReshardCollectionAborted);

               // Wait for all participants to acknowledge the operation reached an unrecoverable
               // error.
               return future_util::withCancellation(_awaitAllParticipantShardsDone(executor),
                                                    _ctHolder->getStepdownToken());
           })
        .onTransientError([](const Status& retryStatus) {
            LOGV2(5093707,
                  "Resharding coordinator encountered transient error while aborting all "
                  "participants",
                  "error"_attr = retryStatus);
        })
        .onUnrecoverableError([](const Status& retryStatus) {})
        .until<Status>([](const Status& retryStatus) { return retryStatus.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        // Return back the original status.
        .then([status] { return status; });
}

void ReshardingCoordinatorService::ReshardingCoordinator::abort() {
    _ctHolder->abort();
}

boost::optional<BSONObj> ReshardingCoordinatorService::ReshardingCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode,
    MongoProcessInterface::CurrentOpSessionsMode) noexcept {
    ReshardingMetrics::ReporterOptions options(ReshardingMetrics::Role::kCoordinator,
                                               _coordinatorDoc.getReshardingUUID(),
                                               _coordinatorDoc.getSourceNss(),
                                               _coordinatorDoc.getReshardingKey().toBSON(),
                                               false);
    return ReshardingMetrics::get(cc().getServiceContext())->reportForCurrentOp(options);
}

std::shared_ptr<ReshardingCoordinatorObserver>
ReshardingCoordinatorService::ReshardingCoordinator::getObserver() {
    return _reshardingCoordinatorObserver;
}

void ReshardingCoordinatorService::ReshardingCoordinator::onOkayToEnterCritical() {
    auto lg = stdx::lock_guard(_fulfillmentMutex);
    if (_canEnterCritical.getFuture().isReady())
        return;
    LOGV2(5391601, "Marking resharding operation okay to enter critical section");
    _canEnterCritical.emplaceValue();
}

void ReshardingCoordinatorService::ReshardingCoordinator::_insertCoordDocAndChangeOrigCollEntry() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kUnused) {
        if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
            _coordinatorDocWrittenPromise.emplaceValue();
            ReshardingMetrics::get(cc().getServiceContext())
                ->onStepUp(ReshardingMetrics::Role::kCoordinator);
        }

        return;
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kInitializing);
    resharding::insertCoordDocAndChangeOrigCollEntry(opCtx.get(), updatedCoordinatorDoc);
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    {
        // Note: don't put blocking or interruptible code in this block.
        _coordinatorDocWrittenPromise.emplaceValue();

        // TODO SERVER-53914 to accommodate loading metrics for the coordinator.
        ReshardingMetrics::get(cc().getServiceContext())
            ->onStart(ReshardingMetrics::Role::kCoordinator, getCurrentTime());
    }

    pauseBeforeInsertCoordinatorDoc.pauseWhileSet();
}

void ReshardingCoordinatorService::ReshardingCoordinator::
    _calculateParticipantsAndChunksThenWriteToDisk() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return;
    }
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;

    auto shardsAndChunks = _reshardingCoordinatorExternalState->calculateParticipantShardsAndChunks(
        opCtx.get(), updatedCoordinatorDoc);

    updatedCoordinatorDoc.setDonorShards(std::move(shardsAndChunks.donorShards));
    updatedCoordinatorDoc.setRecipientShards(std::move(shardsAndChunks.recipientShards));
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    // Remove the presetReshardedChunks and zones from the coordinator document to reduce
    // the possibility of the document reaching the BSONObj size constraint.
    std::vector<BSONObj> zones;
    if (updatedCoordinatorDoc.getZones()) {
        zones = buildTagsDocsFromZones(updatedCoordinatorDoc.getTempReshardingNss(),
                                       *updatedCoordinatorDoc.getZones());
    }
    updatedCoordinatorDoc.setPresetReshardedChunks(boost::none);
    updatedCoordinatorDoc.setZones(boost::none);

    resharding::writeParticipantShardsAndTempCollInfo(opCtx.get(),
                                                      updatedCoordinatorDoc,
                                                      std::move(shardsAndChunks.initialChunks),
                                                      std::move(zones));
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    reshardingPauseCoordinatorAfterPreparingToDonate.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());
}

ReshardingApproxCopySize computeApproxCopySize(ReshardingCoordinatorDocument& coordinatorDoc) {
    const auto numRecipients = coordinatorDoc.getRecipientShards().size();
    iassert(ErrorCodes::BadValue,
            "Expected to find at least one recipient in the coordinator document",
            numRecipients > 0);

    // Compute the aggregate for the number of documents and bytes to copy.
    long aggBytesToCopy = 0, aggDocumentsToCopy = 0;
    for (auto donor : coordinatorDoc.getDonorShards()) {
        if (const auto bytesToClone = donor.getMutableState().getBytesToClone()) {
            aggBytesToCopy += *bytesToClone;
        }

        if (const auto documentsToClone = donor.getMutableState().getDocumentsToClone()) {
            aggDocumentsToCopy += *documentsToClone;
        }
    }

    // Calculate the approximate number of documents and bytes that each recipient will clone.
    ReshardingApproxCopySize approxCopySize;
    approxCopySize.setApproxBytesToCopy(aggBytesToCopy / numRecipients);
    approxCopySize.setApproxDocumentsToCopy(aggDocumentsToCopy / numRecipients);
    return approxCopySize;
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllDonorsReadyToDonate(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeCloning.pauseWhileSetAndNotCanceled(
                    opCtx.get(), _ctHolder->getAbortToken());
            }

            auto highestMinFetchTimestamp =
                getHighestMinFetchTimestamp(coordinatorDocChangedOnDisk.getDonorShards());
            _updateCoordinatorDocStateAndCatalogEntries(
                CoordinatorStateEnum::kCloning,
                coordinatorDocChangedOnDisk,
                highestMinFetchTimestamp,
                computeApproxCopySize(coordinatorDocChangedOnDisk));
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsFinishedCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsFinishedCloning(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kApplying,
                                                              coordinatorDocChangedOnDisk);
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::_startCommitMonitor(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    _ctHolder->getAbortToken().onCancel().thenRunOn(**executor).getAsync([this](Status status) {
        if (status.isOK())
            _commitMonitorCancellationSource.cancel();
    });

    auto commitMonitor = std::make_shared<resharding::CoordinatorCommitMonitor>(
        _coordinatorDoc.getSourceNss(),
        extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards()),
        **executor,
        _commitMonitorCancellationSource.token());

    commitMonitor->waitUntilRecipientsAreWithinCommitThreshold()
        .thenRunOn(**executor)
        .getAsync([this](Status) { onOkayToEnterCritical(); });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsFinishedApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kApplying) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return ExecutorFuture<void>(**executor)
        .then([this, executor] {
            _startCommitMonitor(executor);

            LOGV2(5391602, "Resharding operation waiting for an okay to enter critical section");
            return _canEnterCritical.getFuture().thenRunOn(**executor).then([this] {
                _commitMonitorCancellationSource.cancel();
                LOGV2(5391603, "Resharding operation is okay to enter critical section");
            });
        })
        .then([this, executor] {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeBlockingWrites.pauseWhileSetAndNotCanceled(
                    opCtx.get(), _ctHolder->getAbortToken());
            }

            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kBlockingWrites,
                                                              _coordinatorDoc);
            const auto criticalSectionTimeout =
                Milliseconds(resharding::gReshardingCriticalSectionTimeoutMillis.load());
            const auto criticalSectionExpiresAt = (*executor)->now() + criticalSectionTimeout;
            LOGV2_INFO(
                5573001, "Engaging critical section", "timeoutAt"_attr = criticalSectionExpiresAt);

            auto swCbHandle = (*executor)->scheduleWorkAt(
                criticalSectionExpiresAt,
                [this](const executor::TaskExecutor::CallbackArgs& cbData) {
                    if (!cbData.status.isOK()) {
                        return;
                    }
                    _reshardingCoordinatorObserver->onCriticalSectionTimeout();
                });

            if (!swCbHandle.isOK()) {
                _reshardingCoordinatorObserver->interrupt(swCbHandle.getStatus());
            }

            _criticalSectionTimeoutCbHandle = swCbHandle.getValue();
        });
}

ExecutorFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kBlockingWrites) {
        // If in recovery, just return the existing _stateDoc.
        return ExecutorFuture<ReshardingCoordinatorDocument>(**executor, _coordinatorDoc);
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor);
}

Future<void> ReshardingCoordinatorService::ReshardingCoordinator::_commit(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kBlockingWrites) {
        return Status::OK();
    }

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitting);

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    reshardingPauseCoordinatorBeforeDecisionPersisted.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());

    // The new epoch and timestamp to use for the resharded collection to indicate that the
    // collection is a new incarnation of the namespace
    auto newCollectionEpoch = OID::gen();
    boost::optional<Timestamp> newCollectionTimestamp;
    if (serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
            ServerGlobalParams::FeatureCompatibility::Version::kVersion50)) {
        auto now = VectorClock::get(opCtx.get())->getTime();
        newCollectionTimestamp = now.clusterTime().asTimestamp();
    }

    resharding::writeDecisionPersistedState(
        opCtx.get(), updatedCoordinatorDoc, newCollectionEpoch, newCollectionTimestamp);

    // Update the in memory state
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    return Status::OK();
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllParticipantShardsDone(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    std::vector<ExecutorFuture<ReshardingCoordinatorDocument>> futures;
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllRecipientsDone().thenRunOn(**executor));
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllDonorsDone().thenRunOn(**executor));

    // We only allow the stepdown token to cancel operations after progressing past
    // kCommitting.
    return future_util::withCancellation(whenAllSucceed(std::move(futures)),
                                         _ctHolder->getStepdownToken())
        .thenRunOn(**executor)
        .then([this, executor](const auto& coordinatorDocsChangedOnDisk) {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            auto& coordinatorDoc = coordinatorDocsChangedOnDisk[1];

            reshardingPauseCoordinatorBeforeRemovingStateDoc.pauseWhileSetAndNotCanceled(
                opCtx.get(), _ctHolder->getStepdownToken());

            boost::optional<Status> abortReason;
            if (coordinatorDoc.getAbortReason()) {
                abortReason = getStatusFromAbortReason(coordinatorDoc);
            }

            resharding::removeCoordinatorDocAndReshardingFields(
                opCtx.get(), coordinatorDoc, abortReason);
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::
    _updateCoordinatorDocStateAndCatalogEntries(
        CoordinatorStateEnum nextState,
        ReshardingCoordinatorDocument coordinatorDoc,
        boost::optional<Timestamp> cloneTimestamp,
        boost::optional<ReshardingApproxCopySize> approxCopySize,
        boost::optional<Status> abortReason) {
    // Build new state doc for coordinator state update
    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(nextState);
    emplaceApproxBytesToCopyIfExists(updatedCoordinatorDoc, std::move(approxCopySize));
    emplaceCloneTimestampIfExists(updatedCoordinatorDoc, std::move(cloneTimestamp));
    emplaceTruncatedAbortReasonIfExists(updatedCoordinatorDoc, abortReason);

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    resharding::writeStateTransitionAndCatalogUpdatesThenBumpShardVersions(opCtx.get(),
                                                                           updatedCoordinatorDoc);

    // Update in-memory coordinator doc
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_sendCommandToAllParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const BSONObj& command) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto donorShardIds = extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());
    auto recipientShardIds =
        extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards());
    std::set<ShardId> participantShardIds{donorShardIds.begin(), donorShardIds.end()};
    participantShardIds.insert(recipientShardIds.begin(), recipientShardIds.end());

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(),
        NamespaceString::kAdminDb,
        command,
        {participantShardIds.begin(), participantShardIds.end()},
        **executor);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_sendCommandToAllRecipients(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const BSONObj& command) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto recipientShardIds =
        extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards());

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(),
        NamespaceString::kAdminDb,
        command,
        {recipientShardIds.begin(), recipientShardIds.end()},
        **executor);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_sendCommandToAllDonors(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const BSONObj& command) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto donorShardIds = extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(),
        NamespaceString::kAdminDb,
        command,
        {donorShardIds.begin(), donorShardIds.end()},
        **executor);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_establishAllDonorsAsParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kPreparingToDonate);
    auto flushCmd = makeFlushRoutingTableCacheUpdatesCmd(_coordinatorDoc.getSourceNss());
    _sendCommandToAllDonors(executor, flushCmd);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_establishAllRecipientsAsParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kPreparingToDonate);
    auto flushCmd = makeFlushRoutingTableCacheUpdatesCmd(_coordinatorDoc.getTempReshardingNss());
    _sendCommandToAllRecipients(executor, flushCmd);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllRecipientsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    NamespaceString nssToRefresh;
    // Refresh the temporary namespace if the coordinator is in a state prior to 'kCommitting'.
    // A refresh of recipients while in 'kCommitting' should be accompanied by a refresh of
    // all participants for the original namespace to ensure correctness.
    if (_coordinatorDoc.getState() < CoordinatorStateEnum::kCommitting) {
        nssToRefresh = _coordinatorDoc.getTempReshardingNss();
    } else {
        nssToRefresh = _coordinatorDoc.getSourceNss();
    }

    auto refreshCmd =
        createFlushReshardingStateChangeCommand(nssToRefresh, _coordinatorDoc.getReshardingUUID());
    _sendCommandToAllRecipients(executor, refreshCmd);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllDonorsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto refreshCmd = createFlushReshardingStateChangeCommand(_coordinatorDoc.getSourceNss(),
                                                              _coordinatorDoc.getReshardingUUID());
    _sendCommandToAllDonors(executor, refreshCmd);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllParticipantsToCommit(
    const NamespaceString& nss, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto commitCmd =
        createShardsvrCommitReshardCollectionCmd(nss, _coordinatorDoc.getReshardingUUID());
    _sendCommandToAllParticipants(executor, commitCmd);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllParticipantsToAbort(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, bool isUserAborted) {
    ShardsvrAbortReshardCollection abortCmd(_coordinatorDoc.getReshardingUUID(), isUserAborted);
    abortCmd.setDbName("admin");
    _sendCommandToAllParticipants(executor,
                                  abortCmd.toBSON(BSON(WriteConcernOptions::kWriteConcernField
                                                       << WriteConcernOptions::Majority)));
}

void ReshardingCoordinatorService::ReshardingCoordinator::_updateChunkImbalanceMetrics(
    const NamespaceString& nss) {
    auto cancellableOpCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto opCtx = cancellableOpCtx.get();

    try {
        auto routingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss));

        const auto collectionZones =
            uassertStatusOK(Grid::get(opCtx)->catalogClient()->getTagsForCollection(opCtx, nss));

        const auto& keyPattern = routingInfo.getShardKeyPattern().getKeyPattern();

        ZoneInfo zoneInfo;
        for (const auto& tag : collectionZones) {
            uassertStatusOK(zoneInfo.addRangeToZone(
                ZoneRange(keyPattern.extendRangeBound(tag.getMinKey(), false),
                          keyPattern.extendRangeBound(tag.getMaxKey(), false),
                          tag.getTag())));
        }

        const auto allShardsWithOpTime =
            uassertStatusOK(Grid::get(opCtx)->catalogClient()->getAllShards(
                opCtx, repl::ReadConcernLevel::kLocalReadConcern));

        auto imbalanceCount =
            getMaxChunkImbalanceCount(routingInfo, allShardsWithOpTime.value, zoneInfo);

        ReshardingMetrics::get(opCtx->getServiceContext())
            ->setLastReshardChunkImbalanceCount(imbalanceCount);
    } catch (const DBException& ex) {
        LOGV2_WARNING(5543000,
                      "Encountered error while trying to update resharding chunk imbalance metrics",
                      "namespace"_attr = nss,
                      "error"_attr = redact(ex.toStatus()));
    }
}

}  // namespace mongo
