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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <initializer_list>
#include <memory>
#include <utility>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/repl/rollback_test_fixture.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace {

using namespace mongo::repl;
using namespace mongo::repl::rollback_internal;

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

class RSRollbackTest : public RollbackTest {};


OplogInterfaceMock::Operation makeNoopOplogEntryAndRecordId(Seconds seconds) {
    OpTime ts(Timestamp(seconds, 0), 0);
    return std::make_pair(BSON("ts" << ts.getTimestamp()), RecordId(1));
}

OplogInterfaceMock::Operation makeDropIndexOplogEntry(const CollectionPtr& collection,
                                                      BSONObj key,
                                                      std::string indexName,
                                                      int time) {
    auto indexSpec =
        BSON("key" << key << "name" << indexName << "v" << static_cast<int>(kIndexVersion));

    return std::make_pair(
        BSON("ts" << Timestamp(Seconds(time), 0) << "op"
                  << "c"
                  << "ui" << collection->uuid() << "ns"
                  << "test.$cmd"
                  << "o" << BSON("dropIndexes" << collection->ns().coll() << "index" << indexName)
                  << "o2" << indexSpec << "wall" << Date_t()),
        RecordId(time));
}

OplogInterfaceMock::Operation makeStartIndexBuildOplogEntry(const CollectionPtr& collection,
                                                            UUID buildUUID,
                                                            BSONObj spec,
                                                            int time) {
    auto entry = BSON("startIndexBuild" << collection->ns().coll() << "indexBuildUUID" << buildUUID
                                        << "indexes" << BSON_ARRAY(spec));

    return std::make_pair(BSON("ts" << Timestamp(Seconds(time), 0) << "op"
                                    << "c"
                                    << "ns"
                                    << "test.$cmd"
                                    << "ui" << collection->uuid() << "o" << entry << "wall"
                                    << Date_t()),
                          RecordId(time));
}

OplogInterfaceMock::Operation makeCommitIndexBuildOplogEntry(const CollectionPtr& collection,
                                                             UUID buildUUID,
                                                             BSONObj spec,
                                                             int time) {
    auto entry = BSON("commitIndexBuild" << collection->ns().coll() << "indexBuildUUID" << buildUUID
                                         << "indexes" << BSON_ARRAY(spec));

    return std::make_pair(BSON("ts" << Timestamp(Seconds(time), 0) << "op"
                                    << "c"
                                    << "ns"
                                    << "test.$cmd"
                                    << "ui" << collection->uuid() << "o" << entry << "wall"
                                    << Date_t()),
                          RecordId(time));
}

OplogInterfaceMock::Operation makeAbortIndexBuildOplogEntry(const CollectionPtr& collection,
                                                            UUID buildUUID,
                                                            BSONObj spec,
                                                            int time) {
    Status cause = {ErrorCodes::IndexBuildAborted, "test"};

    BSONObjBuilder causeBuilder;
    causeBuilder.appendBool("ok", 0);
    cause.serializeErrorToBSON(&causeBuilder);
    auto entry =
        BSON("abortIndexBuild" << collection->ns().coll() << "indexBuildUUID" << buildUUID
                               << "indexes" << BSON_ARRAY(spec) << "cause" << causeBuilder.done());

    return std::make_pair(BSON("ts" << Timestamp(Seconds(time), 0) << "op"
                                    << "c"
                                    << "ns"
                                    << "test.$cmd"
                                    << "ui" << collection->uuid() << "o" << entry << "wall"
                                    << Date_t()),
                          RecordId(time));
}

OplogInterfaceMock::Operation makeCreateIndexOplogEntry(const CollectionPtr& collection,
                                                        BSONObj key,
                                                        std::string indexName,
                                                        int time) {
    auto indexSpec =
        BSON("createIndexes" << collection->ns().coll() << "v" << static_cast<int>(kIndexVersion)
                             << "key" << key << "name" << indexName);

    return std::make_pair(BSON("ts" << Timestamp(Seconds(time), 0) << "op"
                                    << "c"
                                    << "ns"
                                    << "test.$cmd"
                                    << "ui" << collection->uuid() << "o" << indexSpec << "wall"
                                    << Date_t()),
                          RecordId(time));
}

OplogInterfaceMock::Operation makeRenameCollectionOplogEntry(const NamespaceString& renameFrom,
                                                             const NamespaceString& renameTo,
                                                             const UUID collectionUUID,
                                                             OptionalCollectionUUID dropTarget,
                                                             const bool stayTemp,
                                                             OpTime opTime) {
    BSONObjBuilder cmd;
    cmd.append("renameCollection", renameFrom.ns());
    cmd.append("to", renameTo.ns());
    cmd.append("stayTemp", stayTemp);

    BSONObj obj = cmd.obj();

    if (dropTarget) {
        obj = obj.addField(BSON("dropTarget" << *dropTarget).firstElement());
    }
    return std::make_pair(BSON("ts" << opTime.getTimestamp() << "t" << opTime.getTerm() << "op"
                                    << "c"
                                    << "ui" << collectionUUID << "ns" << renameFrom.ns() << "o"
                                    << obj << "wall" << Date_t()),
                          RecordId(opTime.getTimestamp().getSecs()));
}

BSONObj makeOp(long long seconds) {
    auto uuid = unittest::assertGet(UUID::parse("f005ba11-cafe-bead-f00d-123456789abc"));
    return BSON("ts" << Timestamp(seconds, seconds) << "t" << seconds << "op"
                     << "n"
                     << "o" << BSONObj() << "ns"
                     << "rs_rollback.test"
                     << "ui" << uuid << "wall" << Date_t());
}

int recordId = 0;
OplogInterfaceMock::Operation makeOpAndRecordId(long long seconds) {
    return std::make_pair(makeOp(seconds), RecordId(++recordId));
}

// Create an index on an empty collection. Returns the number of indexes that exist on the
// collection after the given index is created.
int _createIndexOnEmptyCollection(OperationContext* opCtx,
                                  Collection* coll,
                                  NamespaceString nss,
                                  BSONObj indexSpec) {
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
    auto indexCatalog = coll->getIndexCatalog();
    WriteUnitOfWork wunit(opCtx);
    ASSERT_OK(indexCatalog->createIndexOnEmptyCollection(opCtx, coll, indexSpec).getStatus());
    wunit.commit();
    return indexCatalog->numIndexesReady(opCtx);
}

TEST_F(RSRollbackTest, InconsistentMinValid) {
    _replicationProcess->getConsistencyMarkers()->setAppliedThrough(
        _opCtx.get(), OpTime(Timestamp(Seconds(1), 0), 0));
    _replicationProcess->getConsistencyMarkers()->setMinValid(_opCtx.get(),
                                                              OpTime(Timestamp(Seconds(2), 0), 0));
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock(),
                               RollbackSourceMock(std::make_unique<OplogInterfaceMock>()),
                               {},
                               {},
                               _coordinator,
                               _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
}

TEST_F(RSRollbackTest, OplogStartMissing) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation = std::make_pair(BSON("ts" << ts.getTimestamp()), RecordId());
    OplogInterfaceMock::Operations remoteOperations({operation});
    auto remoteOplog = std::make_unique<OplogInterfaceMock>(remoteOperations);
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  syncRollback(_opCtx.get(),
                               OplogInterfaceMock(),
                               RollbackSourceMock(std::move(remoteOplog)),
                               {},
                               {},
                               _coordinator,
                               _replicationProcess.get())
                      .code());
}

TEST_F(RSRollbackTest, NoRemoteOpLog) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation = std::make_pair(BSON("ts" << ts.getTimestamp()), RecordId());
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({operation}),
                               RollbackSourceMock(std::make_unique<OplogInterfaceMock>()),
                               {},
                               {},
                               _coordinator,
                               _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
}

TEST_F(RSRollbackTest, RemoteGetRollbackIdThrows) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation = std::make_pair(BSON("ts" << ts.getTimestamp()), RecordId());
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}
        int getRollbackId() const override {
            uassert(ErrorCodes::UnknownError, "getRollbackId() failed", false);
        }
    };
    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({operation}),
                                    RollbackSourceLocal(std::make_unique<OplogInterfaceMock>()),
                                    {},
                                    {},
                                    _coordinator,
                                    _replicationProcess.get()),
                       AssertionException,
                       ErrorCodes::UnknownError);
}

TEST_F(RSRollbackTest, RemoteGetRollbackIdDiffersFromRequiredRBID) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation = std::make_pair(BSON("ts" << ts.getTimestamp()), RecordId());

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        using RollbackSourceMock::RollbackSourceMock;
        int getRollbackId() const override {
            return 2;
        }
    };

    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({operation}),
                                    RollbackSourceLocal(std::make_unique<OplogInterfaceMock>()),
                                    {},
                                    1,
                                    _coordinator,
                                    _replicationProcess.get()),
                       AssertionException,
                       ErrorCodes::duplicateCodeForTest(40506));
}

TEST_F(RSRollbackTest, BothOplogsAtCommonPoint) {
    createOplog(_opCtx.get());
    auto operation = makeOpAndRecordId(1);
    ASSERT_OK(
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({operation}),
                     RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                         operation,
                     }))),
                     {},
                     {},
                     _coordinator,
                     _replicationProcess.get()));
}

/**
 * Test function to roll back a delete operation.
 * Returns number of records in collection after rolling back delete operation.
 * If collection does not exist after rolling back, returns -1.
 */
