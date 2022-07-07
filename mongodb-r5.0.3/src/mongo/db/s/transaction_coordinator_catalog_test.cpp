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

#include "mongo/db/s/transaction_coordinator_catalog.h"
#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class TransactionCoordinatorCatalogTest : public TransactionCoordinatorTestFixture {
protected:
    void setUp() override {
        TransactionCoordinatorTestFixture::setUp();

        _coordinatorCatalog.emplace();
        _coordinatorCatalog->exitStepUp(Status::OK());
    }

    void tearDown() override {
        _coordinatorCatalog->onStepDown();
        _coordinatorCatalog.reset();

        TransactionCoordinatorTestFixture::tearDown();
    }

    void createCoordinatorInCatalog(OperationContext* opCtx,
                                    LogicalSessionId lsid,
                                    TxnNumber txnNumber) {
        auto newCoordinator = std::make_shared<TransactionCoordinator>(
            operationContext(),
            lsid,
            txnNumber,
            std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
            Date_t::max());

        _coordinatorCatalog->insert(opCtx, lsid, txnNumber, newCoordinator);
    }

    boost::optional<TransactionCoordinatorCatalog> _coordinatorCatalog;
};

TEST_F(TransactionCoordinatorCatalogTest, GetOnSessionThatDoesNotExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    auto coordinator = _coordinatorCatalog->get(operationContext(), lsid, txnNumber);
    ASSERT(coordinator == nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest,
       GetOnSessionThatExistsButTxnNumberThatDoesntExistReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber);
    auto coordinatorInCatalog = _coordinatorCatalog->get(operationContext(), lsid, txnNumber + 1);
    ASSERT(coordinatorInCatalog == nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest, CreateFollowedByGetReturnsCoordinator) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber);
    auto coordinatorInCatalog = _coordinatorCatalog->get(operationContext(), lsid, txnNumber);
    ASSERT(coordinatorInCatalog != nullptr);
}

TEST_F(TransactionCoordinatorCatalogTest, SecondCreateForSessionDoesNotOverwriteFirstCreate) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber1 = 1;
    TxnNumber txnNumber2 = 2;
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber1);
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber2);

    auto coordinator1InCatalog = _coordinatorCatalog->get(operationContext(), lsid, txnNumber1);
    ASSERT(coordinator1InCatalog != nullptr);
}

DEATH_TEST_F(TransactionCoordinatorCatalogTest,
             CreatingACoordinatorWithASessionIdAndTxnNumberThatAlreadyExistFails,
             "Invariant failure") {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber);
    // Re-creating w/ same session id and txn number should cause invariant failure
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber);
}

TEST_F(TransactionCoordinatorCatalogTest, GetLatestOnSessionWithNoCoordinatorsReturnsNone) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    auto latestTxnNumAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);
    ASSERT_FALSE(latestTxnNumAndCoordinator);
}

TEST_F(TransactionCoordinatorCatalogTest,
       CreateFollowedByGetLatestOnSessionReturnsOnlyCoordinator) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber);
    auto latestTxnNumAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);

    ASSERT_TRUE(latestTxnNumAndCoordinator);
    ASSERT_EQ(latestTxnNumAndCoordinator->first, txnNumber);
}

TEST_F(TransactionCoordinatorCatalogTest, CoordinatorsRemoveThemselvesFromCatalogWhenTheyComplete) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber);
    auto coordinator = _coordinatorCatalog->get(operationContext(), lsid, txnNumber);

    coordinator->cancelIfCommitNotYetStarted();
    coordinator->onCompletion().wait();

    // Wait for the coordinator to be removed before attempting to call getLatestOnSession() since
    // the coordinator is removed from the catalog asynchronously.
    _coordinatorCatalog->join();

    auto latestTxnNumAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);
    ASSERT_FALSE(latestTxnNumAndCoordinator);
}

TEST_F(TransactionCoordinatorCatalogTest,
       TwoCreatesFollowedByGetLatestOnSessionReturnsCoordinatorWithHighestTxnNumber) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber1 = 1;
    TxnNumber txnNumber2 = 2;
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber1);
    createCoordinatorInCatalog(operationContext(), lsid, txnNumber2);
    auto latestTxnNumAndCoordinator =
        _coordinatorCatalog->getLatestOnSession(operationContext(), lsid);

    ASSERT_EQ(latestTxnNumAndCoordinator->first, txnNumber2);
}

TEST_F(TransactionCoordinatorCatalogTest, StepDownBeforeCoordinatorInsertedIntoCatalog) {
    LogicalSessionId lsid = makeLogicalSessionIdForTest();
    TxnNumber txnNumber = 1;

    txn::AsyncWorkScheduler aws(getServiceContext());
    TransactionCoordinatorCatalog catalog;
    catalog.exitStepUp(Status::OK());

    auto coordinator = std::make_shared<TransactionCoordinator>(operationContext(),
                                                                lsid,
                                                                txnNumber,
                                                                aws.makeChildScheduler(),
                                                                network()->now() + Seconds{5});

    aws.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Test step down"});
    catalog.onStepDown();

    advanceClockAndExecuteScheduledTasks();

    catalog.insert(operationContext(), lsid, txnNumber, coordinator);
    catalog.join();

    coordinator->onCompletion().wait();
}

}  // namespace
}  // namespace mongo
