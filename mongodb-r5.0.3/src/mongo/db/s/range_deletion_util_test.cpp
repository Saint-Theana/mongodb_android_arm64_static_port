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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString("foo", "bar");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

class RangeDeleterTest : public ShardServerTestFixture {
public:
    // Needed because UUID default constructor is private
    RangeDeleterTest() : _uuid(UUID::gen()) {}

    void setUp() override {
        ShardServerTestFixture::setUp();
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
        // Set up replication coordinator to be primary and have no replication delay.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        replCoord->setCanAcceptNonLocalWrites(true);
        std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY);
        // Make waitForWriteConcern return immediately.
        replCoord->setAwaitReplicationReturnValueFunction([this](OperationContext* opCtx,
                                                                 const repl::OpTime& opTime) {
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(operationContext());
            uassertStatusOK(createCollection(
                operationContext(), kNss.db().toString(), BSON("create" << kNss.coll())));
        }

        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
        _uuid = autoColl.getCollection()->uuid();
    }

    void tearDown() override {
        DBDirectClient client(operationContext());
        client.dropCollection(kNss.ns());

        while (migrationutil::getMigrationUtilExecutor(getServiceContext())->hasTasks()) {
            continue;
        }

        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    void setFilteringMetadataWithUUID(const UUID& uuid) {
        const OID epoch = OID::gen();

        auto rt = RoutingTableHistory::makeNew(
            kNss,
            uuid,
            kShardKeyPattern,
            nullptr,
            false,
            epoch,
            boost::none /* timestamp */,
            boost::none /* timeseriesFields */,
            boost::none,
            true,
            {ChunkType{kNss,
                       ChunkRange{BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)},
                       ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                       ShardId("dummyShardId")}});

        AutoGetDb autoDb(operationContext(), kNss.db(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), kNss, MODE_IX);
        CollectionShardingRuntime::get(operationContext(), kNss)
            ->setFilteringMetadata(
                operationContext(),
                CollectionMetadata(ChunkManager(ShardId("dummyShardId"),
                                                DatabaseVersion(UUID::gen()),
                                                makeStandaloneRoutingTableHistory(std::move(rt)),
                                                boost::none),
                                   ShardId("dummyShardId")));
    }

    UUID uuid() const {
        return _uuid;
    }

private:
    UUID _uuid;
};

/**
 * Simple fixture for testing functions to rename range deletions.
 */
class RenameRangeDeletionsTest : public RangeDeleterTest {
public:
    const NamespaceString kToNss = NamespaceString(kNss.db(), "toColl");

    void setUp() override {
        RangeDeleterTest::setUp();

        // Suspending range deletions in order to rename tasks with "pending" set to false.
        // Otherwise, they could potentially complete before the rename.
        globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::alwaysOn);
    }

    void tearDown() override {
        DBDirectClient client(operationContext());
        client.dropCollection(kToNss.ns());
        // Re-enabling range deletions to drain tasks on the executor
        globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
        RangeDeleterTest::tearDown();
    }
};

// Helper function to count number of documents in config.rangeDeletions.
int countDocsInConfigRangeDeletions(PersistentTaskStore<RangeDeletionTask>& store,
                                    OperationContext* opCtx) {
    auto numDocsInRangeDeletionsCollection = 0;
    store.forEach(opCtx, BSONObj(), [&](const RangeDeletionTask&) {
        ++numDocsInRangeDeletionsCollection;
        return true;
    });
    return numDocsInRangeDeletionsCollection;
};

// The 'pending' field must not be set in order for a range deletion task to succeed, but the
// ShardServerOpObserver will submit the task for deletion upon seeing an insert without the
// 'pending' field. The tests call removeDocumentsFromRange directly, so we want to avoid having
// the op observer also submit the task. The ShardServerOpObserver will ignore replacement
//  updates on the range deletions namespace though, so we can get around the issue by inserting
// the task with the 'pending' field set, and then remove the field using a replacement update
// after.
RangeDeletionTask insertRangeDeletionTask(OperationContext* opCtx, UUID uuid, ChunkRange range) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto migrationId = UUID::gen();
    RangeDeletionTask t(migrationId, kNss, uuid, ShardId("donor"), range, CleanWhenEnum::kDelayed);
    t.setPending(true);
    store.add(opCtx, t);

    // Document should be in the store.
    ASSERT_EQUALS(countDocsInConfigRangeDeletions(store, opCtx), 1);

    auto query = QUERY(RangeDeletionTask::kIdFieldName << migrationId);
    t.setPending(boost::none);
    auto update = t.toBSON();
    store.update(opCtx, query, update);

    return t;
}

