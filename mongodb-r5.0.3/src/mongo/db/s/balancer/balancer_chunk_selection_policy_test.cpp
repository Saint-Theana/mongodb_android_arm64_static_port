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

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/balancer/migration_test_fixture.h"
#include "mongo/platform/random.h"
#include "mongo/s/type_collection_timeseries_fields_gen.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const std::string kDbName = "TestDb";
const NamespaceString kNamespace(kDbName, "TestColl");
const int kSizeOnDisk = 1;

class BalancerChunkSelectionTest : public MigrationTestFixture {
protected:
    BalancerChunkSelectionTest()
        : _random(std::random_device{}()),
          _clusterStats(std::make_unique<ClusterStatisticsImpl>(_random)),
          _chunkSelectionPolicy(
              std::make_unique<BalancerChunkSelectionPolicyImpl>(_clusterStats.get(), _random)) {}

    /**
     * Sets up mock network to expect a listDatabases command and returns a BSON response with
     * a dummy sizeOnDisk.
     */
    void expectListDatabasesCommand() {
        BSONObjBuilder resultBuilder;
        CommandHelpers::appendCommandStatusNoThrow(resultBuilder, Status::OK());

        onCommand([&resultBuilder](const RemoteCommandRequest& request) {
            ASSERT(request.cmdObj["listDatabases"]);
            std::vector<BSONObj> dbInfos;
            BSONObjBuilder b;
            b.append("name", kDbName);
            b.append("sizeOnDisk", kSizeOnDisk);
            b.append("empty", kSizeOnDisk > 0);
            resultBuilder.append("databases", dbInfos);
            resultBuilder.append("totalSize", kSizeOnDisk);
            return resultBuilder.obj();
        });
    }

    /**
     * Sets up mock network to expect a serverStatus command and returns a BSON response with
     * a dummy version.
     */
    void expectServerStatusCommand() {
        BSONObjBuilder resultBuilder;
        CommandHelpers::appendCommandStatusNoThrow(resultBuilder, Status::OK());

        onCommand([&resultBuilder](const RemoteCommandRequest& request) {
            ASSERT(request.cmdObj["serverStatus"]);
            resultBuilder.append("version", "MONGO_VERSION");
            return resultBuilder.obj();
        });
    }

    /**
     * Sets up mock network for all the shards to expect the commands executed for computing cluster
     * stats, which include listDatabase and serverStatus.
     */
    void expectGetStatsCommands(int numShards) {
        for (int i = 0; i < numShards; i++) {
            expectListDatabasesCommand();
            expectServerStatusCommand();
        }
    }

    /**
     * Returns a new BSON object with the tags appended.
     */
    BSONObj appendTags(const BSONObj shardBSON, std::vector<std::string> tags) {
        BSONObjBuilder appendedShardBSON(shardBSON);
        BSONArrayBuilder tagsBuilder;
        for (auto& tag : tags) {
            tagsBuilder.append(tag);
        }
        tagsBuilder.done();
        appendedShardBSON.append("tags", tagsBuilder.arr());
        return appendedShardBSON.obj();
    }

    BalancerRandomSource _random;
    std::unique_ptr<ClusterStatistics> _clusterStats;
    std::unique_ptr<BalancerChunkSelectionPolicy> _chunkSelectionPolicy;
};

TEST_F(BalancerChunkSelectionTest, TagRangesOverlap) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard1, kMajorityWriteConcern));

    // Set up a database and a sharded collection in the metadata.
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));
    setUpDatabase(kDbName, kShardId0);
    setUpCollection(kNamespace, collUUID, version);

    // Set up one chunk for the collection in the metadata.
    ChunkType chunk = setUpChunk(
        kNamespace, collUUID, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    auto assertRangeOverlapConflictWhenMoveChunk =
        [this, &chunk](const StringMap<ChunkRange>& tagChunkRanges) {
            // Set up two zones whose ranges overlap.
            setUpTags(kNamespace, tagChunkRanges);

            auto future = launchAsync([this, &chunk] {
                ThreadClient tc(getServiceContext());
                auto opCtx = Client::getCurrent()->makeOperationContext();

                // Requesting chunks to be relocated requires running commands on each shard to get
                // shard statistics. Set up dummy hosts for the source shards.
                shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
                shardTargeterMock(opCtx.get(), kShardId1)->setFindHostReturnValue(kShardHost1);

                auto migrateInfoStatus =
                    _chunkSelectionPolicy.get()->selectSpecificChunkToMove(opCtx.get(), chunk);
                ASSERT_EQUALS(ErrorCodes::RangeOverlapConflict,
                              migrateInfoStatus.getStatus().code());
            });

            expectGetStatsCommands(2);
            future.default_timed_get();
            removeAllTags(kNamespace);
        };

    assertRangeOverlapConflictWhenMoveChunk(
        {{"A", {kKeyPattern.globalMin(), BSON(kPattern << -10)}},
         {"B", {BSON(kPattern << -15), kKeyPattern.globalMax()}}});
    assertRangeOverlapConflictWhenMoveChunk(
        {{"A", {kKeyPattern.globalMin(), BSON(kPattern << -5)}},
         {"B", {BSON(kPattern << -10), kKeyPattern.globalMax()}}});
    assertRangeOverlapConflictWhenMoveChunk(
        {{"A", {kKeyPattern.globalMin(), kKeyPattern.globalMax()}},
         {"B", {BSON(kPattern << -15), kKeyPattern.globalMax()}}});
}

