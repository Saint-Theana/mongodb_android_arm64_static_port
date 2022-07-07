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

#include "mongo/s/write_ops/batch_write_exec.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/util/exit.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);

//
// Map which allows associating ConnectionString hosts with TargetedWriteBatches
// This is needed since the dispatcher only returns hosts with responses.
//

WriteErrorDetail errorFromStatus(const Status& status) {
    WriteErrorDetail error;
    error.setStatus(status);
    return error;
}

// Helper to note several stale shard errors from a response
void noteStaleShardResponses(OperationContext* opCtx,
                             const std::vector<ShardError>& staleErrors,
                             NSTargeter* targeter) {
    for (const auto& error : staleErrors) {
        LOGV2_DEBUG(22902,
                    4,
                    "Noting stale config response from {shardId}: {errorInfo}",
                    "Noting stale config response",
                    "shardId"_attr = error.endpoint.shardName,
                    "errorInfo"_attr = error.error.getErrInfo());
        targeter->noteStaleShardResponse(
            opCtx,
            error.endpoint,
            StaleConfigInfo::parseFromCommandError(
                error.error.isErrInfoSet() ? error.error.getErrInfo() : BSONObj()));
    }
}

// Helper to note several stale db errors from a response
void noteStaleDbResponses(OperationContext* opCtx,
                          const std::vector<ShardError>& staleErrors,
                          NSTargeter* targeter) {
    for (const auto& error : staleErrors) {
        LOGV2_DEBUG(22903,
                    4,
                    "Noting stale database response",
                    "shardId"_attr = error.endpoint.shardName,
                    "errorInfo"_attr = error.error);
        targeter->noteStaleDbResponse(
            opCtx,
            error.endpoint,
            StaleDbRoutingVersion::parseFromCommandError(error.error.toBSON()));
    }
}

bool hasTransientTransactionError(const BatchedCommandResponse& response) {
    if (!response.isErrorLabelsSet()) {
        return false;
    }

    const auto& errorLabels = response.getErrorLabels();
    auto iter = std::find_if(errorLabels.begin(), errorLabels.end(), [](const std::string& label) {
        return label == ErrorLabel::kTransientTransaction;
    });
    return iter != errorLabels.end();
}

// The number of times we'll try to continue a batch op if no progress is being made. This only
// applies when no writes are occurring and metadata is not changing on reload.
const int kMaxRoundsWithoutProgress(5);

}  // namespace