TEST_F(RangeDeleterTest,
       RemoveDocumentsInRangeRemovesAllDocumentsInRangeWhenAllDocumentsFitInSingleBatch) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    const int numDocsToRemovePerBatch = 10;
    auto queriesComplete = SemiFuture<void>::makeReady();

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               boost::none,
                               numDocsToRemovePerBatch,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();
    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 0);
}

TEST_F(RangeDeleterTest,
       RemoveDocumentsInRangeRemovesAllDocumentsInRangeWhenSeveralBatchesAreRequired) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    // More documents than the batch size.
    const auto numDocsToInsert = 3;
    const auto numDocsToRemovePerBatch = 1;
    auto queriesComplete = SemiFuture<void>::makeReady();

    // Insert documents in range.
    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    for (auto i = 0; i < numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               boost::none,
                               numDocsToRemovePerBatch,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();
    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 0);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeInsertsDocumentToNotifySecondariesOfRangeDeletion) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    const int numDocsToRemovePerBatch = 10;
    auto queriesComplete = SemiFuture<void>::makeReady();

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               boost::none,
                               numDocsToRemovePerBatch,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();

    ASSERT_EQUALS(dbclient.count(NamespaceString::kServerConfigurationNamespace,
                                 BSON(kShardKey << "startRangeDeletion")),
                  1);
}

TEST_F(
    RangeDeleterTest,
    RemoveDocumentsInRangeOnlyInsertsStartRangeDeletionDocumentOnceWhenSeveralBatchesAreRequired) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    // More documents than the batch size.
    const auto numDocsToInsert = 3;
    const auto numDocsToRemovePerBatch = 1;
    auto queriesComplete = SemiFuture<void>::makeReady();

    // Insert documents in range.
    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    for (auto i = 0; i < numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               boost::none,
                               numDocsToRemovePerBatch,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();
    ASSERT_EQUALS(dbclient.count(NamespaceString::kServerConfigurationNamespace,
                                 BSON(kShardKey << "startRangeDeletion")),
                  1);
}

TEST_F(RangeDeleterTest,
       RemoveDocumentsInRangeDoesNotRemoveDocumentsWithKeysLowerThanMinKeyOfRange) {
    const auto numDocsToInsert = 3;

    const auto minKey = 0;
    const auto range = ChunkRange(BSON(kShardKey << minKey), BSON(kShardKey << 10));

    auto queriesComplete = SemiFuture<void>::makeReady();

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    // All documents below the range.
    for (auto i = minKey - numDocsToInsert; i < minKey; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               boost::none,
                               1 /* numDocsToRemovePerBatch */,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();
    // No documents should have been deleted.
    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), numDocsToInsert);
}

TEST_F(RangeDeleterTest,
       RemoveDocumentsInRangeDoesNotRemoveDocumentsWithKeysGreaterThanOrEqualToMaxKeyOfRange) {
    const auto numDocsToInsert = 3;

    const auto maxKey = 10;
    const auto range = ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << maxKey));

    auto queriesComplete = SemiFuture<void>::makeReady();

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    // All documents greater than or equal to the range.
    for (auto i = maxKey; i < maxKey + numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               boost::none,
                               1 /* numDocsToRemovePerBatch */,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();
    // No documents should have been deleted.
    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), numDocsToInsert);
}

