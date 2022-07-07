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

#include "mongo/db/s/balancer/balancer_policy.h"

#include <random>

#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(balancerShouldReturnRandomMigrations);

using std::map;
using std::numeric_limits;
using std::set;
using std::string;
using std::vector;

namespace {

// This value indicates the minimum deviation shard's number of chunks need to have from the
// optimal average across all shards for a zone for a rebalancing migration to be initiated.
const size_t kDefaultImbalanceThreshold = 1;

}  // namespace

DistributionStatus::DistributionStatus(NamespaceString nss, ShardToChunksMap shardToChunksMap)
    : _nss(std::move(nss)), _shardChunks(std::move(shardToChunksMap)) {}

size_t DistributionStatus::totalChunks() const {
    size_t total = 0;

    for (const auto& shardChunk : _shardChunks) {
        total += shardChunk.second.size();
    }

    return total;
}

size_t DistributionStatus::totalChunksWithTag(const std::string& tag) const {
    size_t total = 0;

    for (const auto& shardChunk : _shardChunks) {
        total += numberOfChunksInShardWithTag(shardChunk.first, tag);
    }

    return total;
}

size_t DistributionStatus::numberOfChunksInShard(const ShardId& shardId) const {
    const auto& shardChunks = getChunks(shardId);
    return shardChunks.size();
}

size_t DistributionStatus::numberOfChunksInShardWithTag(const ShardId& shardId,
                                                        const string& tag) const {
    const auto& shardChunks = getChunks(shardId);

    size_t total = 0;

    for (const auto& chunk : shardChunks) {
        if (tag == getTagForChunk(chunk)) {
            total++;
        }
    }

    return total;
}

const vector<ChunkType>& DistributionStatus::getChunks(const ShardId& shardId) const {
    ShardToChunksMap::const_iterator i = _shardChunks.find(shardId);
    invariant(i != _shardChunks.end());

    return i->second;
}

Status DistributionStatus::addRangeToZone(const ZoneRange& range) {
    return _zoneInfo.addRangeToZone(range);
}

string DistributionStatus::getTagForChunk(const ChunkType& chunk) const {
    return _zoneInfo.getZoneForChunk(chunk.getRange());
}

ZoneInfo::ZoneInfo()
    : _zoneRanges(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<ZoneRange>()) {}

Status ZoneInfo::addRangeToZone(const ZoneRange& range) {
    const auto minIntersect = _zoneRanges.upper_bound(range.min);
    const auto maxIntersect = _zoneRanges.upper_bound(range.max);

    // Check for partial overlap
    if (minIntersect != maxIntersect) {
        invariant(minIntersect != _zoneRanges.end());
        const auto& intersectingRange =
            (SimpleBSONObjComparator::kInstance.evaluate(minIntersect->second.min < range.max))
            ? minIntersect->second
            : maxIntersect->second;

        if (SimpleBSONObjComparator::kInstance.evaluate(intersectingRange.min == range.min) &&
            SimpleBSONObjComparator::kInstance.evaluate(intersectingRange.max == range.max) &&
            intersectingRange.zone == range.zone) {
            return Status::OK();
        }

        return {ErrorCodes::RangeOverlapConflict,
                str::stream() << "Zone range: " << range.toString()
                              << " is overlapping with existing: " << intersectingRange.toString()};
    }

    // Check for containment
    if (minIntersect != _zoneRanges.end()) {
        const ZoneRange& nextRange = minIntersect->second;
        if (SimpleBSONObjComparator::kInstance.evaluate(range.max > nextRange.min)) {
            invariant(SimpleBSONObjComparator::kInstance.evaluate(range.max < nextRange.max));
            return {ErrorCodes::RangeOverlapConflict,
                    str::stream() << "Zone range: " << range.toString()
                                  << " is overlapping with existing: " << nextRange.toString()};
        }
    }

    // This must be a new entry
    _zoneRanges.emplace(range.max.getOwned(), range);
    _allZones.insert(range.zone);
    return Status::OK();
}

string ZoneInfo::getZoneForChunk(const ChunkRange& chunk) const {
    const auto minIntersect = _zoneRanges.upper_bound(chunk.getMin());
    const auto maxIntersect = _zoneRanges.lower_bound(chunk.getMax());

    // We should never have a partial overlap with a chunk range. If it happens, treat it as if this
    // chunk doesn't belong to a tag
    if (minIntersect != maxIntersect) {
        return "";
    }

    if (minIntersect == _zoneRanges.end()) {
        return "";
    }

    const ZoneRange& intersectRange = minIntersect->second;

    // Check for containment
    if (SimpleBSONObjComparator::kInstance.evaluate(intersectRange.min <= chunk.getMin()) &&
        SimpleBSONObjComparator::kInstance.evaluate(chunk.getMax() <= intersectRange.max)) {
        return intersectRange.zone;
    }

    return "";
}

