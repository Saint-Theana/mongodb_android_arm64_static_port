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

#include "mongo/db/s/chunk_splitter.h"

#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/query.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/chunk_split_state_driver.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/split_chunk.h"
#include "mongo/db/s/split_vector.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

/**
 * Constructs the default options for the thread pool used to schedule splits.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "ChunkSplitter";
    options.minThreads = 0;
    options.maxThreads = 20;

    // Ensure all threads have a client
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
        stdx::lock_guard<Client> lk(cc());
        cc().setSystemOperationKillableByStepdown(lk);
    };
    return options;
}

/**
 * Attempts to split the chunk described by min/maxKey at the split points provided.
 */
Status splitChunkAtMultiplePoints(OperationContext* opCtx,
                                  const ShardId& shardId,
                                  const NamespaceString& nss,
                                  const ShardKeyPattern& shardKeyPattern,
                                  const ChunkVersion& collectionVersion,
                                  const ChunkRange& chunkRange,
                                  const std::vector<BSONObj>& splitPoints) {
    invariant(!splitPoints.empty());

    const size_t kMaxSplitPoints = 8192;

    if (splitPoints.size() > kMaxSplitPoints) {
        return {ErrorCodes::BadValue,
                str::stream() << "Cannot split chunk in more than " << kMaxSplitPoints
                              << " parts at a time."};
    }

    return splitChunk(opCtx,
                      nss,
                      shardKeyPattern.toBSON(),
                      chunkRange,
                      splitPoints,
                      shardId.toString(),
                      collectionVersion.epoch())
        .getStatus()
        .withContext("split failed");
}

/**
 * Attempts to move the chunk specified by minKey away from its current shard.
 */
void moveChunk(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& minKey) {
    // We need to have the most up-to-date view of the chunk we are about to move.
    const auto cm =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    uassert(ErrorCodes::NamespaceNotSharded,
            "Could not move chunk. Collection is no longer sharded",
            cm.isSharded());

    const auto suggestedChunk = cm.findIntersectingChunkWithSimpleCollation(minKey);

    ChunkType chunkToMove;
    chunkToMove.setNS(nss);
    chunkToMove.setShard(suggestedChunk.getShardId());
    chunkToMove.setMin(suggestedChunk.getMin());
    chunkToMove.setMax(suggestedChunk.getMax());
    chunkToMove.setVersion(suggestedChunk.getLastmod());

    uassertStatusOK(configsvr_client::rebalanceChunk(opCtx, chunkToMove));
}

/**
 * Returns the split point that will result in one of the chunks having exactly one document. Also
 * returns an empty document if the split point cannot be determined.
 *
 * doSplitAtLower - determines which side of the split will have exactly one document. True means
 * that the split point chosen will be closer to the lower bound.
 *
 * NOTE: this assumes that the shard key is not "special"- that is, the shardKeyPattern is simply an
 * ordered list of ascending/descending field names. For example {a : 1, b : -1} is not special, but
 * {a : "hashed"} is.
 */
BSONObj findExtremeKeyForShard(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const ShardKeyPattern& shardKeyPattern,
                               bool doSplitAtLower) {
    Query q;

    if (doSplitAtLower) {
        q.sort(shardKeyPattern.toBSON());
    } else {
        // need to invert shard key pattern to sort backwards
        BSONObjBuilder r;

        BSONObjIterator i(shardKeyPattern.toBSON());
        while (i.more()) {
            BSONElement e = i.next();
            uassert(40617, "can only handle numbers here - which i think is correct", e.isNumber());
            r.append(e.fieldName(), -1 * e.number());
        }

        q.sort(r.obj());
    }

    DBDirectClient client(opCtx);

    BSONObj end;

    if (doSplitAtLower) {
        // Splitting close to the lower bound means that the split point will be the
        // upper bound. Chunk range upper bounds are exclusive so skip a document to
        // make the lower half of the split end up with a single document.
        std::unique_ptr<DBClientCursor> cursor = client.query(nss,
                                                              q,
                                                              1, /* nToReturn */
                                                              1 /* nToSkip */);

        uassert(40618,
                str::stream() << "failed to initialize cursor during auto split due to "
                              << "connection problem with " << client.getServerAddress(),
                cursor.get() != nullptr);

        if (cursor->more()) {
            end = cursor->next().getOwned();
        }
    } else {
        end = client.findOne(nss.ns(), q);
    }

    if (end.isEmpty()) {
        return BSONObj();
    }

    return shardKeyPattern.extractShardKeyFromDoc(end);
}

/**
 * Checks if autobalance is enabled on the current sharded collection.
 */
