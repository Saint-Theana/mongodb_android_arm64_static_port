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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/transport/service_executor_synchronous.h"

#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_executor_gen.h"
#include "mongo/transport/service_executor_utils.h"
#include "mongo/util/thread_safety_context.h"

namespace mongo {
namespace transport {
namespace {
constexpr auto kExecutorName = "passthrough"_sd;

constexpr auto kThreadsRunning = "threadsRunning"_sd;
constexpr auto kClientsInTotal = "clientsInTotal"_sd;
constexpr auto kClientsRunning = "clientsRunning"_sd;
constexpr auto kClientsWaiting = "clientsWaitingForData"_sd;

const auto getServiceExecutorSynchronous =
    ServiceContext::declareDecoration<std::unique_ptr<ServiceExecutorSynchronous>>();

const auto serviceExecutorSynchronousRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ServiceExecutorSynchronous", [](ServiceContext* ctx) {
        getServiceExecutorSynchronous(ctx) = std::make_unique<ServiceExecutorSynchronous>(ctx);
    }};
}  // namespace

thread_local std::deque<ServiceExecutor::Task> ServiceExecutorSynchronous::_localWorkQueue = {};
thread_local int ServiceExecutorSynchronous::_localRecursionDepth = 0;
thread_local int64_t ServiceExecutorSynchronous::_localThreadIdleCounter = 0;

ServiceExecutorSynchronous::ServiceExecutorSynchronous(ServiceContext* ctx)
    : _shutdownCondition(std::make_shared<stdx::condition_variable>()) {}

Status ServiceExecutorSynchronous::start() {
    _stillRunning.store(true);

    return Status::OK();
}

Status ServiceExecutorSynchronous::shutdown(Milliseconds timeout) {
    LOGV2_DEBUG(22982, 3, "Shutting down passthrough executor");

    _stillRunning.store(false);

    stdx::unique_lock<Latch> lock(_shutdownMutex);
    bool result = _shutdownCondition->wait_for(lock, timeout.toSystemDuration(), [this]() {
        return _numRunningWorkerThreads.load() == 0;
    });

    return result
        ? Status::OK()
        : Status(ErrorCodes::Error::ExceededTimeLimit,
                 "passthrough executor couldn't shutdown all worker threads within time limit.");
}

ServiceExecutorSynchronous* ServiceExecutorSynchronous::get(ServiceContext* ctx) {
    auto& ref = getServiceExecutorSynchronous(ctx);
    invariant(ref);
    return ref.get();
}

Status ServiceExecutorSynchronous::scheduleTask(Task task, ScheduleFlags flags) {
    if (!_stillRunning.load()) {
        return Status{ErrorCodes::ShutdownInProgress, "Executor is not running"};
    }

    if (!_localWorkQueue.empty()) {
        if (flags & ScheduleFlags::kMayYieldBeforeSchedule) {
            yieldIfAppropriate();
        }

        // Execute task directly (recurse) if allowed by the caller as it produced better
        // performance in testing. Try to limit the amount of recursion so we don't blow up the
        // stack, even though this shouldn't happen with this executor that uses blocking network
        // I/O.
        if ((flags & ScheduleFlags::kMayRecurse) &&
            (_localRecursionDepth < synchronousServiceExecutorRecursionLimit.loadRelaxed())) {
            ++_localRecursionDepth;
            task();
        } else {
            _localWorkQueue.emplace_back(std::move(task));
        }
        return Status::OK();
    }

    // First call to scheduleTask() for this connection, spawn a worker thread that will push jobs
    // into the thread local job queue.
    LOGV2_DEBUG(22983, 3, "Starting new executor thread in passthrough mode");

    Status status = launchServiceWorkerThread(
        [this, condVarAnchor = _shutdownCondition, task = std::move(task)]() mutable {
            _numRunningWorkerThreads.addAndFetch(1);

            _localWorkQueue.emplace_back(std::move(task));
            while (!_localWorkQueue.empty() && _stillRunning.loadRelaxed()) {
                _localRecursionDepth = 1;
                _localWorkQueue.front()();
                _localWorkQueue.pop_front();
            }

            // We maintain an anchor to "_shutdownCondition" to ensure it remains alive even if the
            // service executor is freed. Any access to the service executor (through "this") is
            // prohibited (and unsafe) after the following line. For more context, see SERVER-49432.
            auto numWorkerThreadsStillRunning = _numRunningWorkerThreads.subtractAndFetch(1);
            if (numWorkerThreadsStillRunning == 0) {
                condVarAnchor->notify_all();
            }
        });

    return status;
}

void ServiceExecutorSynchronous::appendStats(BSONObjBuilder* bob) const {
    // The ServiceExecutorSynchronous has one client per thread and waits synchronously on thread.
    auto threads = static_cast<int>(_numRunningWorkerThreads.loadRelaxed());

    BSONObjBuilder subbob = bob->subobjStart(kExecutorName);
    subbob.append(kThreadsRunning, threads);
    subbob.append(kClientsInTotal, threads);
    subbob.append(kClientsRunning, threads);
    subbob.append(kClientsWaiting, 0);
}

void ServiceExecutorSynchronous::runOnDataAvailable(const SessionHandle& session,
                                                    OutOfLineExecutor::Task callback) {
    invariant(session);
    yieldIfAppropriate();

    schedule([callback = std::move(callback)](Status status) { callback(std::move(status)); });
}


}  // namespace transport
}  // namespace mongo