void DistributionStatus::report(BSONObjBuilder* builder) const {
    builder->append("ns", _nss.ns());

    // Report all shards
    BSONArrayBuilder shardArr(builder->subarrayStart("shards"));
    for (const auto& shardChunk : _shardChunks) {
        BSONObjBuilder shardEntry(shardArr.subobjStart());
        shardEntry.append("name", shardChunk.first.toString());

        BSONArrayBuilder chunkArr(shardEntry.subarrayStart("chunks"));
        for (const auto& chunk : shardChunk.second) {
            chunkArr.append(chunk.toConfigBSON());
        }
        chunkArr.doneFast();

        shardEntry.doneFast();
    }
    shardArr.doneFast();

    // Report all tags
    BSONArrayBuilder tagsArr(builder->subarrayStart("tags"));
    tagsArr.append(_zoneInfo.allZones());
    tagsArr.doneFast();

    // Report all tag ranges
    BSONArrayBuilder tagRangesArr(builder->subarrayStart("tagRanges"));
    for (const auto& tagRange : _zoneInfo.zoneRanges()) {
        BSONObjBuilder tagRangeEntry(tagRangesArr.subobjStart());
        tagRangeEntry.append("tag", tagRange.second.zone);
        tagRangeEntry.append("mapKey", tagRange.first);
        tagRangeEntry.append("min", tagRange.second.min);
        tagRangeEntry.append("max", tagRange.second.max);
        tagRangeEntry.doneFast();
    }
    tagRangesArr.doneFast();
}

string DistributionStatus::toString() const {
    BSONObjBuilder builder;
    report(&builder);

    return builder.obj().toString();
}

Status BalancerPolicy::isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                               const string& chunkTag) {
    if (stat.isSizeMaxed()) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " has reached its maximum storage size."};
    }

    if (stat.isDraining) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is currently draining."};
    }

    if (!chunkTag.empty() && !stat.shardTags.count(chunkTag)) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is not in the correct zone " << chunkTag};
    }

    return Status::OK();
}

ShardId BalancerPolicy::_getLeastLoadedReceiverShard(const ShardStatisticsVector& shardStats,
                                                     const DistributionStatus& distribution,
                                                     const string& tag,
                                                     const set<ShardId>& excludedShards) {
    ShardId best;
    unsigned minChunks = numeric_limits<unsigned>::max();

    for (const auto& stat : shardStats) {
        if (excludedShards.count(stat.shardId))
            continue;

        auto status = isShardSuitableReceiver(stat, tag);
        if (!status.isOK()) {
            continue;
        }

        unsigned myChunks = distribution.numberOfChunksInShard(stat.shardId);
        if (myChunks >= minChunks) {
            continue;
        }

        best = stat.shardId;
        minChunks = myChunks;
    }

    return best;
}

ShardId BalancerPolicy::_getMostOverloadedShard(const ShardStatisticsVector& shardStats,
                                                const DistributionStatus& distribution,
                                                const string& chunkTag,
                                                const set<ShardId>& excludedShards) {
    ShardId worst;
    unsigned maxChunks = 0;

    for (const auto& stat : shardStats) {
        if (excludedShards.count(stat.shardId))
            continue;

        const unsigned shardChunkCount =
            distribution.numberOfChunksInShardWithTag(stat.shardId, chunkTag);
        if (shardChunkCount <= maxChunks)
            continue;

        worst = stat.shardId;
        maxChunks = shardChunkCount;
    }

    return worst;
}

// Returns a random integer in [0, max) using a uniform random distribution.
int getRandomIndex(int max) {
    std::default_random_engine gen(time(nullptr));
    std::uniform_int_distribution<int> dist(0, max - 1);

    return dist(gen);
}

// Iterates through the shardStats vector starting from index until it finds an element that has > 0
// chunks. It will wrap around at the end and stop at the starting index. If no shards have chunks,
// it will return the original index value.
int getNextShardWithChunks(const ShardStatisticsVector& shardStats,
                           const DistributionStatus& distribution,
                           int index) {
    int retIndex = index;

    while (distribution.numberOfChunksInShard(shardStats[retIndex].shardId) == 0) {
        retIndex = (retIndex + 1) % shardStats.size();

        if (retIndex == index)
            return index;
    }

    return retIndex;
}

