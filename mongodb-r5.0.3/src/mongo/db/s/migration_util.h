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

#pragma once

#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

class BSONObj;
class NamespaceString;
class ShardId;

namespace migrationutil {

constexpr auto kRangeDeletionThreadName = "range-deleter"_sd;

/**
 * Creates a report document with the provided parameters:
 *
 * {
 *     source:          "shard0000"
 *     destination:     "shard0001"
 *     isDonorShard:    true or false
 *     chunk:           {"min": <MinKey>, "max": <MaxKey>}
 *     collection:      "dbName.collName"
 * }
 *
 */
BSONObj makeMigrationStatusDocument(const NamespaceString& nss,
                                    const ShardId& fromShard,
                                    const ShardId& toShard,
                                    const bool& isDonorShard,
                                    const BSONObj& min,
                                    const BSONObj& max);

/**
 * Returns a chunk range with extended or truncated boundaries to match the number of fields in the
 * given metadata's shard key pattern.
 */
ChunkRange extendOrTruncateBoundsForMetadata(const CollectionMetadata& metadata,
                                             const ChunkRange& range);

/**
 * Returns an executor to be used to run commands related to submitting tasks to the range deleter.
 * The executor is initialized on the first call to this function. Uses a shared_ptr
 * because a shared_ptr is required to work with ExecutorFutures.
 */
std::shared_ptr<executor::ThreadPoolTaskExecutor> getMigrationUtilExecutor(
    ServiceContext* serviceContext);

/**
 * Creates a query object that can used to find overlapping ranges in the pending range deletions
 * collection.
 */
Query overlappingRangeQuery(const ChunkRange& range, const UUID& uuid);

/**
 * Checks the pending range deletions collection to see if there are any pending ranges that
 * conflict with the passed in range.
 */
size_t checkForConflictingDeletions(OperationContext* opCtx,
                                    const ChunkRange& range,
                                    const UUID& uuid);

/**
 * Asynchronously attempts to submit the RangeDeletionTask for processing.
 *
 * Note that if the current filtering metadata's UUID does not match the task's UUID, the filtering
 * metadata will be refreshed once. If the UUID's still don't match, the task will be deleted from
 * disk. If the UUID's do match, the range will be submitted for deletion.
 *
 * If the range is submitted for deletion, the returned future is set when the range deletion
 * completes. If the range is not submitted for deletion, the returned future is set with an error
 * explaining why.
 */
ExecutorFuture<void> submitRangeDeletionTask(OperationContext* oppCtx,
                                             const RangeDeletionTask& deletionTask);

/**
 * Queries the rangeDeletions collection for ranges that are ready to be deleted and submits them to
 * the range deleter.
 */
void submitPendingDeletions(OperationContext* opCtx);

/**
 * Asynchronously calls submitPendingDeletions using the fixed executor pool.
 */
void resubmitRangeDeletionsOnStepUp(ServiceContext* serviceContext);

void dropRangeDeletionsCollection(OperationContext* opCtx);

/**
 * Find and submit all orphan ranges for deletion.
 */
void submitOrphanRangesForCleanup(OperationContext* opCtx);

/**
 * Writes the migration coordinator document to config.migrationCoordinators and waits for majority
 * write concern.
 */
void persistMigrationCoordinatorLocally(OperationContext* opCtx,
                                        const MigrationCoordinatorDocument& migrationDoc);

/**
 * Writes the range deletion task document to config.rangeDeletions and waits for majority write
 * concern.
 */
void persistRangeDeletionTaskLocally(OperationContext* opCtx,
                                     const RangeDeletionTask& deletionTask,
                                     const WriteConcernOptions& writeConcern);

/**
 * Updates the migration coordinator document to set the decision field to "committed" and waits for
 * majority writeConcern.
 */
void persistCommitDecision(OperationContext* opCtx,
                           const MigrationCoordinatorDocument& migrationDoc);

/**
 * Updates the migration coordinator document to set the decision field to "aborted" and waits for
 * majority writeConcern.
 */
void persistAbortDecision(OperationContext* opCtx,
                          const MigrationCoordinatorDocument& migrationDoc);

/**
 * Deletes the range deletion task document with the specified id from config.rangeDeletions and
 * waits for majority write concern.
 */
void deleteRangeDeletionTaskLocally(
    OperationContext* opCtx,
    const UUID& deletionTaskId,
    const WriteConcernOptions& writeConcern = WriteConcerns::kMajorityWriteConcern);

/**
 * Deletes the range deletion task document with the specified id from config.rangeDeletions on the
 * specified shard and waits for majority write concern.
 */
void deleteRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                        const ShardId& recipientId,
                                        const UUID& migrationId);

/**
 * Advances the optime for the current transaction by performing a write operation as a retryable
 * write. This is to prevent a write of the deletion task once the decision has been recorded.
 */
void advanceTransactionOnRecipient(OperationContext* opCtx,
                                   const ShardId& recipientId,
                                   const LogicalSessionId& lsid,
                                   TxnNumber txnNumber);

/**
 * Removes the 'pending' flag from the range deletion task document with the specified id from
 * config.rangeDeletions and waits for majority write concern. This marks the range as ready for
 * deletion.
 */
void markAsReadyRangeDeletionTaskLocally(OperationContext* opCtx, const UUID& migrationId);


/**
 * Removes the 'pending' flag from the range deletion task document with the specified id from
 * config.rangeDeletions on the specified shard and waits for majority write concern. This marks the
 * range as ready for deletion.
 */
void markAsReadyRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                             const ShardId& recipientId,
                                             const UUID& migrationId);

/**
 * Deletes the migration coordinator document with the specified id from
 * config.migrationCoordinators without waiting for majority writeConcern.
 */
void deleteMigrationCoordinatorDocumentLocally(OperationContext* opCtx, const UUID& migrationId);

/**
 * Sends _configsvrEnsureChunkVersionIsGreaterThan for the range and preMigrationChunkVersion until
 * hearing success or the node steps down or shuts down.
 */
void ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                     const ChunkRange& range,
                                     const ChunkVersion& preMigrationChunkVersion);

/**
 * Submits an asynchronous task to scan config.migrationCoordinators and drive each unfinished
 * migration coordination to completion.
 */
void resumeMigrationCoordinationsOnStepUp(OperationContext* opCtx);

/**
 * Drive each unfished migration coordination in the given namespace to completion.
 * Assumes the caller to have entered CollectionCriticalSection.
 */
void recoverMigrationCoordinations(OperationContext* opCtx, NamespaceString nss);

}  // namespace migrationutil
}  // namespace mongo
