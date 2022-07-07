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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_util.h"

#include <fmt/format.h>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/query.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_coordinator.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/ensure_chunk_version_is_greater_than_gen.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"
#include "mongo/util/future_util.h"

namespace mongo {
namespace migrationutil {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangBeforeFilteringMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateCommitDecisionInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateAbortDecisionInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionOnRecipientInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionLocallyInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionOnRecipientInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionLocallyInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInAdvanceTxnNumInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInAdvanceTxnNumThenSimulateErrorUninterruptible);

const char kSourceShard[] = "source";
const char kDestinationShard[] = "destination";
const char kIsDonorShard[] = "isDonorShard";
const char kChunk[] = "chunk";
const char kCollection[] = "collection";
const auto kLogRetryAttemptThreshold = 20;
const Backoff kExponentialBackoff(Seconds(10), Milliseconds::max());

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout);


class MigrationUtilExecutor {
public:
    MigrationUtilExecutor()
        : _executor(std::make_shared<executor::ThreadPoolTaskExecutor>(
              _makePool(), executor::makeNetworkInterface("MigrationUtil-TaskExecutor"))) {}

    void shutDownAndJoin() {
        _executor->shutdown();
        _executor->join();
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> getExecutor() {
        stdx::lock_guard<Latch> lg(_mutex);
        if (!_started) {
            _executor->startup();
            _started = true;
        }
        return _executor;
    }

private:
    std::unique_ptr<ThreadPool> _makePool() {
        ThreadPool::Options options;
        options.poolName = "MoveChunk";
        options.minThreads = 0;
        options.maxThreads = 16;
        return std::make_unique<ThreadPool>(std::move(options));
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;

    // TODO SERVER-57253: get rid of _mutex and _started fields
    Mutex _mutex = MONGO_MAKE_LATCH("MigrationUtilExecutor::_mutex");
    bool _started = false;
};

const auto migrationUtilExecutorDecoration =
    ServiceContext::declareDecoration<MigrationUtilExecutor>();
const ServiceContext::ConstructorActionRegisterer migrationUtilExecutorRegisterer{
    "MigrationUtilExecutor",
    [](ServiceContext* service) {
        // TODO SERVER-57253: start migration util executor at decoration construction time
    },
    [](ServiceContext* service) { migrationUtilExecutorDecoration(service).shutDownAndJoin(); }};

template <typename Cmd>
void sendToRecipient(OperationContext* opCtx,
                     const ShardId& recipientId,
                     const Cmd& cmd,
                     const BSONObj& passthroughFields = {}) {
    auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, recipientId));

    auto cmdBSON = cmd.toBSON(passthroughFields);
    LOGV2_DEBUG(22023, 1, "Sending request to recipient", "commandToSend"_attr = redact(cmdBSON));

    auto response = recipientShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        cmd.getDbName().toString(),
        cmdBSON,
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
}

/**
 * Runs doWork until it doesn't throw an error, the node is shutting down, the node has stepped
 * down, or the node has stepped down and up.
 *
 * Note that it is not guaranteed that 'doWork' will not be executed while the node is secondary
 * or after the node has stepped down and up, only that 'doWork' will eventually stop being retried
 * if one of those events has happened.
 *
 * Requirements:
 * - doWork must be idempotent.
 */
void retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
    OperationContext* opCtx,
    StringData taskDescription,
    std::function<void(OperationContext*)> doWork) {
    const std::string newClientName = "{}-{}"_format(getThreadName(), taskDescription);
    const auto initialTerm = repl::ReplicationCoordinator::get(opCtx)->getTerm();

    for (int attempt = 1;; attempt++) {
        // If the server is already doing a clean shutdown, join the shutdown.
        if (globalInShutdownDeprecated()) {
            shutdown(waitForShutdown());
        }

        // If the node is no longer primary, stop retrying.
        uassert(ErrorCodes::InterruptedDueToReplStateChange,
                "Stepped down while {}"_format(taskDescription),
                repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                    repl::MemberState::RS_PRIMARY);

        // If the term changed, that means that the step up recovery could have run or is running
        // so stop retrying in order to avoid duplicate work.
        uassert(ErrorCodes::InterruptedDueToReplStateChange,
                "Term changed while {}"_format(taskDescription),
                initialTerm == repl::ReplicationCoordinator::get(opCtx)->getTerm());

        try {
            auto newClient = opCtx->getServiceContext()->makeClient(newClientName);

            {
                stdx::lock_guard<Client> lk(*newClient.get());
                newClient->setSystemOperationKillableByStepdown(lk);
            }

            auto newOpCtx = newClient->makeOperationContext();
            AlternativeClientRegion altClient(newClient);

            doWork(newOpCtx.get());
            break;
        } catch (DBException& ex) {
            if (attempt % kLogRetryAttemptThreshold == 1) {
                LOGV2_WARNING(23937,
                              "Retrying task after failed attempt",
                              "taskDescription"_attr = redact(taskDescription),
                              "attempt"_attr = attempt,
                              "error"_attr = redact(ex));
            }
        }
    }
}