// Returns a randomly chosen pair of source -> destination shards for testing.
// The random pair is chosen by the following algorithm:
//  - create an array of indices with values [0, n)
//  - select a random index from this set
//  - advance the chosen index until we encounter a shard with chunks to move
//  - remove the chosen index from the set by swapping it with the last element
//  - select the destination index from the remaining indices
MigrateInfo chooseRandomMigration(const ShardStatisticsVector& shardStats,
                                  const DistributionStatus& distribution) {
    std::vector<int> indices(shardStats.size());

    int i = 0;
    std::generate(indices.begin(), indices.end(), [&i] { return i++; });

    int choice = getRandomIndex(indices.size());

    const int sourceIndex = getNextShardWithChunks(shardStats, distribution, indices[choice]);
    const auto& sourceShardId = shardStats[sourceIndex].shardId;
    std::swap(indices[sourceIndex], indices[indices.size() - 1]);

    choice = getRandomIndex(indices.size() - 1);
    const int destIndex = indices[choice];
    const auto& destShardId = shardStats[destIndex].shardId;

    LOGV2_DEBUG(21880,
                1,
                "balancerShouldReturnRandomMigrations: source: {fromShardId} dest: {toShardId}",
                "balancerShouldReturnRandomMigrations",
                "fromShardId"_attr = sourceShardId,
                "toShardId"_attr = destShardId);

    const auto& chunks = distribution.getChunks(sourceShardId);

    return {destShardId,
            chunks[getRandomIndex(chunks.size())],
            MoveChunkRequest::ForceJumbo::kDoNotForce,
            MigrateInfo::chunksImbalance};
}

