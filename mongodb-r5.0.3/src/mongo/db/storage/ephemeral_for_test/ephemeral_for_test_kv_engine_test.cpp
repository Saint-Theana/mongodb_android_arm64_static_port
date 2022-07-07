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

#include "mongo/db/storage/kv/kv_engine_test_harness.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_kv_engine.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace ephemeral_for_test {

class KVHarnessHelper : public mongo::KVHarnessHelper {
public:
    KVHarnessHelper(ServiceContext* svcCtx) : _engine(std::make_unique<KVEngine>()) {
        repl::ReplicationCoordinator::set(
            svcCtx,
            std::unique_ptr<repl::ReplicationCoordinator>(new repl::ReplicationCoordinatorMock(
                getGlobalServiceContext(), repl::ReplSettings())));
    }

    virtual KVEngine* getEngine() override {
        return _engine.get();
    }

    virtual KVEngine* restartEngine() override {
        return _engine.get();
    }

private:
    std::unique_ptr<KVEngine> _engine;
};

std::unique_ptr<mongo::KVHarnessHelper> makeHelper(ServiceContext* svcCtx) {
    return std::make_unique<KVHarnessHelper>(svcCtx);
}

MONGO_INITIALIZER(RegisterEphemeralForTestKVHarnessFactory)(InitializerContext*) {
    KVHarnessHelper::registerFactory(makeHelper);
}

class EphemeralForTestKVEngineTest : public ServiceContextTest {
public:
    EphemeralForTestKVEngineTest() : _helper(getServiceContext()), _engine(_helper.getEngine()) {}

protected:
    std::unique_ptr<KVHarnessHelper> helper;
    KVHarnessHelper _helper;
    KVEngine* _engine;
};

class OperationContextFromKVEngine : public OperationContextNoop {
public:
    OperationContextFromKVEngine(KVEngine* engine)
        : OperationContextNoop(engine->newRecoveryUnit()) {}
};


TEST_F(EphemeralForTestKVEngineTest, AvailableHistoryUpdate) {
    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<mongo::RecordStore> rs;
    {
        OperationContextFromKVEngine opCtx(_engine);
        ASSERT_OK(_engine->createRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions));
        rs = _engine->getRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions);
        ASSERT(rs);
    }

    Timestamp lastMaster;
    Timestamp currentMaster;

    ASSERT_EQ(1, _engine->getHistory_forTest().size());
    currentMaster = _engine->getHistory_forTest().rbegin()->first;
    ASSERT_EQ(_engine->getOldestTimestamp(), currentMaster);

    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        uow.commit();
    }

    ASSERT_EQ(1, _engine->getHistory_forTest().size());
    lastMaster = currentMaster;
    currentMaster = _engine->getHistory_forTest().rbegin()->first;
    ASSERT_GT(currentMaster, lastMaster);
    ASSERT_EQ(_engine->getOldestTimestamp(), currentMaster);
}

TEST_F(EphemeralForTestKVEngineTest, PinningOldestTimestampWithReadTransaction) {
    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<mongo::RecordStore> rs;
    {
        OperationContextFromKVEngine opCtx(_engine);
        ASSERT_OK(_engine->createRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions));
        rs = _engine->getRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions);
        ASSERT(rs);
    }

    // _availableHistory starts off with master.
    ASSERT_EQ(1, _engine->getHistory_forTest().size());

    RecordId loc;
    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    OperationContextFromKVEngine opCtxRead(_engine);
    RecordData rd;
    ASSERT(rs->findRecord(&opCtxRead, loc, &rd));

    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        uow.commit();
    }

    // Open read transaction prevents deletion of history.
    ASSERT_EQ(2, _engine->getHistory_forTest().size());
    ASSERT_GT(_engine->getHistory_forTest().rbegin()->first, _engine->getOldestTimestamp());
}

TEST_F(EphemeralForTestKVEngineTest, SettingOldestTimestampClearsHistory) {
    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<mongo::RecordStore> rs;
    {
        OperationContextFromKVEngine opCtx(_engine);
        ASSERT_OK(_engine->createRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions));
        rs = _engine->getRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions);
        ASSERT(rs);
    }

    // _availableHistory starts off with master.
    ASSERT_EQ(1, _engine->getHistory_forTest().size());

    RecordId loc;
    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    OperationContextFromKVEngine opCtxRead(_engine);
    RecordData rd;
    ASSERT(rs->findRecord(&opCtxRead, loc, &rd));

    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        uow.commit();
    }

    ASSERT_EQ(2, _engine->getHistory_forTest().size());
    _engine->setOldestTimestamp(_engine->getHistory_forTest().rbegin()->first, false);
    ASSERT_EQ(1, _engine->getHistory_forTest().size());
}

TEST_F(EphemeralForTestKVEngineTest, SettingOldestTimestampToMax) {
    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<mongo::RecordStore> rs;
    {
        OperationContextFromKVEngine opCtx(_engine);
        ASSERT_OK(_engine->createRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions));
        rs = _engine->getRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions);
        ASSERT(rs);
    }

    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        uow.commit();
    }

    // Check that setting oldest to Timestamp::max() does not clear history.
    ASSERT_GTE(_engine->getHistory_forTest().size(), 1);
    ASSERT_LT(_engine->getHistory_forTest().rbegin()->first, Timestamp::max());
    _engine->setOldestTimestamp(Timestamp::max(), true);
    ASSERT_GTE(_engine->getHistory_forTest().size(), 1);
    ASSERT_EQ(Timestamp::max(), _engine->getHistory_forTest().rbegin()->first);
}