void refreshFilteringMetadataUntilSuccess(OperationContext* opCtx, const NamespaceString& nss) {
    retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
        opCtx, "refreshFilteringMetadataUntilSuccess", [&nss](OperationContext* newOpCtx) {
            hangInRefreshFilteringMetadataUntilSuccessInterruptible.pauseWhileSet(newOpCtx);

            try {
                onShardVersionMismatch(newOpCtx, nss, boost::none);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // Can throw NamespaceNotFound if the collection/database was dropped
            }

            if (hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response for onShardVersionMismatch");
            }
        });
}


}  // namespace

std::shared_ptr<executor::ThreadPoolTaskExecutor> getMigrationUtilExecutor(
    ServiceContext* serviceContext) {
    return migrationUtilExecutorDecoration(serviceContext).getExecutor();
}

BSONObj makeMigrationStatusDocument(const NamespaceString& nss,
                                    const ShardId& fromShard,
                                    const ShardId& toShard,
                                    const bool& isDonorShard,
                                    const BSONObj& min,
                                    const BSONObj& max) {
    BSONObjBuilder builder;
    builder.append(kSourceShard, fromShard.toString());
    builder.append(kDestinationShard, toShard.toString());
    builder.append(kIsDonorShard, isDonorShard);
    builder.append(kChunk, BSON(ChunkType::min(min) << ChunkType::max(max)));
    builder.append(kCollection, nss.ns());
    return builder.obj();
}

ChunkRange extendOrTruncateBoundsForMetadata(const CollectionMetadata& metadata,
                                             const ChunkRange& range) {
    auto metadataShardKeyPattern = KeyPattern(metadata.getKeyPattern());

    // If the input range is shorter than the range in the ChunkManager inside
    // 'metadata', we must extend its bounds to get a correct comparison. If the input
    // range is longer than the range in the ChunkManager, we likewise must shorten it.
    // We make sure to match what's in the ChunkManager instead of the other way around,
    // since the ChunkManager only stores ranges and compares overlaps using a string version of the
    // key, rather than a BSONObj. This logic is necessary because the _metadata list can
    // contain ChunkManagers with different shard keys if the shard key has been refined.
    //
    // Note that it's safe to use BSONObj::nFields() (which returns the number of top level
    // fields in the BSONObj) to compare the two, since shard key refine operations can only add
    // top-level fields.
    //
    // Using extractFieldsUndotted to shorten the input range is correct because the ChunkRange and
    // the shard key pattern will both already store nested shard key fields as top-level dotted
    // fields, and extractFieldsUndotted uses the top-level fields verbatim rather than treating
    // dots as accessors for subfields.
    auto metadataShardKeyPatternBson = metadataShardKeyPattern.toBSON();
    auto numFieldsInMetadataShardKey = metadataShardKeyPatternBson.nFields();
    auto numFieldsInInputRangeShardKey = range.getMin().nFields();
    if (numFieldsInInputRangeShardKey < numFieldsInMetadataShardKey) {
        auto extendedRangeMin = metadataShardKeyPattern.extendRangeBound(
            range.getMin(), false /* makeUpperInclusive */);
        auto extendedRangeMax = metadataShardKeyPattern.extendRangeBound(
            range.getMax(), false /* makeUpperInclusive */);
        return ChunkRange(extendedRangeMin, extendedRangeMax);
    } else if (numFieldsInInputRangeShardKey > numFieldsInMetadataShardKey) {
        auto shortenedRangeMin = range.getMin().extractFieldsUndotted(metadataShardKeyPatternBson);
        auto shortenedRangeMax = range.getMax().extractFieldsUndotted(metadataShardKeyPatternBson);
        return ChunkRange(shortenedRangeMin, shortenedRangeMax);
    } else {
        return range;
    }
}

