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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/json.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_group.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using unittest::assertGet;

std::ostream& operator<<(std::ostream& os, stdx::cv_status cvStatus) {
    switch (cvStatus) {
        case stdx::cv_status::timeout:
            return os << "timeout";
        case stdx::cv_status::no_timeout:
            return os << "no_timeout";
        default:
            MONGO_UNREACHABLE;
    }
}

std::ostream& operator<<(std::ostream& os, stdx::future_status futureStatus) {
    switch (futureStatus) {
        case stdx::future_status::ready:
            return os << "ready";
        case stdx::future_status::deferred:
            return os << "deferred";
        case stdx::future_status::timeout:
            return os << "timeout";
        default:
            MONGO_UNREACHABLE;
    }
}

TEST(OperationContextTest, NoSessionIdNoTransactionNumber) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    ASSERT(!opCtx->getLogicalSessionId());
    ASSERT(!opCtx->getTxnNumber());
}

TEST(OperationContextTest, SessionIdNoTransactionNumber) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    const auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);

    ASSERT(opCtx->getLogicalSessionId());
    ASSERT_EQUALS(lsid, *opCtx->getLogicalSessionId());

    ASSERT(!opCtx->getTxnNumber());
}

TEST(OperationContextTest, SessionIdAndTransactionNumber) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    const auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(5);

    ASSERT(opCtx->getTxnNumber());
    ASSERT_EQUALS(5, *opCtx->getTxnNumber());
}

DEATH_TEST(OperationContextTest, SettingTransactionNumberWithoutSessionIdShouldCrash, "invariant") {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    opCtx->setTxnNumber(5);
}

DEATH_TEST(OperationContextTest, CallingMarkKillWithExtraInfoCrashes, "invariant") {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    opCtx->markKilled(ErrorCodes::ForTestingErrorExtraInfo);
}

DEATH_TEST(OperationContextTest, CallingSetDeadlineWithExtraInfoCrashes, "invariant") {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(Date_t::now(), ErrorCodes::ForTestingErrorExtraInfo);
}

TEST(OperationContextTest, CallingMarkKillWithOptionalExtraInfoSucceeds) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    opCtx->markKilled(ErrorCodes::ForTestingOptionalErrorExtraInfo);
}

TEST(OperationContextTest, OpCtxGroup) {
    OperationContextGroup group1;
    ASSERT_TRUE(group1.isEmpty());
    {
        auto serviceCtx1 = ServiceContext::make();
        auto client1 = serviceCtx1->makeClient("OperationContextTest1");
        auto opCtx1 = group1.makeOperationContext(*client1);
        ASSERT_FALSE(group1.isEmpty());

        auto serviceCtx2 = ServiceContext::make();
        auto client2 = serviceCtx2->makeClient("OperationContextTest2");
        {
            auto opCtx2 = group1.makeOperationContext(*client2);
            opCtx1.discard();
            ASSERT_FALSE(group1.isEmpty());
        }
        ASSERT_TRUE(group1.isEmpty());

        auto opCtx3 = group1.makeOperationContext(*client1);
        auto opCtx4 = group1.makeOperationContext(*client2);
        ASSERT_TRUE(opCtx3->checkForInterruptNoAssert().isOK());    // use member op->
        ASSERT_TRUE((*opCtx4).checkForInterruptNoAssert().isOK());  // use conversion to OC*
        group1.interrupt(ErrorCodes::InternalError);
        ASSERT_FALSE(opCtx3->checkForInterruptNoAssert().isOK());
        ASSERT_FALSE((*opCtx4).checkForInterruptNoAssert().isOK());
    }
    ASSERT_TRUE(group1.isEmpty());

    OperationContextGroup group2;
    {
        auto serviceCtx = ServiceContext::make();
        auto client = serviceCtx->makeClient("OperationContextTest1");
        auto opCtx2 = group2.adopt(client->makeOperationContext());
        ASSERT_FALSE(group2.isEmpty());
        ASSERT_TRUE(opCtx2->checkForInterruptNoAssert().isOK());
        group2.interrupt(ErrorCodes::InternalError);
        ASSERT_FALSE(opCtx2->checkForInterruptNoAssert().isOK());
        opCtx2.discard();
        ASSERT(opCtx2.opCtx() == nullptr);
        ASSERT_TRUE(group2.isEmpty());
    }

    OperationContextGroup group3;
    OperationContextGroup group4;
    {
        auto serviceCtx = ServiceContext::make();
        auto client3 = serviceCtx->makeClient("OperationContextTest3");
        auto opCtx3 = group3.makeOperationContext(*client3);
        auto p3 = opCtx3.opCtx();
        auto opCtx4 = group4.take(std::move(opCtx3));
        ASSERT_EQ(p3, opCtx4.opCtx());
        ASSERT(opCtx3.opCtx() == nullptr);  // NOLINT(bugprone-use-after-move)
        ASSERT_TRUE(group3.isEmpty());
        ASSERT_FALSE(group4.isEmpty());
        group3.interrupt(ErrorCodes::InternalError);
        ASSERT_TRUE(opCtx4->checkForInterruptNoAssert().isOK());
        group4.interrupt(ErrorCodes::InternalError);
        ASSERT_FALSE(opCtx4->checkForInterruptNoAssert().isOK());
    }
}