TEST_F(RangeDeleterTest,
       RemoveDocumentsInRangeDoesNotRemoveDocumentsForCollectionWithSameNamespaceAndDifferentUUID) {
    const auto numDocsToInsert = 3;

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    for (auto i = 0; i < numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    auto queriesComplete = SemiFuture<void>::makeReady();
    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               // Use a different UUID from the collection UUID.
                               UUID::gen(),
                               kShardKeyPattern,
                               ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 10)),
                               boost::none,
                               10 /* numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);


    ASSERT_THROWS_CODE(cleanupComplete.get(),
                       DBException,
                       ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist);
    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), numDocsToInsert);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeThrowsErrorWhenCollectionDoesNotExist) {
    auto queriesComplete = SemiFuture<void>::makeReady();
    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               NamespaceString("someFake", "namespace"),
                               UUID::gen(),
                               kShardKeyPattern,
                               ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 10)),
                               boost::none,
                               10 /* numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);


    ASSERT_THROWS_CODE(cleanupComplete.get(),
                       DBException,
                       ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeLeavesDocumentsWhenTaskDocumentDoesNotExist) {
    auto replCoord = checked_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()));

    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    // We intentionally skip inserting a range deletion task document to simulate it already having
    // been deleted.

    // We should wait for replication after attempting to delete the document in the range even when
    // the task document doesn't exist.
    const auto expectedNumTimesWaitedForReplication = 1;
    int numTimesWaitedForReplication = 0;

    // Override special handler for waiting for replication to count the number of times we wait for
    // replication.
    replCoord->setAwaitReplicationReturnValueFunction(
        [&](OperationContext* opCtx, const repl::OpTime& opTime) {
            ++numTimesWaitedForReplication;
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });

    auto queriesComplete = SemiFuture<void>::makeReady();
    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               UUID::gen(),
                               10 /*numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();

    // Document should not have been deleted.
    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 1);
    ASSERT_EQ(numTimesWaitedForReplication, expectedNumTimesWaitedForReplication);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeWaitsForReplicationAfterDeletingSingleBatch) {
    auto replCoord = checked_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()));

    const auto numDocsToInsert = 3;
    const auto numDocsToRemovePerBatch = 10;
    const auto numBatches = ceil((double)numDocsToInsert / numDocsToRemovePerBatch);
    ASSERT_EQ(numBatches, 1);
    // We should wait twice: Once after deleting documents in the range, and once after deleting the
    // range deletion task.
    const auto expectedNumTimesWaitedForReplication = 2;

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    for (auto i = 0; i < numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    // Insert range deletion task for this collection and range.
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto t = insertRangeDeletionTask(operationContext(), uuid(), range);

    int numTimesWaitedForReplication = 0;
    // Override special handler for waiting for replication to count the number of times we wait for
    // replication.
    replCoord->setAwaitReplicationReturnValueFunction(
        [&](OperationContext* opCtx, const repl::OpTime& opTime) {
            ++numTimesWaitedForReplication;
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });

    auto queriesComplete = SemiFuture<void>::makeReady();
    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               t.getId(),
                               numDocsToRemovePerBatch,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();

    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 0);
    ASSERT_EQ(numTimesWaitedForReplication, expectedNumTimesWaitedForReplication);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeWaitsForReplicationOnlyOnceAfterSeveralBatches) {
    auto replCoord = checked_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()));

    const auto numDocsToInsert = 3;
    const auto numDocsToRemovePerBatch = 1;
    const auto numBatches = ceil((double)numDocsToInsert / numDocsToRemovePerBatch);
    ASSERT_GTE(numBatches, 1);

    // We should wait twice: Once after deleting documents in the range, and once after deleting the
    // range deletion task.
    const auto expectedNumTimesWaitedForReplication = 2;

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    for (auto i = 0; i < numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    // Insert range deletion task for this collection and range.
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto t = insertRangeDeletionTask(operationContext(), uuid(), range);

    int numTimesWaitedForReplication = 0;

    // Set special handler for waiting for replication.
    replCoord->setAwaitReplicationReturnValueFunction(
        [&](OperationContext* opCtx, const repl::OpTime& opTime) {
            ++numTimesWaitedForReplication;
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });

    auto queriesComplete = SemiFuture<void>::makeReady();
    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               t.getId(),
                               numDocsToRemovePerBatch,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();

    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 0);
    ASSERT_EQ(numTimesWaitedForReplication, expectedNumTimesWaitedForReplication);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeDoesNotWaitForReplicationIfErrorDuringDeletion) {
    auto replCoord = checked_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()));

    const auto numDocsToInsert = 3;
    const auto numDocsToRemovePerBatch = 10;

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    for (auto i = 0; i < numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    // Insert range deletion task for this collection and range.
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto t = insertRangeDeletionTask(operationContext(), uuid(), range);

    int numTimesWaitedForReplication = 0;
    // Override special handler for waiting for replication to count the number of times we wait for
    // replication.
    replCoord->setAwaitReplicationReturnValueFunction(
        [&](OperationContext* opCtx, const repl::OpTime& opTime) {
            ++numTimesWaitedForReplication;
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });

    // Pretend we stepped down.
    replCoord->setCanAcceptNonLocalWrites(false);
    std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY);

    auto queriesComplete = SemiFuture<void>::makeReady();
    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               t.getId(),
                               numDocsToRemovePerBatch,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    ASSERT_THROWS_CODE(cleanupComplete.get(), DBException, ErrorCodes::PrimarySteppedDown);
    ASSERT_EQ(numTimesWaitedForReplication, 0);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeRetriesOnWriteConflictException) {
    // Enable fail point to throw WriteConflictException.
    globalFailPointRegistry()
        .find("throwWriteConflictExceptionInDeleteRange")
        ->setMode(FailPoint::nTimes, 3 /* Throw a few times before disabling. */);

    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto queriesComplete = SemiFuture<void>::makeReady();

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    // Insert range deletion task for this collection and range.
    auto t = insertRangeDeletionTask(operationContext(), uuid(), range);

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               t.getId(),
                               10 /*numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();

    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 0);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeRetriesOnUnexpectedError) {
    // Enable fail point to throw InternalError.
    globalFailPointRegistry()
        .find("throwInternalErrorInDeleteRange")
        ->setMode(FailPoint::nTimes, 3 /* Throw a few times before disabling. */);

    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto queriesComplete = SemiFuture<void>::makeReady();

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    // Insert range deletion task for this collection and range.
    auto t = insertRangeDeletionTask(operationContext(), uuid(), range);

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               t.getId(),
                               10 /*numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();

    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 0);
}


TEST_F(RangeDeleterTest, RemoveDocumentsInRangeRespectsDelayInBetweenBatches) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    // More documents than the batch size.
    const auto numDocsToInsert = 3;
    const auto numDocsToRemovePerBatch = 1;
    const auto delayBetweenBatches = Milliseconds(10);
    auto queriesComplete = SemiFuture<void>::makeReady();

    // Insert documents in range.
    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    for (auto i = 0; i < numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               boost::none,
                               numDocsToRemovePerBatch,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               delayBetweenBatches);

    // A best-effort check that cleanup has not completed without advancing the clock.
    sleepsecs(1);
    ASSERT_FALSE(cleanupComplete.isReady());

    // Advance the time until cleanup is complete. This explicit advancement of the clock is
    // required in order to allow the delay between batches to complete. This cannot be made exact
    // because there's no way to tell when the sleep operation gets hit exactly, so instead we
    // incrementally advance time until it's ready.
    while (!cleanupComplete.isReady()) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Milliseconds(1));
    }

    cleanupComplete.get();
    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 0);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeRespectsOrphanCleanupDelay) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    // More documents than the batch size.
    const auto numDocsToInsert = 3;
    const auto numDocsToRemovePerBatch = 1;
    const auto orphanCleanupDelay = Seconds(10);
    auto queriesComplete = SemiFuture<void>::makeReady();

    // Insert documents in range.
    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    for (auto i = 0; i < numDocsToInsert; ++i) {
        dbclient.insert(kNss.toString(), BSON(kShardKey << i));
    }

    auto cleanupComplete = removeDocumentsInRange(executor(),
                                                  std::move(queriesComplete),
                                                  kNss,
                                                  uuid(),
                                                  kShardKeyPattern,
                                                  range,
                                                  boost::none,
                                                  numDocsToRemovePerBatch,
                                                  orphanCleanupDelay,
                                                  Milliseconds(0) /* delayBetweenBatches */);

    // A best-effort check that cleanup has not completed without advancing the clock.
    sleepsecs(1);
    ASSERT_FALSE(cleanupComplete.isReady());

    // Advance the time past the delay until cleanup is complete. This cannot be made exact because
    // there's no way to tell when the sleep operation gets hit exactly, so instead we incrementally
    // advance time until it's ready.
    while (!cleanupComplete.isReady()) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + orphanCleanupDelay);
    }

    cleanupComplete.get();

    ASSERT_EQUALS(dbclient.count(kNss, BSONObj()), 0);
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeRemovesRangeDeletionTaskOnSuccess) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto queriesComplete = SemiFuture<void>::makeReady();

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    // Insert range deletion task for this collection and range.
    auto t = insertRangeDeletionTask(operationContext(), uuid(), range);

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               t.getId(),
                               10 /*numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               Milliseconds(0) /* delayBetweenBatches */);

    cleanupComplete.get();
    // Document should have been deleted.
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    ASSERT_EQUALS(countDocsInConfigRangeDeletions(store, operationContext()), 0);
}