TEST_F(EphemeralForTestKVEngineTest, CleanHistoryWithOpenTransaction) {
    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<mongo::RecordStore> rs;
    {
        OperationContextFromKVEngine opCtx(_engine);
        ASSERT_OK(_engine->createRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions));
        rs = _engine->getRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions);
        ASSERT(rs);
    }

    // _availableHistory starts off with master.
    ASSERT_EQ(1, _engine->getHistory_forTest().size());

    RecordId loc;
    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    OperationContextFromKVEngine opCtxRead(_engine);
    Timestamp readTime1 = _engine->getHistory_forTest().rbegin()->first;
    RecordData rd;
    ASSERT(rs->findRecord(&opCtxRead, loc, &rd));

    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        uow.commit();
    }

    Timestamp readTime2 = _engine->getHistory_forTest().rbegin()->first;

    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        uow.commit();
    }

    Timestamp readTime3 = _engine->getHistory_forTest().rbegin()->first;
    _engine->cleanHistory();

    // use_count() should be {2, 1, 2} without the copy from getHistory_forTest().
    ASSERT_EQ(3, _engine->getHistory_forTest().size());
    ASSERT_EQ(3, _engine->getHistory_forTest().at(readTime1).use_count());
    ASSERT_EQ(2, _engine->getHistory_forTest().at(readTime2).use_count());
    ASSERT_EQ(3, _engine->getHistory_forTest().at(readTime3).use_count());
}

TEST_F(EphemeralForTestKVEngineTest, ReadOlderSnapshotsSimple) {
    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<mongo::RecordStore> rs;
    {
        OperationContextFromKVEngine opCtx(_engine);
        ASSERT_OK(_engine->createRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions));
        rs = _engine->getRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions);
        ASSERT(rs);
    }

    // Pin oldest timestamp with a read transaction.
    OperationContextFromKVEngine pinningOldest(_engine);
    ASSERT(!rs->findRecord(&pinningOldest, RecordId(1), nullptr));

    // Set readFrom to timestamp with no committed transactions.
    Timestamp readFrom = _engine->getHistory_forTest().rbegin()->first;

    OperationContextFromKVEngine opCtx(_engine);
    WriteUnitOfWork uow1(&opCtx);
    StatusWith<RecordId> res1 =
        rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
    ASSERT_OK(res1.getStatus());
    RecordId loc1 = res1.getValue();
    uow1.commit();

    WriteUnitOfWork uow2(&opCtx);
    StatusWith<RecordId> res2 =
        rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
    ASSERT_OK(res2.getStatus());
    RecordId loc2 = res2.getValue();
    uow2.commit();

    RecordData rd;
    opCtx.recoveryUnit()->abandonSnapshot();
    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, readFrom);
    ASSERT(!rs->findRecord(&opCtx, loc1, &rd));
    ASSERT(!rs->findRecord(&opCtx, loc2, &rd));

    opCtx.recoveryUnit()->abandonSnapshot();
    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    ASSERT(rs->findRecord(&opCtx, loc1, &rd));
    ASSERT(rs->findRecord(&opCtx, loc2, &rd));
}

TEST_F(EphemeralForTestKVEngineTest, ReadOutdatedSnapshot) {
    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<mongo::RecordStore> rs;
    {
        OperationContextFromKVEngine opCtx(_engine);
        ASSERT_OK(_engine->createRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions));
        rs = _engine->getRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions);
        ASSERT(rs);
    }

    RecordId loc1;
    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc1 = res.getValue();
        uow.commit();
    }

    OperationContextFromKVEngine opCtxRead(_engine);
    RecordData rd;
    ASSERT(rs->findRecord(&opCtxRead, loc1, &rd));
    Timestamp readFrom = _engine->getHistory_forTest().rbegin()->first;

    RecordId loc2;
    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc2 = res.getValue();
        uow.commit();
    }

    ASSERT(rs->findRecord(&opCtxRead, loc1, &rd));
    opCtxRead.recoveryUnit()->abandonSnapshot();
    opCtxRead.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, readFrom);
    ASSERT_THROWS_CODE(
        rs->findRecord(&opCtxRead, loc1, &rd), DBException, ErrorCodes::SnapshotTooOld);
}

TEST_F(EphemeralForTestKVEngineTest, SetReadTimestampBehindOldestTimestamp) {
    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<mongo::RecordStore> rs;
    {
        OperationContextFromKVEngine opCtx(_engine);
        ASSERT_OK(_engine->createRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions));
        rs = _engine->getRecordStore(&opCtx, nss.ns(), ident, defaultCollectionOptions);
        ASSERT(rs);
    }

    RecordId loc1;
    {
        OperationContextFromKVEngine opCtx(_engine);
        WriteUnitOfWork uow(&opCtx);
        StatusWith<RecordId> res =
            rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc1 = res.getValue();
        uow.commit();
    }

    RecordData rd;
    Timestamp readFrom = _engine->getHistory_forTest().begin()->first;
    OperationContextFromKVEngine opCtx(_engine);
    WriteUnitOfWork uow(&opCtx);
    StatusWith<RecordId> res =
        rs->insertRecord(&opCtx, record.c_str(), record.length() + 1, Timestamp());
    ASSERT_OK(res.getStatus());
    RecordId loc2 = res.getValue();
    uow.commit();

    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, readFrom);
    _engine->setOldestTimestamp(Timestamp::max(), true);
    ASSERT_THROWS_CODE(rs->findRecord(&opCtx, loc2, &rd), DBException, ErrorCodes::SnapshotTooOld);

    opCtx.recoveryUnit()->abandonSnapshot();
    opCtx.recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    ASSERT(rs->findRecord(&opCtx, loc1, &rd));
    ASSERT(rs->findRecord(&opCtx, loc2, &rd));
}

}  // namespace ephemeral_for_test
}  // namespace mongo