TEST(OperationContextTest, IgnoreInterruptsWorks) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    opCtx->markKilled(ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::BadValue);
    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::BadValue);

    opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        ASSERT_OK(opCtx->checkForInterruptNoAssert());
        ASSERT_OK(opCtx->getKillStatus());
    });

    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::BadValue);

    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::BadValue);

    serviceCtx->setKillAllOperations();

    opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        ASSERT_THROWS_CODE(
            opCtx->checkForInterrupt(), DBException, ErrorCodes::InterruptedAtShutdown);
    });
}

TEST(OperationContextTest, setIsExecutingShutdownWorks) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    opCtx->markKilled(ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::BadValue);
    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::BadValue);

    opCtx->setIsExecutingShutdown();

    ASSERT_OK(opCtx->checkForInterruptNoAssert());
    ASSERT_OK(opCtx->getKillStatus());

    serviceCtx->setKillAllOperations();

    ASSERT_OK(opCtx->checkForInterruptNoAssert());
    ASSERT_OK(opCtx->getKillStatus());
}

TEST(OperationContextTest, CancellationTokenIsCanceledWhenMarkKilledIsCalled) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();
    auto cancelToken = opCtx->getCancellationToken();

    // Should not be canceled yet.
    ASSERT_FALSE(cancelToken.isCanceled());

    opCtx->markKilled();

    // Now should be canceled.
    ASSERT_TRUE(cancelToken.isCanceled());
}

TEST(OperationContextTest, CancellationTokenIsCancelableAtFirst) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();
    auto cancelToken = opCtx->getCancellationToken();
    ASSERT_TRUE(cancelToken.isCancelable());
}

class OperationDeadlineTests : public unittest::Test {
public:
    void setUp() {
        service = ServiceContext::make();
        service->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(mockClock));
        service->setPreciseClockSource(std::make_unique<SharedClockSourceAdapter>(mockClock));
        service->setTickSource(std::make_unique<TickSourceMock<>>());
        client = service->makeClient("OperationDeadlineTest");
    }

    void checkForInterruptForTimeout(OperationContext* opCtx) {
        auto m = MONGO_MAKE_LATCH();
        stdx::condition_variable cv;
        stdx::unique_lock<Latch> lk(m);
        opCtx->waitForConditionOrInterrupt(cv, lk, [] { return false; });
    }

    const std::shared_ptr<ClockSourceMock> mockClock = std::make_shared<ClockSourceMock>();
    ServiceContext::UniqueServiceContext service;
    ServiceContext::UniqueClient client;
};

TEST_F(OperationDeadlineTests, OperationDeadlineExpiration) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Seconds{1}, ErrorCodes::ExceededTimeLimit);
    mockClock->advance(Milliseconds{500});
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    // 1ms before relative deadline reports no interrupt
    mockClock->advance(Milliseconds{499});
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    // Exactly at deadline reports no interrupt, because setDeadlineAfterNowBy adds one clock
    // precision unit to the deadline, to ensure that the deadline does not expire in less than the
    // requested amount of time.
    mockClock->advance(Milliseconds{1});
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    // Since the mock clock's precision is 1ms, at test start + 1001 ms, we expect
    // checkForInterruptNoAssert to return ExceededTimeLimit.
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, opCtx->checkForInterruptNoAssert());

    // Also at times greater than start + 1001ms, we expect checkForInterruptNoAssert to keep
    // returning ExceededTimeLimit.
    mockClock->advance(Milliseconds{1});
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, opCtx->checkForInterruptNoAssert());
}