TEST_F(RangeDeleterTest,
       RemoveDocumentsInRangeRemovesRangeDeletionTaskOnCollectionDroppedErrorWhenStillPrimary) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto queriesComplete = SemiFuture<void>::makeReady();

    auto fakeUuid = UUID::gen();

    setFilteringMetadataWithUUID(fakeUuid);
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    // Insert range deletion task for this collection and range.
    auto t = insertRangeDeletionTask(operationContext(), uuid(), range);

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               fakeUuid,
                               kShardKeyPattern,
                               range,
                               t.getId(),
                               10 /*numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               Milliseconds(0) /* delayBetweenBatches */);

    ASSERT_THROWS_CODE(cleanupComplete.get(),
                       DBException,
                       ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist);

    // Document should have been deleted.
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    ASSERT_EQUALS(countDocsInConfigRangeDeletions(store, operationContext()), 0);
}

TEST_F(RangeDeleterTest,
       RemoveDocumentsInRangeDoesNotRemoveRangeDeletionTaskOnErrorWhenNotStillPrimary) {
    const ChunkRange range(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto queriesComplete = SemiFuture<void>::makeReady();

    setFilteringMetadataWithUUID(uuid());
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    // Insert range deletion task for this collection and range.
    auto t = insertRangeDeletionTask(operationContext(), uuid(), range);

    // Pretend we stepped down.
    auto replCoord = checked_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()));
    replCoord->setCanAcceptNonLocalWrites(false);
    std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY);

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               range,
                               t.getId(),
                               10 /*numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               Milliseconds(0) /* delayBetweenBatches */);

    ASSERT_THROWS_CODE(cleanupComplete.get(), DBException, ErrorCodes::PrimarySteppedDown);

    // Pretend we stepped back up so we can read the task store.
    replCoord->setCanAcceptNonLocalWrites(true);
    std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY);

    // Document should not have been deleted.
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    ASSERT_EQUALS(countDocsInConfigRangeDeletions(store, operationContext()), 1);
}