Query overlappingRangeQuery(const ChunkRange& range, const UUID& uuid) {
    return QUERY(RangeDeletionTask::kCollectionUuidFieldName
                 << uuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey << LT
                 << range.getMax() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
                 << GT << range.getMin());
}

size_t checkForConflictingDeletions(OperationContext* opCtx,
                                    const ChunkRange& range,
                                    const UUID& uuid) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    return store.count(opCtx, overlappingRangeQuery(range, uuid));
}

bool deletionTaskUuidMatchesFilteringMetadataUuid(
    OperationContext* opCtx,
    const boost::optional<mongo::CollectionMetadata>& optCollDescr,
    const RangeDeletionTask& deletionTask) {
    return optCollDescr && optCollDescr->isSharded() &&
        optCollDescr->uuidMatches(deletionTask.getCollectionUuid());
}

ExecutorFuture<void> cleanUpRange(ServiceContext* serviceContext,
                                  const std::shared_ptr<executor::ThreadPoolTaskExecutor>& executor,
                                  const RangeDeletionTask& deletionTask) {

    return AsyncTry([=]() mutable {
               ThreadClient tc(kRangeDeletionThreadName, serviceContext);
               {
                   stdx::lock_guard<Client> lk(*tc.get());
                   tc->setSystemOperationKillableByStepdown(lk);
               }
               auto uniqueOpCtx = tc->makeOperationContext();
               auto opCtx = uniqueOpCtx.get();
               opCtx->setAlwaysInterruptAtStepDownOrUp();

               AutoGetCollection autoColl(opCtx, deletionTask.getNss(), MODE_IS);
               auto csr = CollectionShardingRuntime::get(opCtx, deletionTask.getNss());
               // Keep the collection metadata from changing for the rest of this scope.
               auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);
               auto optCollDescr = csr->getCurrentMetadataIfKnown();
               uassert(
                   ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist,
                   str::stream()
                       << "Even after forced refresh, filtering metadata for namespace in "
                          "deletion task "
                       << (optCollDescr
                               ? (optCollDescr->isSharded()
                                      ? " has UUID that does not match UUID of the deletion task"
                                      : " is unsharded")
                               : " is not known"),
                   deletionTaskUuidMatchesFilteringMetadataUuid(opCtx, optCollDescr, deletionTask));

               LOGV2(22026,
                     "Submitting range deletion task",
                     "deletionTask"_attr = redact(deletionTask.toBSON()),
                     "migrationId"_attr = deletionTask.getId());

               const auto whenToClean = deletionTask.getWhenToClean() == CleanWhenEnum::kNow
                   ? CollectionShardingRuntime::kNow
                   : CollectionShardingRuntime::kDelayed;

               return csr->cleanUpRange(deletionTask.getRange(), deletionTask.getId(), whenToClean);
           })
        .until([](Status status) mutable {
            // Resubmit the range for deletion on a RangeOverlapConflict error.
            return status != ErrorCodes::RangeOverlapConflict;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(executor, CancellationToken::uncancelable());
}

ExecutorFuture<void> submitRangeDeletionTask(OperationContext* opCtx,
                                             const RangeDeletionTask& deletionTask) {
    const auto serviceContext = opCtx->getServiceContext();
    auto executor = getMigrationUtilExecutor(serviceContext);
    return ExecutorFuture<void>(executor)
        .then([=] {
            ThreadClient tc(kRangeDeletionThreadName, serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            uassert(
                ErrorCodes::ResumableRangeDeleterDisabled,
                str::stream()
                    << "Not submitting range deletion task " << redact(deletionTask.toBSON())
                    << " because the disableResumableRangeDeleter server parameter is set to true",
                !disableResumableRangeDeleter.load());

            // Make sure the collection metadata is up-to-date before submitting.
            boost::optional<CollectionMetadata> optCollDescr;
            {
                AutoGetCollection autoColl(opCtx, deletionTask.getNss(), MODE_IS);
                auto csr = CollectionShardingRuntime::get(opCtx, deletionTask.getNss());
                optCollDescr = csr->getCurrentMetadataIfKnown();
            }

            if (!deletionTaskUuidMatchesFilteringMetadataUuid(opCtx, optCollDescr, deletionTask)) {

                // If the collection's filtering metadata is not known, is unsharded, or its
                // UUID does not match the UUID of the deletion task, force a filtering metadata
                // refresh, because this node may have just stepped up and therefore may have a
                // stale cache.
                LOGV2(22024,
                      "Filtering metadata for this range deletion task may be outdated; "
                      "forcing refresh",
                      "deletionTask"_attr = redact(deletionTask.toBSON()),
                      "error"_attr =
                          (optCollDescr ? (optCollDescr->isSharded()
                                               ? "Collection has UUID that does not match "
                                                 "UUID of the deletion task"
                                               : "Collection is unsharded")
                                        : "Collection's sharding state is not known"),
                      "namespace"_attr = deletionTask.getNss(),
                      "migrationId"_attr = deletionTask.getId());

                refreshFilteringMetadataUntilSuccess(opCtx, deletionTask.getNss());
            }

            return cleanUpRange(serviceContext, executor, deletionTask);
        })
        .onError([=](const Status status) {
            ThreadClient tc(kRangeDeletionThreadName, serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            LOGV2(22027,
                  "Failed to submit range deletion task",
                  "deletionTask"_attr = redact(deletionTask.toBSON()),
                  "error"_attr = redact(status),
                  "migrationId"_attr = deletionTask.getId());

            if (status == ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist) {
                deleteRangeDeletionTaskLocally(
                    opCtx, deletionTask.getId(), ShardingCatalogClient::kLocalWriteConcern);
            }

            // Note, we use onError and make it return its input status, because ExecutorFuture does
            // not support tapError.
            return status;
        });
}

void submitPendingDeletions(OperationContext* opCtx) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    auto query = QUERY("pending" << BSON("$exists" << false));

    store.forEach(opCtx, query, [&opCtx](const RangeDeletionTask& deletionTask) {
        migrationutil::submitRangeDeletionTask(opCtx, deletionTask).getAsync([](auto) {});
        return true;
    });
}

void resubmitRangeDeletionsOnStepUp(ServiceContext* serviceContext) {
    LOGV2(22028, "Starting pending deletion submission thread.");

    ExecutorFuture<void>(getMigrationUtilExecutor(serviceContext))
        .then([serviceContext] {
            ThreadClient tc("ResubmitRangeDeletions", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }

            auto opCtx = tc->makeOperationContext();

            submitPendingDeletions(opCtx.get());
        })
        .getAsync([](const Status& status) {
            if (!status.isOK()) {
                LOGV2(45739,
                      "Error while submitting pending range deletions",
                      "error"_attr = redact(status));
            }
        });
}

void dropRangeDeletionsCollection(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    client.dropCollection(NamespaceString::kRangeDeletionNamespace.toString(),
                          WriteConcerns::kMajorityWriteConcern);
}

template <typename Callable>
void forEachOrphanRange(OperationContext* opCtx, const NamespaceString& nss, Callable&& handler) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);

    const auto csr = CollectionShardingRuntime::get(opCtx, nss);
    const auto metadata = csr->getCurrentMetadataIfKnown();
    const auto emptyChunkMap =
        RangeMap{SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()};

    if (!metadata) {
        LOGV2(474680,
              "Upgrade: Skipping orphaned range enumeration because the collection's sharding "
              "state is not known",
              "namespace"_attr = nss);
        return;
    }

    if (!metadata->isSharded()) {
        LOGV2(22029,
              "Upgrade: Skipping orphaned range enumeration because the collection is not sharded",
              "namespace"_attr = nss);
        return;
    }

    auto startingKey = metadata->getMinKey();

    while (true) {
        auto range = metadata->getNextOrphanRange(emptyChunkMap, startingKey);
        if (!range) {
            LOGV2_DEBUG(22030,
                        2,
                        "Upgrade: Completed orphanged range enumeration; no orphaned ranges "
                        "remain",
                        "namespace"_attr = nss.toString(),
                        "startingKey"_attr = redact(startingKey));

            return;
        }

        handler(*range);

        startingKey = range->getMax();
    }
}

