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

#include "mongo/db/storage/recovery_unit_test_harness.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
std::function<std::unique_ptr<RecoveryUnitHarnessHelper>()> recoveryUnitHarnessFactory;
}
}  // namespace mongo

void mongo::registerRecoveryUnitHarnessHelperFactory(
    std::function<std::unique_ptr<RecoveryUnitHarnessHelper>()> factory) {
    recoveryUnitHarnessFactory = std::move(factory);
}

namespace mongo {

auto newRecoveryUnitHarnessHelper() -> std::unique_ptr<RecoveryUnitHarnessHelper> {
    return recoveryUnitHarnessFactory();
}

namespace {

class RecoveryUnitTestHarness : public unittest::Test {
public:
    void setUp() override {
        harnessHelper = newRecoveryUnitHarnessHelper();
        opCtx = harnessHelper->newOperationContext();
        ru = opCtx->recoveryUnit();
    }

    std::unique_ptr<RecoveryUnitHarnessHelper> harnessHelper;
    ServiceContext::UniqueOperationContext opCtx;
    RecoveryUnit* ru;
};

class TestChange final : public RecoveryUnit::Change {
public:
    TestChange(int* count) : _count(count) {}

    void commit(boost::optional<Timestamp>) override {
        *_count = *_count + 1;
    }