// The input future should never have an error.
DEATH_TEST_F(RangeDeleterTest, RemoveDocumentsInRangeCrashesIfInputFutureHasError, "invariant") {
    DBDirectClient dbclient(operationContext());
    dbclient.insert(kNss.toString(), BSON(kShardKey << 5));

    auto queriesCompletePf = makePromiseFuture<void>();
    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move((queriesCompletePf.future)).semi(),
                               kNss,
                               uuid(),
                               kShardKeyPattern,
                               ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 10)),
                               boost::none,
                               10 /* numDocsToRemovePerBatch */,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete */,
                               Milliseconds(0) /* delayBetweenBatches */);


    // Should cause an invariant failure.
    queriesCompletePf.promise.setError(Status(ErrorCodes::InternalError, "Some unexpected error"));
    cleanupComplete.get();
}

TEST_F(RangeDeleterTest, RemoveDocumentsInRangeDoesNotCrashWhenShardKeyIndexDoesNotExist) {
    auto queriesComplete = SemiFuture<void>::makeReady();
    const std::string kNoShardKeyIndexMsg("Unable to find shard key index for");
    auto logCountBefore = countTextFormatLogLinesContaining(kNoShardKeyIndexMsg);

    auto cleanupComplete =
        removeDocumentsInRange(executor(),
                               std::move(queriesComplete),
                               kNss,
                               uuid(),
                               BSON("x" << 1) /* shard key pattern */,
                               ChunkRange(BSON("x" << 0), BSON("x" << 10)),
                               boost::none,
                               10 /* numDocsToRemovePerBatch*/,
                               Seconds(0) /* delayForActiveQueriesOnSecondariesToComplete*/,
                               Milliseconds(0) /* delayBetweenBatches */);

    // Range deleter will keep on retrying when it encounters non-stepdown errors. Make it run
    // a few iterations and then create the index to make it exit the retry loop.
    while (countTextFormatLogLinesContaining(kNoShardKeyIndexMsg) < logCountBefore) {
        sleepmicros(100);
    }

    DBDirectClient client(operationContext());
    client.createIndex(kNss.ns(), BSON("x" << 1));

    cleanupComplete.get();
}

/**
 *  Tests that the rename range deletion flow:
 *  - Renames range deletions from source to target collection
 *  - Doesn't leave garbage
 */
