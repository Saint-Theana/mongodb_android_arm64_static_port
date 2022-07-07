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

#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

using repl::OplogEntry;
using unittest::assertGet;

class OpObserverTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();
        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::createOplog(opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
    }

protected:
    // Assert that the oplog has the expected number of entries, and return them
    std::vector<BSONObj> getNOplogEntries(OperationContext* opCtx, int n) {
        std::vector<BSONObj> result(n);
        repl::OplogInterfaceLocal oplogInterface(opCtx);
        auto oplogIter = oplogInterface.makeIterator();
        for (int i = n - 1; i >= 0; i--) {
            // The oplogIterator returns the entries in reverse order.
            auto opEntry = unittest::assertGet(oplogIter->next());
            result[i] = opEntry.first;
        }
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
        // Some unittests reuse the same OperationContext to read the oplog and end up acquiring the
        // RSTL lock after using the OplogInterfaceLocal. This is a hack to make sure we do not hold
        // RSTL lock for prepared transactions.
        if (opCtx->inMultiDocumentTransaction() &&
            TransactionParticipant::get(opCtx).transactionIsPrepared()) {
            opCtx->lockState()->unlockRSTLforPrepare();
        }
        return result;
    }

    // Assert that oplog only has a single entry and return that oplog entry.
    BSONObj getSingleOplogEntry(OperationContext* opCtx) {
        return getNOplogEntries(opCtx, 1).back();
    }

    ReadWriteConcernDefaultsLookupMock _lookupMock;

private:
    // Creates a reasonable set of ReplSettings for most tests.  We need to be able to
    // override this to create a larger oplog.
    virtual repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

TEST_F(OpObserverTest, StartIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();
    NamespaceString nss("test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v" << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v" << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onStartIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, false /*fromMigrate*/);
        wunit.commit();
    }

    // Create expected startIndexBuild command.
    BSONObjBuilder startIndexBuildBuilder;
    startIndexBuildBuilder.append("startIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&startIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(startIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObj startIndexBuildCmd = startIndexBuildBuilder.done();

    // Ensure the startIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(startIndexBuildCmd, o);
}

TEST_F(OpObserverTest, CommitIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();
    NamespaceString nss("test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v" << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v" << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCommitIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, false /*fromMigrate*/);
        wunit.commit();
    }

    // Create expected commitIndexBuild command.
    BSONObjBuilder commitIndexBuildBuilder;
    commitIndexBuildBuilder.append("commitIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&commitIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(commitIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObj commitIndexBuildCmd = commitIndexBuildBuilder.done();

    // Ensure the commitIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(commitIndexBuildCmd, o);
}

TEST_F(OpObserverTest, AbortIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();
    NamespaceString nss("test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v" << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v" << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    Status cause(ErrorCodes::OperationFailed, "index build failed");
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        auto fromMigrate = false;
        opObserver.onAbortIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, cause, fromMigrate);
        wunit.commit();
    }

    // Create expected abortIndexBuild command.
    BSONObjBuilder abortIndexBuildBuilder;
    abortIndexBuildBuilder.append("abortIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&abortIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(abortIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObjBuilder causeBuilder(abortIndexBuildBuilder.subobjStart("cause"));
    causeBuilder.appendBool("ok", 0);
    cause.serializeErrorToBSON(&causeBuilder);
    causeBuilder.done();
    BSONObj abortIndexBuildCmd = abortIndexBuildBuilder.done();

    // Ensure the abortIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(abortIndexBuildCmd, o);

    // Should be able to extract a Status from the 'cause' field.
    ASSERT_EQUALS(cause, getStatusFromCommandResult(o.getObjectField("cause")));
}

TEST_F(OpObserverTest, CollModWithCollectionOptionsAndTTLInfo) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'collMod' command.
    NamespaceString nss("test.coll");
    BSONObj collModCmd = BSON("collMod" << nss.coll() << "validationLevel"
                                        << "off"
                                        << "validationAction"
                                        << "warn"
                                        // We verify that 'onCollMod' ignores this field.
                                        << "index"
                                        << "indexData");

    CollectionOptions oldCollOpts;
    oldCollOpts.validationLevel = ValidationLevelEnum::strict;
    oldCollOpts.validationAction = ValidationActionEnum::error;

    IndexCollModInfo indexInfo;
    indexInfo.expireAfterSeconds = Seconds(10);
    indexInfo.oldExpireAfterSeconds = Seconds(5);
    indexInfo.indexName = "name_of_index";

    // Write to the oplog.
    {
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCollMod(opCtx.get(), nss, uuid, collModCmd, oldCollOpts, indexInfo);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that collMod fields were properly added to the oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON(
        "collMod" << nss.coll() << "validationLevel"
                  << "off"
                  << "validationAction"
                  << "warn"
                  << "index"
                  << BSON("name" << indexInfo.indexName << "expireAfterSeconds"
                                 << durationCount<Seconds>(indexInfo.expireAfterSeconds.get())));
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the old collection metadata was saved.
    auto o2 = oplogEntry.getObjectField("o2");
    auto o2Expected = BSON("collectionOptions_old"
                           << BSON("validationLevel"
                                   << ValidationLevel_serializer(*oldCollOpts.validationLevel)
                                   << "validationAction"
                                   << ValidationAction_serializer(*oldCollOpts.validationAction))
                           << "expireAfterSeconds_old"
                           << durationCount<Seconds>(indexInfo.oldExpireAfterSeconds.get()));

    ASSERT_BSONOBJ_EQ(o2Expected, o2);
}

TEST_F(OpObserverTest, CollModWithOnlyCollectionOptions) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'collMod' command.
    NamespaceString nss("test.coll");
    BSONObj collModCmd = BSON("collMod" << nss.coll() << "validationLevel"
                                        << "off"
                                        << "validationAction"
                                        << "warn");

    CollectionOptions oldCollOpts;
    oldCollOpts.validationLevel = ValidationLevelEnum::strict;
    oldCollOpts.validationAction = ValidationActionEnum::error;

    // Write to the oplog.
    {
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCollMod(opCtx.get(), nss, uuid, collModCmd, oldCollOpts, boost::none);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that collMod fields were properly added to oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = collModCmd;
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the old collection metadata was saved and that TTL info is not present.
    auto o2 = oplogEntry.getObjectField("o2");
    auto o2Expected = BSON("collectionOptions_old"
                           << BSON("validationLevel"
                                   << ValidationLevel_serializer(*oldCollOpts.validationLevel)
                                   << "validationAction"
                                   << ValidationAction_serializer(*oldCollOpts.validationAction)));

    ASSERT_BSONOBJ_EQ(o2Expected, o2);
}

TEST_F(OpObserverTest, OnDropCollectionReturnsDropOpTime) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    auto uuid = CollectionUUID::gen();

    // Create 'drop' command.
    NamespaceString nss("test.coll");
    auto dropCmd = BSON("drop" << nss.coll());

    // Write to the oplog.
    repl::OpTime dropOpTime;
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onDropCollection(
            opCtx.get(), nss, uuid, 0U, OpObserver::CollectionDropType::kTwoPhase);
        dropOpTime = OpObserver::Times::get(opCtx.get()).reservedOpTimes.front();
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that drop fields were properly added to oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = dropCmd;
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the drop optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(repl::ReplClientInfo::forClient(&cc()).getLastOp(), dropOpTime);
}

TEST_F(OpObserverTest, OnRenameCollectionReturnsRenameOpTime) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    auto uuid = CollectionUUID::gen();
    auto dropTargetUuid = CollectionUUID::gen();
    auto stayTemp = false;
    NamespaceString sourceNss("test.foo");
    NamespaceString targetNss("test.bar");

    // Write to the oplog.
    repl::OpTime renameOpTime;
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(
            opCtx.get(), sourceNss, targetNss, uuid, dropTargetUuid, 0U, stayTemp);
        renameOpTime = OpObserver::Times::get(opCtx.get()).reservedOpTimes.front();
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntry["ui"])));
    auto o = oplogEntry.getObjectField("o");
    auto oExpected =
        BSON("renameCollection" << sourceNss.ns() << "to" << targetNss.ns() << "stayTemp"
                                << stayTemp << "dropTarget" << dropTargetUuid);
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the rename optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(repl::ReplClientInfo::forClient(&cc()).getLastOp(), renameOpTime);
}