TEST_F(BalancerChunkSelectionTest, TagRangeMaxNotAlignedWithChunkMax) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    ShardType::ConfigNS,
                                                    appendTags(kShard0, {"A"}),
                                                    kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    ShardType::ConfigNS,
                                                    appendTags(kShard1, {"A"}),
                                                    kMajorityWriteConcern));

    // Set up a database and a sharded collection in the metadata.
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));
    setUpDatabase(kDbName, kShardId0);
    setUpCollection(kNamespace, collUUID, version);

    // Set up the zone.
    setUpTags(kNamespace, {{"A", {kKeyPattern.globalMin(), BSON(kPattern << -10)}}});

    auto assertErrorWhenMoveChunk = [this, &version, &collUUID](
                                        const std::vector<ChunkRange>& chunkRanges) {
        // Give shard0 all the chunks so the cluster is imbalanced.
        for (const auto& chunkRange : chunkRanges) {
            setUpChunk(
                kNamespace, collUUID, chunkRange.getMin(), chunkRange.getMax(), kShardId0, version);
            version.incMinor();
        }

        auto future = launchAsync([this] {
            ThreadClient tc(getServiceContext());
            auto opCtx = Client::getCurrent()->makeOperationContext();

            // Requests chunks to be relocated requires running commands on each shard to
            // get shard statistics. Set up dummy hosts for the source shards.
            shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
            shardTargeterMock(opCtx.get(), kShardId1)->setFindHostReturnValue(kShardHost1);

            auto candidateChunksStatus =
                _chunkSelectionPolicy.get()->selectChunksToMove(opCtx.get());
            ASSERT_OK(candidateChunksStatus.getStatus());

            // The balancer does not bubble up the IllegalOperation error, but it is expected
            // to postpone the balancing work for the zones with the error until the chunks
            // are split appropriately.
            ASSERT_EQUALS(0U, candidateChunksStatus.getValue().size());
        });

        expectGetStatsCommands(2);
        future.default_timed_get();
        removeAllChunks(kNamespace);
    };

    assertErrorWhenMoveChunk({{kKeyPattern.globalMin(), BSON(kPattern << -5)},
                              {BSON(kPattern << -5), kKeyPattern.globalMax()}});
    assertErrorWhenMoveChunk({{kKeyPattern.globalMin(), BSON(kPattern << -15)},
                              {BSON(kPattern << -15), kKeyPattern.globalMax()}});
}

TEST_F(BalancerChunkSelectionTest, ShardedTimeseriesCollectionsCannotBeAutoSplitted) {
    // Set up two shards in the metadata, each one with its own tag
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    ShardType::ConfigNS,
                                                    appendTags(kShard0, {"A"}),
                                                    kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    ShardType::ConfigNS,
                                                    appendTags(kShard1, {"B"}),
                                                    kMajorityWriteConcern));

    // Set up a database and a sharded collection in the metadata.
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));
    setUpDatabase(kDbName, kShardId0);
    setUpCollection(kNamespace, collUUID, version, TypeCollectionTimeseriesFields("fieldName"));

    // Set up two zones
    setUpTags(kNamespace,
              {
                  {"A", {kKeyPattern.globalMin(), BSON(kPattern << 0)}},
                  {"B", {BSON(kPattern << 0), kKeyPattern.globalMax()}},
              });

    // Create just one chunk covering the whole space
    setUpChunk(
        kNamespace, collUUID, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    auto future = launchAsync([this] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();

        // Requests chunks to be relocated requires running commands on each shard to
        // get shard statistics. Set up dummy hosts for the source shards.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId1)->setFindHostReturnValue(kShardHost1);

        auto candidateChunksStatus = _chunkSelectionPolicy.get()->selectChunksToSplit(opCtx.get());
        ASSERT_OK(candidateChunksStatus.getStatus());

        // No chunks to split since the coll is a sharded time-series collection
        ASSERT_EQUALS(0U, candidateChunksStatus.getValue().size());
    });

    expectGetStatsCommands(2);
    future.default_timed_get();
}

TEST_F(BalancerChunkSelectionTest, ShardedTimeseriesCollectionsCannotBeBalanced) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard1, kMajorityWriteConcern));

    // Set up a database and a sharded collection in the metadata.
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));
    setUpDatabase(kDbName, kShardId0);
    setUpCollection(kNamespace, collUUID, version, TypeCollectionTimeseriesFields("fieldName"));

    auto addChunk = [&](const BSONObj& min, const BSONObj& max) {
        setUpChunk(kNamespace, collUUID, min, max, kShardId0, version);
        version.incMinor();
    };

    addChunk(kKeyPattern.globalMin(), BSON(kPattern << 0));
    for (int i = 1; i <= 100; ++i) {
        addChunk(BSON(kPattern << (i - 1)), BSON(kPattern << i));
    }
    addChunk(BSON(kPattern << 100), kKeyPattern.globalMax());

    auto future = launchAsync([this] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();

        // Requests chunks to be relocated requires running commands on each shard to
        // get shard statistics. Set up dummy hosts for the source shards.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId1)->setFindHostReturnValue(kShardHost1);

        auto candidateChunksStatus = _chunkSelectionPolicy.get()->selectChunksToMove(opCtx.get());
        ASSERT_OK(candidateChunksStatus.getStatus());

        // No chunks to move since the coll is a sharded time-series collection
        ASSERT_EQUALS(0, candidateChunksStatus.getValue().size());
    });

    expectGetStatsCommands(2);
    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