TEST_F(OperationDeadlineTests, CancellationTokenIsCanceledAfterDeadlineExpires) {
    auto opCtx = client->makeOperationContext();
    const Seconds timeout{1};
    opCtx->setDeadlineAfterNowBy(timeout, ErrorCodes::ExceededTimeLimit);

    auto cancelToken = opCtx->getCancellationToken();

    // Should not be canceled yet.
    ASSERT_FALSE(cancelToken.isCanceled());

    // Advance past the timeout.
    mockClock->advance(timeout * 2);

    // This is required for the OperationContext to realize that the timeout has passed and mark
    // itself killed, which is what triggers cancellation.
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, opCtx->checkForInterruptNoAssert());

    // Should be canceled now.
    ASSERT_TRUE(cancelToken.isCanceled());
}

TEST_F(OperationDeadlineTests,
       WaitingOnAFutureWithAnOperationContextThatHasCancellationCallbacksDoesNotDeadlock) {
    auto opCtx = client->makeOperationContext();
    const Seconds timeout{1};
    opCtx->setDeadlineAfterNowBy(timeout, ErrorCodes::ExceededTimeLimit);

    auto cancelToken = opCtx->getCancellationToken();

    // Should not be canceled yet.
    ASSERT_FALSE(cancelToken.isCanceled());

    // Advance past the timeout.
    mockClock->advance(timeout * 2);

    // Chain a callback to the token. This will mean that calling cancel() on the CancellationSource
    // will eventually have to acquire a mutex when fulfilling its SharedPromie.
    auto fut = cancelToken.onCancel().unsafeToInlineFuture().then([] {});

    // Make sure this does not deadlock. (Because in a previous implementation, it did.)
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, std::move(fut).waitNoThrow(opCtx.get()));

    // Should be canceled now.
    ASSERT_TRUE(cancelToken.isCanceled());
}

template <typename D>
void assertLargeRelativeDeadlineLikeInfinity(Client& client, D maxTime) {
    auto opCtx = client.makeOperationContext();
    opCtx->setDeadlineAfterNowBy(maxTime, ErrorCodes::ExceededTimeLimit);
    ASSERT_FALSE(opCtx->hasDeadline()) << "Tried to set maxTime to " << maxTime;
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesHours) {
    ASSERT_FALSE(client->makeOperationContext()->hasDeadline());
    assertLargeRelativeDeadlineLikeInfinity(*client, Hours::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMinutes) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Minutes::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesSeconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Seconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMilliseconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Milliseconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesMicroseconds) {
    assertLargeRelativeDeadlineLikeInfinity(*client, Microseconds::max());
}

TEST_F(OperationDeadlineTests, VeryLargeRelativeDeadlinesNanoseconds) {
    // Nanoseconds::max() is less than Microseconds::max(), so it is possible to set
    // a deadline of that duration.
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Nanoseconds::max(), ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->hasDeadline());
    ASSERT_EQ(mockClock->now() + mockClock->getPrecision() +
                  duration_cast<Milliseconds>(Nanoseconds::max()),
              opCtx->getDeadline());
}

TEST_F(OperationDeadlineTests, WaitForMaxTimeExpiredCV) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now(), ErrorCodes::ExceededTimeLimit);
    auto m = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(m);
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    ASSERT_THROWS_CODE(opCtx->waitForConditionOrInterrupt(cv, lk, [] { return false; }),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(OperationDeadlineTests, WaitForMaxTimeExpiredCVWithWaitUntilSet) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now(), ErrorCodes::ExceededTimeLimit);
    auto m = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(m);
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    ASSERT_THROWS_CODE(opCtx->waitForConditionOrInterruptUntil(
                           cv, lk, mockClock->now() + Seconds{10}, [] { return false; }),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(OperationDeadlineTests, NestedTimeoutsTimeoutInOrder) {
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(mockClock->now() + Milliseconds(500), ErrorCodes::MaxTimeMSExpired);

    bool reachedA = false;
    bool reachedB = false;
    bool reachedC = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());

                try {
                    opCtx->runWithDeadline(
                        mockClock->now() + Milliseconds(50), ErrorCodes::ExceededTimeLimit, [&] {
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            try {
                                opCtx->runWithDeadline(mockClock->now() + Milliseconds(10),
                                                       ErrorCodes::ExceededTimeLimit,
                                                       [&] {
                                                           ASSERT_OK(
                                                               opCtx->checkForInterruptNoAssert());
                                                           ASSERT_OK(opCtx->getKillStatus());
                                                           mockClock->advance(Milliseconds(20));
                                                           checkForInterruptForTimeout(opCtx.get());
                                                           ASSERT(false);
                                                       });
                            } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                                opCtx->checkForInterrupt();
                                ASSERT_OK(opCtx->getKillStatus());
                                mockClock->advance(Milliseconds(50));
                                reachedA = true;
                            }

                            opCtx->checkForInterrupt();
                        });
                } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                    opCtx->checkForInterrupt();
                    ASSERT_OK(opCtx->getKillStatus());
                    mockClock->advance(Milliseconds(50));
                    reachedB = true;
                }

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        reachedC = true;
        ASSERT_OK(opCtx->getKillStatus());
        ASSERT_OK(opCtx->checkForInterruptNoAssert());
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT(reachedC);

    ASSERT_OK(opCtx->getKillStatus());

    mockClock->advance(Seconds(1));

    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::MaxTimeMSExpired);
}