TEST_F(OpObserverTest, OnRenameCollectionOmitsDropTargetFieldIfDropTargetUuidIsNull) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    auto uuid = CollectionUUID::gen();
    auto stayTemp = true;
    NamespaceString sourceNss("test.foo");
    NamespaceString targetNss("test.bar");

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(opCtx.get(), sourceNss, targetNss, uuid, {}, 0U, stayTemp);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntry["ui"])));
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON("renameCollection" << sourceNss.ns() << "to" << targetNss.ns()
                                             << "stayTemp" << stayTemp);
    ASSERT_BSONOBJ_EQ(oExpected, o);
}

TEST_F(OpObserverTest, MustBePrimaryToWriteOplogEntries) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx.get())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    Lock::GlobalWrite globalWrite(opCtx.get());
    WriteUnitOfWork wunit(opCtx.get());

    // No-op writes should be prohibited.
    ASSERT_THROWS_CODE(
        opObserver.onOpMessage(opCtx.get(), {}), DBException, ErrorCodes::NotWritablePrimary);
}

TEST_F(OpObserverTest, ImportCollectionOplogEntry) {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    auto importUUID = UUID::gen();
    NamespaceString nss("test.coll");
    long long numRecords = 1;
    long long dataSize = 2;
    // A dummy invalid catalog entry. We do not need a valid catalog entry for this test.
    auto catalogEntry = BSON("ns" << nss.ns() << "ident"
                                  << "collection-7-1792004489479993697");
    auto storageMetadata = BSON("storage"
                                << "metadata");
    bool isDryRun = false;

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onImportCollection(opCtx.get(),
                                      importUUID,
                                      nss,
                                      numRecords,
                                      dataSize,
                                      catalogEntry,
                                      storageMetadata,
                                      isDryRun);
        wunit.commit();
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    ASSERT_TRUE(repl::OpTypeEnum::kCommand == oplogEntry.getOpType());
    ASSERT_TRUE(OplogEntry::CommandType::kImportCollection == oplogEntry.getCommandType());

    ImportCollectionOplogEntry importCollection(
        nss, importUUID, numRecords, dataSize, catalogEntry, storageMetadata, isDryRun);
    ASSERT_BSONOBJ_EQ(importCollection.toBSON(), oplogEntry.getObject());
}

/**
 * Test fixture for testing OpObserver behavior specific to the SessionCatalog.
 */
class OpObserverSessionCatalogRollbackTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();

        auto opCtx = cc().makeOperationContext();
        MongoDSessionCatalog::onStepUp(opCtx.get());
    }

    /**
     * Simulate a new write occurring on given session with the given transaction number and
     * statement id.
     */
    void simulateSessionWrite(OperationContext* opCtx,
                              TransactionParticipant::Participant txnParticipant,
                              NamespaceString nss,
                              TxnNumber txnNum,
                              StmtId stmtId) {
        txnParticipant.beginOrContinue(opCtx, txnNum, boost::none, boost::none);

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            auto opTime = repl::OpTime(Timestamp(10, 1), 1);  // Dummy timestamp.
            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setSessionId(*opCtx->getLogicalSessionId());
            sessionTxnRecord.setTxnNum(txnNum);
            sessionTxnRecord.setLastWriteOpTime(opTime);
            sessionTxnRecord.setLastWriteDate(Date_t::now());
            txnParticipant.onWriteOpCompletedOnPrimary(opCtx, {stmtId}, sessionTxnRecord);
            wuow.commit();
        }
    }
};

TEST_F(OpObserverSessionCatalogRollbackTest,
       OnRollbackDoesntInvalidateSessionCatalogIfNoSessionOpsRolledBack) {
    const NamespaceString nss("testDB", "testColl");

    auto sessionId = makeLogicalSessionIdForTest();

    const TxnNumber txnNum = 0;
    const StmtId stmtId = 1000;

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);
        MongoDOperationContextSession ocs(opCtx.get());
        auto txnParticipant = TransactionParticipant::get(opCtx.get());
        txnParticipant.refreshFromStorageIfNeeded(opCtx.get());

        // Simulate a write occurring on that session
        simulateSessionWrite(opCtx.get(), txnParticipant, nss, txnNum, stmtId);

        // Check that the statement executed
        ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(stmtId));
    }

    // Because there are no sessions to rollback, the OpObserver should not invalidate the in-memory
    // session state, so the check after this should still succeed.
    {
        auto opCtx = cc().makeOperationContext();

        OpObserverImpl opObserver;
        OpObserver::RollbackObserverInfo rbInfo;
        opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    }

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);
        MongoDOperationContextSession ocs(opCtx.get());
        auto txnParticipant = TransactionParticipant::get(opCtx.get());
        ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(stmtId));
    }
}

TEST_F(OpObserverTest, MultipleAboutToDeleteAndOnDelete) {
    auto uuid = UUID::gen();
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss = {"test", "coll"};
    AutoGetDb autoDb(opCtx.get(), nss.db(), MODE_X);
    WriteUnitOfWork wunit(opCtx.get());
    opObserver.aboutToDelete(opCtx.get(), nss, BSON("_id" << 1));
    opObserver.onDelete(opCtx.get(), nss, uuid, kUninitializedStmtId, {});
    opObserver.aboutToDelete(opCtx.get(), nss, BSON("_id" << 1));
    opObserver.onDelete(opCtx.get(), nss, uuid, kUninitializedStmtId, {});
}

DEATH_TEST_F(OpObserverTest, AboutToDeleteMustPreceedOnDelete, "invariant") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    cc().swapLockState(std::make_unique<LockerNoop>());
    NamespaceString nss = {"test", "coll"};
    opObserver.onDelete(opCtx.get(), nss, {}, kUninitializedStmtId, {});
}

DEATH_TEST_F(OpObserverTest, EachOnDeleteRequiresAboutToDelete, "invariant") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();
    cc().swapLockState(std::make_unique<LockerNoop>());
    NamespaceString nss = {"test", "coll"};
    opObserver.aboutToDelete(opCtx.get(), nss, {});
    opObserver.onDelete(opCtx.get(), nss, {}, kUninitializedStmtId, {});
    opObserver.onDelete(opCtx.get(), nss, {}, kUninitializedStmtId, {});
}