int _testRollbackDelete(OperationContext* opCtx,
                        ReplicationCoordinator* coordinator,
                        ReplicationProcess* replicationProcess,
                        UUID uuid,
                        const BSONObj& documentAtSource,
                        const bool collectionAtSourceExists = true) {
    auto commonOperation = makeOpAndRecordId(1);
    auto deleteOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "op"
                                 << "d"
                                 << "ui" << uuid << "ns"
                                 << "test.t"
                                 << "o" << BSON("_id" << 0) << "wall" << Date_t()),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(const BSONObj& documentAtSource,
                            std::unique_ptr<OplogInterface> oplog,
                            const bool collectionAtSourceExists)
            : RollbackSourceMock(std::move(oplog)),
              called(false),
              _documentAtSource(documentAtSource),
              _collectionAtSourceExists(collectionAtSourceExists) {}
        std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                          UUID uuid,
                                                          const BSONObj& filter) const override {
            called = true;
            if (!_collectionAtSourceExists) {
                uassertStatusOKWithContext(
                    Status(ErrorCodes::NamespaceNotFound, "MockNamespaceNotFoundMsg"),
                    "find command using UUID failed.");
            }
            return {_documentAtSource, NamespaceString()};
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
        bool _collectionAtSourceExists;
    };
    RollbackSourceLocal rollbackSource(documentAtSource,
                                       std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                                           commonOperation,
                                       })),
                                       collectionAtSourceExists);
    ASSERT_OK(syncRollback(opCtx,
                           OplogInterfaceMock({deleteOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           coordinator,
                           replicationProcess));
    ASSERT_TRUE(rollbackSource.called);

    Lock::DBLock dbLock(opCtx, "test", MODE_S);
    Lock::CollectionLock collLock(opCtx, NamespaceString("test.t"), MODE_S);
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, "test");
    ASSERT_TRUE(db);
    auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
        opCtx, NamespaceString("test.t"));
    if (!collection) {
        return -1;
    }
    return collection->getRecordStore()->numRecords(opCtx);
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionDoesNotExist) {
    createOplog(_opCtx.get());
    ASSERT_EQUALS(
        -1,
        _testRollbackDelete(
            _opCtx.get(), _coordinator, _replicationProcess.get(), UUID::gen(), BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteDocCmdCollectionAtSourceDropped) {
    const bool collectionAtSourceExists = false;
    const NamespaceString nss("test.t");
    createOplog(_opCtx.get());
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_X);
        auto databaseHolder = DatabaseHolder::get(_opCtx.get());
        auto db = databaseHolder->openDb(_opCtx.get(), nss.db());
        ASSERT_TRUE(db);
    }
    ASSERT_EQUALS(-1,
                  _testRollbackDelete(_opCtx.get(),
                                      _coordinator,
                                      _replicationProcess.get(),
                                      UUID::gen(),
                                      BSONObj(),
                                      collectionAtSourceExists));
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionExistsNonCapped) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    _testRollbackDelete(
        _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid(), BSONObj());
    ASSERT_EQUALS(
        0,
        _testRollbackDelete(
            _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid(), BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionExistsCapped) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    options.capped = true;
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    ASSERT_EQUALS(
        0,
        _testRollbackDelete(
            _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid(), BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteRestoreDocument) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    BSONObj doc = BSON("_id" << 0 << "a" << 1);
    _testRollbackDelete(_opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid(), doc);
    ASSERT_EQUALS(1,
                  _testRollbackDelete(
                      _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid(), doc));
}

TEST_F(RSRollbackTest, RollbackInsertDocumentWithNoId) {
    createOplog(_opCtx.get());
    auto commonOperation = makeOpAndRecordId(1);
    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "op"
                                 << "i"
                                 << "ui" << UUID::gen() << "ns"
                                 << "test.t"
                                 << "wall" << Date_t() << "o" << BSON("a" << 1)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const {
            called = true;
            return BSONObj();
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({insertDocumentOperation, commonOperation}),
                               rollbackSource,
                               {},
                               {},
                               _coordinator,
                               _replicationProcess.get());
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Cannot roll back op with no _id"));
    ASSERT_FALSE(rollbackSource.called);
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommand) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto collection = _createCollection(_opCtx.get(), nss.toString(), options);
    auto indexSpec =
        BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON("a" << 1) << "name"
                 << "a_1");

    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), collection, nss, indexSpec);
    ASSERT_EQUALS(2, numIndexes);

    auto commonOperation = makeOpAndRecordId(1);
    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    // Collection pointer will be stale after rollback
    collection = nullptr;

    // Repeat index creation operation and confirm that rollback attempts to drop index just once.
    // This can happen when an index is re-created with different options.
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    startCapturingLogMessages();
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({createIndexOperation, createIndexOperation, commonOperation}),
        rollbackSource,
        {},
        {},
        _coordinator,
        _replicationProcess.get()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Dropped index in rollback"));
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = CollectionCatalog::get(_opCtx.get())
                                ->lookupCollectionByNamespace(_opCtx.get(), nss)
                                ->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommandIndexNotInCatalog) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test.t");
    auto collection = _createCollection(_opCtx.get(), nss, options);
    auto indexSpec = BSON("key" << BSON("a" << 1) << "name"
                                << "a_1");
    // Skip index creation to trigger warning during rollback.
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation = makeOpAndRecordId(1);
    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    // Collection pointer will be stale after rollback
    collection = nullptr;

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({createIndexOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Rollback failed to drop index"));
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = CollectionCatalog::get(_opCtx.get())
                                ->lookupCollectionByNamespace(_opCtx.get(), nss)
                                ->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollbackDropIndexCommandWithOneIndex) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test.t");
    auto collection = _createCollection(_opCtx.get(), nss, options);
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation = makeOpAndRecordId(1);
    auto dropIndexOperation = makeDropIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    // Collection pointer will be stale after rollback
    collection = nullptr;

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({dropIndexOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = CollectionCatalog::get(_opCtx.get())
                                ->lookupCollectionByNamespace(_opCtx.get(), nss)
                                ->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(2, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollbackDropIndexCommandWithMultipleIndexes) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test.t");
    auto collection = _createCollection(_opCtx.get(), nss, options);
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation = makeOpAndRecordId(1);

    auto dropIndexOperation1 = makeDropIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);
    auto dropIndexOperation2 = makeDropIndexOplogEntry(collection, BSON("b" << 1), "b_1", 3);

    // Collection pointer will be stale after rollback
    collection = nullptr;

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({dropIndexOperation2, dropIndexOperation1, commonOperation}),
        rollbackSource,
        {},
        {},
        _coordinator,
        _replicationProcess.get()));
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = CollectionCatalog::get(_opCtx.get())
                                ->lookupCollectionByNamespace(_opCtx.get(), nss)
                                ->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(3, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollingBackCreateAndDropOfSameIndexIgnoresBothCommands) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test.t");
    auto collection = _createCollection(_opCtx.get(), nss, options);

    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_X);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation = makeOpAndRecordId(1);

    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    auto dropIndexOperation = makeDropIndexOplogEntry(collection, BSON("a" << 1), "a_1", 3);

    // Collection pointer will be stale after rollback
    collection = nullptr;

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({dropIndexOperation, createIndexOperation, commonOperation}),
        rollbackSource,
        {},
        {},
        _coordinator,
        _replicationProcess.get()));
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = CollectionCatalog::get(_opCtx.get())
                                ->lookupCollectionByNamespace(_opCtx.get(), nss)
                                ->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
        auto indexDescriptor = indexCatalog->findIndexByName(_opCtx.get(), "a_1", false);
        ASSERT(!indexDescriptor);
    }
}

TEST_F(RSRollbackTest, RollingBackCreateIndexAndRenameWithLongName) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto collection = _createCollection(_opCtx.get(), nss.toString(), options);

    auto longName = std::string(115, 'a');
    auto indexSpec = BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON("b" << 1)
                              << "name" << longName);

    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), collection, nss, indexSpec);
    ASSERT_EQUALS(2, numIndexes);

    auto commonOperation = makeOpAndRecordId(1);

    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("b" << 1), longName, 2);

    // A collection rename will fail if it would cause an index name to become more than 128 bytes.
    // The old collection name plus the index name is not too long, but the new collection name
    // plus the index name is too long.
    auto newName = NamespaceString("test", "collcollcollcollcoll");
    auto renameCollectionOperation = makeRenameCollectionOplogEntry(
        newName, nss, collection->uuid(), boost::none, false, OpTime(Timestamp(Seconds(2), 0), 1));

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({createIndexOperation, renameCollectionOperation, commonOperation}),
        rollbackSource,
        {},
        {},
        _coordinator,
        _replicationProcess.get()));

    {
        AutoGetCollectionForReadCommand coll(_opCtx.get(), newName);
        auto indexCatalog = coll.getCollection()->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));

        std::vector<const IndexDescriptor*> indexes;
        indexCatalog->findIndexesByKeyPattern(_opCtx.get(), BSON("b" << 1), false, &indexes);
        ASSERT(indexes.size() == 0);
    }
}

TEST_F(RSRollbackTest, RollingBackDropAndCreateOfSameIndexNameWithDifferentSpecs) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto collection = _createCollection(_opCtx.get(), nss.toString(), options);

    auto indexSpec =
        BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON("b" << 1) << "name"
                 << "a_1");

    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), collection, nss, indexSpec);
    ASSERT_EQUALS(2, numIndexes);

    auto commonOperation = makeOpAndRecordId(1);

    auto dropIndexOperation = makeDropIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("b" << 1), "a_1", 3);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    startCapturingLogMessages();
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({createIndexOperation, dropIndexOperation, commonOperation}),
        rollbackSource,
        {},
        {},
        _coordinator,
        _replicationProcess.get()));
    stopCapturingLogMessages();
    {
        Lock::DBLock dbLock(_opCtx.get(), nss.db(), MODE_S);
        auto indexCatalog = CollectionCatalog::get(_opCtx.get())
                                ->lookupCollectionByNamespace(_opCtx.get(), nss)
                                ->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(2, indexCatalog->numIndexesReady(_opCtx.get()));
        ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Dropped index in rollback"));
        ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Created index in rollback"));
        std::vector<const IndexDescriptor*> indexes;
        indexCatalog->findIndexesByKeyPattern(_opCtx.get(), BSON("a" << 1), false, &indexes);
        ASSERT(indexes.size() == 1);
        ASSERT(indexes[0]->indexName() == "a_1");

        std::vector<const IndexDescriptor*> indexes2;
        indexCatalog->findIndexesByKeyPattern(_opCtx.get(), BSON("b" << 1), false, &indexes2);
        ASSERT(indexes2.size() == 0);
    }
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommandMissingIndexName) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.t", options);
    auto commonOperation = makeOpAndRecordId(1);
    BSONObj command = BSON("createIndexes"
                           << "t"
                           << "ns"
                           << "test.t"
                           << "wall" << Date_t() << "v" << static_cast<int>(kIndexVersion) << "key"
                           << BSON("a" << 1));

    auto createIndexOperation = std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "op"
                                                         << "c"
                                                         << "ns"
                                                         << "test.$cmd"
                                                         << "wall" << Date_t() << "ui"
                                                         << collection->uuid() << "o" << command),
                                               RecordId(2));
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({createIndexOperation, commonOperation}),
                               rollbackSource,
                               {},
                               {},
                               _coordinator,
                               _replicationProcess.get());
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Missing index name in createIndexes operation on rollback"));
}

// Generators of standard index keys and names given an index 'id'.
std::string idxKey(std::string id) {
    return "key_" + id;
};
std::string idxName(std::string id) {
    return "index_" + id;
};

// Create an index spec object given the namespace and the index 'id'.
BSONObj idxSpec(NamespaceString nss, std::string id) {
    return BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON(idxKey(id) << 1) << "name"
                    << idxName(id));
}

// Returns the number of indexes that exist on the given collection.
int numIndexesOnColl(OperationContext* opCtx, NamespaceString nss, const CollectionPtr& coll) {
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
    auto indexCatalog = coll->getIndexCatalog();
    ASSERT(indexCatalog);
    return indexCatalog->numIndexesReady(opCtx);
}

int numIndexesInProgress(OperationContext* opCtx, NamespaceString nss, const CollectionPtr& coll) {
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
    auto indexCatalog = coll->getIndexCatalog();
    ASSERT(indexCatalog);
    return indexCatalog->numIndexesInProgress(opCtx);
}

TEST_F(RSRollbackTest, RollbackDropIndexOnCollectionWithTwoExistingIndexes) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto coll = _createCollection(_opCtx.get(), nss.toString(), options);

    // Create the necessary indexes. Index 0 is created and dropped in the sequence of ops that will
    // be rolled back, so we only create index 1.
    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, idxSpec(nss, "1"));
    ASSERT_EQUALS(2, numIndexes);

    auto commonOp = makeOpAndRecordId(1);

    // The ops that will be rolled back.
    auto createIndex0Op = makeCreateIndexOplogEntry(coll, BSON(idxKey("0") << 1), idxName("0"), 2);
    auto createIndex1Op = makeCreateIndexOplogEntry(coll, BSON(idxKey("1") << 1), idxName("1"), 3);
    auto dropIndex0Op = makeDropIndexOplogEntry(coll, BSON(idxKey("0") << 1), idxName("0"), 4);

    auto remoteOplog = {commonOp};
    auto localOplog = {dropIndex0Op, createIndex1Op, createIndex0Op, commonOp};

    // Set up the mock rollback source and then run rollback.
    RollbackSourceMock rollbackSource(std::make_unique<OplogInterfaceMock>(remoteOplog));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock(localOplog),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    // Make sure the collection indexes are in the proper state post-rollback.
    ASSERT_EQUALS(
        1,
        numIndexesOnColl(
            _opCtx.get(),
            nss,
            CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), nss)));
}