TEST_F(OperationDeadlineTests, NestedTimeoutsThatViolateMaxTime) {
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(mockClock->now() + Milliseconds(10), ErrorCodes::MaxTimeMSExpired);

    bool reachedA = false;
    bool reachedB = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());
                try {
                    opCtx->runWithDeadline(
                        mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            ASSERT_OK(opCtx->getKillStatus());
                            mockClock->advance(Milliseconds(50));
                            opCtx->checkForInterrupt();
                        });
                } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                    reachedA = true;
                }

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::MaxTimeMSExpired>&) {
        reachedB = true;
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
}

TEST_F(OperationDeadlineTests, NestedNonMaxTimeMSTimeoutsThatAreLargerAreIgnored) {
    auto opCtx = client->makeOperationContext();

    bool reachedA = false;
    bool reachedB = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(10), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());
                try {
                    opCtx->runWithDeadline(
                        mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            ASSERT_OK(opCtx->getKillStatus());
                            mockClock->advance(Milliseconds(50));
                            opCtx->checkForInterrupt();
                        });
                } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                    reachedA = true;
                }

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        reachedB = true;
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
}

TEST_F(OperationDeadlineTests, DeadlineAfterIgnoreInterruptsReopens) {
    auto opCtx = client->makeOperationContext();

    bool reachedA = false;
    bool reachedB = false;
    bool reachedC = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(500), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());

                opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
                    try {
                        opCtx->runWithDeadline(
                            mockClock->now() + Seconds(1), ErrorCodes::ExceededTimeLimit, [&] {
                                ASSERT_OK(opCtx->checkForInterruptNoAssert());
                                ASSERT_OK(opCtx->getKillStatus());
                                mockClock->advance(Milliseconds(750));
                                ASSERT_OK(opCtx->checkForInterruptNoAssert());
                                mockClock->advance(Milliseconds(500));
                                reachedA = true;
                                opCtx->checkForInterrupt();
                            });
                    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                        opCtx->checkForInterrupt();
                        reachedB = true;
                    }
                });

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        reachedC = true;
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT(reachedC);
}

TEST_F(OperationDeadlineTests, DeadlineAfterSetIsExecutingShutdownReopens) {
    auto opCtx = client->makeOperationContext();

    bool reachedA = false;
    bool reachedB = false;
    bool reachedC = false;

    try {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(500), ErrorCodes::ExceededTimeLimit, [&] {
                ASSERT_OK(opCtx->checkForInterruptNoAssert());

                opCtx->setIsExecutingShutdown();
                try {
                    opCtx->runWithDeadline(
                        mockClock->now() + Seconds(1), ErrorCodes::ExceededTimeLimit, [&] {
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            ASSERT_OK(opCtx->getKillStatus());
                            mockClock->advance(Milliseconds(750));
                            ASSERT_OK(opCtx->checkForInterruptNoAssert());
                            mockClock->advance(Milliseconds(500));
                            reachedA = true;
                            opCtx->checkForInterrupt();
                        });
                } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
                    opCtx->checkForInterrupt();
                    reachedB = true;
                }

                opCtx->checkForInterrupt();
            });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        reachedC = true;
    }

    ASSERT(reachedA);
    ASSERT(reachedB);
    ASSERT_FALSE(reachedC);
}