vector<MigrateInfo> BalancerPolicy::balance(const ShardStatisticsVector& shardStats,
                                            const DistributionStatus& distribution,
                                            std::set<ShardId>* usedShards,
                                            bool forceJumbo) {
    vector<MigrateInfo> migrations;

    if (MONGO_unlikely(balancerShouldReturnRandomMigrations.shouldFail()) &&
        !distribution.nss().isConfigDB()) {
        LOGV2_DEBUG(21881, 1, "balancerShouldReturnRandomMigrations failpoint is set");

        if (shardStats.size() < 2)
            return migrations;

        migrations.push_back(chooseRandomMigration(shardStats, distribution));

        return migrations;
    }

    // 1) Check for shards, which are in draining mode
    {
        for (const auto& stat : shardStats) {
            if (!stat.isDraining)
                continue;

            if (usedShards->count(stat.shardId))
                continue;

            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);

            if (chunks.empty())
                continue;

            // Now we know we need to move to chunks off this shard, but only if permitted by the
            // tags policy
            unsigned numJumboChunks = 0;

            // Since we have to move all chunks, lets just do in order
            for (const auto& chunk : chunks) {
                if (chunk.getJumbo()) {
                    numJumboChunks++;
                    continue;
                }

                const string tag = distribution.getTagForChunk(chunk);

                const ShardId to =
                    _getLeastLoadedReceiverShard(shardStats, distribution, tag, *usedShards);
                if (!to.isValid()) {
                    if (migrations.empty()) {
                        LOGV2_WARNING(21889,
                                      "Chunk {chunk} is on a draining shard, but no appropriate "
                                      "recipient found",
                                      "Chunk is on a draining shard, but no appropriate "
                                      "recipient found",
                                      "chunk"_attr = redact(chunk.toString()));
                    }
                    continue;
                }

                invariant(to != stat.shardId);
                migrations.emplace_back(
                    to, chunk, MoveChunkRequest::ForceJumbo::kForceBalancer, MigrateInfo::drain);
                invariant(usedShards->insert(stat.shardId).second);
                invariant(usedShards->insert(to).second);
                break;
            }

            if (migrations.empty()) {
                LOGV2_WARNING(21890,
                              "Unable to find any chunk to move from draining shard "
                              "{shardId}. numJumboChunks: {numJumboChunks}",
                              "Unable to find any chunk to move from draining shard",
                              "shardId"_attr = stat.shardId,
                              "numJumboChunks"_attr = numJumboChunks);
            }
        }
    }

    // 2) Check for chunks, which are on the wrong shard and must be moved off of it
    if (!distribution.tags().empty()) {
        for (const auto& stat : shardStats) {
            if (usedShards->count(stat.shardId))
                continue;

            const vector<ChunkType>& chunks = distribution.getChunks(stat.shardId);

            for (const auto& chunk : chunks) {
                const string tag = distribution.getTagForChunk(chunk);

                if (tag.empty())
                    continue;

                if (stat.shardTags.count(tag))
                    continue;

                if (chunk.getJumbo()) {
                    LOGV2_WARNING(
                        21891,
                        "Chunk {chunk} violates zone {zone}, but it is jumbo and cannot be moved",
                        "Chunk violates zone, but it is jumbo and cannot be moved",
                        "chunk"_attr = redact(chunk.toString()),
                        "zone"_attr = redact(tag));
                    continue;
                }

                const ShardId to =
                    _getLeastLoadedReceiverShard(shardStats, distribution, tag, *usedShards);
                if (!to.isValid()) {
                    if (migrations.empty()) {
                        LOGV2_WARNING(21892,
                                      "Chunk {chunk} violates zone {zone}, but no appropriate "
                                      "recipient found",
                                      "Chunk violates zone, but no appropriate recipient found",
                                      "chunk"_attr = redact(chunk.toString()),
                                      "zone"_attr = redact(tag));
                    }
                    continue;
                }

                invariant(to != stat.shardId);
                migrations.emplace_back(to,
                                        chunk,
                                        forceJumbo ? MoveChunkRequest::ForceJumbo::kForceBalancer
                                                   : MoveChunkRequest::ForceJumbo::kDoNotForce,
                                        MigrateInfo::zoneViolation);
                invariant(usedShards->insert(stat.shardId).second);
                invariant(usedShards->insert(to).second);
                break;
            }
        }
    }

    // 3) for each tag balance

    vector<string> tagsPlusEmpty(distribution.tags().begin(), distribution.tags().end());
    tagsPlusEmpty.push_back("");

    for (const auto& tag : tagsPlusEmpty) {
        const size_t totalNumberOfChunksWithTag =
            (tag.empty() ? distribution.totalChunks() : distribution.totalChunksWithTag(tag));

        size_t totalNumberOfShardsWithTag = 0;

        for (const auto& stat : shardStats) {
            if (tag.empty() || stat.shardTags.count(tag)) {
                totalNumberOfShardsWithTag++;
            }
        }

        // Skip zones which have no shards assigned to them. This situation is not harmful, but
        // should not be possible so warn the operator to correct it.
        if (totalNumberOfShardsWithTag == 0) {
            if (!tag.empty()) {
                LOGV2_WARNING(
                    21893,
                    "Zone {zone} in collection {namespace} has no assigned shards and chunks "
                    "which fall into it cannot be balanced. This should be corrected by either "
                    "assigning shards to the zone or by deleting it.",
                    "Zone in collection has no assigned shards and chunks which fall into it "
                    "cannot be balanced. This should be corrected by either assigning shards "
                    "to the zone or by deleting it.",
                    "zone"_attr = redact(tag),
                    "namespace"_attr = distribution.nss());
            }
            continue;
        }

        // Calculate the rounded optimal number of chunks per shard
        const size_t idealNumberOfChunksPerShardForTag =
            (size_t)std::roundf(totalNumberOfChunksWithTag / (float)totalNumberOfShardsWithTag);

        while (_singleZoneBalance(shardStats,
                                  distribution,
                                  tag,
                                  idealNumberOfChunksPerShardForTag,
                                  &migrations,
                                  usedShards,
                                  forceJumbo ? MoveChunkRequest::ForceJumbo::kForceBalancer
                                             : MoveChunkRequest::ForceJumbo::kDoNotForce))
            ;
    }

    return migrations;
}

boost::optional<MigrateInfo> BalancerPolicy::balanceSingleChunk(
    const ChunkType& chunk,
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution) {
    const string tag = distribution.getTagForChunk(chunk);

    ShardId newShardId =
        _getLeastLoadedReceiverShard(shardStats, distribution, tag, set<ShardId>());
    if (!newShardId.isValid() || newShardId == chunk.getShard()) {
        return boost::optional<MigrateInfo>();
    }

    return MigrateInfo(
        newShardId, chunk, MoveChunkRequest::ForceJumbo::kDoNotForce, MigrateInfo::chunksImbalance);
}

