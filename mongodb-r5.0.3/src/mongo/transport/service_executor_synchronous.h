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

#pragma once

#include <deque>

#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {
namespace transport {

/**
 * The passthrough service executor emulates a thread per connection.
 * Each connection has its own worker thread where jobs get scheduled.
 */
class ServiceExecutorSynchronous final : public ServiceExecutor {
public:
    explicit ServiceExecutorSynchronous(ServiceContext* ctx);

    static ServiceExecutorSynchronous* get(ServiceContext* ctx);

    Status start() override;
    Status shutdown(Milliseconds timeout) override;
    Status scheduleTask(Task task, ScheduleFlags flags) override;

    size_t getRunningThreads() const override {
        return _numRunningWorkerThreads.loadRelaxed();
    }

    Mode transportMode() const override {
        return Mode::kSynchronous;
    }

    void runOnDataAvailable(const SessionHandle& session,
                            OutOfLineExecutor::Task onCompletionCallback) override;

    void appendStats(BSONObjBuilder* bob) const override;

private:
    static thread_local std::deque<Task> _localWorkQueue;
    static thread_local int _localRecursionDepth;
    static thread_local int64_t _localThreadIdleCounter;

    AtomicWord<bool> _stillRunning{false};

    mutable Mutex _shutdownMutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0),
                                                    "ServiceExecutorSynchronous::_shutdownMutex");
    std::shared_ptr<stdx::condition_variable> _shutdownCondition;

    AtomicWord<size_t> _numRunningWorkerThreads{0};
    size_t _numHardwareCores{0};
};

}  // namespace transport
}  // namespace mongo