TEST_F(OperationDeadlineTests, DeadlineAfterRunWithoutInterruptSeesViolatedMaxMS) {
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(mockClock->now() + Milliseconds(100), ErrorCodes::MaxTimeMSExpired);

    ASSERT_THROWS_CODE(opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(200), ErrorCodes::ExceededTimeLimit, [&] {
                mockClock->advance(Milliseconds(300));
                opCtx->checkForInterrupt();
            });
    }),
                       DBException,
                       ErrorCodes::MaxTimeMSExpired);
}

TEST_F(OperationDeadlineTests, DeadlineAfterRunWithoutInterruptDoesntSeeUnviolatedMaxMS) {
    auto opCtx = client->makeOperationContext();

    opCtx->setDeadlineByDate(mockClock->now() + Milliseconds(200), ErrorCodes::MaxTimeMSExpired);

    ASSERT_THROWS_CODE(opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
        opCtx->runWithDeadline(
            mockClock->now() + Milliseconds(100), ErrorCodes::ExceededTimeLimit, [&] {
                mockClock->advance(Milliseconds(150));
                opCtx->checkForInterrupt();
            });
    }),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);
}

TEST_F(OperationDeadlineTests, WaitForKilledOpCV) {
    auto opCtx = client->makeOperationContext();
    opCtx->markKilled();
    auto m = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(m);
    ASSERT_THROWS_CODE(opCtx->waitForConditionOrInterrupt(cv, lk, [] { return false; }),
                       DBException,
                       ErrorCodes::Interrupted);
}

TEST_F(OperationDeadlineTests, WaitForUntilExpiredCV) {
    auto opCtx = client->makeOperationContext();
    auto m = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(m);
    ASSERT_FALSE(
        opCtx->waitForConditionOrInterruptUntil(cv, lk, mockClock->now(), [] { return false; }));
}

TEST_F(OperationDeadlineTests, WaitForUntilExpiredCVWithMaxTimeSet) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now() + Seconds{10}, ErrorCodes::ExceededTimeLimit);
    auto m = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(m);
    ASSERT_FALSE(
        opCtx->waitForConditionOrInterruptUntil(cv, lk, mockClock->now(), [] { return false; }));
}

TEST_F(OperationDeadlineTests, WaitForDurationExpired) {
    auto opCtx = client->makeOperationContext();
    auto m = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(m);
    ASSERT_FALSE(opCtx->waitForConditionOrInterruptFor(
        cv, lk, Milliseconds(-1000), []() -> bool { return false; }));
}