DEATH_TEST_REGEX_F(OpObserverTest,
                   NodeCrashesIfShardIdentityDocumentRolledBack,
                   "Fatal assertion.*50712") {
    OpObserverImpl opObserver;
    auto opCtx = cc().makeOperationContext();

    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.shardIdentityRolledBack = true;
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
}

class OpObserverTxnParticipantTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();
        _opCtx = cc().makeOperationContext();

        _opObserver.emplace();

        MongoDSessionCatalog::onStepUp(opCtx());
        _times.emplace(opCtx());

        opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
        opCtx()->setTxnNumber(txnNum());
        opCtx()->setInMultiDocumentTransaction();
        _sessionCheckout = std::make_unique<MongoDOperationContextSession>(opCtx());
        _txnParticipant.emplace(TransactionParticipant::get(opCtx()));
    }

    void tearDown() override {
        _sessionCheckout.reset();
        _times.reset();
        _opCtx.reset();

        OpObserverTest::tearDown();
    }


protected:
    Session* session() {
        return OperationContextSession::get(opCtx());
    }

    OpObserverImpl& opObserver() {
        return *_opObserver;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    TxnNumber& txnNum() {
        return _txnNum;
    }

    TransactionParticipant::Participant& txnParticipant() {
        return *_txnParticipant;
    }

private:
    class ExposeOpObserverTimes : public OpObserver {
    public:
        typedef OpObserver::ReservedTimes ReservedTimes;
    };

    ServiceContext::UniqueOperationContext _opCtx;

    boost::optional<OpObserverImpl> _opObserver;
    boost::optional<ExposeOpObserverTimes::ReservedTimes> _times;
    boost::optional<TransactionParticipant::Participant> _txnParticipant;

    std::unique_ptr<MongoDOperationContextSession> _sessionCheckout;
    TxnNumber _txnNum = 0;
};

/**
 * Test fixture for testing OpObserver behavior specific to multi-document transactions.
 */

class OpObserverTransactionTest : public OpObserverTxnParticipantTest {
public:
    void setUp() override {
        OpObserverTxnParticipantTest::setUp();
        txnParticipant().beginOrContinue(opCtx(), *opCtx()->getTxnNumber(), false, true);
    }

protected:
    void checkSessionAndTransactionFields(const BSONObj& oplogEntry) {
        ASSERT_BSONOBJ_EQ(session()->getSessionId().toBSON(), oplogEntry.getObjectField("lsid"));
        ASSERT_EQ(*opCtx()->getTxnNumber(), oplogEntry.getField("txnNumber").safeNumberLong());
    }
    void checkCommonFields(const BSONObj& oplogEntry) {
        ASSERT_EQ("c"_sd, oplogEntry.getStringField("op"));
        ASSERT_EQ("admin.$cmd"_sd, oplogEntry.getStringField("ns"));
        checkSessionAndTransactionFields(oplogEntry);
    }

    void assertTxnRecord(TxnNumber txnNum,
                         repl::OpTime opTime,
                         boost::optional<DurableTxnStateEnum> txnState) {
        DBDirectClient client(opCtx());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                   {BSON("_id" << session()->getSessionId().toBSON())});
        ASSERT(cursor);
        ASSERT(cursor->more());

        auto txnRecordObj = cursor->next();
        auto txnRecord =
            SessionTxnRecord::parse(IDLParserErrorContext("SessionEntryWritten"), txnRecordObj);
        ASSERT(!cursor->more());
        ASSERT_EQ(session()->getSessionId(), txnRecord.getSessionId());
        ASSERT_EQ(txnNum, txnRecord.getTxnNum());
        ASSERT(txnRecord.getState() == txnState);
        ASSERT_EQ(txnState != boost::none,
                  txnRecordObj.hasField(SessionTxnRecord::kStateFieldName));

        auto txnParticipant = TransactionParticipant::get(opCtx());
        if (!opTime.isNull()) {
            ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
            ASSERT_EQ(opTime, txnParticipant.getLastWriteOpTime());
        } else {
            ASSERT_EQ(txnRecord.getLastWriteOpTime(), txnParticipant.getLastWriteOpTime());
        }
    }

    void assertNoTxnRecord() {
        DBDirectClient client(opCtx());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                   {BSON("_id" << session()->getSessionId().toBSON())});
        ASSERT(cursor);
        ASSERT(!cursor->more());
    }

    void assertTxnRecordStartOpTime(boost::optional<repl::OpTime> startOpTime) {
        DBDirectClient client(opCtx());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                   {BSON("_id" << session()->getSessionId().toBSON())});
        ASSERT(cursor);
        ASSERT(cursor->more());

        auto txnRecordObj = cursor->next();
        auto txnRecord =
            SessionTxnRecord::parse(IDLParserErrorContext("SessionEntryWritten"), txnRecordObj);
        ASSERT(!cursor->more());
        ASSERT_EQ(session()->getSessionId(), txnRecord.getSessionId());
        if (!startOpTime) {
            ASSERT(!txnRecord.getStartOpTime());
        } else {
            ASSERT(txnRecord.getStartOpTime());
            ASSERT_EQ(*startOpTime, *txnRecord.getStartOpTime());
        }
    }
};

TEST_F(OpObserverTransactionTest, TransactionalPrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 0 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);
    opObserver().onUpdate(opCtx(), update2);

    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, {});

    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        // One reserved slot for each statement, plus the prepare.
        auto reservedSlots = repl::getNextOpTimes(opCtx(), 5);
        auto prepareOpTime = reservedSlots.back();
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
        opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 0);
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                           << BSON("_id" << 0 << "data"
                                                         << "x"))
                                      << BSON("op"
                                              << "i"
                                              << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                              << BSON("_id" << 1 << "data"
                                                            << "y"))
                                      << BSON("op"
                                              << "u"
                                              << "ns" << nss2.toString() << "ui" << uuid2 << "o"
                                              << BSON("$set" << BSON("data"
                                                                     << "y"))
                                              << "o2" << BSON("_id" << 0))
                                      << BSON("op"
                                              << "d"
                                              << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                              << BSON("_id" << 0)))
                        << "prepare" << true);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(oplogEntry.shouldPrepare());
    ASSERT_EQ(oplogEntry.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
}