bool isAutoBalanceEnabled(OperationContext* opCtx,
                          const NamespaceString& nss,
                          BalancerConfiguration* balancerConfig) {
    if (!balancerConfig->shouldBalanceForAutoSplit())
        return false;

    try {
        return Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss).getAllowBalance();
    } catch (const DBException& ex) {
        LOGV2(21903,
              "Auto-split for {namespace} failed to load collection metadata: {error}",
              "Auto-split failed to load collection metadata",
              "namespace"_attr = nss,
              "error"_attr = redact(ex));
        return false;
    }
}

const auto getChunkSplitter = ServiceContext::declareDecoration<ChunkSplitter>();

}  // namespace

ChunkSplitter::ChunkSplitter() : _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

ChunkSplitter::~ChunkSplitter() {
    _threadPool.shutdown();
    _threadPool.join();
}

ChunkSplitter& ChunkSplitter::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ChunkSplitter& ChunkSplitter::get(ServiceContext* serviceContext) {
    return getChunkSplitter(serviceContext);
}

void ChunkSplitter::onShardingInitialization(bool isPrimary) {
    stdx::lock_guard<Latch> scopedLock(_mutex);
    _isPrimary = isPrimary;
}

void ChunkSplitter::onStepUp() {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_isPrimary) {
        return;
    }
    _isPrimary = true;

    LOGV2(21904, "The ChunkSplitter has started and will accept autosplit tasks");
}

void ChunkSplitter::onStepDown() {
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_isPrimary) {
        return;
    }
    _isPrimary = false;

    LOGV2(21905,
          "The ChunkSplitter has stopped and will no longer run new autosplit tasks. Any autosplit "
          "tasks that have already started will be allowed to finish");
}

void ChunkSplitter::waitForIdle() {
    _threadPool.waitForIdle();
}

void ChunkSplitter::trySplitting(std::shared_ptr<ChunkSplitStateDriver> chunkSplitStateDriver,
                                 const NamespaceString& nss,
                                 const BSONObj& min,
                                 const BSONObj& max,
                                 long dataWritten) {
    if (!_isPrimary) {
        return;
    }
    _threadPool.schedule(
        [ this, csd = std::move(chunkSplitStateDriver), nss, min, max,
          dataWritten ](auto status) noexcept {
            invariant(status);

            _runAutosplit(csd, nss, min, max, dataWritten);
        });
}