TEST_F(RSRollbackTest, RollbackTwoIndexDropsPrecededByTwoIndexCreationsOnSameCollection) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto coll = _createCollection(_opCtx.get(), nss.toString(), options);

    auto commonOp = makeOpAndRecordId(1);

    // The ops that will be rolled back.
    auto createIndex0Op = makeCreateIndexOplogEntry(coll, BSON(idxKey("0") << 1), idxName("0"), 2);
    auto createIndex1Op = makeCreateIndexOplogEntry(coll, BSON(idxKey("1") << 1), idxName("1"), 3);
    auto dropIndex0Op = makeDropIndexOplogEntry(coll, BSON(idxKey("0") << 1), idxName("0"), 4);
    auto dropIndex1Op = makeDropIndexOplogEntry(coll, BSON(idxKey("1") << 1), idxName("1"), 5);

    auto remoteOplog = {commonOp};
    auto localOplog = {dropIndex1Op, dropIndex0Op, createIndex1Op, createIndex0Op, commonOp};

    // Set up the mock rollback source and then run rollback.
    RollbackSourceMock rollbackSource(std::make_unique<OplogInterfaceMock>(remoteOplog));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock(localOplog),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    // Make sure the collection indexes are in the proper state post-rollback.
    ASSERT_EQUALS(
        1,
        numIndexesOnColl(
            _opCtx.get(),
            nss,
            CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), nss)));
}

TEST_F(RSRollbackTest, RollbackMultipleCreateIndexesOnSameCollection) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto coll = _createCollection(_opCtx.get(), nss.toString(), options);

    auto commonOp = makeOpAndRecordId(1);

    // Create all of the necessary indexes.
    _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, idxSpec(nss, "0"));
    _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, idxSpec(nss, "1"));
    _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, idxSpec(nss, "2"));
    ASSERT_EQUALS(4, numIndexesOnColl(_opCtx.get(), nss, coll));

    // The ops that will be rolled back.
    auto createIndex0Op = makeCreateIndexOplogEntry(coll, BSON(idxKey("0") << 1), idxName("0"), 2);
    auto createIndex1Op = makeCreateIndexOplogEntry(coll, BSON(idxKey("1") << 1), idxName("1"), 3);
    auto createIndex2Op = makeCreateIndexOplogEntry(coll, BSON(idxKey("2") << 1), idxName("2"), 4);

    auto remoteOplog = {commonOp};
    auto localOplog = {createIndex2Op, createIndex1Op, createIndex0Op, commonOp};

    // Set up the mock rollback source and then run rollback.
    RollbackSourceMock rollbackSource(std::make_unique<OplogInterfaceMock>(remoteOplog));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock(localOplog),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    // Make sure the collection indexes are in the proper state post-rollback.
    ASSERT_EQUALS(
        1,
        numIndexesOnColl(
            _opCtx.get(),
            nss,
            CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), nss)));
}

TEST_F(RSRollbackTest, RollbackCreateDropRecreateIndexOnCollection) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto coll = _createCollection(_opCtx.get(), nss.toString(), options);

    // Create the necessary indexes. Index 0 is created, dropped, and created again in the
    // sequence of ops, so we create that index.
    auto indexSpec = BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON(idxKey("0") << 1)
                              << "name" << idxName("0"));

    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, indexSpec);
    ASSERT_EQUALS(2, numIndexes);

    auto commonOp = makeOpAndRecordId(1);

    // The ops that will be rolled back.
    auto createIndex0Op = makeCreateIndexOplogEntry(coll, BSON(idxKey("0") << 1), idxName("0"), 2);
    auto dropIndex0Op = makeDropIndexOplogEntry(coll, BSON(idxKey("0") << 1), idxName("0"), 3);
    auto createIndex0AgainOp =
        makeCreateIndexOplogEntry(coll, BSON(idxKey("0") << 1), idxName("0"), 4);

    auto remoteOplog = {commonOp};
    auto localOplog = {createIndex0AgainOp, dropIndex0Op, createIndex0Op, commonOp};

    // Set up the mock rollback source and then run rollback.
    RollbackSourceMock rollbackSource(std::make_unique<OplogInterfaceMock>(remoteOplog));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock(localOplog),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    // Make sure the collection indexes are in the proper state post-rollback.
    ASSERT_EQUALS(
        1,
        numIndexesOnColl(
            _opCtx.get(),
            nss,
            CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), nss)));
}

TEST_F(RSRollbackTest, RollbackCommitIndexBuild) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto coll = _createCollection(_opCtx.get(), nss.toString(), options);

    // Create the necessary index.
    auto indexSpec = BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON(idxKey("0") << 1)
                              << "name" << idxName("0") << "collation"
                              << BSON("locale"
                                      << "fr"));

    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, indexSpec);
    ASSERT_EQUALS(2, numIndexes);

    auto buildUUID = UUID::gen();
    // Store the commit quorum value for the index build in config.system.indexBuilds collection.
    _insertDocument(_opCtx.get(),
                    NamespaceString::kIndexBuildEntryNamespace,
                    BSON("_id" << buildUUID << "collectionUUID" << options.uuid.get()
                               << "indexNames" << BSON_ARRAY(idxName("0")) << "commitQuorum" << 0));

    auto commonOp = makeOpAndRecordId(1);

    auto commitIndexBuild = makeCommitIndexBuildOplogEntry(coll, buildUUID, indexSpec, 2);

    // Roll back a commit oplog entry, which will drop and restart the index build.
    auto remoteOplog = {commonOp};
    auto localOplog = {commitIndexBuild, commonOp};

    // Collection pointer will be stale after rollback
    coll = nullptr;

    // Set up the mock rollback source and then run rollback.
    RollbackSourceMock rollbackSource(std::make_unique<OplogInterfaceMock>(remoteOplog));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock(localOplog),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    auto collAfterRollback =
        CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), nss);

    // Make sure the collection indexes are in the proper state post-rollback.
    ASSERT_EQUALS(1, numIndexesOnColl(_opCtx.get(), nss, collAfterRollback));
    ASSERT_EQUALS(1, numIndexesInProgress(_opCtx.get(), nss, collAfterRollback));

    // Kill the index build we just restarted so the fixture can shut down.
    ASSERT_OK(_coordinator->setFollowerMode(MemberState::RS_ROLLBACK));
    ASSERT(IndexBuildsCoordinator::get(_opCtx.get())
               ->abortIndexBuildByBuildUUID(
                   _opCtx.get(), buildUUID, IndexBuildAction::kRollbackAbort, ""));
}

TEST_F(RSRollbackTest, RollbackAbortIndexBuild) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto coll = _createCollection(_opCtx.get(), nss.toString(), options);

    // Create the necessary index.
    auto indexSpec = BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON(idxKey("0") << 1)
                              << "name" << idxName("0") << "collation"
                              << BSON("locale"
                                      << "fr"));

    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, indexSpec);
    ASSERT_EQUALS(2, numIndexes);

    auto buildUUID = UUID::gen();
    // Store the commit quorum value for the index build in config.system.indexBuilds collection.
    _insertDocument(_opCtx.get(),
                    NamespaceString::kIndexBuildEntryNamespace,
                    BSON("_id" << buildUUID << "collectionUUID" << options.uuid.get()
                               << "indexNames" << BSON_ARRAY(idxName("0")) << "commitQuorum" << 0));

    auto commonOp = makeOpAndRecordId(1);

    auto abortIndexBuild = makeAbortIndexBuildOplogEntry(coll, buildUUID, indexSpec, 2);

    // Roll back an abort oplog entry, which will drop and restart the index build.
    auto remoteOplog = {commonOp};
    auto localOplog = {abortIndexBuild, commonOp};

    // Collection pointer will be stale after rollback
    coll = nullptr;

    // Set up the mock rollback source and then run rollback.
    RollbackSourceMock rollbackSource(std::make_unique<OplogInterfaceMock>(remoteOplog));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock(localOplog),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    auto collAfterRollback =
        CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), nss);

    // Make sure the collection indexes are in the proper state post-rollback.
    ASSERT_EQUALS(1, numIndexesOnColl(_opCtx.get(), nss, collAfterRollback));
    ASSERT_EQUALS(1, numIndexesInProgress(_opCtx.get(), nss, collAfterRollback));

    // Kill the index build we just restarted so the fixture can shut down.
    ASSERT_OK(_coordinator->setFollowerMode(MemberState::RS_ROLLBACK));
    ASSERT(IndexBuildsCoordinator::get(_opCtx.get())
               ->abortIndexBuildByBuildUUID(
                   _opCtx.get(), buildUUID, IndexBuildAction::kRollbackAbort, ""));
}

TEST_F(RSRollbackTest, AbortedIndexBuildsAreRestarted) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto coll = _createCollection(_opCtx.get(), nss.toString(), options);

    // Create the necessary index.
    auto indexSpec = BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON(idxKey("0") << 1)
                              << "name" << idxName("0") << "collation"
                              << BSON("locale"
                                      << "fr"));

    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, indexSpec);
    ASSERT_EQUALS(2, numIndexes);

    auto buildUUID = UUID::gen();
    // Store the commit quorum value for the index build in config.system.indexBuilds collection.
    _insertDocument(_opCtx.get(),
                    NamespaceString::kIndexBuildEntryNamespace,
                    BSON("_id" << buildUUID << "collectionUUID" << options.uuid.get()
                               << "indexNames" << BSON_ARRAY(idxName("0")) << "commitQuorum" << 0));

    auto commonOp = makeOpAndRecordId(1);

    // Don't roll-back anything.
    auto remoteOplog = {commonOp};
    auto localOplog = {commonOp};

    // Even though the index has already completed, simulate that we aborted the index build before
    // rollback. We expect the index to be dropped and rebuilt.
    IndexBuildDetails build(coll->uuid());
    build.indexSpecs.push_back(indexSpec);

    IndexBuilds abortedBuilds{{buildUUID, build}};

    // Collection pointer will be stale after rollback
    coll = nullptr;

    // Set up the mock rollback source and then run rollback.
    RollbackSourceMock rollbackSource(std::make_unique<OplogInterfaceMock>(remoteOplog));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock(localOplog),
                           rollbackSource,
                           abortedBuilds,
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    auto collAfterRollback =
        CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), nss);

    // Make sure the collection indexes are in the proper state post-rollback.
    ASSERT_EQUALS(1, numIndexesOnColl(_opCtx.get(), nss, collAfterRollback));
    ASSERT_EQUALS(1, numIndexesInProgress(_opCtx.get(), nss, collAfterRollback));

    // Kill the index build we just restarted so the fixture can shut down.
    ASSERT_OK(_coordinator->setFollowerMode(MemberState::RS_ROLLBACK));
    ASSERT(IndexBuildsCoordinator::get(_opCtx.get())
               ->abortIndexBuildByBuildUUID(
                   _opCtx.get(), buildUUID, IndexBuildAction::kRollbackAbort, ""));
}