TEST_F(OpObserverTransactionTest, TransactionalPreparedCommitTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    const auto doc = BSON("_id" << 0 << "data"
                                << "x");

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0, doc);

    OplogSlot commitSlot;
    Timestamp prepareTimestamp;
    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        const auto prepareSlot = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareSlot);
        prepareTimestamp = prepareSlot.getTimestamp();
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), {prepareSlot}, &txnOps, 0);

        commitSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic committing the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();

    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onPreparedTransactionCommit(
            opCtx(),
            commitSlot,
            prepareTimestamp,
            txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    }
    repl::OplogInterfaceLocal oplogInterface(opCtx());
    auto oplogIter = oplogInterface.makeIterator();
    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("commitTransaction" << 1 << "commitTimestamp" << prepareTimestamp);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT_FALSE(oplogEntry.shouldPrepare());
    }

    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON(
            "applyOps" << BSON_ARRAY(BSON("op"
                                          << "i"
                                          << "ns" << nss.toString() << "ui" << uuid << "o" << doc))
                       << "prepare" << true);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT(oplogEntry.shouldPrepare());
    }

    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, TransactionalPreparedAbortTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    const auto doc = BSON("_id" << 0 << "data"
                                << "x");

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0, doc);

    OplogSlot abortSlot;
    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        const auto prepareSlot = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareSlot);
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), {prepareSlot}, &txnOps, 0);
        abortSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic aborting the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onTransactionAbort(opCtx(), abortSlot);
    }
    txnParticipant.transitionToAbortedWithPrepareforTest(opCtx());

    repl::OplogInterfaceLocal oplogInterface(opCtx());
    auto oplogIter = oplogInterface.makeIterator();
    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("abortTransaction" << 1);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT_FALSE(oplogEntry.shouldPrepare());
    }

    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON(
            "applyOps" << BSON_ARRAY(BSON("op"
                                          << "i"
                                          << "ns" << nss.toString() << "ui" << uuid << "o" << doc))
                       << "prepare" << true);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT(oplogEntry.shouldPrepare());
    }

    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, TransactionalUnpreparedAbortTest) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);

        txnParticipant.transitionToAbortedWithoutPrepareforTest(opCtx());
        opObserver().onTransactionAbort(opCtx(), boost::none);
    }

    // Assert no oplog entries were written.
    repl::OplogInterfaceLocal oplogInterface(opCtx());
    auto oplogIter = oplogInterface.makeIterator();
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest,
       PreparingEmptyTransactionLogsEmptyApplyOpsAndWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        prepareOpTime = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
        opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), {prepareOpTime}, &txnOps, 0);
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON("applyOps" << BSONArray() << "prepare" << true);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(oplogEntry.shouldPrepare());
    const auto startOpTime = oplogEntry.getOpTime();
    ASSERT_EQ(startOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());

    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecordStartOpTime(startOpTime);
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
}

TEST_F(OpObserverTransactionTest, PreparingTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), slot);
        prepareOpTime = slot;
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), {slot}, &txnOps, 0);
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.getTimestamp());
    }

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverTransactionTest, AbortingUnpreparedTransactionDoesNotWriteToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    opObserver().onTransactionAbort(opCtx(), boost::none);
    txnParticipant.stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertNoTxnRecord();
}

TEST_F(OpObserverTransactionTest, AbortingPreparedTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    OplogSlot abortSlot;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), {slot}, &txnOps, 0);
        txnParticipant.transitionToPreparedforTest(opCtx(), slot);
        abortSlot = repl::getNextOpTime(opCtx());
    }

    // Mimic aborting the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onTransactionAbort(opCtx(), abortSlot);
        txnParticipant.transitionToAbortedWithPrepareforTest(opCtx());
    }
    txnParticipant.stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertTxnRecord(txnNum(), {}, DurableTxnStateEnum::kAborted);
}

TEST_F(OpObserverTransactionTest, CommittingUnpreparedNonEmptyTransactionWritesToTransactionTable) {
    const NamespaceString nss("testDB", "testColl");
    const auto uuid = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(), nss, uuid, insert.begin(), insert.end(), false);
    }

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    opCtx()->getWriteUnitOfWork()->commit();

    assertTxnRecord(txnNum(), {}, DurableTxnStateEnum::kCommitted);
}

TEST_F(OpObserverTransactionTest,
       CommittingUnpreparedEmptyTransactionDoesNotWriteToTransactionTableOrOplog) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);

    txnParticipant.stashTransactionResources(opCtx());

    getNOplogEntries(opCtx(), 0);

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertNoTxnRecord();
}

TEST_F(OpObserverTransactionTest, CommittingPreparedTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = repl::getNextOpTime(opCtx());
        prepareOpTime = slot;
        opCtx()->recoveryUnit()->setPrepareTimestamp(slot.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), {slot}, &txnOps, 0);
        txnParticipant.transitionToPreparedforTest(opCtx(), slot);
    }

    OplogSlot commitSlot = repl::getNextOpTime(opCtx());
    repl::OpTime commitOpTime = commitSlot;
    ASSERT_LTE(prepareOpTime, commitOpTime);

    // Mimic committing the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();

    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onPreparedTransactionCommit(
            opCtx(),
            commitSlot,
            prepareOpTime.getTimestamp(),
            txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    }
    assertTxnRecord(txnNum(), commitOpTime, DurableTxnStateEnum::kCommitted);
}

TEST_F(OpObserverTransactionTest, TransactionalInsertTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0,
                          BSON("_id" << 2 << "data"
                                     << "z"));
    inserts2.emplace_back(1,
                          BSON("_id" << 3 << "data"
                                     << "w"));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                           << BSON("_id" << 0 << "data"
                                                         << "x"))
                                      << BSON("op"
                                              << "i"
                                              << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                              << BSON("_id" << 1 << "data"
                                                            << "y"))
                                      << BSON("op"
                                              << "i"
                                              << "ns" << nss2.toString() << "ui" << uuid2 << "o"
                                              << BSON("_id" << 2 << "data"
                                                            << "z"))
                                      << BSON("op"
                                              << "i"
                                              << "ns" << nss2.toString() << "ui" << uuid2 << "o"
                                              << BSON("_id" << 3 << "data"
                                                            << "w"))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(!oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalUpdateTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    CollectionUpdateArgs updateArgs1;
    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    updateArgs1.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 1);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "u"
                                           << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                           << BSON("$set" << BSON("data"
                                                                  << "x"))
                                           << "o2" << BSON("_id" << 0))
                                      << BSON("op"
                                              << "u"
                                              << "ns" << nss2.toString() << "ui" << uuid2 << "o"
                                              << BSON("$set" << BSON("data"
                                                                     << "y"))
                                              << "o2" << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_FALSE(oplogEntry.hasField("prepare"));
    ASSERT_FALSE(oplogEntry.getBoolField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalDeleteTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, {});
    opObserver().aboutToDelete(opCtx(),
                               nss2,
                               BSON("_id" << 1 << "data"
                                          << "y"));
    opObserver().onDelete(opCtx(), nss2, uuid2, 0, {});
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "d"
                                                        << "ns" << nss1.toString() << "ui" << uuid1
                                                        << "o" << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "d"
                                                           << "ns" << nss2.toString() << "ui"
                                                           << uuid2 << "o" << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_FALSE(oplogEntry.hasField("prepare"));
    ASSERT_FALSE(oplogEntry.getBoolField("prepare"));
}

class OpObserverMultiEntryTransactionTest : public OpObserverTransactionTest {
    void setUp() override {
        _prevPackingLimit = gMaxNumberOfTransactionOperationsInSingleOplogEntry;
        gMaxNumberOfTransactionOperationsInSingleOplogEntry = 1;
        OpObserverTransactionTest::setUp();
    }

    void tearDown() override {
        OpObserverTransactionTest::tearDown();
        gMaxNumberOfTransactionOperationsInSingleOplogEntry = _prevPackingLimit;
    }

private:
    int _prevPackingLimit;
};

