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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(reshardCollectionJoinedExistingOperation);

namespace {

// Returns a resharding instance if one exists for the same collection and shard key already.
boost::optional<std::shared_ptr<ReshardingCoordinatorService::ReshardingCoordinator>>
getExistingInstanceToJoin(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& newShardKey) {
    auto reshardingCoordinatorService = checked_cast<ReshardingCoordinatorService*>(
        repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
            ->lookupServiceByName(ReshardingCoordinatorService::kServiceName));
    auto instances = reshardingCoordinatorService->getAllReshardingInstances(opCtx);
    for (auto& instance : instances) {
        auto reshardingCoordinator =
            checked_pointer_cast<ReshardingCoordinatorService::ReshardingCoordinator>(instance);

        auto instanceMetadata = reshardingCoordinator->getMetadata();
        if (SimpleBSONObjComparator::kInstance.evaluate(
                instanceMetadata.getReshardingKey().toBSON() == newShardKey) &&
            instanceMetadata.getSourceNss() == nss) {
            return reshardingCoordinator;
        };
    }

    return boost::none;
}

class ConfigsvrReshardCollectionCommand final
    : public TypedCommand<ConfigsvrReshardCollectionCommand> {
public:
    using Request = ConfigsvrReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrReshardCollection can only be run on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
            uassert(ErrorCodes::InvalidOptions,
                    "_configsvrReshardCollection must be called with majority writeConcern",
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const NamespaceString& nss = ns();

            uassert(ErrorCodes::BadValue,
                    "The unique field must be false",
                    !request().getUnique().get_value_or(false));

            if (request().getCollation()) {
                auto& collation = request().getCollation().get();
                auto collator =
                    uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                        ->makeFromBSON(collation));
                uassert(ErrorCodes::BadValue,
                        str::stream()
                            << "The collation for reshardCollection must be {locale: 'simple'}, "
                            << "but found: " << collation,
                        !collator);
            }

            const auto& authoritativeTags = uassertStatusOK(
                Grid::get(opCtx)->catalogClient()->getTagsForCollection(opCtx, nss));
            if (!authoritativeTags.empty()) {
                uassert(ErrorCodes::BadValue,
                        "Must specify value for zones field",
                        request().getZones());
            }

            if (const auto& presetChunks = request().get_presetReshardedChunks()) {
                uassert(ErrorCodes::BadValue,
                        "Test commands must be enabled when a value is provided for field: "
                        "_presetReshardedChunks",
                        getTestCommandsEnabled());

                uassert(ErrorCodes::BadValue,
                        "Must specify only one of _presetReshardedChunks or numInitialChunks",
                        !(bool(request().getNumInitialChunks())));

                validateReshardedChunks(
                    *presetChunks, opCtx, ShardKeyPattern(request().getKey()).getKeyPattern());
            }

            // Returns boost::none if there isn't any work to be done by the resharding operation.
            auto instance = ([&]() -> boost::optional<std::shared_ptr<
                                       ReshardingCoordinatorService::ReshardingCoordinator>> {
                FixedFCVRegion fixedFcv(opCtx);

                uassert(ErrorCodes::CommandNotSupported,
                        "reshardCollection command not enabled",
                        resharding::gFeatureFlagResharding.isEnabled(
                            serverGlobalParams.featureCompatibility));

                if (auto existingInstance =
                        getExistingInstanceToJoin(opCtx, nss, request().getKey())) {
                    // Join the existing resharding operation to prevent generating a new resharding
                    // instance if the same command is issued consecutively due to client disconnect
                    // etc.
                    reshardCollectionJoinedExistingOperation.pauseWhileSet(opCtx);
                    existingInstance.get()->getCoordinatorDocWrittenFuture().get(opCtx);
                    return existingInstance;
                }

                const auto cm = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
                        opCtx, nss));

                const auto currentShardKey = cm.getShardKeyPattern().getKeyPattern();
                if (SimpleBSONObjComparator::kInstance.evaluate(currentShardKey.toBSON() ==
                                                                request().getKey())) {
                    // There isn't any work to be done by the resharding operation since the
                    // existing shard key matches the desired new shard key.
                    return boost::none;
                }

                auto tempReshardingNss = constructTemporaryReshardingNss(
                    nss.db(), getCollectionUUIDFromChunkManger(nss, cm));


                if (auto zones = request().getZones()) {
                    checkForOverlappingZones(*zones);
                }

                auto coordinatorDoc =
                    ReshardingCoordinatorDocument(std::move(CoordinatorStateEnum::kUnused),
                                                  {},   // donorShards
                                                  {});  // recipientShards

                // Generate the resharding metadata for the ReshardingCoordinatorDocument.
                auto reshardingUUID = UUID::gen();
                auto existingUUID = getCollectionUUIDFromChunkManger(ns(), cm);
                auto commonMetadata = CommonReshardingMetadata(std::move(reshardingUUID),
                                                               ns(),
                                                               std::move(existingUUID),
                                                               std::move(tempReshardingNss),
                                                               request().getKey());
                coordinatorDoc.setCommonReshardingMetadata(std::move(commonMetadata));
                coordinatorDoc.setZones(request().getZones());
                coordinatorDoc.setPresetReshardedChunks(request().get_presetReshardedChunks());
                coordinatorDoc.setNumInitialChunks(request().getNumInitialChunks());

                opCtx->setAlwaysInterruptAtStepDownOrUp();
                auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
                auto service =
                    registry->lookupServiceByName(ReshardingCoordinatorService::kServiceName);
                auto instance = ReshardingCoordinatorService::ReshardingCoordinator::getOrCreate(
                    opCtx, service, coordinatorDoc.toBSON());

                instance->getCoordinatorDocWrittenFuture().get(opCtx);
                return instance;
            })();

            if (instance) {
                // There is work to be done in order to have the collection's shard key match the
                // requested shard key. Wait until the work is complete.
                instance.get()->getCompletionFuture().get(opCtx);
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Reshards a collection on a new shard key.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} configsvrReshardCollectionCmd;

}  // namespace
}  // namespace mongo
