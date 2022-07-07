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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"

namespace mongo {
namespace {

using unittest::assertGet;

using CommitChunkMigrate = ConfigServerTestFixture;

const NamespaceString kNamespace("TestDB.TestColl");
const KeyPattern kKeyPattern(BSON("x" << 1));

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectly) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = boost::none;

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkType migratedChunk, controlChunk;
    {
        ChunkVersion origVersion(12, 7, collEpoch, collTimestamp);

        migratedChunk.setName(OID::gen());
        migratedChunk.setNS(kNamespace);
        migratedChunk.setVersion(origVersion);
        migratedChunk.setShard(shard0.getName());
        migratedChunk.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});
        migratedChunk.setMin(BSON("a" << 1));
        migratedChunk.setMax(BSON("a" << 10));

        origVersion.incMinor();

        controlChunk.setName(OID::gen());
        controlChunk.setNS(kNamespace);
        controlChunk.setVersion(origVersion);
        controlChunk.setShard(shard0.getName());
        controlChunk.setHistory({ChunkHistory(Timestamp(50, 0), shard0.getName())});
        controlChunk.setMin(BSON("a" << 10));
        controlChunk.setMax(BSON("a" << 20));
        controlChunk.setJumbo(true);
    }

    setupCollection(kNamespace, kKeyPattern, {migratedChunk, controlChunk});

    Timestamp validAfter{101, 0};
    BSONObj versions = assertGet(ShardingCatalogManager::get(operationContext())
                                     ->commitChunkMigration(operationContext(),
                                                            kNamespace,
                                                            migratedChunk,
                                                            migratedChunk.getVersion().epoch(),
                                                            ShardId(shard0.getName()),
                                                            ShardId(shard1.getName()),
                                                            validAfter));

    // Verify the versions returned match expected values.
    auto mver = assertGet(ChunkVersion::parseWithField(versions, "shardVersion"));
    ASSERT_EQ(ChunkVersion(migratedChunk.getVersion().majorVersion() + 1,
                           1,
                           migratedChunk.getVersion().epoch(),
                           migratedChunk.getVersion().getTimestamp()),
              mver);

    // Verify that a collection version is returned
    auto cver = assertGet(ChunkVersion::parseWithField(versions, "collectionVersion"));
    ASSERT_TRUE(mver.isOlderOrEqualThan(cver));

    // Verify the chunks ended up in the right shards.
    auto chunkDoc0 = uassertStatusOK(
        getChunkDoc(operationContext(), migratedChunk.getMin(), collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());

    // The migrated chunk's history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());

    auto chunkDoc1 = uassertStatusOK(
        getChunkDoc(operationContext(), controlChunk.getMin(), collEpoch, collTimestamp));
    ASSERT_EQ("shard0", chunkDoc1.getShard().toString());

    // The control chunk's history and jumbo status should be unchanged.
    ASSERT_EQ(1UL, chunkDoc1.getHistory().size());
    ASSERT_EQ(controlChunk.getHistory().front().getValidAfter(),
              chunkDoc1.getHistory().front().getValidAfter());
    ASSERT_EQ(controlChunk.getHistory().front().getShard(),
              chunkDoc1.getHistory().front().getShard());
    ASSERT(chunkDoc1.getJumbo());
}

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectlyWithoutControlChunk) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = boost::none;

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 15;
    auto const origVersion = ChunkVersion(origMajorVersion, 4, collEpoch, collTimestamp);

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(kNamespace, kKeyPattern, {chunk0});

    Timestamp validAfter{101, 0};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_OK(resultBSON.getStatus());

    // Verify the version returned matches expected value.
    BSONObj versions = resultBSON.getValue();
    auto mver = ChunkVersion::parseWithField(versions, "shardVersion");
    ASSERT_OK(mver.getStatus());
    ASSERT_EQ(ChunkVersion(0, 0, origVersion.epoch(), origVersion.getTimestamp()), mver.getValue());

    // Verify the chunk ended up in the right shard.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());
    // The history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());
}