TEST_F(OpObserverMultiEntryTransactionTest, TransactionSingleStatementTest) {
    const NamespaceString nss("testDB", "testColl");
    auto uuid = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    std::vector<InsertStatement> inserts;
    inserts.emplace_back(0, BSON("_id" << 0));

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss, MODE_IX);
    opObserver().onInserts(opCtx(), nss, uuid, inserts.begin(), inserts.end(), false);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntryObj = getNOplogEntries(opCtx(), 1)[0];
    checkSessionAndTransactionFields(oplogEntryObj);
    auto oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    ASSERT(!oplogEntry.shouldPrepare());
    ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(repl::OpTime(), *oplogEntry.getPrevWriteOpTimeInTransaction());

    // The implicit commit oplog entry.
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss.toString() << "ui" << uuid
                                                        << "o" << BSON("_id" << 0))));
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntry.getObject());
}

/**
 * Test fixture for testing OpObserver behavior specific to retryable findAndModify.
 */
class OpObserverRetryableFindAndModifyTest : public OpObserverTxnParticipantTest {
public:
    void setUp() override {
        OpObserverTxnParticipantTest::setUp();
        // Set parameter to indicate that pre- and post- images should be stored in a side
        // collection rather than the oplog.
        std::ignore = ServerParameterSet::getGlobal()
                          ->getMap()
                          .find("storeFindAndModifyImagesInSideCollection")
                          ->second->setFromString("true");
        txnParticipant().beginOrContinue(opCtx(), txnNum(), boost::none, boost::none);
    }

    void tearDown() override {
        // Ensure the server parameter is set back to the default if we fail the test midway.
        std::ignore = ServerParameterSet::getGlobal()
                          ->getMap()
                          .find("storeFindAndModifyImagesInSideCollection")
                          ->second->setFromString("false");
        OpObserverTxnParticipantTest::tearDown();
    }
};

TEST_F(OpObserverRetryableFindAndModifyTest,
       RetryableFindAndModifyUpdateRequestingPostImageHasNeedsRetryImage) {
    NamespaceString nss = {"test", "coll"};
    const auto uuid = CollectionUUID::gen();

    CollectionUpdateArgs updateArgs;
    updateArgs.stmtIds = {0};
    updateArgs.updatedDoc = BSON("_id" << 0 << "data"
                                       << "x");
    updateArgs.update = BSON("$set" << BSON("data"
                                            << "x"));
    updateArgs.criteria = BSON("_id" << 0);
    updateArgs.storeDocOption = CollectionUpdateArgs::StoreDocOption::PostImage;
    updateArgs.storeImageInSideCollection = true;
    OplogUpdateEntryArgs update(std::move(updateArgs), nss, uuid);

    WriteUnitOfWork wunit(opCtx());
    AutoGetDb autoDb(opCtx(), nss.db(), MODE_X);
    opObserver().onUpdate(opCtx(), update);
    // Asserts that only a single oplog entry was created. In essence, we did not create any
    // no-op image entries in the oplog.
    const auto oplogEntry = getSingleOplogEntry(opCtx());
    ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPreImageOpTimeFieldName));
    ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPostImageOpTimeFieldName));
    ASSERT_TRUE(oplogEntry.hasField(repl::OplogEntryBase::kNeedsRetryImageFieldName));
    ASSERT_EQUALS(oplogEntry.getStringField(repl::OplogEntryBase::kNeedsRetryImageFieldName),
                  "postImage"_sd);
}

TEST_F(OpObserverRetryableFindAndModifyTest,
       RetryableFindAndModifyUpdateRequestingPreImageHasNeedsRetryImage) {
    NamespaceString nss = {"test", "coll"};
    const auto uuid = CollectionUUID::gen();

    CollectionUpdateArgs updateArgs;
    updateArgs.stmtIds = {0};
    updateArgs.preImageDoc = BSON("_id" << 0 << "data"
                                        << "y");
    updateArgs.update = BSON("$set" << BSON("data"
                                            << "x"));
    updateArgs.criteria = BSON("_id" << 0);
    updateArgs.storeDocOption = CollectionUpdateArgs::StoreDocOption::PreImage;
    updateArgs.storeImageInSideCollection = true;
    OplogUpdateEntryArgs update(std::move(updateArgs), nss, uuid);

    WriteUnitOfWork wunit(opCtx());
    AutoGetDb autoDb(opCtx(), nss.db(), MODE_X);
    opObserver().onUpdate(opCtx(), update);
    // Asserts that only a single oplog entry was created. In essence, we did not create any
    // no-op image entries in the oplog.
    const auto oplogEntry = getSingleOplogEntry(opCtx());
    ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPreImageOpTimeFieldName));
    ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPostImageOpTimeFieldName));
    ASSERT_TRUE(oplogEntry.hasField(repl::OplogEntryBase::kNeedsRetryImageFieldName));
    ASSERT_EQUALS(oplogEntry.getStringField(repl::OplogEntryBase::kNeedsRetryImageFieldName),
                  "preImage"_sd);
}

