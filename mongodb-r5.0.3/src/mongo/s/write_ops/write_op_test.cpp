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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss("foo.bar");

WriteErrorDetail buildError(int code, const BSONObj& info, const std::string& message) {
    WriteErrorDetail error;
    error.setStatus({ErrorCodes::Error(code), message});
    error.setErrInfo(info);

    return error;
}

write_ops::DeleteOpEntry buildDelete(const BSONObj& query, bool multi) {
    write_ops::DeleteOpEntry entry;
    entry.setQ(query);
    entry.setMulti(multi);
    return entry;
}

void sortByEndpoint(std::vector<TargetedWrite*>* writes) {
    struct EndpointComp {
        bool operator()(const TargetedWrite* writeA, const TargetedWrite* writeB) const {
            return writeA->endpoint.shardName.compare(writeB->endpoint.shardName) < 0;
        }
    };
    std::sort(writes->begin(), writes->end(), EndpointComp());
}

class WriteOpTest : public ServiceContextTest {
protected:
    const ServiceContext::UniqueOperationContext _opCtxHolder{makeOperationContext()};
    OperationContext* const _opCtx{_opCtxHolder.get()};
};

// Test of basic error-setting on write op
TEST_F(WriteOpTest, BasicError) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    const auto error(buildError(ErrorCodes::UnknownError, BSON("data" << 12345), "some message"));

    writeOp.setOpError(error);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().toStatus().code(), error.toStatus().code());
    ASSERT_EQUALS(writeOp.getOpError().getErrInfo()["data"].Int(),
                  error.getErrInfo()["data"].Int());
    ASSERT_EQUALS(writeOp.getOpError().toStatus().reason(), error.toStatus().reason());
}

TEST_F(WriteOpTest, TargetSingle) {
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED(), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op

    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    writeOp.noteWriteComplete(*targeted.front());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

// Multi-write targeting test where our query goes to one shard
TEST_F(WriteOpTest, TargetMultiOneShard) {
    ShardEndpoint endpointA(
        ShardId("shardA"), ChunkVersion(10, 0, OID(), boost::none /* timestamp */), boost::none);
    ShardEndpoint endpointB(
        ShardId("shardB"), ChunkVersion(20, 0, OID(), boost::none /* timestamp */), boost::none);
    ShardEndpoint endpointC(
        ShardId("shardB"), ChunkVersion(20, 0, OID(), boost::none /* timestamp */), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        // Only hits first shard
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -2 << LT << -1), false)});
        return deleteOp;
    }());

    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss,
                            {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                             MockRange(endpointB, BSON("x" << 0), BSON("x" << 10)),
                             MockRange(endpointC, BSON("x" << 10), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpointA);

    writeOp.noteWriteComplete(*targeted.front());

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

// Multi-write targeting test where our write goes to more than one shard
TEST_F(WriteOpTest, TargetMultiAllShards) {
    ShardEndpoint endpointA(
        ShardId("shardA"), ChunkVersion(10, 0, OID(), boost::none /* timestamp */), boost::none);
    ShardEndpoint endpointB(
        ShardId("shardB"), ChunkVersion(20, 0, OID(), boost::none /* timestamp */), boost::none);
    ShardEndpoint endpointC(
        ShardId("shardB"), ChunkVersion(20, 0, OID(), boost::none /* timestamp */), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 1), false)});
        return deleteOp;
    }());

    // Do multi-target write op
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss,
                            {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                             MockRange(endpointB, BSON("x" << 0), BSON("x" << 10)),
                             MockRange(endpointC, BSON("x" << 10), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 3u);
    sortByEndpoint(&targeted);
    ASSERT_EQUALS(targeted[0]->endpoint.shardName, endpointA.shardName);
    ASSERT(ChunkVersion::isIgnoredVersion(*targeted[0]->endpoint.shardVersion));
    ASSERT_EQUALS(targeted[1]->endpoint.shardName, endpointB.shardName);
    ASSERT(ChunkVersion::isIgnoredVersion(*targeted[1]->endpoint.shardVersion));
    ASSERT_EQUALS(targeted[2]->endpoint.shardName, endpointC.shardName);
    ASSERT(ChunkVersion::isIgnoredVersion(*targeted[2]->endpoint.shardVersion));

    writeOp.noteWriteComplete(*targeted[0]);
    writeOp.noteWriteComplete(*targeted[1]);
    writeOp.noteWriteComplete(*targeted[2]);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

TEST_F(WriteOpTest, TargetMultiAllShardsAndErrorSingleChildOp) {
    ShardEndpoint endpointA(
        ShardId("shardA"), ChunkVersion(10, 0, OID(), boost::none /* timestamp */), boost::none);
    ShardEndpoint endpointB(
        ShardId("shardB"), ChunkVersion(20, 0, OID(), boost::none /* timestamp */), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 1), false)});
        return deleteOp;
    }());

    // Do multi-target write op
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss,
                            {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                             MockRange(endpointB, BSON("x" << 0), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 2u);
    sortByEndpoint(&targeted);
    ASSERT_EQUALS(targeted[0]->endpoint.shardName, endpointA.shardName);
    ASSERT(ChunkVersion::isIgnoredVersion(*targeted[0]->endpoint.shardVersion));
    ASSERT_EQUALS(targeted[1]->endpoint.shardName, endpointB.shardName);
    ASSERT(ChunkVersion::isIgnoredVersion(*targeted[1]->endpoint.shardVersion));

    // Simulate retryable error.
    WriteErrorDetail retryableError;
    retryableError.setIndex(0);
    retryableError.setStatus({ErrorCodes::StaleShardVersion, "simulate ssv error for test"});
    writeOp.noteWriteError(*targeted[0], retryableError);

    // State should not change until we have result from all nodes.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);

    writeOp.noteWriteComplete(*targeted[1]);

    // State resets back to ready because of retryable error.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);
}