void submitOrphanRanges(OperationContext* opCtx, const NamespaceString& nss, const UUID& uuid) {
    try {

        onShardVersionMismatch(opCtx, nss, boost::none);

        LOGV2_DEBUG(22031,
                    2,
                    "Upgrade: Cleaning up existing orphans",
                    "namespace"_attr = nss,
                    "uuid"_attr = uuid);

        std::vector<RangeDeletionTask> deletions;
        forEachOrphanRange(opCtx, nss, [&deletions, &opCtx, &nss, &uuid](const auto& range) {
            // Since this is not part of an active migration, the migration UUID and the donor shard
            // are set to unused values so that they don't conflict.
            RangeDeletionTask task(
                UUID::gen(), nss, uuid, ShardId("fromFCVUpgrade"), range, CleanWhenEnum::kDelayed);
            deletions.emplace_back(task);
        });

        if (deletions.empty())
            return;

        PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

        for (const auto& task : deletions) {
            LOGV2_DEBUG(22032,
                        2,
                        "Upgrade: Submitting chunk range for cleanup",
                        "range"_attr = redact(task.getRange().toString()),
                        "namespace"_attr = nss);
            store.add(opCtx, task);
        }
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& e) {
        LOGV2(22033,
              "Upgrade: Failed to clean up orphans because the namespace was not found; the "
              "collection must have been dropped",
              "namespace"_attr = nss,
              "error"_attr = redact(e.what()));
    }
}