TEST_F(OpObserverRetryableFindAndModifyTest, RetryableFindAndModifyDeleteHasNeedsRetryImage) {
    NamespaceString nss = {"test", "coll"};
    const auto uuid = CollectionUUID::gen();

    WriteUnitOfWork wunit(opCtx());
    AutoGetDb autoDb(opCtx(), nss.db(), MODE_X);
    const auto deletedDoc = BSON("_id" << 0 << "data"
                                       << "x");
    opObserver().aboutToDelete(opCtx(), nss, deletedDoc);
    OpObserver::OplogDeleteEntryArgs args;
    args.storeImageInSideCollection = true;
    args.deletedDoc = &deletedDoc;
    opObserver().onDelete(opCtx(), nss, uuid, 0, args);
    // Asserts that only a single oplog entry was created. In essence, we did not create any
    // no-op image entries in the oplog.
    const auto oplogEntry = getSingleOplogEntry(opCtx());
    ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPreImageOpTimeFieldName));
    ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPostImageOpTimeFieldName));
    ASSERT_TRUE(oplogEntry.hasField(repl::OplogEntryBase::kNeedsRetryImageFieldName));
    ASSERT_EQUALS(oplogEntry.getStringField(repl::OplogEntryBase::kNeedsRetryImageFieldName),
                  "preImage"_sd);
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalInsertTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 4);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss1.toString() << "ui" << uuid1
                                                        << "o" << BSON("_id" << 0)))
                                     << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                   << "i"
                                                   << "ns" << nss1.toString() << "ui" << uuid1
                                                   << "o" << BSON("_id" << 1)))
                                << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                   << "i"
                                                   << "ns" << nss2.toString() << "ui" << uuid2
                                                   << "o" << BSON("_id" << 2)))
                                << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[2].getObject());

    // This should be the implicit commit oplog entry, indicated by the absence of the 'partialTxn'
    // field.
    oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                   << "i"
                                                   << "ns" << nss2.toString() << "ui" << uuid2
                                                   << "o" << BSON("_id" << 3)))
                                << "count" << 4);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[3].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalUpdateTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    CollectionUpdateArgs updateArgs1;
    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    updateArgs1.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 1);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "u"
                                           << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                           << BSON("$set" << BSON("data"
                                                                  << "x"))
                                           << "o2" << BSON("_id" << 0)))
                        << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    // This should be the implicit commit oplog entry, indicated by the absence of the 'partialTxn'
    // field.
    oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "u"
                                           << "ns" << nss2.toString() << "ui" << uuid2 << "o"
                                           << BSON("$set" << BSON("data"
                                                                  << "y"))
                                           << "o2" << BSON("_id" << 1)))
                        << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionPreImageTest) {
    const NamespaceString nss1("testDB", "testColl");
    auto uuid1 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "txntest");

    CollectionUpdateArgs updateArgs1;
    const auto updateSpec = BSON("$set" << BSON("data"
                                                << "x"));
    const auto updatePreImage = BSON("_id" << 0 << "data"
                                           << "y");
    const auto updatePostImage = BSON("_id" << 0 << "data"
                                            << "x");
    const auto updateFilter = BSON("_id" << 0);

    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = updatePostImage;
    updateArgs1.update = updateSpec;
    updateArgs1.preImageDoc = updatePreImage;
    updateArgs1.preImageRecordingEnabledForCollection = true;
    updateArgs1.criteria = updateFilter;
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);

    const auto deletedDoc = BSON("_id" << 1 << "data"
                                       << "z");
    OpObserver::OplogDeleteEntryArgs args;
    args.deletedDoc = &deletedDoc;
    args.preImageRecordingEnabledForCollection = true;
    opObserver().aboutToDelete(opCtx(), nss1, deletedDoc);
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, args);

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 2);

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 4);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        if (oplogEntry.getOpType() == repl::OpTypeEnum::kNoop) {
            continue;
        }
        checkSessionAndTransactionFields(oplogEntryObj);
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    ASSERT(oplogEntries[0].getOpType() == repl::OpTypeEnum::kNoop);
    ASSERT_BSONOBJ_EQ(updatePreImage, oplogEntries[0].getObject());
    ASSERT(oplogEntries[1].getOpType() == repl::OpTypeEnum::kNoop);
    ASSERT_BSONOBJ_EQ(deletedDoc, oplogEntries[1].getObject());
    ASSERT_BSONOBJ_EQ(BSON("applyOps"
                           << BSON_ARRAY(BSON("op"
                                              << "u"
                                              << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                              << updateSpec << "o2" << BSON("_id" << 0)
                                              << "preImageOpTime" << oplogEntries[0].getOpTime()))
                           << "partialTxn" << true),
                      oplogEntries[2].getObject());
    ASSERT_BSONOBJ_EQ(BSON("applyOps"
                           << BSON_ARRAY(BSON("op"
                                              << "d"
                                              << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                              << BSON("_id" << 1) << "preImageOpTime"
                                              << oplogEntries[1].getOpTime()))
                           << "count" << 2),
                      oplogEntries[3].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, PreparedTransactionPreImageTest) {
    const NamespaceString nss1("testDB", "testColl");
    auto uuid1 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "txntest");

    CollectionUpdateArgs updateArgs1;
    const auto updateSpec = BSON("$set" << BSON("data"
                                                << "x"));
    const auto updatePreImage = BSON("_id" << 0 << "data"
                                           << "y");
    const auto updatePostImage = BSON("_id" << 0 << "data"
                                            << "x");
    const auto updateFilter = BSON("_id" << 0);

    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = updatePostImage;
    updateArgs1.update = updateSpec;
    updateArgs1.preImageDoc = updatePreImage;
    updateArgs1.preImageRecordingEnabledForCollection = true;
    updateArgs1.criteria = updateFilter;
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);

    const auto deletedDoc = BSON("_id" << 1 << "data"
                                       << "z");
    OpObserver::OplogDeleteEntryArgs args;
    args.deletedDoc = &deletedDoc;
    args.preImageRecordingEnabledForCollection = true;
    opObserver().aboutToDelete(opCtx(), nss1, deletedDoc);
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, args);

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        auto reservedSlots = repl::getNextOpTimes(opCtx(), 4);
        prepareOpTime = reservedSlots.back();
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
        opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 2);
    }

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 4);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        if (oplogEntry.getOpType() == repl::OpTypeEnum::kNoop) {
            continue;
        }
        checkSessionAndTransactionFields(oplogEntryObj);
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    ASSERT(oplogEntries[0].getOpType() == repl::OpTypeEnum::kNoop);
    ASSERT_BSONOBJ_EQ(updatePreImage, oplogEntries[0].getObject());
    ASSERT(oplogEntries[1].getOpType() == repl::OpTypeEnum::kNoop);
    ASSERT_BSONOBJ_EQ(deletedDoc, oplogEntries[1].getObject());
    ASSERT_BSONOBJ_EQ(BSON("applyOps"
                           << BSON_ARRAY(BSON("op"
                                              << "u"
                                              << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                              << updateSpec << "o2" << BSON("_id" << 0)
                                              << "preImageOpTime" << oplogEntries[0].getOpTime()))
                           << "partialTxn" << true),
                      oplogEntries[2].getObject());
    ASSERT_BSONOBJ_EQ(BSON("applyOps"
                           << BSON_ARRAY(BSON("op"
                                              << "d"
                                              << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                              << BSON("_id" << 1) << "preImageOpTime"
                                              << oplogEntries[1].getOpTime()))
                           << "prepare" << true << "count" << 2),
                      oplogEntries[3].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalDeleteTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, {});
    opObserver().aboutToDelete(opCtx(),
                               nss2,
                               BSON("_id" << 1 << "data"
                                          << "y"));
    opObserver().onDelete(opCtx(), nss2, uuid2, 0, {});
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "d"
                                                        << "ns" << nss1.toString() << "ui" << uuid1
                                                        << "o" << BSON("_id" << 0)))
                                     << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    // This should be the implicit commit oplog entry, indicated by the absence of the 'partialTxn'
    // field.
    oExpected = oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                               << "d"
                                                               << "ns" << nss2.toString() << "ui"
                                                               << uuid2 << "o" << BSON("_id" << 1)))
                                            << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalInsertPrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));

    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        auto reservedSlots = repl::getNextOpTimes(opCtx(), 4);
        prepareOpTime = reservedSlots.back();
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
        opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 0);
    }
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 4);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss1.toString() << "ui" << uuid1
                                                        << "o" << BSON("_id" << 0)))
                                     << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                   << "i"
                                                   << "ns" << nss1.toString() << "ui" << uuid1
                                                   << "o" << BSON("_id" << 1)))
                                << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                   << "i"
                                                   << "ns" << nss2.toString() << "ui" << uuid2
                                                   << "o" << BSON("_id" << 2)))
                                << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[2].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                   << "i"
                                                   << "ns" << nss2.toString() << "ui" << uuid2
                                                   << "o" << BSON("_id" << 3)))
                                << "prepare" << true << "count" << 4);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[3].getObject());

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());

    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalUpdatePrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    CollectionUpdateArgs updateArgs1;
    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    updateArgs1.criteria = BSON("_id" << 0);
    OplogUpdateEntryArgs update1(std::move(updateArgs1), nss1, uuid1);

    CollectionUpdateArgs updateArgs2;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    updateArgs2.criteria = BSON("_id" << 1);
    OplogUpdateEntryArgs update2(std::move(updateArgs2), nss2, uuid2);

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);

    repl::OpTime prepareOpTime;
    auto reservedSlots = repl::getNextOpTimes(opCtx(), 2);
    prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 0);

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "u"
                                           << "ns" << nss1.toString() << "ui" << uuid1 << "o"
                                           << BSON("$set" << BSON("data"
                                                                  << "x"))
                                           << "o2" << BSON("_id" << 0)))
                        << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "u"
                                           << "ns" << nss2.toString() << "ui" << uuid2 << "o"
                                           << BSON("$set" << BSON("data"
                                                                  << "y"))
                                           << "o2" << BSON("_id" << 1)))
                        << "prepare" << true << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());

    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalDeletePrepareTest) {
    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().aboutToDelete(opCtx(),
                               nss1,
                               BSON("_id" << 0 << "data"
                                          << "x"));
    opObserver().onDelete(opCtx(), nss1, uuid1, 0, {});
    opObserver().aboutToDelete(opCtx(),
                               nss2,
                               BSON("_id" << 1 << "data"
                                          << "y"));
    opObserver().onDelete(opCtx(), nss2, uuid2, 0, {});

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        auto reservedSlots = repl::getNextOpTimes(opCtx(), 2);
        prepareOpTime = reservedSlots.back();
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
        prepareOpTime = reservedSlots.back();
        opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 0);
    }

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "d"
                                                        << "ns" << nss1.toString() << "ui" << uuid1
                                                        << "o" << BSON("_id" << 0)))
                                     << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                   << "d"
                                                   << "ns" << nss2.toString() << "ui" << uuid2
                                                   << "o" << BSON("_id" << 1)))
                                << "prepare" << true << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());

    ASSERT_EQ(prepareOpTime.getTimestamp(), opCtx()->recoveryUnit()->getPrepareTimestamp());
    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, CommitPreparedTest) {
    const NamespaceString nss1("testDB", "testColl");
    auto uuid1 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));

    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        auto reservedSlots = repl::getNextOpTimes(opCtx(), 2);
        prepareOpTime = reservedSlots.back();
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);

        opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 0);
    }

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);

    const auto insertEntry = assertGet(OplogEntry::parse(oplogEntryObjs[0]));
    ASSERT_TRUE(insertEntry.getOpType() == repl::OpTypeEnum::kCommand);
    ASSERT_TRUE(insertEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);

    // This should be the implicit prepare entry.
    const auto prepareEntry = assertGet(OplogEntry::parse(oplogEntryObjs[1]));
    ASSERT_TRUE(prepareEntry.getOpType() == repl::OpTypeEnum::kCommand);
    ASSERT_TRUE(prepareEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT_EQ(prepareEntry.getObject()["prepare"].boolean(), true);

    const auto startOpTime = insertEntry.getOpTime();

    const auto prepareTimestamp = prepareOpTime.getTimestamp();
    ASSERT_EQ(prepareTimestamp, opCtx()->recoveryUnit()->getPrepareTimestamp());

    // Reserve oplog entry for the commit oplog entry.
    OplogSlot commitSlot = repl::getNextOpTime(opCtx());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecordStartOpTime(startOpTime);
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // Mimic committing the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();

    // commitTimestamp must be greater than the prepareTimestamp.
    auto commitTimestamp = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onPreparedTransactionCommit(
            opCtx(),
            commitSlot,
            commitTimestamp,
            txnParticipant.retrieveCompletedTransactionOperations(opCtx()));
    }
    oplogEntryObjs = getNOplogEntries(opCtx(), 3);
    const auto commitOplogObj = oplogEntryObjs.back();
    checkSessionAndTransactionFields(commitOplogObj);
    auto commitEntry = assertGet(OplogEntry::parse(commitOplogObj));
    auto o = commitEntry.getObject();
    auto oExpected = BSON("commitTransaction" << 1 << "commitTimestamp" << commitTimestamp);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_TRUE(commitEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(*commitEntry.getPrevWriteOpTimeInTransaction(), prepareEntry.getOpTime());

    assertTxnRecord(txnNum(), commitSlot, DurableTxnStateEnum::kCommitted);
    // startTimestamp should no longer be set once the transaction has been committed.
    assertTxnRecordStartOpTime(boost::none);
}