TEST_F(RSRollbackTest, AbortedIndexBuildsAreNotRestartedWhenStartIsRolledBack) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    NamespaceString nss("test", "coll");
    auto coll = _createCollection(_opCtx.get(), nss.toString(), options);

    // Create the necessary index.
    auto indexSpec = BSON("v" << static_cast<int>(kIndexVersion) << "key" << BSON(idxKey("0") << 1)
                              << "name" << idxName("0") << "collation"
                              << BSON("locale"
                                      << "fr"));

    int numIndexes = _createIndexOnEmptyCollection(_opCtx.get(), coll, nss, indexSpec);
    ASSERT_EQUALS(2, numIndexes);

    auto commonOp = makeOpAndRecordId(1);

    // Roll-back a startIndexBuild oplog entry. This will cancel out with the aborted index build,
    // and the index will be dropped after rollback.
    auto buildUUID = UUID::gen();
    auto startIndexBuildOp = makeStartIndexBuildOplogEntry(coll, buildUUID, indexSpec, 2);

    auto remoteOplog = {commonOp};
    auto localOplog = {startIndexBuildOp, commonOp};

    // Create an index build to abort.
    IndexBuildDetails build(coll->uuid());
    build.indexSpecs.push_back(indexSpec);
    IndexBuilds abortedBuilds{{buildUUID, build}};

    // Collection pointer will be stale after rollback
    coll = nullptr;

    // Set up the mock rollback source and then run rollback.
    RollbackSourceMock rollbackSource(std::make_unique<OplogInterfaceMock>(remoteOplog));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock(localOplog),
                           rollbackSource,
                           abortedBuilds,
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    auto collAfterRollback =
        CollectionCatalog::get(_opCtx.get())->lookupCollectionByNamespace(_opCtx.get(), nss);

    // The aborted index build should have been dropped.
    ASSERT_EQUALS(1, numIndexesOnColl(_opCtx.get(), nss, collAfterRollback));
    ASSERT_EQUALS(0, numIndexesInProgress(_opCtx.get(), nss, collAfterRollback));
}

TEST_F(RSRollbackTest, RollbackUnknownCommand) {
    createOplog(_opCtx.get());
    auto commonOperation = makeOpAndRecordId(1);
    auto unknownCommandOperation = std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "op"
                                                            << "c"
                                                            << "ui" << UUID::gen() << "ns"
                                                            << "test.t"
                                                            << "wall" << Date_t() << "o"
                                                            << BSON("emptycapped"
                                                                    << "t")),
                                                  RecordId(2));

    auto status =
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({unknownCommandOperation, commonOperation}),
                     RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                         commonOperation,
                     }))),
                     {},
                     {},
                     _coordinator,
                     _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
}

TEST_F(RSRollbackTest, RollbackRenameCollectionInSameDatabaseCommand) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.y", options);
    UUID collectionUUID = collection->uuid();

    OpTime renameTime = OpTime(Timestamp(2, 0), 5);

    auto commonOperation = makeOpAndRecordId(1);
    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
                                                                    NamespaceString("test.y"),
                                                                    collectionUUID,
                                                                    boost::none,
                                                                    false,
                                                                    renameTime);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(renamedColl.getCollection());

        AutoGetCollectionForReadCommand oldCollName(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_FALSE(oldCollName.getCollection());
    }

    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({renameCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    {
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_FALSE(renamedColl.getCollection());

        AutoGetCollectionForReadCommand oldCollName(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(oldCollName.getCollection());

        // Remote collection options should have been empty.
        auto collAfterRollbackOptions = oldCollName->getCollectionOptions();
        ASSERT_BSONOBJ_EQ(BSON("uuid" << *options.uuid), collAfterRollbackOptions.toBSON());
    }
}

TEST_F(RSRollbackTest,
       RollingBackRenameCollectionFromTempToPermanentCollectionSetsCollectionOptionToTemp) {
    createOplog(_opCtx.get());

    auto renameFromNss = NamespaceString("test.renameFrom");
    auto renameToNss = NamespaceString("test.renameTo");

    CollectionOptions options;
    options.uuid = UUID::gen();
    ASSERT_FALSE(options.temp);

    // Create the collection and save its UUID.
    auto collection = _createCollection(_opCtx.get(), renameToNss, options);
    auto collectionUUID = collection->uuid();

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}
        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db, const UUID& uuid) const {
            getCollectionInfoCalled = true;
            return BSON("info" << BSON("uuid" << uuid) << "options" << BSON("temp" << true));
        }
        mutable bool getCollectionInfoCalled = false;
    };

    auto commonOperation = makeOpAndRecordId(1);

    bool stayTemp = false;
    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString(renameFromNss),
                                                                    NamespaceString(renameToNss),
                                                                    collectionUUID,
                                                                    boost::none,
                                                                    stayTemp,
                                                                    OpTime(Timestamp(2, 0), 5));

    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({renameCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    ASSERT_TRUE(rollbackSource.getCollectionInfoCalled);

    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), NamespaceString(renameFromNss));
    auto collAfterRollbackOptions = autoColl->getCollectionOptions();
    ASSERT_TRUE(collAfterRollbackOptions.temp);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << *options.uuid << "temp" << true),
                      collAfterRollbackOptions.toBSON());
}

TEST_F(RSRollbackTest, RollbackRenameCollectionInDatabaseWithDropTargetTrueCommand) {
    createOplog(_opCtx.get());

    OpTime dropTime = OpTime(Timestamp(2, 0), 5);
    auto dpns = NamespaceString("test.y").makeDropPendingNamespace(dropTime);
    CollectionOptions droppedCollOptions;
    droppedCollOptions.uuid = UUID::gen();
    auto droppedColl = _createCollection(_opCtx.get(), dpns, droppedCollOptions);
    _dropPendingCollectionReaper->addDropPendingNamespace(_opCtx.get(), dropTime, dpns);
    auto droppedCollectionUUID = droppedColl->uuid();

    CollectionOptions renamedCollOptions;
    renamedCollOptions.uuid = UUID::gen();
    auto renamedCollection = _createCollection(_opCtx.get(), "test.y", renamedCollOptions);
    auto renamedCollectionUUID = renamedCollection->uuid();

    auto commonOperation = makeOpAndRecordId(1);
    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
                                                                    NamespaceString("test.y"),
                                                                    renamedCollectionUUID,
                                                                    droppedCollectionUUID,
                                                                    false,
                                                                    dropTime);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_TRUE(autoCollDropPending.getCollection());

        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(renamedColl.getCollection());

        AutoGetCollectionForReadCommand oldCollName(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_FALSE(oldCollName.getCollection());
    }
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({renameCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_FALSE(autoCollDropPending.getCollection());

        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(renamedColl.getCollection());
        ASSERT_EQUALS(renamedColl.getCollection()->uuid(), renamedCollectionUUID);

        AutoGetCollectionForReadCommand droppedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(droppedColl.getCollection());
        ASSERT_EQUALS(droppedColl.getCollection()->uuid(), droppedCollectionUUID);
    }
}


void _testRollbackRenamingCollectionsToEachOther(OperationContext* opCtx,
                                                 ReplicationCoordinator* replicationCoordinator,
                                                 ReplicationProcess* replicationProcess,
                                                 const CollectionOptions& coll1Options,
                                                 const CollectionOptions& coll2Options) {
    createOplog(opCtx);

    auto collection1 = RollbackTest::_createCollection(opCtx, "test.y", coll1Options);
    auto collection1UUID = collection1->uuid();

    auto collection2 = RollbackTest::_createCollection(opCtx, "test.x", coll2Options);
    auto collection2UUID = collection2->uuid();

    ASSERT_NOT_EQUALS(collection1UUID, collection2UUID);

    auto commonOperation = makeOpAndRecordId(1);
    auto renameCollectionOperationXtoZ = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
                                                                        NamespaceString("test.z"),
                                                                        collection1UUID,
                                                                        boost::none,
                                                                        false,
                                                                        OpTime(Timestamp(2, 0), 5));

    auto renameCollectionOperationYtoX = makeRenameCollectionOplogEntry(NamespaceString("test.y"),
                                                                        NamespaceString("test.x"),
                                                                        collection2UUID,
                                                                        boost::none,
                                                                        false,
                                                                        OpTime(Timestamp(3, 0), 5));

    auto renameCollectionOperationZtoY = makeRenameCollectionOplogEntry(NamespaceString("test.z"),
                                                                        NamespaceString("test.y"),
                                                                        collection1UUID,
                                                                        boost::none,
                                                                        false,
                                                                        OpTime(Timestamp(4, 0), 5));

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    ASSERT_OK(syncRollback(opCtx,
                           OplogInterfaceMock({renameCollectionOperationZtoY,
                                               renameCollectionOperationYtoX,
                                               renameCollectionOperationXtoZ,
                                               commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           replicationCoordinator,
                           replicationProcess));

    {

        AutoGetCollectionForReadCommand coll1(opCtx, NamespaceString("test.x"));
        ASSERT_TRUE(coll1.getCollection());
        ASSERT_EQUALS(coll1.getCollection()->uuid(), collection1UUID);

        AutoGetCollectionForReadCommand coll2(opCtx, NamespaceString("test.y"));
        ASSERT_TRUE(coll2.getCollection());
        ASSERT_EQUALS(coll2.getCollection()->uuid(), collection2UUID);
    }
}

TEST_F(RSRollbackTest, RollbackRenamingCollectionsToEachOtherWithoutValidationOptions) {
    CollectionOptions coll1Options;
    coll1Options.uuid = UUID::gen();

    CollectionOptions coll2Options;
    coll2Options.uuid = UUID::gen();

    _testRollbackRenamingCollectionsToEachOther(
        _opCtx.get(), _coordinator, _replicationProcess.get(), coll1Options, coll2Options);
}

TEST_F(RSRollbackTest, RollbackRenamingCollectionsToEachOtherWithValidationOptions) {
    CollectionOptions coll1Options;
    coll1Options.uuid = UUID::gen();
    coll1Options.validator = BSON("x" << BSON("$exists" << 1));
    coll1Options.validationLevel = ValidationLevelEnum::moderate;
    coll1Options.validationAction = ValidationActionEnum::warn;

    CollectionOptions coll2Options;
    coll2Options.uuid = UUID::gen();
    coll2Options.validator = BSON("y" << BSON("$exists" << 1));
    coll2Options.validationLevel = ValidationLevelEnum::strict;
    coll2Options.validationAction = ValidationActionEnum::error;

    // renameOutOfTheWay() uses a temporary namespace to rename either of the two collections
    // affected by rollback. The temporary namespace should be able to support collections with
    // validation enabled.
    _testRollbackRenamingCollectionsToEachOther(
        _opCtx.get(), _coordinator, _replicationProcess.get(), coll1Options, coll2Options);
}

TEST_F(RSRollbackTest, RollbackDropCollectionThenRenameCollectionToDroppedCollectionNS) {
    createOplog(_opCtx.get());

    CollectionOptions renamedCollOptions;
    renamedCollOptions.uuid = UUID::gen();
    auto renamedCollection = _createCollection(_opCtx.get(), "test.x", renamedCollOptions);
    auto renamedCollectionUUID = renamedCollection->uuid();

    OpTime dropTime = OpTime(Timestamp(2, 0), 5);
    auto dpns = NamespaceString("test.x").makeDropPendingNamespace(dropTime);
    CollectionOptions droppedCollOptions;
    droppedCollOptions.uuid = UUID::gen();
    auto droppedCollection = _createCollection(_opCtx.get(), dpns, droppedCollOptions);
    auto droppedCollectionUUID = droppedCollection->uuid();
    _dropPendingCollectionReaper->addDropPendingNamespace(_opCtx.get(), dropTime, dpns);

    auto commonOperation = makeOpAndRecordId(1);

    auto dropCollectionOperation =
        std::make_pair(BSON("ts" << dropTime.getTimestamp() << "t" << dropTime.getTerm() << "op"
                                 << "c"
                                 << "ui" << droppedCollectionUUID << "ns"
                                 << "test.x"
                                 << "wall" << Date_t() << "o"
                                 << BSON("drop"
                                         << "x")),
                       RecordId(2));

    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.y"),
                                                                    NamespaceString("test.x"),
                                                                    renamedCollectionUUID,
                                                                    boost::none,
                                                                    false,
                                                                    OpTime(Timestamp(3, 0), 5));

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_TRUE(autoCollDropPending.getCollection());
        AutoGetCollectionForReadCommand autoCollX(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(autoCollX.getCollection());
        AutoGetCollectionForReadCommand autoCollY(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_FALSE(autoCollY.getCollection());
    }
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({renameCollectionOperation, dropCollectionOperation, commonOperation}),
        rollbackSource,
        {},
        {},
        _coordinator,
        _replicationProcess.get()));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_FALSE(autoCollDropPending.getCollection());

        AutoGetCollectionForReadCommand autoCollX(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(autoCollX.getCollection());
        ASSERT_EQUALS(autoCollX.getCollection()->uuid(), droppedCollectionUUID);

        AutoGetCollectionForReadCommand autoCollY(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(autoCollY.getCollection());
        ASSERT_EQUALS(autoCollY.getCollection()->uuid(), renamedCollectionUUID);
    }
}

