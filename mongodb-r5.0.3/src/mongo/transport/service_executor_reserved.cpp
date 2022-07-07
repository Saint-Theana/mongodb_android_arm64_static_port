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

#include "mongo/transport/service_executor_reserved.h"

#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_executor_gen.h"
#include "mongo/transport/service_executor_utils.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/thread_safety_context.h"

namespace mongo {
namespace transport {
namespace {

constexpr auto kExecutorName = "reserved"_sd;

constexpr auto kThreadsRunning = "threadsRunning"_sd;
constexpr auto kClientsInTotal = "clientsInTotal"_sd;
constexpr auto kClientsRunning = "clientsRunning"_sd;
constexpr auto kClientsWaiting = "clientsWaitingForData"_sd;

const auto getServiceExecutorReserved =
    ServiceContext::declareDecoration<std::unique_ptr<ServiceExecutorReserved>>();

const auto serviceExecutorReservedRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ServiceExecutorReserved", [](ServiceContext* ctx) {
        if (!serverGlobalParams.reservedAdminThreads) {
            return;
        }

        getServiceExecutorReserved(ctx) = std::make_unique<transport::ServiceExecutorReserved>(
            ctx, "admin/internal connections", serverGlobalParams.reservedAdminThreads);
    }};
}  // namespace

thread_local std::deque<ServiceExecutor::Task> ServiceExecutorReserved::_localWorkQueue = {};
thread_local int ServiceExecutorReserved::_localRecursionDepth = 0;
thread_local int64_t ServiceExecutorReserved::_localThreadIdleCounter = 0;

ServiceExecutorReserved::ServiceExecutorReserved(ServiceContext* ctx,
                                                 std::string name,
                                                 size_t reservedThreads)
    : _name(std::move(name)), _reservedThreads(reservedThreads) {}

Status ServiceExecutorReserved::start() {
    {
        stdx::unique_lock<Latch> lk(_mutex);
        _stillRunning.store(true);
        _numStartingThreads = _reservedThreads;
    }

    for (size_t i = 0; i < _reservedThreads; i++) {
        auto status = _startWorker();
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status ServiceExecutorReserved::_startWorker() {
    LOGV2(22978,
          "Starting new worker thread for {name} service executor",
          "Starting new worker thread for service executor",
          "name"_attr = _name);
    return launchServiceWorkerThread([this] {
        stdx::unique_lock<Latch> lk(_mutex);
        _numRunningWorkerThreads.addAndFetch(1);
        auto numRunningGuard = makeGuard([&] {
            _numRunningWorkerThreads.subtractAndFetch(1);
            _shutdownCondition.notify_one();
        });

        _numStartingThreads--;
        _numReadyThreads++;

        while (_stillRunning.load()) {
            _threadWakeup.wait(lk, [&] { return (!_stillRunning.load() || !_readyTasks.empty()); });

            if (!_stillRunning.loadRelaxed()) {
                break;
            }

            if (_readyTasks.empty()) {
                continue;
            }

            auto task = std::move(_readyTasks.front());
            _readyTasks.pop_front();
            _numReadyThreads -= 1;
            bool launchReplacement = false;
            if (_numReadyThreads + _numStartingThreads < _reservedThreads) {
                _numStartingThreads++;
                launchReplacement = true;
            }

            lk.unlock();

            if (launchReplacement) {
                auto threadStartStatus = _startWorker();
                if (!threadStartStatus.isOK()) {
                    LOGV2_WARNING(22981,
                                  "Could not start new reserve worker thread: {error}",
                                  "Could not start new reserve worker thread",
                                  "error"_attr = threadStartStatus);
                }
            }

            _localWorkQueue.emplace_back(std::move(task));
            while (!_localWorkQueue.empty() && _stillRunning.loadRelaxed()) {
                _localRecursionDepth = 1;
                _localWorkQueue.front()();
                _localWorkQueue.pop_front();
            }

            lk.lock();
            if (_numReadyThreads + 1 > _reservedThreads) {
                break;
            } else {
                _numReadyThreads += 1;
            }
        }

        LOGV2_DEBUG(22979,
                    3,
                    "Exiting worker thread in {name} service executor",
                    "Exiting worker thread in service executor",
                    "name"_attr = _name);
    });
}

ServiceExecutorReserved* ServiceExecutorReserved::get(ServiceContext* ctx) {
    auto& ref = getServiceExecutorReserved(ctx);

    // The ServiceExecutorReserved could be absent, so nullptr is okay.
    return ref.get();
}

Status ServiceExecutorReserved::shutdown(Milliseconds timeout) {
    LOGV2_DEBUG(22980, 3, "Shutting down reserved executor");

    stdx::unique_lock<Latch> lock(_mutex);
    _stillRunning.store(false);
    _threadWakeup.notify_all();

    bool result = _shutdownCondition.wait_for(lock, timeout.toSystemDuration(), [this]() {
        return _numRunningWorkerThreads.load() == 0;
    });

    return result
        ? Status::OK()
        : Status(ErrorCodes::Error::ExceededTimeLimit,
                 "reserved executor couldn't shutdown all worker threads within time limit.");
}

Status ServiceExecutorReserved::scheduleTask(Task task, ScheduleFlags flags) {
    if (!_stillRunning.load()) {
        return Status{ErrorCodes::ShutdownInProgress, "Executor is not running"};
    }

    if (!_localWorkQueue.empty()) {
        // Execute task directly (recurse) if allowed by the caller as it produced better
        // performance in testing. Try to limit the amount of recursion so we don't blow up the
        // stack, even though this shouldn't happen with this executor that uses blocking
        // network I/O.
        if ((flags & ScheduleFlags::kMayRecurse) &&
            (_localRecursionDepth < reservedServiceExecutorRecursionLimit.loadRelaxed())) {
            ++_localRecursionDepth;
            task();
        } else {
            _localWorkQueue.emplace_back(std::move(task));
        }
        return Status::OK();
    }

    stdx::lock_guard<Latch> lk(_mutex);
    _readyTasks.push_back(std::move(task));
    _threadWakeup.notify_one();

    return Status::OK();
}

void ServiceExecutorReserved::appendStats(BSONObjBuilder* bob) const {
    // The ServiceExecutorReserved loans a thread to one client for its lifetime and waits
    // synchronously on thread.
    struct Statlet {
        int threads;
        int total;
        int running;
        int waiting;
    };

    auto statlet = [&] {
        stdx::lock_guard lk(_mutex);
        auto threads = static_cast<int>(_numRunningWorkerThreads.loadRelaxed());
        auto total = static_cast<int>(threads - _numReadyThreads - _numStartingThreads);
        auto running = total;
        auto waiting = 0;
        return Statlet{threads, total, running, waiting};
    }();

    BSONObjBuilder subbob = bob->subobjStart(kExecutorName);
    subbob.append(kThreadsRunning, statlet.threads);
    subbob.append(kClientsInTotal, statlet.total);
    subbob.append(kClientsRunning, statlet.running);
    subbob.append(kClientsWaiting, statlet.waiting);
}

void ServiceExecutorReserved::runOnDataAvailable(const SessionHandle& session,
                                                 OutOfLineExecutor::Task onCompletionCallback) {
    scheduleCallbackOnDataAvailable(session, std::move(onCompletionCallback), this);
}

}  // namespace transport
}  // namespace mongo
