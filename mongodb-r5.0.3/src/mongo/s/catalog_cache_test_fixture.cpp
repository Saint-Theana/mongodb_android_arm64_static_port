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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog_cache_test_fixture.h"

#include <memory>
#include <set>
#include <vector>

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void CatalogCacheTestFixture::setUp() {
    ShardingTestFixture::setUp();
    setRemote(HostAndPort("FakeRemoteClient:34567"));
    configTargeter()->setFindHostReturnValue(kConfigHostAndPort);

    CollatorFactoryInterface::set(getServiceContext(), std::make_unique<CollatorFactoryMock>());
}

executor::NetworkTestEnv::FutureHandle<boost::optional<ChunkManager>>
CatalogCacheTestFixture::scheduleRoutingInfoForcedRefresh(const NamespaceString& nss) {
    return launchAsync([this, nss] {
        auto client = getServiceContext()->makeClient("Test");
        auto const catalogCache = Grid::get(getServiceContext())->catalogCache();

        return boost::make_optional(uassertStatusOK(
            catalogCache->getCollectionRoutingInfoWithRefresh(operationContext(), nss)));
    });
}

executor::NetworkTestEnv::FutureHandle<boost::optional<ChunkManager>>
CatalogCacheTestFixture::scheduleRoutingInfoUnforcedRefresh(const NamespaceString& nss) {
    return launchAsync([this, nss] {
        auto client = getServiceContext()->makeClient("Test");
        auto const catalogCache = Grid::get(getServiceContext())->catalogCache();

        return boost::optional<ChunkManager>(
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(operationContext(), nss)));
    });
}

executor::NetworkTestEnv::FutureHandle<boost::optional<ChunkManager>>
CatalogCacheTestFixture::scheduleRoutingInfoIncrementalRefresh(const NamespaceString& nss) {
    auto catalogCache = Grid::get(getServiceContext())->catalogCache();
    const auto cm =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(operationContext(), nss));
    ASSERT(cm.isSharded());

    // Simulates the shard wanting a higher version than the one sent by the router.
    catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
        nss, boost::none, cm.dbPrimary());

    return launchAsync([this, nss] {
        auto client = getServiceContext()->makeClient("Test");
        auto const catalogCache = Grid::get(getServiceContext())->catalogCache();

        return boost::make_optional(
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(operationContext(), nss)));
    });
}

std::vector<ShardType> CatalogCacheTestFixture::setupNShards(int numShards) {
    std::vector<ShardType> shards;
    for (int i = 0; i < numShards; i++) {
        ShardId name(str::stream() << i);
        HostAndPort host(str::stream() << "Host" << i << ":12345");

        ShardType shard;
        shard.setName(name.toString());
        shard.setHost(host.toString());
        shards.emplace_back(std::move(shard));

        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        targeter->setConnectionStringReturnValue(ConnectionString(host));
        targeter->setFindHostReturnValue(host);
        targeterFactory()->addTargeterToReturn(ConnectionString(host), std::move(targeter));
    }

    setupShards(shards);
    return shards;
}

ChunkManager CatalogCacheTestFixture::makeChunkManager(
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    const std::vector<BSONObj>& splitPoints,
    boost::optional<ReshardingFields> reshardingFields) {
    ChunkVersion version(1, 0, OID::gen(), boost::none /* timestamp */);

    const BSONObj databaseBSON = [&]() {
        DatabaseType db(nss.db().toString(), {"0"}, true, DatabaseVersion(UUID::gen()));
        return db.toBSON();
    }();

    const BSONObj collectionBSON = [&]() {
        CollectionType coll(nss, version.epoch(), Date_t::now(), UUID::gen());
        coll.setKeyPattern(shardKeyPattern.getKeyPattern());
        coll.setUnique(unique);

        if (defaultCollator) {
            coll.setDefaultCollation(defaultCollator->getSpec().toBSON());
        }

        if (reshardingFields) {
            coll.setReshardingFields(std::move(reshardingFields));
        }

        return coll.toBSON();
    }();

    std::vector<BSONObj> initialChunks;

    auto splitPointsIncludingEnds(splitPoints);
    splitPointsIncludingEnds.insert(splitPointsIncludingEnds.begin(),
                                    shardKeyPattern.getKeyPattern().globalMin());
    splitPointsIncludingEnds.push_back(shardKeyPattern.getKeyPattern().globalMax());

    for (size_t i = 1; i < splitPointsIncludingEnds.size(); ++i) {
        ChunkType chunk(
            nss,
            {shardKeyPattern.getKeyPattern().extendRangeBound(splitPointsIncludingEnds[i - 1],
                                                              false),
             shardKeyPattern.getKeyPattern().extendRangeBound(splitPointsIncludingEnds[i], false)},
            version,
            ShardId{str::stream() << (i - 1)});
        chunk.setName(OID::gen());

        initialChunks.push_back(chunk.toConfigBSON());

        version.incMajor();
    }

    setupNShards(initialChunks.size());

    auto future = scheduleRoutingInfoUnforcedRefresh(nss);

    expectFindSendBSONObjVector(kConfigHostAndPort, {databaseBSON});
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        std::vector<BSONObj> aggResult{collectionBSON};
        std::transform(initialChunks.begin(),
                       initialChunks.end(),
                       std::back_inserter(aggResult),
                       [](const auto& chunk) { return BSON("chunks" << chunk); });
        return aggResult;
    }());

    return *future.default_timed_get();
}