TEST_F(RSRollbackTest, RollbackRenameCollectionThenCreateNewCollectionWithOldName) {
    createOplog(_opCtx.get());

    CollectionOptions renamedCollOptions;
    renamedCollOptions.uuid = UUID::gen();
    auto renamedCollection = _createCollection(_opCtx.get(), "test.y", renamedCollOptions);
    auto renamedCollectionUUID = renamedCollection->uuid();

    CollectionOptions createdCollOptions;
    createdCollOptions.uuid = UUID::gen();
    auto createdCollection = _createCollection(_opCtx.get(), "test.x", createdCollOptions);
    auto createdCollectionUUID = createdCollection->uuid();

    auto commonOperation = makeOpAndRecordId(1);

    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
                                                                    NamespaceString("test.y"),
                                                                    renamedCollectionUUID,
                                                                    boost::none,
                                                                    false,
                                                                    OpTime(Timestamp(2, 0), 5));

    auto createCollectionOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(3), 0) << "op"
                                 << "c"
                                 << "ui" << createdCollectionUUID << "ns"
                                 << "test.x"
                                 << "wall" << Date_t() << "o"
                                 << BSON("create"
                                         << "x")),
                       RecordId(3));


    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(renamedColl.getCollection());
        AutoGetCollectionForReadCommand createdColl(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(createdColl.getCollection());
    }
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({createCollectionOperation, renameCollectionOperation, commonOperation}),
        rollbackSource,
        {},
        {},
        _coordinator,
        _replicationProcess.get()));

    {

        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(renamedColl.getCollection());
        ASSERT_EQUALS(renamedColl.getCollection()->uuid(), renamedCollectionUUID);

        AutoGetCollectionForReadCommand createdColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_FALSE(createdColl.getCollection());
    }
}

TEST_F(RSRollbackTest, RollbackCollModCommandFailsIfRBIDChangesWhileSyncingCollectionMetadata) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);

    auto commonOperation = makeOpAndRecordId(1);
    auto collModOperation = std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "op"
                                                     << "c"
                                                     << "ui" << coll->uuid() << "ns"
                                                     << "test.t"
                                                     << "wall" << Date_t() << "o"
                                                     << BSON("collMod"
                                                             << "t"
                                                             << "validationLevel"
                                                             << "off")),
                                           RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        using RollbackSourceMock::RollbackSourceMock;
        int getRollbackId() const override {
            return getCollectionInfoCalled ? 1 : 0;
        }
        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db,
                                                    const UUID& uuid) const override {
            getCollectionInfoCalled = true;
            return BSONObj();
        }
        mutable bool getCollectionInfoCalled = false;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({collModOperation, commonOperation}),
                                    rollbackSource,
                                    {},
                                    0,
                                    _coordinator,
                                    _replicationProcess.get()),
                       DBException,
                       40508);
    ASSERT(rollbackSource.getCollectionInfoCalled);
}

TEST_F(RSRollbackTest, RollbackDropDatabaseCommand) {
    createOplog(_opCtx.get());
    auto commonOperation = makeOpAndRecordId(1);
    // 'dropDatabase' operations are special and do not include a UUID field.
    auto dropDatabaseOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "op"
                                 << "c"
                                 << "ns"
                                 << "test.$cmd"
                                 << "wall" << Date_t() << "o" << BSON("dropDatabase" << 1)),
                       RecordId(2));
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({dropDatabaseOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
}

BSONObj makeApplyOpsOplogEntry(Timestamp ts, std::initializer_list<BSONObj> ops) {
    // applyOps oplog entries are special and do not include a UUID field.
    BSONObjBuilder entry;
    entry << "ts" << ts << "op"
          << "c"
          << "ns"
          << "admin"
          << "wall" << Date_t();
    {
        BSONObjBuilder cmd(entry.subobjStart("o"));
        BSONArrayBuilder subops(entry.subarrayStart("applyOps"));
        for (const auto& op : ops) {
            subops << op;
        }
    }
    return entry.obj();
}

OpTime getOpTimeFromOplogEntry(const BSONObj& entry) {
    const BSONElement tsElement = entry["ts"];
    const BSONElement termElement = entry["t"];
    ASSERT_EQUALS(bsonTimestamp, tsElement.type()) << entry;
    ASSERT_TRUE(termElement.eoo() || termElement.isNumber()) << entry;
    long long term = 1LL;
    if (!termElement.eoo()) {
        term = termElement.numberLong();
    }
    return OpTime(tsElement.timestamp(), term);
}

TEST_F(RSRollbackTest, RollbackApplyOpsCommand) {
    createOplog(_opCtx.get());
    CollectionPtr coll;
    CollectionOptions options;
    options.uuid = UUID::gen();
    {
        AutoGetDb autoDb(_opCtx.get(), "test", MODE_X);
        mongo::WriteUnitOfWork wuow(_opCtx.get());
        coll = CollectionCatalog::get(_opCtx.get())
                   ->lookupCollectionByNamespace(_opCtx.get(), NamespaceString("test.t"));
        if (!coll) {
            auto db = autoDb.ensureDbExists();
            coll = db->createCollection(_opCtx.get(), NamespaceString("test.t"), options);
        }
        ASSERT(coll);
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(coll->insertDocument(
            _opCtx.get(), InsertStatement(BSON("_id" << 1 << "v" << 2)), nullOpDebug, false));
        ASSERT_OK(coll->insertDocument(
            _opCtx.get(), InsertStatement(BSON("_id" << 2 << "v" << 4)), nullOpDebug, false));
        ASSERT_OK(coll->insertDocument(
            _opCtx.get(), InsertStatement(BSON("_id" << 4)), nullOpDebug, false));
        wuow.commit();
    }
    UUID uuid = coll->uuid();
    const auto commonOperation = makeOpAndRecordId(1);
    const auto applyOpsOperation =
        std::make_pair(makeApplyOpsOplogEntry(
                           Timestamp(Seconds(2), 0),
                           {BSON("op"
                                 << "u"
                                 << "ui" << uuid << "ts" << Timestamp(1, 1) << "t" << 1LL << "ns"
                                 << "test.t"
                                 << "o2" << BSON("_id" << 1) << "wall" << Date_t() << "o"
                                 << BSON("_id" << 1 << "v" << 2)),
                            BSON("op"
                                 << "u"
                                 << "ui" << uuid << "ts" << Timestamp(2, 1) << "t" << 1LL << "ns"
                                 << "test.t"
                                 << "o2" << BSON("_id" << 2) << "wall" << Date_t() << "o"
                                 << BSON("_id" << 2 << "v" << 4)),
                            BSON("op"
                                 << "d"
                                 << "ui" << uuid << "ts" << Timestamp(3, 1) << "t" << 1LL << "ns"
                                 << "test.t"
                                 << "wall" << Date_t() << "o" << BSON("_id" << 3)),
                            BSON("op"
                                 << "i"
                                 << "ui" << uuid << "ts" << Timestamp(4, 1) << "t" << 1LL << "ns"
                                 << "test.t"
                                 << "wall" << Date_t() << "o" << BSON("_id" << 4)),
                            // applyOps internal oplog entries are not required
                            // to have a timestamp.
                            BSON("op"
                                 << "i"
                                 << "ui" << uuid << "ts" << Timestamp(4, 1) << "t" << 1LL << "ns"
                                 << "test.t"
                                 << "wall" << Date_t() << "o" << BSON("_id" << 4)),
                            BSON("op"
                                 << "i"
                                 << "ui" << uuid << "t" << 1LL << "ns"
                                 << "test.t"
                                 << "wall" << Date_t() << "o" << BSON("_id" << 4)),
                            BSON("op"
                                 << "i"
                                 << "ui" << uuid << "t" << 1LL << "ns"
                                 << "test.t"
                                 << "wall" << Date_t() << "o" << BSON("_id" << 4))}),
                       RecordId(2));

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}

        std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                          UUID uuid,
                                                          const BSONObj& filter) const override {
            int numFields = 0;
            for (const auto& element : filter) {
                ++numFields;
                ASSERT_EQUALS("_id", element.fieldNameStringData()) << filter;
            }
            ASSERT_EQUALS(1, numFields) << filter;
            searchedIds.insert(filter.firstElement().numberInt());
            switch (filter.firstElement().numberInt()) {
                case 1:
                    return {BSON("_id" << 1 << "v" << 1), NamespaceString()};
                case 2:
                    return {BSON("_id" << 2 << "v" << 3), NamespaceString()};
                case 3:
                    return {BSON("_id" << 3 << "v" << 5), NamespaceString()};
                case 4:
                    return {};
            }
            FAIL("Unexpected findOne request") << filter;
            return {};  // Unreachable; why doesn't compiler know?
        }

        mutable std::multiset<int> searchedIds;
    } rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({applyOpsOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    ASSERT_EQUALS(4U, rollbackSource.searchedIds.size());
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(1));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(2));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(3));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(4));

    AutoGetCollectionForReadCommand acr(_opCtx.get(), NamespaceString("test.t"));
    BSONObj result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 1), result));
    ASSERT_EQUALS(1, result["v"].numberInt()) << result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 2), result));
    ASSERT_EQUALS(3, result["v"].numberInt()) << result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 3), result));
    ASSERT_EQUALS(5, result["v"].numberInt()) << result;
    ASSERT_FALSE(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 4), result))
        << result;
}

TEST_F(RSRollbackTest, RollbackCreateCollectionCommand) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);

    auto commonOperation = makeOpAndRecordId(1);
    auto createCollectionOperation = std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "op"
                                                              << "c"
                                                              << "ui" << coll->uuid() << "ns"
                                                              << "test.t"
                                                              << "wall" << Date_t() << "o"
                                                              << BSON("create"
                                                                      << "t")),
                                                    RecordId(2));
    RollbackSourceMock rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({createCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto databaseHolder = DatabaseHolder::get(_opCtx.get());
        auto db = databaseHolder->getDb(_opCtx.get(), "test");
        ASSERT_TRUE(db);
        ASSERT_FALSE(CollectionCatalog::get(_opCtx.get())
                         ->lookupCollectionByNamespace(_opCtx.get(), NamespaceString("test.t")));
    }
}