void BatchWriteExec::executeBatch(OperationContext* opCtx,
                                  NSTargeter& targeter,
                                  const BatchedCommandRequest& clientRequest,
                                  BatchedCommandResponse* clientResponse,
                                  BatchWriteExecStats* stats) {
    const auto& nss(clientRequest.getNS());

    LOGV2_DEBUG(22904,
                4,
                "Starting execution of a write batch of size {size} for collection {namespace}",
                "Starting execution of a write batch",
                "namespace"_attr = nss.ns(),
                "size"_attr = clientRequest.sizeWriteOps());

    BatchWriteOp batchOp(opCtx, clientRequest);

    // Current batch status
    bool refreshedTargeter = false;
    int rounds = 0;
    int numCompletedOps = 0;
    int numRoundsWithoutProgress = 0;
    bool abortBatch = false;

    while (!batchOp.isFinished() && !abortBatch) {
        //
        // Get child batches to send using the targeter
        //
        // Targeting errors can be caused by remote metadata changing (the collection could have
        // been dropped and recreated, for example with a new shard key).  If a remote metadata
        // change occurs *before* a client sends us a batch, we need to make sure that we don't
        // error out just because we're staler than the client - otherwise mongos will be have
        // unpredictable behavior.
        //
        // (If a metadata change happens *during* or *after* a client sends us a batch, however,
        // we make no guarantees about delivery.)
        //
        // For this reason, we don't record targeting errors until we've refreshed our targeting
        // metadata at least once *after* receiving the client batch - at that point, we know:
        //
        // 1) our new metadata is the same as the metadata when the client sent a batch, and so
        //    targeting errors are real.
        // OR
        // 2) our new metadata is a newer version than when the client sent a batch, and so
        //    the metadata must have changed after the client batch was sent.  We don't need to
        //    deliver in this case, since for all the client knows we may have gotten the batch
        //    exactly when the metadata changed.
        //

        OwnedPointerMap<ShardId, TargetedWriteBatch> childBatchesOwned;
        std::map<ShardId, TargetedWriteBatch*>& childBatches = childBatchesOwned.mutableMap();

        // If we've already had a targeting error, we've refreshed the metadata once and can
        // record target errors definitively.
        bool recordTargetErrors = refreshedTargeter;
        Status targetStatus = batchOp.targetBatch(targeter, recordTargetErrors, &childBatches);
        if (!targetStatus.isOK()) {
            // Don't do anything until a targeter refresh
            targeter.noteCouldNotTarget();
            refreshedTargeter = true;
            ++stats->numTargetErrors;
            dassert(childBatches.size() == 0u);

            if (TransactionRouter::get(opCtx)) {
                batchOp.forgetTargetedBatchesOnTransactionAbortingError();

                // Throw when there is a transient transaction error since this should be a top
                // level error and not just a write error.
                if (isTransientTransactionError(targetStatus.code(), false, false)) {
                    uassertStatusOK(targetStatus);
                }

                break;
            }
        }

        //
        // Send all child batches
        //

        const size_t numToSend = childBatches.size();
        size_t numSent = 0;

        while (numSent != numToSend) {
            // Collect batches out on the network, mapped by endpoint
            OwnedPointerMap<ShardId, TargetedWriteBatch> ownedPendingBatches;
            OwnedPointerMap<ShardId, TargetedWriteBatch>::MapType& pendingBatches =
                ownedPendingBatches.mutableMap();

            //
            // Construct the requests.
            //

            std::vector<AsyncRequestsSender::Request> requests;

            // Get as many batches as we can at once
            for (auto& childBatch : childBatches) {
                TargetedWriteBatch* const nextBatch = childBatch.second;

                // If the batch is nullptr, we sent it previously, so skip
                if (!nextBatch)
                    continue;

                // If we already have a batch for this shard, wait until the next time
                const auto& targetShardId = nextBatch->getEndpoint().shardName;
                if (pendingBatches.count(targetShardId))
                    continue;

                stats->noteTargetedShard(targetShardId);

                const auto request = [&] {
                    const auto shardBatchRequest(batchOp.buildBatchRequest(*nextBatch));

                    BSONObjBuilder requestBuilder;
                    shardBatchRequest.serialize(&requestBuilder);
                    logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &requestBuilder);

                    return requestBuilder.obj();
                }();

                LOGV2_DEBUG(22905,
                            4,
                            "Sending write batch to {shardId}: {request}",
                            "Sending write batch",
                            "shardId"_attr = targetShardId,
                            "request"_attr = redact(request));

                requests.emplace_back(targetShardId, request);

                // Indicate we're done by setting the batch to nullptr. We'll only get duplicate
                // hostEndpoints if we have broadcast and non-broadcast endpoints for the same host,
                // so this should be pretty efficient without moving stuff around.
                childBatch.second = nullptr;

                // Recv-side is responsible for cleaning up the nextBatch when used
                pendingBatches.emplace(targetShardId, nextBatch);
            }

            bool isRetryableWrite = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);

            MultiStatementTransactionRequestsSender ars(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                clientRequest.getNS().db().toString(),
                requests,
                kPrimaryOnlyReadPreference,
                isRetryableWrite ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);
            numSent += pendingBatches.size();

            //
            // Receive the responses.
            //

            while (!ars.done()) {
                // Block until a response is available.
                auto response = ars.next();

                // Get the TargetedWriteBatch to find where to put the response
                dassert(pendingBatches.find(response.shardId) != pendingBatches.end());
                TargetedWriteBatch* batch = pendingBatches.find(response.shardId)->second;

                const auto shardInfo = response.shardHostAndPort
                    ? response.shardHostAndPort->toString()
                    : batch->getEndpoint().shardName;

                // Then check if we successfully got a response.
                Status responseStatus = response.swResponse.getStatus();
                BatchedCommandResponse batchedCommandResponse;
                if (responseStatus.isOK()) {
                    std::string errMsg;
                    if (!batchedCommandResponse.parseBSON(response.swResponse.getValue().data,
                                                          &errMsg) ||
                        !batchedCommandResponse.isValid(&errMsg)) {
                        responseStatus = {ErrorCodes::FailedToParse, errMsg};
                    }
                }

                if (responseStatus.isOK()) {
                    TrackedErrors trackedErrors;
                    trackedErrors.startTracking(ErrorCodes::StaleShardVersion);
                    trackedErrors.startTracking(ErrorCodes::StaleDbVersion);

                    LOGV2_DEBUG(22907,
                                4,
                                "Write results received from {shardInfo}: {response}",
                                "Write results received",
                                "shardInfo"_attr = shardInfo,
                                "status"_attr = redact(batchedCommandResponse.toStatus()));

                    // Dispatch was ok, note response
                    batchOp.noteBatchResponse(*batch, batchedCommandResponse, &trackedErrors);

                    // If we are in a transaction, we must fail the whole batch on any error.
                    if (TransactionRouter::get(opCtx)) {
                        // Note: this returns a bad status if any part of the batch failed.
                        auto batchStatus = batchedCommandResponse.toStatus();
                        if (!batchStatus.isOK() &&
                            batchStatus != ErrorCodes::WouldChangeOwningShard) {
                            auto newStatus = batchStatus.withContext(
                                str::stream() << "Encountered error from " << shardInfo
                                              << " during a transaction");

                            batchOp.forgetTargetedBatchesOnTransactionAbortingError();

                            // Throw when there is a transient transaction error since this
                            // should be a top level error and not just a write error.
                            if (hasTransientTransactionError(batchedCommandResponse)) {
                                uassertStatusOK(newStatus);
                            }

                            abortBatch = true;
                            break;
                        }
                    }

                    // Note if anything was stale
                    const auto& staleShardErrors =
                        trackedErrors.getErrors(ErrorCodes::StaleShardVersion);
                    const auto& staleDbErrors = trackedErrors.getErrors(ErrorCodes::StaleDbVersion);

                    if (!staleShardErrors.empty()) {
                        invariant(staleDbErrors.empty());
                        noteStaleShardResponses(opCtx, staleShardErrors, &targeter);
                        ++stats->numStaleShardBatches;
                    }

                    if (!staleDbErrors.empty()) {
                        invariant(staleShardErrors.empty());
                        noteStaleDbResponses(opCtx, staleDbErrors, &targeter);
                        ++stats->numStaleDbBatches;
                    }

                    if (response.shardHostAndPort) {
                        // Remember that we successfully wrote to this shard
                        // NOTE: This will record lastOps for shards where we actually didn't update
                        // or delete any documents, which preserves old behavior but is conservative
                        stats->noteWriteAt(*response.shardHostAndPort,
                                           batchedCommandResponse.isLastOpSet()
                                               ? batchedCommandResponse.getLastOp()
                                               : repl::OpTime(),
                                           batchedCommandResponse.isElectionIdSet()
                                               ? batchedCommandResponse.getElectionId()
                                               : OID());
                    }
                } else {
                    if ((ErrorCodes::isShutdownError(responseStatus) ||
                         responseStatus == ErrorCodes::CallbackCanceled) &&
                        globalInShutdownDeprecated()) {
                        // Throw an error since the mongos itself is shutting down so this should
                        // be a top level error instead of a write error.
                        uassertStatusOK(responseStatus);
                    }

                    // Error occurred dispatching, note it
                    const Status status = responseStatus.withContext(
                        str::stream() << "Write results unavailable "
                                      << (response.shardHostAndPort
                                              ? "from "
                                              : "from failing to target a host in the shard ")
                                      << shardInfo);

                    batchOp.noteBatchError(*batch, errorFromStatus(status));

                    LOGV2_DEBUG(22908,
                                4,
                                "Unable to receive write results from {shardInfo}: {error}",
                                "Unable to receive write results",
                                "shardInfo"_attr = shardInfo,
                                "error"_attr = redact(status));

                    // If we are in a transaction, we must stop immediately (even for unordered).
                    if (TransactionRouter::get(opCtx)) {
                        batchOp.forgetTargetedBatchesOnTransactionAbortingError();

                        // Throw when there is a transient transaction error since this should be a
                        // top level error and not just a write error.
                        if (isTransientTransactionError(status.code(), false, false)) {
                            uassertStatusOK(status);
                        }

                        abortBatch = true;
                        break;
                    }
                }
            }
        }

        ++rounds;
        ++stats->numRounds;

        // If we're done, get out
        if (batchOp.isFinished())
            break;

        // MORE WORK TO DO

        //
        // Refresh the targeter if we need to (no-op if nothing stale)
        //

        bool targeterChanged = false;
        try {
            LOGV2_DEBUG_OPTIONS(4817406,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Starting post-migration commit refresh on the router");
            targeterChanged = targeter.refreshIfNeeded(opCtx);
            LOGV2_DEBUG_OPTIONS(4817407,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Finished post-migration commit refresh on the router");
        } catch (const ExceptionFor<ErrorCodes::StaleEpoch>& ex) {
            LOGV2_DEBUG_OPTIONS(4817408,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Finished post-migration commit refresh on the router with error",
                                "error"_attr = redact(ex));
            batchOp.abortBatch(errorFromStatus(
                ex.toStatus("collection was dropped in the middle of the operation")));
            break;
        } catch (const DBException& ex) {
            LOGV2_DEBUG_OPTIONS(4817409,
                                2,
                                {logv2::LogComponent::kShardMigrationPerf},
                                "Finished post-migration commit refresh on the router with error",
                                "error"_attr = redact(ex));
            // It's okay if we can't refresh, we'll just record errors for the ops if needed
            LOGV2_WARNING(22911,
                          "Could not refresh targeter due to {error}",
                          "Could not refresh targeter",
                          "error"_attr = redact(ex));
        }

        //
        // Ensure progress is being made toward completing the batch op
        //

        int currCompletedOps = batchOp.numWriteOpsIn(WriteOpState_Completed);
        if (currCompletedOps == numCompletedOps && !targeterChanged) {
            ++numRoundsWithoutProgress;
        } else {
            numRoundsWithoutProgress = 0;
        }
        numCompletedOps = currCompletedOps;

        if (numRoundsWithoutProgress > kMaxRoundsWithoutProgress) {
            batchOp.abortBatch(errorFromStatus(
                {ErrorCodes::NoProgressMade,
                 str::stream() << "no progress was made executing batch write op in "
                               << clientRequest.getNS().ns() << " after "
                               << kMaxRoundsWithoutProgress << " rounds (" << numCompletedOps
                               << " ops completed in " << rounds << " rounds total)"}));
            break;
        }
    }

    auto nShardsOwningChunks = batchOp.getNShardsOwningChunks();
    if (nShardsOwningChunks)
        stats->noteNumShardsOwningChunks(*nShardsOwningChunks);

    batchOp.buildClientResponse(clientResponse);

    LOGV2_DEBUG(22910,
                4,
                "Finished execution of write batch. Execution {succeededOrFailed}, writeConcern "
                "{wcSucceededOrFailed} for namespace {namespace}",
                "Finished execution of write batch",
                "succeededOrFailed"_attr =
                    (clientResponse->isErrDetailsSet() ? "failed" : "succeeded"),
                "wcSucceededOrFailed"_attr =
                    (clientResponse->isWriteConcernErrorSet() ? "failed" : "succeeded"),
                "namespace"_attr = clientRequest.getNS());
}

void BatchWriteExecStats::noteTargetedShard(const ShardId& shardId) {
    _targetedShards.insert(shardId);
}

void BatchWriteExecStats::noteWriteAt(const HostAndPort& host,
                                      repl::OpTime opTime,
                                      const OID& electionId) {
    _writeOpTimes[ConnectionString(host)] = HostOpTime(opTime, electionId);
}

void BatchWriteExecStats::noteNumShardsOwningChunks(const int nShardsOwningChunks) {
    _numShardsOwningChunks.emplace(nShardsOwningChunks);
}

const std::set<ShardId>& BatchWriteExecStats::getTargetedShards() const {
    return _targetedShards;
}

const HostOpTimeMap& BatchWriteExecStats::getWriteOpTimes() const {
    return _writeOpTimes;
}

const boost::optional<int> BatchWriteExecStats::getNumShardsOwningChunks() const {
    return _numShardsOwningChunks;
}

}  // namespace mongo