TEST_F(OperationDeadlineTests, DuringWaitMaxTimeExpirationDominatesUntilExpiration) {
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineByDate(mockClock->now(), ErrorCodes::ExceededTimeLimit);
    auto m = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(m);
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    ASSERT_THROWS_CODE(
        opCtx->waitForConditionOrInterruptUntil(cv, lk, mockClock->now(), [] { return false; }),
        DBException,
        ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

class ThreadedOperationDeadlineTests : public OperationDeadlineTests {
public:
    using CvPred = std::function<bool()>;
    using WaitFn = std::function<bool(
        OperationContext*, stdx::condition_variable&, stdx::unique_lock<Latch>&, CvPred)>;

    struct WaitTestState {
        void signal() {
            stdx::lock_guard<Latch> lk(mutex);
            invariant(!isSignaled);
            isSignaled = true;
            cv.notify_all();
        }

        Mutex mutex = MONGO_MAKE_LATCH("WaitTestState::mutex");
        stdx::condition_variable cv;
        bool isSignaled = false;
    };

    stdx::future<bool> startWaiterWithMaxTime(OperationContext* opCtx,
                                              WaitTestState* state,
                                              WaitFn waitFn,
                                              Date_t maxTime) {

        auto barrier = std::make_shared<unittest::Barrier>(2);
        auto task = stdx::packaged_task<bool()>([=] {
            if (maxTime < Date_t::max()) {
                opCtx->setDeadlineByDate(maxTime, ErrorCodes::ExceededTimeLimit);
            }
            auto predicate = [state] { return state->isSignaled; };
            stdx::unique_lock<Latch> lk(state->mutex);
            barrier->countDownAndWait();
            return waitFn(opCtx, state->cv, lk, predicate);
        });
        auto result = task.get_future();
        stdx::thread(std::move(task)).detach();
        barrier->countDownAndWait();

        // Now we know that the waiter task must own the mutex, because it does not signal the
        // barrier until it does.
        stdx::lock_guard<Latch> lk(state->mutex);

        // Assuming that opCtx has not already been interrupted and that maxTime and until are
        // unexpired, we know that the waiter must be blocked in the condition variable, because it
        // held the mutex before we tried to acquire it, and only releases it on condition variable
        // wait.
        return result;
    }

    stdx::future<bool> startWaiterWithUntilAndMaxTime(OperationContext* opCtx,
                                                      WaitTestState* state,
                                                      Date_t until,
                                                      Date_t maxTime) {
        const auto waitFn = [until](OperationContext* opCtx,
                                    stdx::condition_variable& cv,
                                    stdx::unique_lock<Latch>& lk,
                                    CvPred predicate) {
            if (until < Date_t::max()) {
                return opCtx->waitForConditionOrInterruptUntil(cv, lk, until, predicate);
            } else {
                opCtx->waitForConditionOrInterrupt(cv, lk, predicate);
                return true;
            }
        };
        return startWaiterWithMaxTime(opCtx, state, waitFn, maxTime);
    }

    template <typename Period>
    stdx::future<bool> startWaiterWithDurationAndMaxTime(OperationContext* opCtx,
                                                         WaitTestState* state,
                                                         Duration<Period> duration,
                                                         Date_t maxTime) {
        const auto waitFn = [duration](OperationContext* opCtx,
                                       stdx::condition_variable& cv,
                                       stdx::unique_lock<Latch>& lk,
                                       CvPred predicate) {
            return opCtx->waitForConditionOrInterruptFor(cv, lk, duration, predicate);
        };
        return startWaiterWithMaxTime(opCtx, state, waitFn, maxTime);
    }

    stdx::future<bool> startWaiter(OperationContext* opCtx, WaitTestState* state) {
        return startWaiterWithUntilAndMaxTime(opCtx, state, Date_t::max(), Date_t::max());
    }

    stdx::future<bool> startWaiterWithSleepUntilAndMaxTime(OperationContext* opCtx,
                                                           WaitTestState* state,
                                                           Date_t sleepUntil,
                                                           Date_t maxTime) {
        auto waitFn = [sleepUntil](OperationContext* opCtx,
                                   stdx::condition_variable& cv,
                                   stdx::unique_lock<Latch>& lk,
                                   CvPred predicate) {
            lk.unlock();
            opCtx->sleepUntil(sleepUntil);
            lk.lock();
            return false;
        };
        return startWaiterWithMaxTime(opCtx, state, waitFn, maxTime);
    }

    template <typename Period>
    stdx::future<bool> startWaiterWithSleepForAndMaxTime(OperationContext* opCtx,
                                                         WaitTestState* state,
                                                         Duration<Period> sleepFor,
                                                         Date_t maxTime) {
        auto waitFn = [sleepFor](OperationContext* opCtx,
                                 stdx::condition_variable& cv,
                                 stdx::unique_lock<Latch>& lk,
                                 CvPred predicate) {
            lk.unlock();
            opCtx->sleepFor(sleepFor);
            lk.lock();
            return false;
        };
        return startWaiterWithMaxTime(opCtx, state, waitFn, maxTime);
    }
};

TEST_F(ThreadedOperationDeadlineTests, KillArrivesWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto waiterResult = startWaiter(opCtx.get(), &state);
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        opCtx->markKilled();
    }
    ASSERT_THROWS_CODE(waiterResult.get(), DBException, ErrorCodes::Interrupted);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(ThreadedOperationDeadlineTests, MaxTimeExpiresWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(opCtx.get(),
                                                       &state,
                                                       startDate + Seconds{60},   // until
                                                       startDate + Seconds{10});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());
    mockClock->advance(Seconds{2});
    ASSERT_THROWS_CODE(waiterResult.get(), DBException, ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST_F(ThreadedOperationDeadlineTests, UntilExpiresWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(opCtx.get(),
                                                       &state,
                                                       startDate + Seconds{10},   // until
                                                       startDate + Seconds{60});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{2});
    ASSERT_FALSE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, ForExpiresWhileWaiting) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithDurationAndMaxTime(
        opCtx.get(), &state, Seconds{10}, startDate + Seconds{60});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{2});
    ASSERT_FALSE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalOne) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    auto waiterResult = startWaiter(opCtx.get(), &state);

    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    state.signal();
    ASSERT_TRUE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, KillOneSignalAnother) {
    auto client1 = service->makeClient("client1");
    auto client2 = service->makeClient("client2");
    auto txn1 = client1->makeOperationContext();
    auto txn2 = client2->makeOperationContext();
    WaitTestState state1;
    WaitTestState state2;
    auto waiterResult1 = startWaiter(txn1.get(), &state1);
    auto waiterResult2 = startWaiter(txn2.get(), &state2);
    ASSERT(stdx::future_status::ready !=
           waiterResult1.wait_for(Milliseconds::zero().toSystemDuration()));
    ASSERT(stdx::future_status::ready !=
           waiterResult2.wait_for(Milliseconds::zero().toSystemDuration()));
    {
        stdx::lock_guard<Client> clientLock(*txn1->getClient());
        txn1->markKilled();
    }
    ASSERT_THROWS_CODE(waiterResult1.get(), DBException, ErrorCodes::Interrupted);
    ASSERT(stdx::future_status::ready !=
           waiterResult2.wait_for(Milliseconds::zero().toSystemDuration()));
    state2.signal();
    ASSERT_TRUE(waiterResult2.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalBeforeUntilExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(opCtx.get(),
                                                       &state,
                                                       startDate + Seconds{10},   // until
                                                       startDate + Seconds{60});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    state.signal();
    ASSERT_TRUE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SignalBeforeMaxTimeExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithUntilAndMaxTime(opCtx.get(),
                                                       &state,
                                                       startDate + Seconds{60},   // until
                                                       startDate + Seconds{10});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()))
        << waiterResult.get();
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    state.signal();
    ASSERT_TRUE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepUntilWithExpiredUntilDoesNotBlock) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithSleepUntilAndMaxTime(opCtx.get(),
                                                            &state,
                                                            startDate - Seconds{10},   // until
                                                            startDate + Seconds{60});  // maxTime
    ASSERT_FALSE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepUntilExpires) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithSleepUntilAndMaxTime(opCtx.get(),
                                                            &state,
                                                            startDate + Seconds{10},   // until
                                                            startDate + Seconds{60});  // maxTime
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{9});
    ASSERT(stdx::future_status::ready !=
           waiterResult.wait_for(Milliseconds::zero().toSystemDuration()));
    mockClock->advance(Seconds{2});
    ASSERT_FALSE(waiterResult.get());
}