TEST_F(CommitChunkMigrate, CheckCorrectOpsCommandNoCtlTrimHistory) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = boost::none;

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 15;
    auto const origVersion = ChunkVersion(origMajorVersion, 4, collEpoch, collTimestamp);

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(kNamespace, kKeyPattern, {chunk0});

    // Make the time distance between the last history element large enough.
    Timestamp validAfter{200, 0};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_OK(resultBSON.getStatus());

    // Verify the version returned matches expected value.
    BSONObj versions = resultBSON.getValue();
    auto mver = ChunkVersion::parseWithField(versions, "shardVersion");
    ASSERT_OK(mver.getStatus());
    ASSERT_EQ(ChunkVersion(0, 0, origVersion.epoch(), origVersion.getTimestamp()), mver.getValue());

    // Verify the chunk ended up in the right shard.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());

    // The new history entry should be added, but the old one preserved.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());
}

TEST_F(CommitChunkMigrate, RejectOutOfOrderHistory) {

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 15;
    auto const origVersion =
        ChunkVersion(origMajorVersion, 4, OID::gen(), boost::none /* timestamp */);

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(kNamespace, kKeyPattern, {chunk0});

    // Make the time before the last change to trigger the failure.
    Timestamp validAfter{99, 0};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_EQ(ErrorCodes::IncompatibleShardingMetadata, resultBSON.getStatus());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch0) {
    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion =
        ChunkVersion(origMajorVersion, 7, OID::gen(), boost::none /* timestamp */);

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setNS(kNamespace);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    setupCollection(kNamespace, kKeyPattern, {chunk0, chunk1});

    Timestamp validAfter{1};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                OID::gen(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_EQ(ErrorCodes::StaleEpoch, resultBSON.getStatus());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch1) {
    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion =
        ChunkVersion(origMajorVersion, 7, OID::gen(), boost::none /* timestamp */);
    auto const otherVersion =
        ChunkVersion(origMajorVersion, 7, OID::gen(), boost::none /* timestamp */);

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setNS(kNamespace);
    chunk1.setVersion(otherVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    // get version from the control chunk this time
    setupCollection(kNamespace, kKeyPattern, {chunk1, chunk0});

    Timestamp validAfter{1};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_EQ(ErrorCodes::StaleEpoch, resultBSON.getStatus());
}

TEST_F(CommitChunkMigrate, RejectChunkMissing0) {
    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion =
        ChunkVersion(origMajorVersion, 7, OID::gen(), boost::none /* timestamp */);

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setNS(kNamespace);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    setupCollection(kNamespace, kKeyPattern, {chunk1});

    Timestamp validAfter{1};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_EQ(40165, resultBSON.getStatus().code());
}

TEST_F(CommitChunkMigrate, CommitWithLastChunkOnShardShouldNotAffectOtherChunks) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = boost::none;

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, collEpoch, collTimestamp);

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setNS(kNamespace);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard1.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    Timestamp ctrlChunkValidAfter = Timestamp(50, 0);
    chunk1.setHistory({ChunkHistory(ctrlChunkValidAfter, shard1.getName())});

    setupCollection(kNamespace, kKeyPattern, {chunk0, chunk1});

    Timestamp validAfter{101, 0};
    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_OK(resultBSON.getStatus());

    // Verify the versions returned match expected values.
    BSONObj versions = resultBSON.getValue();
    auto mver = ChunkVersion::parseWithField(versions, "shardVersion");
    ASSERT_OK(mver.getStatus());
    ASSERT_EQ(ChunkVersion(0, 0, origVersion.epoch(), origVersion.getTimestamp()), mver.getValue());

    // Verify the chunks ended up in the right shards.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ(shard1.getName(), chunkDoc0.getShard().toString());

    // The migrated chunk's history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());

    auto chunkDoc1 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMax, collEpoch, collTimestamp));
    ASSERT_EQ(shard1.getName(), chunkDoc1.getShard().toString());
    ASSERT_EQ(chunk1.getVersion(), chunkDoc1.getVersion());

    // The control chunk's history should be unchanged.
    ASSERT_EQ(1UL, chunkDoc1.getHistory().size());
    ASSERT_EQ(ctrlChunkValidAfter, chunkDoc1.getHistory().front().getValidAfter());
}