TEST_F(RSRollbackTest, RollbackCollectionModificationCommand) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);

    auto commonOperation = makeOpAndRecordId(1);

    BSONObj collModCmd = BSON("collMod"
                              << "t"
                              << "validationLevel"
                              << "strict");
    auto collectionModificationOperation =
        makeCommandOp(Timestamp(Seconds(2), 0), coll->uuid(), "test.t", collModCmd, 2);

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}

        // Remote collection options are empty.
        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db, const UUID& uuid) const {
            called = true;
            return BSON("options" << BSONObj() << "info" << BSON("uuid" << uuid));
        }
        mutable bool called;
    };
    RollbackSourceLocal rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    startCapturingLogMessages();
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    stopCapturingLogMessages();

    ASSERT_TRUE(rollbackSource.called);
    for (const auto& message : getCapturedTextFormatLogMessages()) {
        ASSERT_TRUE(message.find("ignoring op with no _id during rollback. ns: test.t") ==
                    std::string::npos);
    }

    // Make sure the collection options are correct.
    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), NamespaceString("test.t"));
    auto collAfterRollbackOptions = autoColl->getCollectionOptions();
    ASSERT_BSONOBJ_EQ(BSON("uuid" << *options.uuid), collAfterRollbackOptions.toBSON());
}

TEST_F(RollbackResyncsCollectionOptionsTest,
       FullRemoteCollectionValidationOptionsAndEmptyLocalValidationOptions) {
    // Empty local collection options.
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();

    // Full remote collection validation options.
    BSONObj remoteCollOptionsObj =
        BSON("validator" << BSON("x" << BSON("$exists" << 1)) << "validationLevel"
                         << "moderate"
                         << "validationAction"
                         << "warn");

    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RollbackResyncsCollectionOptionsTest,
       PartialRemoteCollectionValidationOptionsAndEmptyLocalValidationOptions) {
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();

    BSONObj remoteCollOptionsObj = BSON("validationLevel"
                                        << "moderate"
                                        << "validationAction"
                                        << "warn");

    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RollbackResyncsCollectionOptionsTest,
       PartialRemoteCollectionValidationOptionsAndFullLocalValidationOptions) {
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();
    localCollOptions.validator = BSON("x" << BSON("$exists" << 1));
    localCollOptions.validationLevel = ValidationLevelEnum::moderate;
    localCollOptions.validationAction = ValidationActionEnum::warn;

    BSONObj remoteCollOptionsObj = BSON("validationLevel"
                                        << "strict"
                                        << "validationAction"
                                        << "error");


    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RollbackResyncsCollectionOptionsTest,
       EmptyRemoteCollectionValidationOptionsAndEmptyLocalValidationOptions) {
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();

    BSONObj remoteCollOptionsObj = BSONObj();

    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RollbackResyncsCollectionOptionsTest,
       EmptyRemoteCollectionValidationOptionsAndFullLocalValidationOptions) {
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();
    localCollOptions.validator = BSON("x" << BSON("$exists" << 1));
    localCollOptions.validationLevel = ValidationLevelEnum::moderate;
    localCollOptions.validationAction = ValidationActionEnum::warn;

    BSONObj remoteCollOptionsObj = BSONObj();

    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RollbackResyncsCollectionOptionsTest, LocalTempCollectionRemotePermanentCollection) {
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();
    localCollOptions.temp = true;

    BSONObj remoteCollOptionsObj = BSONObj();

    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RollbackResyncsCollectionOptionsTest, LocalPermanentCollectionRemoteTempCollection) {
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();

    BSONObj remoteCollOptionsObj = BSON("temp" << true);

    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RollbackResyncsCollectionOptionsTest, BothCollectionsTemp) {
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();
    localCollOptions.temp = true;

    BSONObj remoteCollOptionsObj = BSON("temp" << true);

    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RollbackResyncsCollectionOptionsTest, ChangingTempStatusAlsoChangesOtherCollectionOptions) {
    CollectionOptions localCollOptions;
    localCollOptions.uuid = UUID::gen();
    localCollOptions.temp = true;

    BSONObj remoteCollOptionsObj = BSON("validationLevel"
                                        << "strict"
                                        << "validationAction"
                                        << "error");

    resyncCollectionOptionsTest(localCollOptions, remoteCollOptionsObj);
}

TEST_F(RSRollbackTest, RollbackCollectionModificationCommandInvalidCollectionOptions) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);

    auto commonOperation = makeOpAndRecordId(1);

    BSONObj collModCmd = BSON("collMod"
                              << "t"
                              << "validationLevel"
                              << "strict");
    auto collectionModificationOperation =
        makeCommandOp(Timestamp(Seconds(2), 0), coll->uuid(), "test.t", collModCmd, 2);


    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}
        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db, const UUID& uuid) const {
            return BSON("options" << 12345);
        }
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    auto status =
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                     rollbackSource,
                     {},
                     {},
                     _coordinator,
                     _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Failed to parse options");
}

TEST(RSRollbackTest, LocalEntryWithoutNsIsFatal) {
    const auto validOplogEntry = BSON("op"
                                      << "i"
                                      << "ui" << UUID::gen() << "ts" << Timestamp(1, 1) << "t"
                                      << 1LL << "ns"
                                      << "test.t"
                                      << "wall" << Date_t() << "o" << BSON("_id" << 1 << "a" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, validOplogEntry, false));
    const auto invalidOplogEntry = BSON("op"
                                        << "i"
                                        << "ui" << UUID::gen() << "ts" << Timestamp(1, 1) << "t"
                                        << 1LL << "ns"
                                        << ""
                                        << "wall" << Date_t() << "o"
                                        << BSON("_id" << 1 << "a" << 1));
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(
                      nullptr /* opCtx */, OplogInterfaceMock(), fui, invalidOplogEntry, false),
                  RSFatalException);
}

TEST(RSRollbackTest, LocalEntryWithoutOIsFatal) {
    const auto validOplogEntry = BSON("op"
                                      << "i"
                                      << "ui" << UUID::gen() << "ts" << Timestamp(1, 1) << "t"
                                      << 1LL << "ns"
                                      << "test.t"
                                      << "wall" << Date_t() << "o" << BSON("_id" << 1 << "a" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, validOplogEntry, false));
    const auto invalidOplogEntry = BSON("op"
                                        << "i"
                                        << "ui" << UUID::gen() << "ts" << Timestamp(1, 1) << "t"
                                        << 1LL << "ns"
                                        << "test.t"
                                        << "wall" << Date_t() << "o" << BSONObj());
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(
                      nullptr /* opCtx */, OplogInterfaceMock(), fui, invalidOplogEntry, false),
                  RSFatalException);
}

DEATH_TEST_F(RSRollbackTest, LocalUpdateEntryWithoutO2IsFatal, "Fatal assertion") {
    const auto invalidOplogEntry = BSON("op"
                                        << "u"
                                        << "ui" << UUID::gen() << "ts" << Timestamp(1, 1) << "t"
                                        << 1LL << "ns"
                                        << "test.t"
                                        << "wall" << Date_t() << "o"
                                        << BSON("_id" << 1 << "a" << 1));
    FixUpInfo fui;
    updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, invalidOplogEntry, false)
        .ignore();
}

TEST(RSRollbackTest, LocalUpdateEntryWithEmptyO2IsFatal) {
    const auto validOplogEntry = BSON("op"
                                      << "u"
                                      << "ui" << UUID::gen() << "ts" << Timestamp(1, 1) << "t"
                                      << 1LL << "ns"
                                      << "test.t"
                                      << "wall" << Date_t() << "o" << BSON("_id" << 1 << "a" << 1)
                                      << "o2" << BSON("_id" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, validOplogEntry, false));
    const auto invalidOplogEntry = BSON("op"
                                        << "u"
                                        << "ui" << UUID::gen() << "ts" << Timestamp(1, 1) << "t"
                                        << 1LL << "ns"
                                        << "test.t"
                                        << "wall" << Date_t() << "o" << BSON("_id" << 1 << "a" << 1)
                                        << "o2" << BSONObj());
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(
                      nullptr /* opCtx */, OplogInterfaceMock(), fui, invalidOplogEntry, false),
                  RSFatalException);
}

DEATH_TEST_F(RSRollbackTest, LocalEntryWithTxnNumberWithoutSessionIdIsFatal, "invariant") {
    auto validOplogEntry = BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "op"
                                     << "i"
                                     << "ui" << UUID::gen() << "ns"
                                     << "test.t"
                                     << "wall" << Date_t() << "o" << BSON("_id" << 1 << "a" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, validOplogEntry, false));

    const auto txnNumber = BSON("txnNumber" << 1LL);
    const auto noSessionIdOrStmtId = validOplogEntry.addField(txnNumber.firstElement());

    const auto stmtId = BSON("stmtId" << 1);
    const auto noSessionId = noSessionIdOrStmtId.addField(stmtId.firstElement());
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(
                      nullptr /* opCtx */, OplogInterfaceMock(), fui, noSessionId, false),
                  RSFatalException);
}

TEST_F(RSRollbackTest, LocalEntryWithTxnNumberWithoutTxnTableUUIDIsFatal) {
    // If txnNumber is present, but the transaction collection has no UUID, rollback fails.
    UUID uuid = UUID::gen();
    auto lsid = makeLogicalSessionIdForTest();
    auto entryWithTxnNumber =
        BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "op"
                  << "i"
                  << "ui" << uuid << "ns"
                  << "test.t"
                  << "wall" << Date_t() << "o" << BSON("_id" << 1 << "a" << 1) << "txnNumber" << 1LL
                  << "stmtId" << 1 << "lsid" << lsid.toBSON());

    FixUpInfo fui;
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(
                      nullptr /* opCtx */, OplogInterfaceMock(), fui, entryWithTxnNumber, false),
                  RSFatalException);
}

TEST_F(RSRollbackTest, LocalEntryWithTxnNumberAddsTransactionTableDocToBeRefetched) {
    FixUpInfo fui;

    // With no txnNumber present, no extra documents need to be refetched.
    auto entryWithoutTxnNumber =
        BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "op"
                  << "i"
                  << "ui" << UUID::gen() << "ns"
                  << "test.t2"
                  << "wall" << Date_t() << "o" << BSON("_id" << 2 << "a" << 2));

    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, entryWithoutTxnNumber, false));
    ASSERT_EQ(fui.docsToRefetch.size(), 1U);

    // If txnNumber is present, and the transaction table exists and has a UUID, the session
    // transactions table document corresponding to the oplog entry's sessionId also needs to be
    // refetched.
    UUID uuid = UUID::gen();
    auto lsid = makeLogicalSessionIdForTest();
    auto entryWithTxnNumber =
        BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "op"
                  << "i"
                  << "ui" << uuid << "ns"
                  << "test.t"
                  << "wall" << Date_t() << "o" << BSON("_id" << 1 << "a" << 1) << "txnNumber" << 1LL
                  << "stmtId" << 1 << "lsid" << lsid.toBSON());
    UUID transactionTableUUID = UUID::gen();
    fui.transactionTableUUID = transactionTableUUID;

    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, entryWithTxnNumber, false));
    ASSERT_EQ(fui.docsToRefetch.size(), 3U);

    auto expectedObj = BSON("_id" << lsid.toBSON());
    DocID expectedTxnDoc(expectedObj, expectedObj.firstElement(), transactionTableUUID);
    ASSERT_TRUE(fui.docsToRefetch.find(expectedTxnDoc) != fui.docsToRefetch.end());
}