// Single error after targeting test
TEST_F(WriteOpTest, ErrorSingle) {
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED(), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    const auto error(buildError(ErrorCodes::UnknownError, BSON("data" << 12345), "some message"));

    writeOp.noteWriteError(*targeted.front(), error);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().toStatus().code(), error.toStatus().code());
    ASSERT_EQUALS(writeOp.getOpError().getErrInfo()["data"].Int(),
                  error.getErrInfo()["data"].Int());
    ASSERT_EQUALS(writeOp.getOpError().toStatus().reason(), error.toStatus().reason());
}

// Cancel single targeting test
TEST_F(WriteOpTest, CancelSingle) {
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED(), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    writeOp.cancelWrites(nullptr);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);
}

//
// Test retryable errors
//

// Retry single targeting test
TEST_F(WriteOpTest, RetrySingleOp) {
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED(), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op

    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    // Stale exception
    const auto error(
        buildError(ErrorCodes::StaleShardVersion, BSON("data" << 12345), "some message"));
    writeOp.noteWriteError(*targeted.front(), error);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);
}

class WriteOpTransactionTest : public WriteOpTest {
private:
    RouterOperationContextSession _routerOpCtxSession{[this] {
        _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
        return _opCtx;
    }()};
};

TEST_F(WriteOpTransactionTest, TargetMultiDoesNotTargetAllShards) {
    ShardEndpoint endpointA(
        ShardId("shardA"), ChunkVersion(10, 0, OID(), boost::none /* timestamp */), boost::none);
    ShardEndpoint endpointB(
        ShardId("shardB"), ChunkVersion(20, 0, OID(), boost::none /* timestamp */), boost::none);
    ShardEndpoint endpointC(
        ShardId("shardB"), ChunkVersion(20, 0, OID(), boost::none /* timestamp */), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 1), true /*multi*/)});
        return deleteOp;
    }());

    // Target the multi-write.
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss,
                            {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                             MockRange(endpointB, BSON("x" << 0), BSON("x" << 10)),
                             MockRange(endpointC, BSON("x" << 10), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);

    // The write should only target shardA and shardB and send real shard versions to each.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 2u);
    sortByEndpoint(&targeted);
    assertEndpointsEqual(targeted.front()->endpoint, endpointA);
    assertEndpointsEqual(targeted.back()->endpoint, endpointB);

    writeOp.noteWriteComplete(*targeted[0]);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);

    writeOp.noteWriteComplete(*targeted[1]);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

TEST_F(WriteOpTransactionTest, TargetMultiAllShardsAndErrorSingleChildOp) {
    ShardEndpoint endpointA(
        ShardId("shardA"), ChunkVersion(10, 0, OID(), boost::none /* timestamp */), boost::none);
    ShardEndpoint endpointB(
        ShardId("shardB"), ChunkVersion(20, 0, OID(), boost::none /* timestamp */), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 1), false)});
        return deleteOp;
    }());

    const TxnNumber kTxnNumber = 1;
    _opCtx->setTxnNumber(kTxnNumber);

    auto txnRouter = TransactionRouter::get(_opCtx);
    txnRouter.beginOrContinueTxn(_opCtx, kTxnNumber, TransactionRouter::TransactionActions::kStart);

    // Do multi-target write op
    WriteOp writeOp(BatchItemRef(&request, 0), true);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss,
                            {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                             MockRange(endpointB, BSON("x" << 0), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 2u);
    sortByEndpoint(&targeted);
    ASSERT_EQUALS(targeted[0]->endpoint.shardName, endpointA.shardName);
    ASSERT_EQUALS(targeted[1]->endpoint.shardName, endpointB.shardName);

    // Simulate retryable error.
    WriteErrorDetail retryableError;
    retryableError.setIndex(0);
    retryableError.setStatus({ErrorCodes::StaleShardVersion, "simulate ssv error for test"});
    writeOp.noteWriteError(*targeted[0], retryableError);

    // State should change to error right away even with retryable error when in a transaction.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
}

}  // namespace
}  // namespace mongo