TEST_F(ThreadedOperationDeadlineTests, SleepForWithExpiredForDoesNotBlock) {
    auto opCtx = client->makeOperationContext();
    WaitTestState state;
    const auto startDate = mockClock->now();
    auto waiterResult = startWaiterWithSleepForAndMaxTime(
        opCtx.get(), &state, Seconds{-10}, startDate + Seconds{60});  // maxTime
    ASSERT_FALSE(waiterResult.get());
}

TEST(OperationContextTest, TestWaitForConditionOrInterruptUntilAPI) {
    // `waitForConditionOrInterruptUntil` can have three outcomes:
    //
    // 1) The condition is satisfied before any timeouts.
    // 2) The explicit `deadline` function argument is triggered.
    // 3) The operation context implicitly times out, or is interrupted from a killOp command or
    //    shutdown, etc.
    //
    // Case (1) must return true.
    // Case (2) must return false.
    // Case (3) must throw a DBException.
    //
    // Case (1) is the hardest to test. The condition variable must be notified by a second thread
    // when the client is waiting on it. Case (1) is also the least in need of having the API
    // tested, thus it's omitted from being tested here.
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto opCtx = client->makeOperationContext();

    auto mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable cv;
    stdx::unique_lock<Latch> lk(mutex);

    // Case (2). Expect a Status::OK with a cv_status::timeout.
    Date_t deadline = Date_t::now() + Milliseconds(500);
    ASSERT_EQ(opCtx->waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }),
              false);
    ASSERT_FALSE(opCtx->getCancellationToken().isCanceled());

    // Case (3). Expect an error of `MaxTimeMSExpired`.
    opCtx->setDeadlineByDate(Date_t::now(), ErrorCodes::MaxTimeMSExpired);
    deadline = Date_t::now() + Seconds(500);
    ASSERT_THROWS_CODE(
        opCtx->waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }),
        DBException,
        ErrorCodes::MaxTimeMSExpired);
    ASSERT_TRUE(opCtx->getCancellationToken().isCanceled());
}