bool BalancerPolicy::_singleZoneBalance(const ShardStatisticsVector& shardStats,
                                        const DistributionStatus& distribution,
                                        const string& tag,
                                        size_t idealNumberOfChunksPerShardForTag,
                                        vector<MigrateInfo>* migrations,
                                        set<ShardId>* usedShards,
                                        MoveChunkRequest::ForceJumbo forceJumbo) {
    const ShardId from = _getMostOverloadedShard(shardStats, distribution, tag, *usedShards);
    if (!from.isValid())
        return false;

    const size_t max = distribution.numberOfChunksInShardWithTag(from, tag);

    // Do not use a shard if it already has less entries than the optimal per-shard chunk count
    if (max <= idealNumberOfChunksPerShardForTag)
        return false;

    const ShardId to = _getLeastLoadedReceiverShard(shardStats, distribution, tag, *usedShards);
    if (!to.isValid()) {
        if (migrations->empty()) {
            LOGV2(21882,
                  "No available shards to take chunks for zone {zone}",
                  "No available shards to take chunks for zone",
                  "zone"_attr = tag);
        }
        return false;
    }

    const size_t min = distribution.numberOfChunksInShardWithTag(to, tag);

    // Do not use a shard if it already has more entries than the optimal per-shard chunk count
    if (min >= idealNumberOfChunksPerShardForTag)
        return false;

    const size_t imbalance = max - idealNumberOfChunksPerShardForTag;

    LOGV2_DEBUG(
        21883,
        1,
        "collection: {namespace}, zone: {zone}, donor: {fromShardId} chunks on "
        " {fromShardChunkCount}, receiver: {toShardId} chunks on {toShardChunkCount}, "
        "ideal: {idealNumberOfChunksPerShardForTag}, threshold: {chunkCountImbalanceThreshold}",
        "Balancing single zone",
        "namespace"_attr = distribution.nss().ns(),
        "zone"_attr = tag,
        "fromShardId"_attr = from,
        "fromShardChunkCount"_attr = max,
        "toShardId"_attr = to,
        "toShardChunkCount"_attr = min,
        "idealNumberOfChunksPerShardForTag"_attr = idealNumberOfChunksPerShardForTag,
        "chunkCountImbalanceThreshold"_attr = kDefaultImbalanceThreshold);

    // Check whether it is necessary to balance within this zone
    if (imbalance < kDefaultImbalanceThreshold)
        return false;

    const vector<ChunkType>& chunks = distribution.getChunks(from);

    unsigned numJumboChunks = 0;

    for (const auto& chunk : chunks) {
        if (distribution.getTagForChunk(chunk) != tag)
            continue;

        if (chunk.getJumbo()) {
            numJumboChunks++;
            continue;
        }

        migrations->emplace_back(to, chunk, forceJumbo, MigrateInfo::chunksImbalance);
        invariant(usedShards->insert(chunk.getShard()).second);
        invariant(usedShards->insert(to).second);
        return true;
    }

    if (numJumboChunks) {
        LOGV2_WARNING(
            21894,
            "Shard: {shardId}, collection: {namespace} has only jumbo chunks for "
            "zone \'{zone}\' and cannot be balanced. Jumbo chunks count: {numJumboChunks}",
            "Shard has only jumbo chunks for and cannot be balanced",
            "shardId"_attr = from,
            "namespace"_attr = distribution.nss().ns(),
            "zone"_attr = tag,
            "numJumboChunks"_attr = numJumboChunks);
    }

    return false;
}

ZoneRange::ZoneRange(const BSONObj& a_min, const BSONObj& a_max, const std::string& _zone)
    : min(a_min.getOwned()), max(a_max.getOwned()), zone(_zone) {}

string ZoneRange::toString() const {
    return str::stream() << min << " -->> " << max << "  on  " << zone;
}

MigrateInfo::MigrateInfo(const ShardId& a_to,
                         const ChunkType& a_chunk,
                         const MoveChunkRequest::ForceJumbo a_forceJumbo,
                         MigrationReason a_reason) {
    invariant(a_chunk.validate());
    invariant(a_to.isValid());

    to = a_to;

    nss = a_chunk.getNS();
    from = a_chunk.getShard();
    minKey = a_chunk.getMin();
    maxKey = a_chunk.getMax();
    version = a_chunk.getVersion();
    forceJumbo = a_forceJumbo;
    reason = a_reason;
}

std::string MigrateInfo::getName() const {
    // Generates a unique name for a MigrateInfo based on the namespace and the lower bound of the
    // chunk being moved.
    StringBuilder buf;
    buf << nss.ns() << "-";

    BSONObjIterator i(minKey);
    while (i.more()) {
        BSONElement e = i.next();
        buf << e.fieldName() << "_" << e.toString(false, true);
    }

    return buf.str();
}

BSONObj MigrateInfo::getMigrationTypeQuery() const {
    // Generates a query object for a single MigrationType based on the namespace and the lower
    // bound of the chunk being moved.
    return BSON(MigrationType::ns(nss.ns()) << MigrationType::min(minKey));
}

string MigrateInfo::toString() const {
    return str::stream() << nss.ns() << ": [" << minKey << ", " << maxKey << "), from " << from
                         << ", to " << to;
}

}  // namespace mongo