void submitOrphanRangesForCleanup(OperationContext* opCtx) {
    auto catalog = CollectionCatalog::get(opCtx);
    const auto& dbs = catalog->getAllDbNames();

    for (const auto& dbName : dbs) {
        if (dbName == NamespaceString::kLocalDb)
            continue;

        for (auto collIt = catalog->begin(opCtx, dbName); collIt != catalog->end(opCtx); ++collIt) {
            auto uuid = collIt.uuid().get();
            auto nss = catalog->lookupNSSByUUID(opCtx, uuid).get();
            LOGV2_DEBUG(22034,
                        2,
                        "Upgrade: Processing collection for orphaned range cleanup",
                        "namespace"_attr = nss);
            if (!nss.isNamespaceAlwaysUnsharded()) {
                submitOrphanRanges(opCtx, nss, uuid);
            }
        }
    }
}

void persistMigrationCoordinatorLocally(OperationContext* opCtx,
                                        const MigrationCoordinatorDocument& migrationDoc) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    try {
        store.add(opCtx, migrationDoc);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(
            31374,
            str::stream() << "While attempting to write migration information for migration "
                          << ", found document with the same migration id. Attempted migration: "
                          << migrationDoc.toBSON());
    }
}

void persistRangeDeletionTaskLocally(OperationContext* opCtx,
                                     const RangeDeletionTask& deletionTask,
                                     const WriteConcernOptions& writeConcern) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    try {
        store.add(opCtx, deletionTask, writeConcern);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(31375,
                  str::stream() << "While attempting to write range deletion task for migration "
                                << ", found document with the same migration id. Attempted range "
                                   "deletion task: "
                                << deletionTask.toBSON());
    }
}

void persistCommitDecision(OperationContext* opCtx,
                           const MigrationCoordinatorDocument& migrationDoc) {
    invariant(migrationDoc.getDecision() &&
              *migrationDoc.getDecision() == DecisionEnum::kCommitted);

    hangInPersistMigrateCommitDecisionInterruptible.pauseWhileSet(opCtx);

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.upsert(opCtx,
                 QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationDoc.getId()),
                 migrationDoc.toBSON());

    if (hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible.shouldFail()) {
        hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when persisting migrate commit decision");
    }
}

void persistAbortDecision(OperationContext* opCtx,
                          const MigrationCoordinatorDocument& migrationDoc) {
    invariant(migrationDoc.getDecision() && *migrationDoc.getDecision() == DecisionEnum::kAborted);

    hangInPersistMigrateAbortDecisionInterruptible.pauseWhileSet(opCtx);

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.upsert(opCtx,
                 QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationDoc.getId()),
                 migrationDoc.toBSON());

    if (hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible.shouldFail()) {
        hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when persisting migrate abort decision");
    }
}

void deleteRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                        const ShardId& recipientId,
                                        const UUID& migrationId) {
    write_ops::DeleteCommandRequest deleteOp(NamespaceString::kRangeDeletionNamespace);
    write_ops::DeleteOpEntry query(BSON(RangeDeletionTask::kIdFieldName << migrationId),
                                   false /*multi*/);
    deleteOp.setDeletes({query});

    hangInDeleteRangeDeletionOnRecipientInterruptible.pauseWhileSet(opCtx);

    sendToRecipient(opCtx,
                    recipientId,
                    deleteOp,
                    BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));

    if (hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible.shouldFail()) {
        hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when deleting range deletion on recipient");
    }
}

void deleteRangeDeletionTaskLocally(OperationContext* opCtx,
                                    const UUID& deletionTaskId,
                                    const WriteConcernOptions& writeConcern) {
    hangInDeleteRangeDeletionLocallyInterruptible.pauseWhileSet(opCtx);
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.remove(opCtx, QUERY(RangeDeletionTask::kIdFieldName << deletionTaskId), writeConcern);

    if (hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible.shouldFail()) {
        hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when deleting range deletion locally");
    }
}

void markAsReadyRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                             const ShardId& recipientId,
                                             const UUID& migrationId) {
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kRangeDeletionNamespace);
    auto queryFilter = BSON(RangeDeletionTask::kIdFieldName << migrationId);
    auto updateModification =
        write_ops::UpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << ""))));
    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(false);
    updateOp.setUpdates({updateEntry});

    retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
        opCtx, "ready remote range deletion", [&](OperationContext* newOpCtx) {
            hangInReadyRangeDeletionOnRecipientInterruptible.pauseWhileSet(newOpCtx);

            try {
                sendToRecipient(
                    newOpCtx,
                    recipientId,
                    updateOp,
                    BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));
            } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& exShardNotFound) {
                LOGV2_DEBUG(4620232,
                            1,
                            "Failed to mark range deletion task on recipient shard as ready",
                            "migrationId"_attr = migrationId,
                            "error"_attr = exShardNotFound);
                return;
            }

            if (hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible.shouldFail()) {
                hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when initiating range deletion on recipient");
            }
        });
}

void advanceTransactionOnRecipient(OperationContext* opCtx,
                                   const ShardId& recipientId,
                                   const LogicalSessionId& lsid,
                                   TxnNumber currentTxnNumber) {
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace);
    auto queryFilter = BSON("_id"
                            << "migrationCoordinatorStats");
    auto updateModification = write_ops::UpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(BSON("$inc" << BSON("count" << 1))));

    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(true);
    updateOp.setUpdates({updateEntry});

    auto passthroughFields = BSON(WriteConcernOptions::kWriteConcernField
                                  << WriteConcernOptions::Majority << "lsid" << lsid.toBSON()
                                  << "txnNumber" << currentTxnNumber + 1);

    hangInAdvanceTxnNumInterruptible.pauseWhileSet(opCtx);
    sendToRecipient(opCtx, recipientId, updateOp, passthroughFields);

    if (hangInAdvanceTxnNumThenSimulateErrorUninterruptible.shouldFail()) {
        hangInAdvanceTxnNumThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when initiating range deletion locally");
    }
}

void markAsReadyRangeDeletionTaskLocally(OperationContext* opCtx, const UUID& migrationId) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto query = QUERY(RangeDeletionTask::kIdFieldName << migrationId);
    auto update = BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << ""));

    hangInReadyRangeDeletionLocallyInterruptible.pauseWhileSet(opCtx);
    store.update(opCtx, query, update);

    if (hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible.shouldFail()) {
        hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when initiating range deletion locally");
    }
}

void deleteMigrationCoordinatorDocumentLocally(OperationContext* opCtx, const UUID& migrationId) {
    // Before deleting the migration coordinator document, ensure that in the case of a crash, the
    // node will start-up from at least the configTime, which it obtained as part of recovery of the
    // shardVersion, which will ensure that it will see at least the same shardVersion.
    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.remove(opCtx,
                 QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationId),
                 {1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)});
}

void ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& collUUID,
                                     const ChunkRange& range,
                                     const ChunkVersion& preMigrationChunkVersion) {
    ConfigsvrEnsureChunkVersionIsGreaterThan ensureChunkVersionIsGreaterThanRequest;
    ensureChunkVersionIsGreaterThanRequest.setDbName(NamespaceString::kAdminDb);
    ensureChunkVersionIsGreaterThanRequest.setMinKey(range.getMin());
    ensureChunkVersionIsGreaterThanRequest.setMaxKey(range.getMax());
    ensureChunkVersionIsGreaterThanRequest.setVersion(preMigrationChunkVersion);
    ensureChunkVersionIsGreaterThanRequest.setNss(nss);
    ensureChunkVersionIsGreaterThanRequest.setCollectionUUID(collUUID);
    const auto ensureChunkVersionIsGreaterThanRequestBSON =
        ensureChunkVersionIsGreaterThanRequest.toBSON({});

    hangInEnsureChunkVersionIsGreaterThanInterruptible.pauseWhileSet(opCtx);

    const auto ensureChunkVersionIsGreaterThanResponse =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            CommandHelpers::appendMajorityWriteConcern(ensureChunkVersionIsGreaterThanRequestBSON),
            Shard::RetryPolicy::kIdempotent);
    const auto ensureChunkVersionIsGreaterThanStatus =
        Shard::CommandResponse::getEffectiveStatus(ensureChunkVersionIsGreaterThanResponse);

    uassertStatusOK(ensureChunkVersionIsGreaterThanStatus);

    if (hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible.shouldFail()) {
        hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible.pauseWhileSet();
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response for _configsvrEnsureChunkVersionIsGreaterThan");
    }
}

void resumeMigrationCoordinationsOnStepUp(OperationContext* opCtx) {
    LOGV2_DEBUG(4798510, 2, "Starting migration coordinator step-up recovery");

    unsigned long long unfinishedMigrationsCount = 0;

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.forEach(opCtx,
                  Query{},
                  [&opCtx, &unfinishedMigrationsCount](const MigrationCoordinatorDocument& doc) {
                      // MigrationCoordinators are only created under the MigrationBlockingGuard,
                      // which means that only one can possibly exist on an instance at a time.
                      // Furthermore, recovery of an incomplete MigrationCoordator also acquires the
                      // MigrationBlockingGuard. Because of this it is not possible to have more
                      // than one unfinished migration.
                      invariant(unfinishedMigrationsCount == 0,
                                str::stream()
                                    << "Upon step-up a second migration coordinator was found"
                                    << redact(doc.toBSON()));

                      unfinishedMigrationsCount++;
                      LOGV2_DEBUG(4798511,
                                  3,
                                  "Found unfinished migration on step-up",
                                  "migrationCoordinatorDoc"_attr = redact(doc.toBSON()),
                                  "unfinishedMigrationsCount"_attr = unfinishedMigrationsCount);

                      const auto& nss = doc.getNss();

                      {
                          AutoGetCollection autoColl(opCtx, nss, MODE_IX);
                          CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);
                      }

                      auto mbg = std::make_shared<MigrationBlockingGuard>(
                          opCtx,
                          str::stream() << "Recovery of migration session "
                                        << doc.getMigrationSessionId().toString()
                                        << " on collection " << nss);

                      ExecutorFuture<void>(getMigrationUtilExecutor(opCtx->getServiceContext()))
                          .then([serviceContext = opCtx->getServiceContext(), nss, mbg] {
                              ThreadClient tc("TriggerMigrationRecovery", serviceContext);
                              {
                                  stdx::lock_guard<Client> lk(*tc.get());
                                  tc->setSystemOperationKillableByStepdown(lk);
                              }

                              auto opCtx = tc->makeOperationContext();

                              hangBeforeFilteringMetadataRefresh.pauseWhileSet();

                              onShardVersionMismatch(
                                  opCtx.get(), nss, boost::none /* shardVersionReceived */);
                          })
                          .onError([](const Status& status) {
                              LOGV2_WARNING(4798512,
                                            "Error on deferred shardVersion recovery execution",
                                            "error"_attr = redact(status));
                          })
                          .getAsync([](auto) {});

                      return true;
                  });

    ShardingStatistics::get(opCtx).unfinishedMigrationFromPreviousPrimary.store(
        unfinishedMigrationsCount);

    LOGV2_DEBUG(4798513,
                2,
                "Finished migration coordinator step-up recovery",
                "unfinishedMigrationsCount"_attr = unfinishedMigrationsCount);
}