TEST(OperationContextTest, TestIsWaitingForConditionOrInterrupt) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    auto optCtx = client->makeOperationContext();

    // Case (1) must return false (immediately after initialization)
    ASSERT_FALSE(optCtx->isWaitingForConditionOrInterrupt());

    // Case (2) must return true while waiting for the condition

    unittest::Barrier barrier(2);

    stdx::thread worker([&] {
        auto mutex = MONGO_MAKE_LATCH();
        stdx::condition_variable cv;
        stdx::unique_lock<Latch> lk(mutex);
        Date_t deadline = Date_t::now() + Milliseconds(300);
        optCtx->waitForConditionOrInterruptUntil(cv, lk, deadline, [&, i = 0]() mutable {
            if (i++ == 0) {
                barrier.countDownAndWait();
            }
            return false;
        });
    });

    barrier.countDownAndWait();
    ASSERT_TRUE(optCtx->isWaitingForConditionOrInterrupt());

    worker.join();
    ASSERT_FALSE(optCtx->isWaitingForConditionOrInterrupt());
}

TEST(OperationContextTest, TestActiveClientOperationsForClientsWithoutSession) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest");
    ASSERT_EQ(serviceCtx->getActiveClientOperations(), 0);
    {
        auto opCtx = client->makeOperationContext();
        ASSERT_EQ(serviceCtx->getActiveClientOperations(), 0);
    }
    ASSERT_EQ(serviceCtx->getActiveClientOperations(), 0);
}

TEST(OperationContextTest, TestActiveClientOperations) {
    transport::TransportLayerMock transportLayer;
    transport::SessionHandle session = transportLayer.createSession();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("OperationContextTest", session);
    ASSERT_EQ(serviceCtx->getActiveClientOperations(), 0);

    {
        auto optCtx = client->makeOperationContext();
        ASSERT_EQ(serviceCtx->getActiveClientOperations(), 1);
    }
    ASSERT_EQ(serviceCtx->getActiveClientOperations(), 0);

    {
        auto optCtx = client->makeOperationContext();
        ASSERT_EQ(serviceCtx->getActiveClientOperations(), 1);
        serviceCtx->killAndDelistOperation(optCtx.get());
        ASSERT_EQ(serviceCtx->getActiveClientOperations(), 0);
    }
    ASSERT_EQ(serviceCtx->getActiveClientOperations(), 0);
}

TEST(OperationContextTest, CurrentOpExcludesKilledOperations) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("MainClient");
    auto opCtx = client->makeOperationContext();

    for (auto truncateOps : {true, false}) {
        for (auto backtraceMode : {true, false}) {
            BSONObjBuilder bobNoOpCtx, bobKilledOpCtx;
            // We use a separate client thread to generate CurrentOp reports in presence and absence
            // of an `opCtx`. This is because `CurOp::reportCurrentOpForClient()` accepts an `opCtx`
            // as input and requires it to be present throughout its execution.
            stdx::thread thread([&]() mutable {
                auto threadClient = serviceCtx->makeClient("ThreadClient");

                // Generate report in absence of any opCtx
                CurOp::reportCurrentOpForClient(
                    opCtx.get(), threadClient.get(), truncateOps, backtraceMode, &bobNoOpCtx);

                auto threadOpCtx = threadClient->makeOperationContext();
                serviceCtx->killAndDelistOperation(threadOpCtx.get());

                // Generate report in presence of a killed opCtx
                CurOp::reportCurrentOpForClient(
                    opCtx.get(), threadClient.get(), truncateOps, backtraceMode, &bobKilledOpCtx);
            });

            thread.join();
            auto objNoOpCtx = bobNoOpCtx.obj();
            auto objKilledOpCtx = bobKilledOpCtx.obj();

            LOGV2_DEBUG(4780201, 1, "With no opCtx", "object"_attr = objNoOpCtx);
            LOGV2_DEBUG(4780202, 1, "With killed opCtx", "object"_attr = objKilledOpCtx);

            ASSERT_EQ(objNoOpCtx.nFields(), objKilledOpCtx.nFields());

            auto compareBSONObjs = [](BSONObj& a, BSONObj& b) -> bool {
                return (a == b).type == BSONObj::DeferredComparison::Type::kEQ;
            };
            ASSERT(compareBSONObjs(objNoOpCtx, objKilledOpCtx));
        }
    }
}

}  // namespace

}  // namespace mongo