TEST_F(RSRollbackTest, LocalEntryWithPartialTxnAddsTransactionTableDocToBeRefetched) {
    FixUpInfo fui;

    // If txnNumber is present, and the transaction table exists and has a UUID, the session
    // transactions table document corresponding to the oplog entry's sessionId also needs to be
    // refetched.  This is true even if "partialTxn" is set indicating this is part of a transaction
    // that may not have been committed.
    UUID uuid = UUID::gen();
    auto lsid = makeLogicalSessionIdForTest();
    auto entryWithTxnNumber =
        BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 1 << "a" << 1)))
                                     << "partialTxn" << true)
                  << "txnNumber" << 1LL << "stmtId" << 1 << "lsid" << lsid.toBSON());
    UUID transactionTableUUID = UUID::gen();
    fui.transactionTableUUID = transactionTableUUID;

    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, entryWithTxnNumber, false));
    ASSERT_EQ(fui.docsToRefetch.size(), 1U);

    auto expectedObj = BSON("_id" << lsid.toBSON());
    DocID expectedTxnDoc(expectedObj, expectedObj.firstElement(), transactionTableUUID);
    ASSERT_TRUE(fui.docsToRefetch.find(expectedTxnDoc) != fui.docsToRefetch.end());
}

TEST_F(RSRollbackTest, LocalAbortTxnRefetchesTransactionTableEntry) {
    // A rolled back abort, even if we rolled back no transaction operations, should refetch the
    // transaction table entry.
    FixUpInfo fui;

    auto lsid = makeLogicalSessionIdForTest();
    auto abortTxnEntry =
        BSON("ts" << Timestamp(Seconds(1), 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o" << BSON("abortTransaction" << 1) << "txnNumber"
                  << 1LL << "stmtId" << 1 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL));

    UUID transactionTableUUID = UUID::gen();
    fui.transactionTableUUID = transactionTableUUID;

    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, abortTxnEntry, false));
    ASSERT_EQ(fui.docsToRefetch.size(), 1U);

    auto expectedObj = BSON("_id" << lsid.toBSON());
    DocID expectedTxnDoc(expectedObj, expectedObj.firstElement(), transactionTableUUID);
    ASSERT_TRUE(fui.docsToRefetch.find(expectedTxnDoc) != fui.docsToRefetch.end());
}

TEST_F(RSRollbackTest, LocalEntryWithAbortedPartialTxnRefetchesOnlyTransactionTableEntry) {
    FixUpInfo fui;

    // If txnNumber is present, and the transaction table exists and has a UUID, the session
    // transactions table document corresponding to the oplog entry's sessionId also needs to be
    // refetched.  This is true even if "partialTxn" is set indicating this is part of a transaction
    // that may not have been committed, and even if it is known that the transaction aborted.
    UUID uuid = UUID::gen();
    auto lsid = makeLogicalSessionIdForTest();
    auto abortTxnEntry =
        BSON("ts" << Timestamp(Seconds(1), 2) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o" << BSON("abortTransaction" << 1) << "txnNumber"
                  << 1LL << "stmtId" << 1 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(Seconds(1), 1) << "t" << 1LL));

    auto entryWithTxnNumber =
        BSON("ts" << Timestamp(Seconds(1), 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 1 << "a" << 1)))
                                     << "partialTxn" << true)
                  << "txnNumber" << 1LL << "stmtId" << 1 << "lsid" << lsid.toBSON());
    UUID transactionTableUUID = UUID::gen();
    fui.transactionTableUUID = transactionTableUUID;

    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, abortTxnEntry, false));
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, entryWithTxnNumber, false));
    ASSERT_EQ(fui.docsToRefetch.size(), 1U);

    auto expectedObj = BSON("_id" << lsid.toBSON());
    DocID expectedTxnDoc(expectedObj, expectedObj.firstElement(), transactionTableUUID);
    ASSERT_TRUE(fui.docsToRefetch.find(expectedTxnDoc) != fui.docsToRefetch.end());
}

TEST_F(RSRollbackTest, LocalEntryWithCommittedTxnRefetchesDocsAndTransactionTableEntry) {
    FixUpInfo fui;
    UUID uuid = UUID::gen();
    auto lsid = makeLogicalSessionIdForTest();
    auto commitTxnEntry =
        BSON("ts" << Timestamp(Seconds(1), 2) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 2 << "a" << 2)))
                                     << "count" << 2)
                  << "txnNumber" << 1LL << "stmtId" << 2 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(Seconds(1), 1) << "t" << 1LL));
    auto commitTxnOperation = std::make_pair(commitTxnEntry, RecordId(2));

    auto partialTxnEntry =
        BSON("ts" << Timestamp(Seconds(1), 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 1 << "a" << 1)))
                                     << "partialTxn" << true)
                  << "txnNumber" << 1LL << "stmtId" << 1 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(0, 0) << "t" << -1LL));

    auto partialTxnOperation = std::make_pair(partialTxnEntry, RecordId(1));
    UUID transactionTableUUID = UUID::gen();
    fui.transactionTableUUID = transactionTableUUID;

    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */,
        OplogInterfaceMock({commitTxnOperation, partialTxnOperation}),
        fui,
        commitTxnEntry,
        false));
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */,
        OplogInterfaceMock({commitTxnOperation, partialTxnOperation}),
        fui,
        partialTxnEntry,
        false));
    ASSERT_EQ(fui.docsToRefetch.size(), 3U);

    auto expectedObj = BSON("_id" << lsid.toBSON());
    DocID expectedTxnDoc(expectedObj, expectedObj.firstElement(), transactionTableUUID);
    ASSERT_TRUE(fui.docsToRefetch.find(expectedTxnDoc) != fui.docsToRefetch.end());

    auto expectedCrudObj = BSON("_id" << 2);
    auto expectedCrudDoc = DocID(expectedObj, expectedObj.firstElement(), transactionTableUUID);
    ASSERT_TRUE(fui.docsToRefetch.find(expectedCrudDoc) != fui.docsToRefetch.end());

    expectedCrudObj = BSON("_id" << 1);
    expectedCrudDoc = DocID(expectedObj, expectedObj.firstElement(), transactionTableUUID);
    ASSERT_TRUE(fui.docsToRefetch.find(expectedCrudDoc) != fui.docsToRefetch.end());
}

TEST_F(RSRollbackTest, RollbackFetchesTransactionOperationBeforeCommonPoint) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    options.uuid = UUID::gen();
    auto txnTable = _createCollection(_opCtx.get(), "config.transactions", options);

    auto commonOperation = makeOpAndRecordId(10);
    UUID uuid = coll->uuid();
    auto lsid = makeLogicalSessionIdForTest();
    auto commitTxnEntry =
        BSON("ts" << Timestamp(Seconds(10), 12) << "t" << 10LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 0 << "a" << 0)))
                                     << "count" << 3)
                  << "txnNumber" << 1LL << "stmtId" << 3 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(Seconds(10), 11) << "t" << 10LL));
    auto commitTxnOperation = std::make_pair(commitTxnEntry, RecordId(12));

    auto entryAfterCommonPoint =
        BSON("ts" << Timestamp(Seconds(10), 11) << "t" << 10LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 1 << "a" << 1)))
                                     << "partialTxn" << true)
                  << "txnNumber" << 1LL << "stmtId" << 2 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(Seconds(10), 9) << "t" << 10LL));
    auto operationAfterCommonPoint = std::make_pair(entryAfterCommonPoint, RecordId(11));

    auto entryBeforeCommonPoint =
        BSON("ts" << Timestamp(Seconds(10), 9) << "t" << 10LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 2 << "a" << 2)))
                                     << "partialTxn" << true)
                  << "txnNumber" << 1LL << "stmtId" << 1 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(0, 0) << "t" << -1LL));
    auto operationBeforeCommonPoint = std::make_pair(entryBeforeCommonPoint, RecordId(9));

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog, const UUID& txnTableUuid)
            : RollbackSourceMock(std::move(oplog)), _txnTableUuid(txnTableUuid) {}

        std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                          UUID uuid,
                                                          const BSONObj& filter) const override {
            int numFields = 0;
            if (uuid == _txnTableUuid) {
                // This unit test does not test transaction table fetches.
                return {BSONObj(), NamespaceString::kSessionTransactionsTableNamespace};
            }
            for (const auto& element : filter) {
                ++numFields;
                ASSERT_EQUALS("_id", element.fieldNameStringData()) << filter;
            }
            ASSERT_EQUALS(1, numFields) << filter;
            searchedIds.insert(filter.firstElement().numberInt());
            switch (filter.firstElement().numberInt()) {
                case 0:
                    return {BSON("_id" << 0 << "v" << 0), NamespaceString()};
                case 1:
                    return {BSON("_id" << 1 << "v" << 1), NamespaceString()};
                case 2:
                    return {BSON("_id" << 2 << "v" << 3), NamespaceString()};
            }
            FAIL("Unexpected findOne request") << filter;
            return {};  // Unreachable; why doesn't compiler know?
        }

        mutable std::multiset<int> searchedIds;

    private:
        UUID _txnTableUuid;

    } rollbackSource(std::unique_ptr<OplogInterface>(
                         new OplogInterfaceMock({commonOperation, operationBeforeCommonPoint})),
                     txnTable->uuid());

    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({commitTxnOperation,
                                               operationAfterCommonPoint,
                                               commonOperation,
                                               operationBeforeCommonPoint}),
                           rollbackSource,
                           {},
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    ASSERT_EQUALS(3U, rollbackSource.searchedIds.size());
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(0));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(1));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(2));
}

TEST_F(RSRollbackTest, RollbackIncompleteTransactionReturnsUnrecoverableRollbackError) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    options.uuid = UUID::gen();
    auto txnTable = _createCollection(_opCtx.get(), "config.transactions", options);

    auto commonOperation = makeOpAndRecordId(10);
    UUID uuid = coll->uuid();
    auto lsid = makeLogicalSessionIdForTest();
    auto commitTxnEntry =
        BSON("ts" << Timestamp(Seconds(10), 12) << "t" << 10LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 0 << "a" << 0)))
                                     << "count" << 3)
                  << "stmtId" << 3 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(Seconds(10), 11) << "t" << 10LL));
    auto commitTxnOperation = std::make_pair(commitTxnEntry, RecordId(12));

    auto entryAfterCommonPoint =
        BSON("ts" << Timestamp(Seconds(10), 11) << "t" << 10LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ui" << uuid << "ns"
                                                        << "test.t"
                                                        << "o" << BSON("_id" << 1 << "a" << 1)))
                                     << "partialTxn" << true)
                  << "txnNumber" << 1LL << "stmtId" << 2 << "lsid" << lsid.toBSON() << "prevOpTime"
                  << BSON("ts" << Timestamp(Seconds(10), 9) << "t" << 10LL));
    auto operationAfterCommonPoint = std::make_pair(entryAfterCommonPoint, RecordId(11));

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog, const UUID& txnTableUuid)
            : RollbackSourceMock(std::move(oplog)), _txnTableUuid(txnTableUuid) {}

        std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                          UUID uuid,
                                                          const BSONObj& filter) const override {
            if (uuid == _txnTableUuid) {
                // This unit test does not test transaction table fetches.
                return {BSONObj(), NamespaceString::kSessionTransactionsTableNamespace};
            } else {
                return {BSONObj(), NamespaceString()};
            }
        }

    private:
        UUID _txnTableUuid;
    } rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})),
                     txnTable->uuid());


    auto status = syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({commitTxnOperation, operationAfterCommonPoint, commonOperation}),
        rollbackSource,
        {},
        {},
        _coordinator,
        _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
}