void ChunkSplitter::_runAutosplit(std::shared_ptr<ChunkSplitStateDriver> chunkSplitStateDriver,
                                  const NamespaceString& nss,
                                  const BSONObj& min,
                                  const BSONObj& max,
                                  long dataWritten) {
    if (!_isPrimary) {
        return;
    }

    try {
        const auto opCtx = cc().makeOperationContext();

        const auto cm = uassertStatusOK(
            Grid::get(opCtx.get())->catalogCache()->getCollectionRoutingInfo(opCtx.get(), nss));
        uassert(ErrorCodes::NamespaceNotSharded,
                "Could not split chunk. Collection is no longer sharded",
                cm.isSharded());

        // Best effort checks that the chunk we're splitting hasn't changed bounds or moved shards
        // since the auto split task was scheduled. Best effort because the chunk metadata may
        // change after this point.
        const auto chunk = cm.findIntersectingChunkWithSimpleCollation(min);
        uassert(4860100,
                "Chunk to be auto split has different boundaries than when the split was initiated",
                chunk.getRange() == ChunkRange(min, max));
        uassert(4860101,
                "Chunk to be auto split isn't owned by this shard",
                ShardingState::get(opCtx.get())->shardId() == chunk.getShardId());

        const auto& shardKeyPattern = cm.getShardKeyPattern();

        const auto balancerConfig = Grid::get(opCtx.get())->getBalancerConfiguration();
        // Ensure we have the most up-to-date balancer configuration
        uassertStatusOK(balancerConfig->refreshAndCheck(opCtx.get()));

        if (!balancerConfig->getShouldAutoSplit()) {
            return;
        }

        const uint64_t maxChunkSizeBytes = balancerConfig->getMaxChunkSizeBytes();

        LOGV2_DEBUG(21906,
                    1,
                    "about to initiate autosplit: {chunk} dataWritten since last check: "
                    "{dataWrittenBytes} maxChunkSizeBytes: {maxChunkSizeBytes}",
                    "about to initiate autosplit",
                    "chunk"_attr = redact(chunk.toString()),
                    "dataWrittenBytes"_attr = dataWritten,
                    "maxChunkSizeBytes"_attr = maxChunkSizeBytes);

        chunkSplitStateDriver->prepareSplit();
        auto splitPoints = splitVector(opCtx.get(),
                                       nss,
                                       shardKeyPattern.toBSON(),
                                       chunk.getMin(),
                                       chunk.getMax(),
                                       false,
                                       boost::none,
                                       boost::none,
                                       maxChunkSizeBytes);

        if (splitPoints.empty()) {
            LOGV2_DEBUG(21907,
                        1,
                        "ChunkSplitter attempted split but not enough split points were found for "
                        "chunk {chunk}",
                        "ChunkSplitter attempted split but not enough split points were found for "
                        "chunk",
                        "chunk"_attr = redact(chunk.toString()));
            // Reset our size estimate that we had prior to splitVector to 0, while still counting
            // the bytes that have been written in parallel to this split task
            chunkSplitStateDriver->abandonPrepare();
            return;
        }

        // We assume that if the chunk being split is the first (or last) one on the collection,
        // this chunk is likely to see more insertions. Instead of splitting mid-chunk, we use the
        // very first (or last) key as a split point.
        //
        // This heuristic is skipped for "special" shard key patterns that are not likely to produce
        // monotonically increasing or decreasing values (e.g. hashed shard keys).

        // Keeps track of the minKey of the top chunk after the split so we can migrate the chunk.
        BSONObj topChunkMinKey;
        const auto skpGlobalMin = shardKeyPattern.getKeyPattern().globalMin();
        const auto skpGlobalMax = shardKeyPattern.getKeyPattern().globalMax();
        if (KeyPattern::isOrderedKeyPattern(shardKeyPattern.toBSON())) {
            if (skpGlobalMin.woCompare(min) == 0) {
                // MinKey is infinity (This is the first chunk on the collection)
                BSONObj key = findExtremeKeyForShard(opCtx.get(), nss, shardKeyPattern, true);
                if (!key.isEmpty()) {
                    splitPoints.front() = key.getOwned();
                    topChunkMinKey = skpGlobalMin;
                }
            } else if (skpGlobalMax.woCompare(max) == 0) {
                // MaxKey is infinity (This is the last chunk on the collection)
                BSONObj key = findExtremeKeyForShard(opCtx.get(), nss, shardKeyPattern, false);
                if (!key.isEmpty()) {
                    splitPoints.back() = key.getOwned();
                    topChunkMinKey = key.getOwned();
                }
            }
        }

        uassertStatusOK(splitChunkAtMultiplePoints(opCtx.get(),
                                                   chunk.getShardId(),
                                                   nss,
                                                   shardKeyPattern,
                                                   cm.getVersion(),
                                                   chunk.getRange(),
                                                   splitPoints));
        chunkSplitStateDriver->commitSplit();

        const bool shouldBalance = isAutoBalanceEnabled(opCtx.get(), nss, balancerConfig);

        LOGV2(21908,
              "autosplitted {namespace} chunk: {chunk} with {splitPoints} split points "
              "(maxChunkSizeBytes: {maxChunkSizeBytes}). {extraInfo}",
              "autosplitted chunk",
              "namespace"_attr = nss,
              "chunk"_attr = redact(chunk.toString()),
              "splitPoints"_attr = splitPoints.size(),
              "maxChunkSizeBytes"_attr = maxChunkSizeBytes,
              "extraInfo"_attr =
                  (topChunkMinKey.isEmpty() ? ""
                                            : "top chunk migration suggested" +
                           (std::string)(shouldBalance ? "" : ", but no migrations allowed)")));

        // Balance the resulting chunks if the autobalance option is enabled and if we split at the
        // first or last chunk on the collection as part of top chunk optimization.
        if (!shouldBalance || topChunkMinKey.isEmpty()) {
            return;
        }

        try {
            // Tries to move the top chunk out of the shard to prevent the hot
            // spot from staying on a single shard. This is based on the
            // assumption that succeeding inserts will fall on the top chunk.
            moveChunk(opCtx.get(), nss, topChunkMinKey);
        } catch (const DBException& ex) {
            LOGV2(21909,
                  "Top-chunk optimization failed to move chunk {chunk} in collection "
                  "{namespace} after a successful split: {error}",
                  "Top-chunk optimization failed to move chunk after a successful split",
                  "chunk"_attr = redact(ChunkRange(min, max).toString()),
                  "namespace"_attr = nss,
                  "error"_attr = redact(ex.toStatus()));
        }
    } catch (const DBException& ex) {
        LOGV2(21910,
              "Unable to auto-split chunk {chunk} in namespace {namespace}: {error}",
              "Unable to auto-split chunk",
              "chunk"_attr = redact(ChunkRange(min, max).toString()),
              "namespace"_attr = nss,
              "error"_attr = redact(ex.toStatus()));
    }
}

}  // namespace mongo