TEST_F(OpObserverMultiEntryTransactionTest, AbortPreparedTest) {
    const NamespaceString nss1("testDB", "testColl");
    auto uuid1 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));

    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        auto reservedSlots = repl::getNextOpTimes(opCtx(), 1);
        prepareOpTime = reservedSlots.back();
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);

        opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
        auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
        opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 0);
    }

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 1);

    const auto insertEntry = assertGet(OplogEntry::parse(oplogEntryObjs[0]));
    ASSERT_TRUE(insertEntry.getOpType() == repl::OpTypeEnum::kCommand);
    ASSERT_TRUE(insertEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);
    const auto startOpTime = insertEntry.getOpTime();

    const auto prepareTimestamp = prepareOpTime.getTimestamp();

    const auto prepareEntry = insertEntry;
    ASSERT_EQ(prepareEntry.getObject()["prepare"].boolean(), true);

    ASSERT_EQ(prepareTimestamp, opCtx()->recoveryUnit()->getPrepareTimestamp());

    // Reserve oplog entry for the abort oplog entry.
    OplogSlot abortSlot = repl::getNextOpTime(opCtx());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecordStartOpTime(startOpTime);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");

    // Mimic aborting the transaction by resetting the WUOW.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onTransactionAbort(opCtx(), abortSlot);
    }
    txnParticipant.transitionToAbortedWithPrepareforTest(opCtx());

    oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    auto abortOplogObj = oplogEntryObjs.back();
    checkSessionAndTransactionFields(abortOplogObj);
    auto abortEntry = assertGet(OplogEntry::parse(abortOplogObj));
    auto o = abortEntry.getObject();
    auto oExpected = BSON("abortTransaction" << 1);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_TRUE(abortEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(*abortEntry.getPrevWriteOpTimeInTransaction(), prepareEntry.getOpTime());

    assertTxnRecord(txnNum(), abortSlot, DurableTxnStateEnum::kAborted);
    // startOpTime should no longer be set once a transaction has been aborted.
    assertTxnRecordStartOpTime(boost::none);
}