void CatalogCacheTestFixture::expectGetDatabase(NamespaceString nss, std::string shardId) {
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        DatabaseType db(nss.db().toString(), {shardId}, true, DatabaseVersion(UUID::gen()));
        return std::vector<BSONObj>{db.toBSON()};
    }());
}

void CatalogCacheTestFixture::expectGetCollection(NamespaceString nss,
                                                  OID epoch,
                                                  UUID uuid,
                                                  const ShardKeyPattern& shardKeyPattern) {
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        CollectionType collType(nss, epoch, Date_t::now(), uuid);
        collType.setKeyPattern(shardKeyPattern.toBSON());
        collType.setUnique(false);
        return std::vector<BSONObj>{collType.toBSON()};
    }());
}

void CatalogCacheTestFixture::expectCollectionAndChunksAggregation(
    NamespaceString nss,
    OID epoch,
    UUID uuid,
    const ShardKeyPattern& shardKeyPattern,
    const std::vector<ChunkType>& chunks) {
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        CollectionType collType(nss, epoch, Date_t::now(), uuid);
        collType.setKeyPattern(shardKeyPattern.toBSON());
        collType.setUnique(false);

        std::vector<BSONObj> aggResult{collType.toBSON()};
        std::transform(chunks.begin(),
                       chunks.end(),
                       std::back_inserter(aggResult),
                       [](const auto& chunk) { return BSON("chunks" << chunk.toConfigBSON()); });
        return aggResult;
    }());
}

ChunkManager CatalogCacheTestFixture::loadRoutingTableWithTwoChunksAndTwoShards(
    NamespaceString nss) {

    return loadRoutingTableWithTwoChunksAndTwoShardsImpl(nss, BSON("_id" << 1));
}

ChunkManager CatalogCacheTestFixture::loadRoutingTableWithTwoChunksAndTwoShardsHash(
    NamespaceString nss) {

    return loadRoutingTableWithTwoChunksAndTwoShardsImpl(nss,
                                                         BSON("_id"
                                                              << "hashed"));
}

ChunkManager CatalogCacheTestFixture::loadRoutingTableWithTwoChunksAndTwoShardsImpl(
    NamespaceString nss,
    const BSONObj& shardKey,
    boost::optional<std::string> primaryShardId,
    UUID uuid) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(shardKey);

    auto future = scheduleRoutingInfoForcedRefresh(nss);

    // Mock the expected config server queries.
    if (!nss.isAdminDB() && !nss.isConfigDB()) {
        if (primaryShardId) {
            expectGetDatabase(nss, *primaryShardId);
        } else {
            expectGetDatabase(nss);
        }
    }
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        CollectionType collType(nss, epoch, Date_t::now(), uuid);
        collType.setKeyPattern(shardKeyPattern.toBSON());
        collType.setUnique(false);

        ChunkVersion version(1, 0, epoch, boost::none /* timestamp */);

        ChunkType chunk1(
            nss, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});
        chunk1.setName(OID::gen());
        version.incMinor();

        ChunkType chunk2(
            nss, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        const auto chunk1Obj = BSON("chunks" << chunk1.toConfigBSON());
        const auto chunk2Obj = BSON("chunks" << chunk2.toConfigBSON());
        return std::vector<BSONObj>{collType.toBSON(), chunk1Obj, chunk2Obj};
    }());

    return *future.default_timed_get();
}

}  // namespace mongo
