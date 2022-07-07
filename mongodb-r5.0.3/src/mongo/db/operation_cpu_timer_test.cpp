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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/db/operation_cpu_timer.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationCPUTimerTest : public ServiceContextTest {
public:
    auto makeClient() const {
        return getGlobalServiceContext()->makeClient("AlternativeClient");
    }

    auto getTimer() const {
        return OperationCPUTimer::get(_opCtx.get());
    }

    void setUp() {
        _opCtx = getGlobalServiceContext()->makeOperationContext(Client::getCurrent());
    }

    void busyWait(Nanoseconds delay) const {
        AtomicWord<bool> mayJoin{false};
        stdx::thread blocker([&] {
            sleepFor(delay);
            mayJoin.store(true);
        });
        while (!mayJoin.load()) {
            // Busy wait for the blocker thread.
        }
        blocker.join();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

#if defined(__linux__)

TEST_F(OperationCPUTimerTest, TestTimer) {
    auto timer = getTimer();

    timer->start();
    busyWait(Nanoseconds(100));  // A small delay to make sure the timer advances.
    ASSERT_GT(timer->getElapsed(), Nanoseconds(0));
    timer->stop();

    const auto elapsedAfterStop = timer->getElapsed();
    busyWait(Microseconds(1));  // A relatively longer delay to ensure the timer doesn't proceed.
    const auto elapsedAfterSleep = timer->getElapsed();
    ASSERT_EQ(elapsedAfterStop, elapsedAfterSleep);
}

TEST_F(OperationCPUTimerTest, TestReset) {
    auto timer = getTimer();

    timer->start();
    busyWait(Microseconds(1));  // Introducing some delay for the timer to measure.
    timer->stop();
    auto elapsedAfterStop = timer->getElapsed();
    ASSERT_GTE(elapsedAfterStop, Microseconds(1));

    timer->start();
    auto elapsedAfterReset = timer->getElapsed();
    ASSERT_LT(elapsedAfterReset, elapsedAfterStop);
}

TEST_F(OperationCPUTimerTest, TestTimerDetachAndAttachHandlers) {
    unittest::Barrier failPointsReady{2};
    stdx::thread observer([&] {
        FailPointEnableBlock fpAttach("hangCPUTimerAfterOnThreadAttach");
        {
            FailPointEnableBlock fpDetach("hangCPUTimerAfterOnThreadDetach");
            failPointsReady.countDownAndWait();
            fpDetach->waitForTimesEntered(1);
        }
        fpAttach->waitForTimesEntered(1);
    });

    auto timer = getTimer();
    timer->start();

    failPointsReady.countDownAndWait();
    {
        auto client = makeClient();
        AlternativeClientRegion acr(client);
    }

    timer->stop();
    observer.join();
}

DEATH_TEST_F(OperationCPUTimerTest,
             AccessTimerForDetachedOperation,
             "Operation not attached to the current thread") {
    auto client = Client::releaseCurrent();
    getTimer();
}

DEATH_TEST_F(OperationCPUTimerTest, StopTimerBeforeStart, "Timer is not running") {
    getTimer()->stop();
}

DEATH_TEST_F(OperationCPUTimerTest, StartTimerMultipleTimes, "Timer has already started") {
    getTimer()->start();
    getTimer()->start();
}

DEATH_TEST_F(OperationCPUTimerTest, OnAttachForAttachedTimer, "Timer has already been attached") {
    auto timer = getTimer();
    timer->start();
    timer->onThreadAttach();
}

DEATH_TEST_F(OperationCPUTimerTest, OnDetachForDetachedTimer, "Timer is not attached") {
    auto timer = getTimer();
    timer->start();
    auto client = Client::releaseCurrent();
    timer->onThreadDetach();
}

DEATH_TEST_F(OperationCPUTimerTest, GetElapsedForPausedTimer, "Not attached to current thread") {
    auto timer = getTimer();
    timer->start();
    auto client = Client::releaseCurrent();
    timer->getElapsed();
}

TEST_F(OperationCPUTimerTest, TimerPausesOnBlockingSleep) {
    // This test checks if the time measured by `OperationCPUTimer` does not include the period of
    // time the operation was blocked (e.g., waiting on a condition variable). The idea is to have
    // the operation block for `kSomeDelay`, ensure the elapsed time observed by the timer is always
    // less than `kSomeDelay`, and repeat the test `kRepeats` times. To account for the sporadic
    // wake ups, the test does not fail unless the number of failures exceeds `kMaxFailures`. This
    // is just a best-effort, and the number of failures is not guaranteed to not exceed the upper
    // bound (i.e., `kMaxFailures`).
    const auto kSomeDelay = Milliseconds(1);
    const auto kRepeats = 1000;
    const auto kMaxFailureRate = 0.1;
    const auto kMaxFailures = kMaxFailureRate * kRepeats;

    auto timer = getTimer();

    auto checkTimer = [&] {
        auto elapsed = timer->getElapsed();
        if (elapsed < kSomeDelay)
            return true;
        LOGV2_WARNING(5160101,
                      "Elapsed operation time exceeded the upper bound",
                      "elapsed"_attr = elapsed,
                      "delay"_attr = kSomeDelay);
        return false;
    };

    auto failures = 0;
    for (auto i = 0; i < kRepeats; i++) {
        timer->start();
        sleepFor(kSomeDelay);
        if (!checkTimer())
            failures++;
        timer->stop();

        stdx::condition_variable cv;
        auto mutex = MONGO_MAKE_LATCH("TimerPausesOnBlockingSleep");
        timer->start();
        stdx::unique_lock lk(mutex);
        cv.wait_for(lk, kSomeDelay.toSystemDuration(), [] { return false; });
        if (!checkTimer())
            failures++;
        timer->stop();
    }

    ASSERT_LTE(failures, kMaxFailures);
}

#else

TEST_F(OperationCPUTimerTest, TimerNotSetIfNotSupported) {
    auto timer = getTimer();
    ASSERT(timer == nullptr);
}

#endif  // defined(__linux__)

}  // namespace mongo