void recoverMigrationCoordinations(OperationContext* opCtx, NamespaceString nss) {
    LOGV2_DEBUG(4798501, 2, "Starting migration recovery", "namespace"_attr = nss);

    unsigned migrationRecoveryCount = 0;
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.forEach(
        opCtx,
        QUERY(MigrationCoordinatorDocument::kNssFieldName << nss.toString()),
        [&opCtx, &migrationRecoveryCount](const MigrationCoordinatorDocument& doc) {
            LOGV2_DEBUG(4798502,
                        2,
                        "Recovering migration",
                        "migrationCoordinatorDocument"_attr = redact(doc.toBSON()));
            // ensure there is only one migrationCoordinatorDocument
            // to be recovered for this namespace
            invariant(++migrationRecoveryCount == 1,
                      "Found more then one migration to recover for a single namespace");


            // Create a MigrationCoordinator to complete the coordination.
            MigrationCoordinator coordinator(doc);

            if (doc.getDecision()) {
                // The decision is already known.
                coordinator.completeMigration(opCtx);
                return true;
            }

            // The decision is not known. Recover the decision from the config server.

            ensureChunkVersionIsGreaterThan(opCtx,
                                            doc.getNss(),
                                            doc.getCollectionUuid(),
                                            doc.getRange(),
                                            doc.getPreMigrationChunkVersion());

            hangInRefreshFilteringMetadataUntilSuccessInterruptible.pauseWhileSet(opCtx);

            auto currentMetadata = forceGetCurrentMetadata(opCtx, doc.getNss());

            if (hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response for forceShardFilteringMetadataRefresh");
            }

            auto setFilteringMetadata = [&opCtx, &currentMetadata, &doc]() {
                AutoGetDb autoDb(opCtx, doc.getNss().db(), MODE_IX);
                Lock::CollectionLock collLock(opCtx, doc.getNss(), MODE_IX);
                auto* const csr = CollectionShardingRuntime::get(opCtx, doc.getNss());

                auto optMetadata = csr->getCurrentMetadataIfKnown();
                invariant(!optMetadata);

                csr->setFilteringMetadata(opCtx, std::move(currentMetadata));
            };

            if (!currentMetadata.isSharded() ||
                !currentMetadata.uuidMatches(doc.getCollectionUuid())) {
                if (!currentMetadata.isSharded()) {
                    LOGV2(4798503,
                          "During migration recovery the collection was discovered to have been "
                          "dropped."
                          "Deleting the range deletion tasks on the donor and the recipient "
                          "as well as the migration coordinator document on this node",
                          "migrationCoordinatorDocument"_attr = redact(doc.toBSON()));
                } else {
                    // UUID don't match
                    LOGV2(4798504,
                          "During migration recovery the collection was discovered to have been "
                          "dropped and recreated. Collection has a UUID that "
                          "does not match the one in the migration coordinator "
                          "document. Deleting the range deletion tasks on the donor and "
                          "recipient as well as the migration coordinator document on this node",
                          "migrationCoordinatorDocument"_attr = redact(doc.toBSON()),
                          "refreshedMetadataUUID"_attr =
                              currentMetadata.getChunkManager()->getUUID(),
                          "coordinatorDocumentUUID"_attr = doc.getCollectionUuid());
                }

                deleteRangeDeletionTaskOnRecipient(opCtx, doc.getRecipientShardId(), doc.getId());
                deleteRangeDeletionTaskLocally(opCtx, doc.getId());
                coordinator.forgetMigration(opCtx);
                setFilteringMetadata();
                return true;
            }

            if (currentMetadata.keyBelongsToMe(doc.getRange().getMin())) {
                coordinator.setMigrationDecision(DecisionEnum::kAborted);
            } else {
                coordinator.setMigrationDecision(DecisionEnum::kCommitted);
            }

            coordinator.completeMigration(opCtx);
            setFilteringMetadata();
            return true;
        });
}

}  // namespace migrationutil
}  // namespace mongo