TEST_F(OpObserverMultiEntryTransactionTest, UnpreparedTransactionPackingTest) {
    gMaxNumberOfTransactionOperationsInSingleOplogEntry = std::numeric_limits<int>::max();

    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 1);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss1.toString() << "ui" << uuid1
                                                        << "o" << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns" << nss1.toString() << "ui"
                                                           << uuid1 << "o" << BSON("_id" << 1))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns" << nss2.toString() << "ui"
                                                           << uuid2 << "o" << BSON("_id" << 2))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns" << nss2.toString() << "ui"
                                                           << uuid2 << "o" << BSON("_id" << 3))));
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, PreparedTransactionPackingTest) {
    gMaxNumberOfTransactionOperationsInSingleOplogEntry = std::numeric_limits<int>::max();

    const NamespaceString nss1("testDB", "testColl");
    const NamespaceString nss2("testDB2", "testColl2");
    auto uuid1 = CollectionUUID::gen();
    auto uuid2 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);
    opObserver().onInserts(opCtx(), nss2, uuid2, inserts2.begin(), inserts2.end(), false);

    repl::OpTime prepareOpTime;
    auto reservedSlots = repl::getNextOpTimes(opCtx(), 4);
    prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 0);
    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    checkSessionAndTransactionFields(oplogEntryObj);
    oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
    const auto& oplogEntry = oplogEntries.back();
    ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
    expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss1.toString() << "ui" << uuid1
                                                        << "o" << BSON("_id" << 0))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns" << nss1.toString() << "ui"
                                                           << uuid1 << "o" << BSON("_id" << 1))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns" << nss2.toString() << "ui"
                                                           << uuid2 << "o" << BSON("_id" << 2))
                                                   << BSON("op"
                                                           << "i"
                                                           << "ns" << nss2.toString() << "ui"
                                                           << uuid2 << "o" << BSON("_id" << 3)))
                                     << "prepare" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, CommitPreparedPackingTest) {
    gMaxNumberOfTransactionOperationsInSingleOplogEntry = std::numeric_limits<int>::max();
    const NamespaceString nss1("testDB", "testColl");
    auto uuid1 = CollectionUUID::gen();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));

    opObserver().onInserts(opCtx(), nss1, uuid1, inserts1.begin(), inserts1.end(), false);

    repl::OpTime prepareOpTime;
    auto reservedSlots = repl::getNextOpTimes(opCtx(), 2);
    prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);

    opCtx()->recoveryUnit()->setPrepareTimestamp(prepareOpTime.getTimestamp());
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onTransactionPrepare(opCtx(), reservedSlots, &txnOps, 0);

    auto oplogEntryObjs = getNOplogEntries(opCtx(), 1);

    // This should be the implicit prepare oplog entry.
    const auto insertEntry = assertGet(OplogEntry::parse(oplogEntryObjs[0]));
    ASSERT_TRUE(insertEntry.getOpType() == repl::OpTypeEnum::kCommand);
    ASSERT_TRUE(insertEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT_EQ(insertEntry.getObject()["prepare"].boolean(), true);

    // If we are only going to write a single prepare oplog entry, but we have reserved multiple
    // oplog slots, at T=1 and T=2, for example, then the 'prepare' oplog entry should be written at
    // T=2 i.e. the last reserved slot.  In this case, the 'startOpTime' of the transaction should
    // also be set to T=2, not T=1. We verify that below.
    const auto startOpTime = prepareOpTime;

    const auto prepareTimestamp = prepareOpTime.getTimestamp();

    // Reserve oplog entry for the commit oplog entry.
    OplogSlot commitSlot = repl::getNextOpTime(opCtx());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecordStartOpTime(startOpTime);
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // Mimic committing the transaction.
    opCtx()->setWriteUnitOfWork(nullptr);
    opCtx()->lockState()->unsetMaxLockTimeout();

    // commitTimestamp must be greater than the prepareTimestamp.
    auto commitTimestamp = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    opObserver().onPreparedTransactionCommit(
        opCtx(),
        commitSlot,
        commitTimestamp,
        txnParticipant.retrieveCompletedTransactionOperations(opCtx()));

    oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    const auto commitOplogObj = oplogEntryObjs.back();
    checkSessionAndTransactionFields(commitOplogObj);
    auto commitEntry = assertGet(OplogEntry::parse(commitOplogObj));
    auto o = commitEntry.getObject();
    auto oExpected = BSON("commitTransaction" << 1 << "commitTimestamp" << commitTimestamp);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_TRUE(commitEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(*commitEntry.getPrevWriteOpTimeInTransaction(), insertEntry.getOpTime());

    assertTxnRecord(txnNum(), commitSlot, DurableTxnStateEnum::kCommitted);
    // startTimestamp should no longer be set once the transaction has been committed.
    assertTxnRecordStartOpTime(boost::none);
}

/**
 * Test fixture with sessions and an extra-large oplog for testing large transactions.
 */
class OpObserverLargeTransactionTest : public OpObserverTransactionTest {
private:
    repl::ReplSettings createReplSettings() override {
        repl::ReplSettings settings;
        // We need an oplog comfortably large enough to hold an oplog entry that exceeds the BSON
        // size limit.  Otherwise we will get the wrong error code when trying to write one.
        settings.setOplogSizeBytes(BSONObjMaxInternalSize + 2 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

// Tests that a large transaction may be committed.  This test creates a transaction with two
// operations that together are just big enough to exceed the size limit, which should result in a
// two oplog entry transaction.
TEST_F(OpObserverLargeTransactionTest, LargeTransactionCreatesMultipleOplogEntries) {
    const NamespaceString nss("testDB", "testColl");
    auto uuid = CollectionUUID::gen();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // This size is crafted such that two operations of this size are not too big to fit in a single
    // oplog entry, but two operations plus oplog overhead are too big to fit in a single oplog
    // entry.
    constexpr size_t kHalfTransactionSize = BSONObjMaxInternalSize / 2 - 175;
    std::unique_ptr<uint8_t[]> halfTransactionData(new uint8_t[kHalfTransactionSize]());
    auto operation1 = repl::DurableOplogEntry::makeInsertOperation(
        nss,
        uuid,
        BSON(
            "_id" << 0 << "data"
                  << BSONBinData(halfTransactionData.get(), kHalfTransactionSize, BinDataGeneral)));
    auto operation2 = repl::DurableOplogEntry::makeInsertOperation(
        nss,
        uuid,
        BSON(
            "_id" << 0 << "data"
                  << BSONBinData(halfTransactionData.get(), kHalfTransactionSize, BinDataGeneral)));
    txnParticipant.addTransactionOperation(opCtx(), operation1);
    txnParticipant.addTransactionOperation(opCtx(), operation2);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    opObserver().onUnpreparedTransactionCommit(opCtx(), &txnOps, 0);
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON("applyOps" << BSON_ARRAY(operation1.toBSON()) << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(operation2.toBSON()) << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());
}

TEST_F(OpObserverTest, OnRollbackInvalidatesDefaultRWConcernCache) {
    auto& rwcDefaults = ReadWriteConcernDefaults::get(getServiceContext());
    auto opCtx = getClient()->makeOperationContext();

    // Put initial defaults in the cache.
    {
        RWConcernDefault origDefaults;
        origDefaults.setUpdateOpTime(Timestamp(10, 20));
        origDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
        _lookupMock.setLookupCallReturnValue(std::move(origDefaults));
    }
    auto origCachedDefaults = rwcDefaults.getDefault(opCtx.get());
    ASSERT_EQ(Timestamp(10, 20), *origCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *origCachedDefaults.getUpdateWallClockTime());

    // Change the mock's defaults, but don't invalidate the cache yet. The cache should still return
    // the original defaults.
    {
        RWConcernDefault newDefaults;
        newDefaults.setUpdateOpTime(Timestamp(50, 20));
        newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
        _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

        auto cachedDefaults = rwcDefaults.getDefault(opCtx.get());
        ASSERT_EQ(Timestamp(10, 20), *cachedDefaults.getUpdateOpTime());
        ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *cachedDefaults.getUpdateWallClockTime());
    }

    // Rollback to a timestamp should invalidate the cache and getting the defaults should now
    // return the latest value.
    {
        OpObserverImpl opObserver;
        OpObserver::RollbackObserverInfo rbInfo;
        opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    }
    auto newCachedDefaults = rwcDefaults.getDefault(opCtx.get());
    ASSERT_EQ(Timestamp(50, 20), *newCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(5678), *newCachedDefaults.getUpdateWallClockTime());
}

}  // namespace
}  // namespace mongo