    void rollback() override {
        *_count = *_count - 1;
    }

private:
    int* _count;
};

TEST_F(RecoveryUnitTestHarness, CommitUnitOfWork) {
    Lock::GlobalLock globalLk(opCtx.get(), MODE_IX);
    const auto rs = harnessHelper->createRecordStore(opCtx.get(), "table1");
    opCtx->lockState()->beginWriteUnitOfWork();
    ru->beginUnitOfWork(opCtx.get());
    StatusWith<RecordId> s = rs->insertRecord(opCtx.get(), "data", 4, Timestamp());
    ASSERT_TRUE(s.isOK());
    ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    ru->commitUnitOfWork();
    opCtx->lockState()->endWriteUnitOfWork();
    RecordData rd;
    ASSERT_TRUE(rs->findRecord(opCtx.get(), s.getValue(), &rd));
}

TEST_F(RecoveryUnitTestHarness, AbortUnitOfWork) {
    Lock::GlobalLock globalLk(opCtx.get(), MODE_IX);
    const auto rs = harnessHelper->createRecordStore(opCtx.get(), "table1");
    opCtx->lockState()->beginWriteUnitOfWork();
    ru->beginUnitOfWork(opCtx.get());
    StatusWith<RecordId> s = rs->insertRecord(opCtx.get(), "data", 4, Timestamp());
    ASSERT_TRUE(s.isOK());
    ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    ru->abortUnitOfWork();
    opCtx->lockState()->endWriteUnitOfWork();
    ASSERT_FALSE(rs->findRecord(opCtx.get(), s.getValue(), nullptr));
}

TEST_F(RecoveryUnitTestHarness, CommitAndRollbackChanges) {
    int count = 0;
    const auto rs = harnessHelper->createRecordStore(opCtx.get(), "table1");

    ru->beginUnitOfWork(opCtx.get());
    ru->registerChange(std::make_unique<TestChange>(&count));
    ASSERT_EQUALS(count, 0);
    ru->commitUnitOfWork();
    ASSERT_EQUALS(count, 1);

    ru->beginUnitOfWork(opCtx.get());
    ru->registerChange(std::make_unique<TestChange>(&count));
    ASSERT_EQUALS(count, 1);
    ru->abortUnitOfWork();
    ASSERT_EQUALS(count, 0);
}

TEST_F(RecoveryUnitTestHarness, CheckIsActiveWithCommit) {
    Lock::GlobalLock globalLk(opCtx.get(), MODE_IX);
    const auto rs = harnessHelper->createRecordStore(opCtx.get(), "table1");
    opCtx->lockState()->beginWriteUnitOfWork();
    ru->beginUnitOfWork(opCtx.get());
    // TODO SERVER-51787: to re-enable this.
    // ASSERT_TRUE(ru->isActive());
    StatusWith<RecordId> s = rs->insertRecord(opCtx.get(), "data", 4, Timestamp());
    ru->commitUnitOfWork();
    opCtx->lockState()->endWriteUnitOfWork();
    ASSERT_FALSE(ru->isActive());
}

TEST_F(RecoveryUnitTestHarness, CheckIsActiveWithAbort) {
    Lock::GlobalLock globalLk(opCtx.get(), MODE_IX);
    const auto rs = harnessHelper->createRecordStore(opCtx.get(), "table1");
    opCtx->lockState()->beginWriteUnitOfWork();
    ru->beginUnitOfWork(opCtx.get());
    // TODO SERVER-51787: to re-enable this.
    // ASSERT_TRUE(ru->isActive());
    StatusWith<RecordId> s = rs->insertRecord(opCtx.get(), "data", 4, Timestamp());
    ru->abortUnitOfWork();
    opCtx->lockState()->endWriteUnitOfWork();
    ASSERT_FALSE(ru->isActive());
}

TEST_F(RecoveryUnitTestHarness, BeginningUnitOfWorkDoesNotIncrementSnapshotId) {
    auto snapshotIdBefore = ru->getSnapshotId();
    ru->beginUnitOfWork(opCtx.get());
    ASSERT_EQ(snapshotIdBefore, ru->getSnapshotId());
    ru->abortUnitOfWork();
}

TEST_F(RecoveryUnitTestHarness, NewlyAllocatedRecoveryUnitHasNewSnapshotId) {
    auto newRu = harnessHelper->newRecoveryUnit();
    ASSERT_NE(newRu->getSnapshotId(), ru->getSnapshotId());
}

TEST_F(RecoveryUnitTestHarness, AbandonSnapshotIncrementsSnapshotId) {
    auto snapshotIdBefore = ru->getSnapshotId();
    ru->abandonSnapshot();
    ASSERT_NE(snapshotIdBefore, ru->getSnapshotId());
}

TEST_F(RecoveryUnitTestHarness, CommitUnitOfWorkIncrementsSnapshotId) {
    auto snapshotIdBefore = ru->getSnapshotId();
    ru->beginUnitOfWork(opCtx.get());
    ru->commitUnitOfWork();
    ASSERT_NE(snapshotIdBefore, ru->getSnapshotId());
}

TEST_F(RecoveryUnitTestHarness, AbortUnitOfWorkIncrementsSnapshotId) {
    auto snapshotIdBefore = ru->getSnapshotId();
    ru->beginUnitOfWork(opCtx.get());
    ru->abortUnitOfWork();
    ASSERT_NE(snapshotIdBefore, ru->getSnapshotId());
}

DEATH_TEST_F(RecoveryUnitTestHarness, RegisterChangeMustBeInUnitOfWork, "invariant") {
    int count = 0;
    opCtx->recoveryUnit()->registerChange(std::make_unique<TestChange>(&count));
}

DEATH_TEST_F(RecoveryUnitTestHarness, CommitMustBeInUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->commitUnitOfWork();
}

DEATH_TEST_F(RecoveryUnitTestHarness, AbortMustBeInUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->abortUnitOfWork();
}

DEATH_TEST_F(RecoveryUnitTestHarness, CannotHaveUnfinishedUnitOfWorkOnExit, "invariant") {
    opCtx->recoveryUnit()->beginUnitOfWork(opCtx.get());
}

DEATH_TEST_F(RecoveryUnitTestHarness, PrepareMustBeInUnitOfWork, "invariant") {
    try {
        opCtx->recoveryUnit()->prepareUnitOfWork();
    } catch (const ExceptionFor<ErrorCodes::CommandNotSupported>&) {
        bool prepareCommandSupported = false;
        invariant(prepareCommandSupported);
    }
}

DEATH_TEST_F(RecoveryUnitTestHarness, WaitUntilDurableMustBeOutOfUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->beginUnitOfWork(opCtx.get());
    opCtx->recoveryUnit()->waitUntilDurable(opCtx.get());
}

DEATH_TEST_F(RecoveryUnitTestHarness, AbandonSnapshotMustBeOutOfUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->beginUnitOfWork(opCtx.get());
    opCtx->recoveryUnit()->abandonSnapshot();
}

}  // namespace
}  // namespace mongo
