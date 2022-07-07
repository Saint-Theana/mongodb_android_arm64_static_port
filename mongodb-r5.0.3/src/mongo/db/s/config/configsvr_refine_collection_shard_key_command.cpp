/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/refine_collection_shard_key_gen.h"
#include "mongo/s/stale_shard_version_helpers.h"

namespace mongo {
namespace {

class ConfigsvrRefineCollectionShardKeyCommand final
    : public TypedCommand<ConfigsvrRefineCollectionShardKeyCommand> {
public:
    using Request = ConfigsvrRefineCollectionShardKey;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrRefineCollectionShardKey can only be run on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
            uassert(ErrorCodes::InvalidOptions,
                    "_configsvrRefineCollectionShardKey must be called with majority writeConcern",
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            // TODO (SERVER-53283): Delete this code when FCV 5.1 becomes the official one
            FixedFCVRegion fcvRegion(opCtx);
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Cannot refine collection shard key while the node is being upgraded or "
                    "downgraded",
                    !fcvRegion->isUpgradingOrDowngrading());

            const boost::optional<bool>& isFromPrimaryShard = request().getIsFromPrimaryShard();
            if (isFromPrimaryShard && *isFromPrimaryShard) {
                // If the request has been received from the primary shard, the distributed lock has
                // already been acquired.
                return _internalRun(opCtx);
            }

            // TODO SERVER-54810 don't acquire distributed lock on CSRS after 5.0 has branched out.
            // The request has been received from a last-lts router, acquire distlocks on the
            // namespace's database and collection.
            DistLockManager::ScopedDistLock dbDistLock(uassertStatusOK(
                DistLockManager::get(opCtx)->lock(opCtx,
                                                  nss.db(),
                                                  "refineCollectionShardKey",
                                                  DistLockManager::kDefaultLockTimeout)));
            DistLockManager::ScopedDistLock collDistLock(uassertStatusOK(
                DistLockManager::get(opCtx)->lock(opCtx,
                                                  nss.ns(),
                                                  "refineCollectionShardKey",
                                                  DistLockManager::kDefaultLockTimeout)));

            _internalRun(opCtx);
        }

    private:
        void _internalRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            audit::logRefineCollectionShardKey(opCtx->getClient(), nss.ns(), request().getKey());

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto catalogClient = Grid::get(opCtx)->catalogClient();

            // Validate the given namespace is (i) sharded, (ii) doesn't already have the proposed
            // key, and (iii) has the same epoch as the router that received
            // refineCollectionShardKey had in its routing table cache.
            CollectionType collType;
            try {
                collType = catalogClient->getCollection(
                    opCtx, nss, repl::ReadConcernLevel::kLocalReadConcern);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                uasserted(ErrorCodes::NamespaceNotSharded,
                          str::stream()
                              << "refineCollectionShardKey namespace " << nss << " is not sharded");
            }

            const ShardKeyPattern oldShardKeyPattern(collType.getKeyPattern());
            const ShardKeyPattern newShardKeyPattern(request().getKey());

            if (SimpleBSONObjComparator::kInstance.evaluate(oldShardKeyPattern.toBSON() ==
                                                            newShardKeyPattern.toBSON())) {
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return;
            }

            uassert(ErrorCodes::StaleEpoch,
                    str::stream()
                        << "refineCollectionShardKey namespace " << nss.toString()
                        << " has a different epoch than mongos had in its routing table cache",
                    request().getEpoch() == collType.getEpoch());

            // Validate the given shard key (i) extends the current shard key, (ii) has a "useful"
            // index, and (iii) the index in question has no null entries.
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "refineCollectionShardKey shard key "
                                  << newShardKeyPattern.toString()
                                  << " does not extend the current shard key "
                                  << oldShardKeyPattern.toString(),
                    oldShardKeyPattern.isExtendedBy(newShardKeyPattern));

            // Indexes are loaded using shard versions, so validating the shard key may need to be
            // retried on StaleConfig errors.
            auto catalogCache = Grid::get(opCtx)->catalogCache();
            shardVersionRetry(opCtx,
                              catalogCache,
                              nss,
                              "validating indexes for refineCollectionShardKey"_sd,
                              [&] {
                                  // Note a shard key index will never be created automatically for
                                  // refining a shard key, so no default collation is needed.
                                  shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
                                      opCtx,
                                      nss,
                                      newShardKeyPattern,
                                      boost::none,
                                      collType.getUnique(),
                                      shardkeyutil::ValidationBehaviorsRefineShardKey(opCtx, nss));
                              });

            LOGV2(21922,
                  "CMD: refineCollectionShardKey: {request}",
                  "CMD: refineCollectionShardKey",
                  "request"_attr = request().toBSON({}));

            ShardingCatalogManager::get(opCtx)->refineCollectionShardKey(
                opCtx, nss, newShardKeyPattern);
        }

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
               "directly. Adds a suffix to the shard key of an existing collection ('refines the "
               "shard key').";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} configsvrRefineCollectionShardKeyCmd;

}  // namespace
}  // namespace mongo