TEST_F(RSRollbackTest, RollbackFailsIfTransactionDocumentRefetchReturnsDifferentNamespace) {
    createOplog(_opCtx.get());

    // Create a valid FixUpInfo struct for rolling back a single CRUD operation that has a
    // transaction number and session id.
    FixUpInfo fui;

    auto entryWithTxnNumber =
        BSON("ts" << Timestamp(Seconds(2), 1) << "t" << 1LL << "op"
                  << "i"
                  << "ui" << UUID::gen() << "ns"
                  << "test.t"
                  << "wall" << Date_t() << "o" << BSON("_id" << 1 << "a" << 1) << "txnNumber" << 1LL
                  << "stmtId" << 1 << "lsid" << makeLogicalSessionIdForTest().toBSON());

    UUID transactionTableUUID = UUID::gen();
    fui.transactionTableUUID = transactionTableUUID;

    auto commonOperation = makeOpAndRecordId(1);
    fui.commonPoint = OpTime(Timestamp(Seconds(1), 1), 1LL);
    fui.commonPointOurDiskloc = RecordId(1);

    fui.rbid = 1;

    // The FixUpInfo will have an extra doc to refetch: the corresponding transaction table entry.
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(
        nullptr /* opCtx */, OplogInterfaceMock(), fui, entryWithTxnNumber, false));
    ASSERT_EQ(fui.docsToRefetch.size(), 2U);

    {
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)) {}
            std::pair<BSONObj, NamespaceString> findOneByUUID(
                const std::string& db, UUID uuid, const BSONObj& filter) const override {
                return {BSONObj(), NamespaceString::kSessionTransactionsTableNamespace};
            }
            int getRollbackId() const override {
                return 1;
            }
        };

        // Should not throw, since findOneByUUID will return the expected namespace.
        syncFixUp(_opCtx.get(),
                  fui,
                  RollbackSourceLocal(
                      std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation}))),
                  _coordinator,
                  _replicationProcess.get());
    }

    {
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)) {}
            std::pair<BSONObj, NamespaceString> findOneByUUID(
                const std::string& db, UUID uuid, const BSONObj& filter) const override {
                return {BSONObj(), NamespaceString("foo.bar")};
            }
            int getRollbackId() const override {
                return 1;
            }
        };

        // The returned namespace will not be the expected one, implying a rename/drop of the
        // transactions collection across this node and the sync source, so rollback should fail.
        ASSERT_THROWS(syncFixUp(_opCtx.get(),
                                fui,
                                RollbackSourceLocal(std::unique_ptr<OplogInterface>(
                                    new OplogInterfaceMock({commonOperation}))),
                                _coordinator,
                                _replicationProcess.get()),
                      RSFatalException);
    }
}

TEST_F(RSRollbackTest, RollbackReturnsImmediatelyOnFailureToTransitionToRollback) {
    // On failing to transition to ROLLBACK, rollback() should return immediately and not call
    // syncRollback(). We provide an empty oplog so that if syncRollback() is called erroneously,
    // we would go fatal.
    OplogInterfaceMock localOplogWithSingleOplogEntry({makeNoopOplogEntryAndRecordId(Seconds(1))});
    RollbackSourceMock rollbackSourceWithInvalidOplog(std::make_unique<OplogInterfaceMock>());

    // Inject ReplicationCoordinator::setFollowerMode() error. We set the current member state
    // because it will be logged by rollback() on failing to transition to ROLLBACK.
    ASSERT_OK(_coordinator->setFollowerMode(MemberState::RS_SECONDARY));
    _coordinator->failSettingFollowerMode(MemberState::RS_ROLLBACK, ErrorCodes::NotSecondary);

    startCapturingLogMessages();
    rollback(_opCtx.get(),
             localOplogWithSingleOplogEntry,
             rollbackSourceWithInvalidOplog,
             {},
             _coordinator,
             _replicationProcess.get());
    stopCapturingLogMessages();

    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining("Cannot perform replica set state transition"));
    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY), _coordinator->getMemberState());
}

DEATH_TEST_REGEX_F(RSRollbackTest,
                   RollbackUnrecoverableRollbackErrorTriggersFatalAssertion,
                   "Unable to complete rollback. A full resync may be needed") {
    // rollback() should abort on getting UnrecoverableRollbackError from syncRollback(). An empty
    // local oplog will make syncRollback() return the intended error.
    OplogInterfaceMock localOplogWithSingleOplogEntry({makeNoopOplogEntryAndRecordId(Seconds(1))});
    RollbackSourceMock rollbackSourceWithInvalidOplog(std::make_unique<OplogInterfaceMock>());

    rollback(_opCtx.get(),
             localOplogWithSingleOplogEntry,
             rollbackSourceWithInvalidOplog,
             {},
             _coordinator,
             _replicationProcess.get());
}

TEST_F(RSRollbackTest, RollbackLogsRetryMessageAndReturnsOnNonUnrecoverableRollbackError) {
    // If local oplog is empty, syncRollback() returns OplogStartMissing (instead of
    // UnrecoverableRollbackError when the remote oplog is missing). rollback() should log a message
    // about retrying rollback later before returning.
    OplogInterfaceMock localOplogWithNoEntries;
    OplogInterfaceMock::Operations remoteOperations({makeNoopOplogEntryAndRecordId(Seconds(1))});
    auto remoteOplog = std::make_unique<OplogInterfaceMock>(remoteOperations);
    RollbackSourceMock rollbackSourceWithValidOplog(std::move(remoteOplog));
    auto noopSleepSecsFn = [](int) {};

    startCapturingLogMessages();
    rollback(_opCtx.get(),
             localOplogWithNoEntries,
             rollbackSourceWithValidOplog,
             {},
             _coordinator,
             _replicationProcess.get(),
             noopSleepSecsFn);
    stopCapturingLogMessages();

    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Rollback cannot complete at this time (retrying later)"));
    ASSERT_EQUALS(MemberState(MemberState::RS_RECOVERING), _coordinator->getMemberState());
}

DEATH_TEST_F(RSRollbackTest,
             RollbackTriggersFatalAssertionOnDetectingShardIdentityDocumentRollback,
             "shardIdentity document rollback detected.  Shutting down to clear in-memory sharding "
             "state.  Restarting this process should safely return it to a healthy state") {
    auto commonOperation = makeNoopOplogEntryAndRecordId(Seconds(1));
    OplogInterfaceMock localOplog({commonOperation});
    RollbackSourceMock rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    ASSERT_FALSE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());
    ShardIdentityRollbackNotifier::get(_opCtx.get())->recordThatRollbackHappened();
    ASSERT_TRUE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());

    createOplog(_opCtx.get());
    rollback(_opCtx.get(), localOplog, rollbackSource, {}, _coordinator, _replicationProcess.get());
}

DEATH_TEST_REGEX_F(
    RSRollbackTest,
    RollbackTriggersFatalAssertionOnFailingToTransitionToRecoveringAfterSyncRollbackReturns,
    "Failed to perform replica set state transition") {
    auto commonOperation = makeNoopOplogEntryAndRecordId(Seconds(1));
    OplogInterfaceMock localOplog({commonOperation});
    RollbackSourceMock rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    _coordinator->failSettingFollowerMode(MemberState::RS_RECOVERING, ErrorCodes::IllegalOperation);

    createOplog(_opCtx.get());
    rollback(_opCtx.get(), localOplog, rollbackSource, {}, _coordinator, _replicationProcess.get());
}

// The testcases used here are trying to detect off-by-one errors in
// FixUpInfo::removeAllDocsToRefectchFor.
TEST(FixUpInfoTest, RemoveAllDocsToRefetchForWorks) {
    const auto normalHolder = BSON("" << OID::gen());
    const auto normalKey = normalHolder.firstElement();

    UUID uuid1 = UUID::gen();
    UUID uuid2 = UUID::gen();
    UUID uuid3 = UUID::gen();

    // Can't use ASSERT_EQ with this since it isn't ostream-able. Failures will at least give you
    // the size. If that isn't enough, use GDB.
    using DocSet = std::set<DocID>;

    FixUpInfo fui;
    fui.docsToRefetch = {
        DocID::minFor(uuid1),
        DocID{{}, normalKey, uuid1},
        DocID::maxFor(uuid1),

        DocID::minFor(uuid2),
        DocID{{}, normalKey, uuid2},
        DocID::maxFor(uuid2),

        DocID::minFor(uuid3),
        DocID{{}, normalKey, uuid3},
        DocID::maxFor(uuid3),
    };

    // Remove from the middle.
    fui.removeAllDocsToRefetchFor(uuid2);
    ASSERT((fui.docsToRefetch ==
            DocSet{
                DocID::minFor(uuid1),
                DocID{{}, normalKey, uuid1},
                DocID::maxFor(uuid1),

                DocID::minFor(uuid3),
                DocID{{}, normalKey, uuid3},
                DocID::maxFor(uuid3),
            }))
        << "remaining docs: " << fui.docsToRefetch.size();

    // Remove from the end.
    fui.removeAllDocsToRefetchFor(uuid3);
    ASSERT((fui.docsToRefetch ==
            DocSet{
                DocID::minFor(uuid1),  // This comment helps clang-format.
                DocID{{}, normalKey, uuid1},
                DocID::maxFor(uuid1),
            }))
        << "remaining docs: " << fui.docsToRefetch.size();

    // Everything else.
    fui.removeAllDocsToRefetchFor(uuid1);
    ASSERT((fui.docsToRefetch == DocSet{})) << "remaining docs: " << fui.docsToRefetch.size();
}

TEST_F(RSRollbackTest, RollbackInvalidatesDefaultRWConcernCache) {
    auto& rwcDefaults = ReadWriteConcernDefaults::get(getServiceContext());

    // Put initial defaults in the cache.
    {
        RWConcernDefault origDefaults;
        origDefaults.setUpdateOpTime(Timestamp(10, 20));
        origDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
        _lookupMock.setLookupCallReturnValue(std::move(origDefaults));
    }
    auto origCachedDefaults = rwcDefaults.getDefault(_opCtx.get());
    ASSERT_EQ(Timestamp(10, 20), *origCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *origCachedDefaults.getUpdateWallClockTime());

    // Change the mock's defaults, but don't invalidate the cache yet. The cache should still return
    // the original defaults.
    {
        RWConcernDefault newDefaults;
        newDefaults.setUpdateOpTime(Timestamp(50, 20));
        newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
        _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

        auto cachedDefaults = rwcDefaults.getDefault(_opCtx.get());
        ASSERT_EQ(Timestamp(10, 20), *cachedDefaults.getUpdateOpTime());
        ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *cachedDefaults.getUpdateWallClockTime());
    }

    // Rollback via refetch should invalidate the cache and getting the defaults should now return
    // the latest value.
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    BSONObj doc = BSON("_id" << 0 << "a" << 1);
    _testRollbackDelete(_opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid(), doc);

    auto newCachedDefaults = rwcDefaults.getDefault(_opCtx.get());
    ASSERT_EQ(Timestamp(50, 20), *newCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(5678), *newCachedDefaults.getUpdateWallClockTime());
}

}  // namespace
}  // namespace mongo