TEST_F(RenameRangeDeletionsTest, BasicRenameRangeDeletionsTest) {
    const auto numTasks = 10;
    std::vector<RangeDeletionTask> tasks;

    // Insert initial range deletions associated to the FROM collection
    PersistentTaskStore<RangeDeletionTask> rangeDeletionsStore(
        NamespaceString::kRangeDeletionNamespace);
    for (int i = 0; i < numTasks; i++) {
        const auto range = ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 1));
        RangeDeletionTask task(
            UUID::gen(), kNss, UUID::gen(), ShardId("donor"), range, CleanWhenEnum::kDelayed);
        task.setPending(false);
        tasks.push_back(task);
        rangeDeletionsStore.add(operationContext(), task);
    }

    // Rename range deletions
    snapshotRangeDeletionsForRename(operationContext(), kNss, kToNss);
    restoreRangeDeletionTasksForRename(operationContext(), kToNss);
    deleteRangeDeletionTasksForRename(operationContext(), kNss, kToNss);

    // Make sure just range deletions for the TO collection are found
    ASSERT_EQ(10, rangeDeletionsStore.count(operationContext()));
    int foundTasks = 0;
    rangeDeletionsStore.forEach(
        operationContext(), BSONObj(), [&](const RangeDeletionTask& newTask) {
            auto task = tasks.at(foundTasks++);
            ASSERT_EQ(newTask.getNss(), kToNss);
            ASSERT_EQ(newTask.getCollectionUuid(), task.getCollectionUuid());
            ASSERT_EQ(newTask.getDonorShardId(), task.getDonorShardId());
            ASSERT(SimpleBSONObjComparator::kInstance.evaluate(newTask.getRange().toBSON() ==
                                                               task.getRange().toBSON()));
            ASSERT(newTask.getWhenToClean() == task.getWhenToClean());
            return true;
        });
    ASSERT_EQ(foundTasks, numTasks);

    // Make sure no garbage is left in intermediate collection
    PersistentTaskStore<RangeDeletionTask> forRenameStore(
        NamespaceString::kRangeDeletionForRenameNamespace);
    ASSERT_EQ(0, forRenameStore.count(operationContext(), BSONObj()));
}

/**
 *  Same as BasicRenameRangeDeletionsTest, but also tests idempotency of single utility functions
 */
TEST_F(RenameRangeDeletionsTest, IdempotentRenameRangeDeletionsTest) {
    const auto numTasks = 10;
    std::vector<RangeDeletionTask> tasks;

    // Insert initial range deletions associated to the FROM collection
    PersistentTaskStore<RangeDeletionTask> rangeDeletionsStore(
        NamespaceString::kRangeDeletionNamespace);
    for (int i = 0; i < numTasks; i++) {
        const auto range = ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 1));
        RangeDeletionTask task(
            UUID::gen(), kNss, UUID::gen(), ShardId("donor"), range, CleanWhenEnum::kDelayed);
        tasks.push_back(task);
        task.setPending(false);
        rangeDeletionsStore.add(operationContext(), task);
    }

    // Rename range deletions, repeating idempotent steps several times
    const auto kMaxRepeat = 10;
    for (int i = 0; i < rand() % kMaxRepeat; i++) {
        snapshotRangeDeletionsForRename(operationContext(), kNss, kToNss);
    }
    for (int i = 0; i < rand() % kMaxRepeat; i++) {
        restoreRangeDeletionTasksForRename(operationContext(), kToNss);
    }
    for (int i = 0; i < rand() % kMaxRepeat; i++) {
        deleteRangeDeletionTasksForRename(operationContext(), kNss, kToNss);
    }

    // Make sure just range deletions for the TO collection are found
    ASSERT_EQ(10, rangeDeletionsStore.count(operationContext()));
    int foundTasks = 0;
    rangeDeletionsStore.forEach(
        operationContext(), BSONObj(), [&](const RangeDeletionTask& newTask) {
            auto task = tasks.at(foundTasks++);
            ASSERT_EQ(newTask.getNss(), kToNss);
            ASSERT_EQ(newTask.getCollectionUuid(), task.getCollectionUuid());
            ASSERT_EQ(newTask.getDonorShardId(), task.getDonorShardId());
            ASSERT(SimpleBSONObjComparator::kInstance.evaluate(newTask.getRange().toBSON() ==
                                                               task.getRange().toBSON()));
            ASSERT(newTask.getWhenToClean() == task.getWhenToClean());
            return true;
        });
    ASSERT_EQ(foundTasks, numTasks);

    // Make sure no garbage is left in intermediate collection
    PersistentTaskStore<RangeDeletionTask> forRenameStore(
        NamespaceString::kRangeDeletionForRenameNamespace);
    ASSERT_EQ(0, forRenameStore.count(operationContext(), BSONObj()));
}

}  // namespace
}  // namespace mongo