TEST_F(CommitChunkMigrate, RejectMissingChunkVersion) {
    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkVersion origVersion(12, 7, OID::gen(), boost::none /* timestamp */);

    // Create migrate chunk with no chunk version set.
    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setNS(kNamespace);
    migratedChunk.setShard(shard0.getName());
    migratedChunk.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});
    migratedChunk.setMin(BSON("a" << 1));
    migratedChunk.setMax(BSON("a" << 10));

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setNS(kNamespace);
    currentChunk.setVersion(origVersion);
    currentChunk.setShard(shard0.getName());
    currentChunk.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});
    currentChunk.setMin(BSON("a" << 1));
    currentChunk.setMax(BSON("a" << 10));

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    Timestamp validAfter{101, 0};
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkMigration(operationContext(),
                                                  kNamespace,
                                                  migratedChunk,
                                                  origVersion.epoch(),
                                                  ShardId(shard0.getName()),
                                                  ShardId(shard1.getName()),
                                                  validAfter),
                       DBException,
                       4683300);
}

TEST_F(CommitChunkMigrate, RejectOlderChunkVersion) {
    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    auto epoch = OID::gen();
    ChunkVersion origVersion(12, 7, epoch, boost::none /* timestamp */);

    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setNS(kNamespace);
    migratedChunk.setVersion(origVersion);
    migratedChunk.setShard(shard0.getName());
    migratedChunk.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});
    migratedChunk.setMin(BSON("a" << 1));
    migratedChunk.setMax(BSON("a" << 10));

    ChunkVersion currentChunkVersion(14, 7, epoch, boost::none /* timestamp */);

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setNS(kNamespace);
    currentChunk.setVersion(currentChunkVersion);
    currentChunk.setShard(shard0.getName());
    currentChunk.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});
    currentChunk.setMin(BSON("a" << 1));
    currentChunk.setMax(BSON("a" << 10));

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    Timestamp validAfter{101, 0};
    auto result = ShardingCatalogManager::get(operationContext())
                      ->commitChunkMigration(operationContext(),
                                             kNamespace,
                                             migratedChunk,
                                             origVersion.epoch(),
                                             ShardId(shard0.getName()),
                                             ShardId(shard1.getName()),
                                             validAfter);

    ASSERT_NOT_OK(result);
    ASSERT_EQ(result, ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CommitChunkMigrate, RejectMismatchedEpoch) {
    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkVersion origVersion(12, 7, OID::gen(), boost::none /* timestamp */);

    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setNS(kNamespace);
    migratedChunk.setVersion(origVersion);
    migratedChunk.setShard(shard0.getName());
    migratedChunk.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});
    migratedChunk.setMin(BSON("a" << 1));
    migratedChunk.setMax(BSON("a" << 10));

    ChunkVersion currentChunkVersion(12, 7, OID::gen(), boost::none /* timestamp */);

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setNS(kNamespace);
    currentChunk.setVersion(currentChunkVersion);
    currentChunk.setShard(shard0.getName());
    currentChunk.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});
    currentChunk.setMin(BSON("a" << 1));
    currentChunk.setMax(BSON("a" << 10));

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    Timestamp validAfter{101, 0};
    auto result = ShardingCatalogManager::get(operationContext())
                      ->commitChunkMigration(operationContext(),
                                             kNamespace,
                                             migratedChunk,
                                             origVersion.epoch(),
                                             ShardId(shard0.getName()),
                                             ShardId(shard1.getName()),
                                             validAfter);

    ASSERT_NOT_OK(result);
    ASSERT_EQ(result, ErrorCodes::StaleEpoch);
}

}  // namespace
}  // namespace mongo
