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

#include <boost/optional.hpp>
#include <functional>
#include <string>

#include "mongo/executor/task_executor.h"
#include "mongo/transport/baton.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"

namespace mongo {
namespace executor {

extern FailPoint networkInterfaceSendRequestsToTargetHostsInAlphabeticalOrder;
extern FailPoint networkInterfaceDiscardCommandsBeforeAcquireConn;
extern FailPoint networkInterfaceHangCommandsAfterAcquireConn;
extern FailPoint networkInterfaceCommandsFailedWithErrorCode;
extern FailPoint networkInterfaceShouldNotKillPendingRequests;

/**
 * Interface to networking for use by TaskExecutor implementations.
 */
class NetworkInterface {
    NetworkInterface(const NetworkInterface&) = delete;
    NetworkInterface& operator=(const NetworkInterface&) = delete;

public:
    using Response = RemoteCommandResponse;
    using RemoteCommandCompletionFn =
        unique_function<void(const TaskExecutor::ResponseOnAnyStatus&)>;
    using RemoteCommandOnReplyFn = unique_function<void(const TaskExecutor::ResponseOnAnyStatus&)>;

    // Indicates that there is no expiration time by when a request needs to complete
    static constexpr Date_t kNoExpirationDate{Date_t::max()};

    virtual ~NetworkInterface();

    /**
     * Returns diagnostic info.
     */
    virtual std::string getDiagnosticString() = 0;

    /**
     * Appends information about the connections on this NetworkInterface.
     */
    virtual void appendConnectionStats(ConnectionPoolStats* stats) const = 0;

    /**
     * Starts up the network interface.
     *
     * It is valid to call all methods except shutdown() before this method completes.  That is,
     * implementations may not assume that startup() completes before startCommand() first
     * executes.
     *
     * Called by the owning TaskExecutor inside its run() method.
     */
    virtual void startup() = 0;

    /**
     * Shuts down the network interface. Must be called before this instance gets deleted,
     * if startup() is called.
     *
     * Called by the owning TaskExecutor inside its run() method.
     */
    virtual void shutdown() = 0;

    /**
     * Returns true if shutdown has been called, false otherwise.
     */
    virtual bool inShutdown() const = 0;

    /**
     * Blocks the current thread (presumably the executor thread) until the network interface
     * knows of work for the executor to perform.
     */
    virtual void waitForWork() = 0;

    /**
     * Similar to waitForWork, but only blocks until "when".
     */
    virtual void waitForWorkUntil(Date_t when) = 0;

    /**
     * Signals to the network interface that there is new work (such as a signaled event) for
     * the executor to process.  Wakes the executor from waitForWork() and friends.
     */
    virtual void signalWorkAvailable() = 0;

    /**
     * Returns the current time.
     */
    virtual Date_t now() = 0;

    /**
     * Returns the hostname of the current process.
     */
    virtual std::string getHostName() = 0;

    struct Counters {
        uint64_t sent = 0;
        uint64_t canceled = 0;
        uint64_t timedOut = 0;
        uint64_t failed = 0;
        uint64_t failedRemotely = 0;
        uint64_t succeeded = 0;
    };
    /*
     * Returns a copy of the operation counters (see struct Counters above). This method should
     * only be used in tests, and will invariant if testing diagnostics are not enabled.
     */
    virtual Counters getCounters() const = 0;

    /**
     * Starts asynchronous execution of the command described by "request".
     *
     * The request mutated to append request metadata to be merged into the request messages.
     *
     * Returns ErrorCodes::ShutdownInProgress if NetworkInterface::shutdown has already started
     * and Status::OK() otherwise. If it returns Status::OK(), then the onFinish argument will be
     * executed by NetworkInterface eventually; otherwise, it will not.
     *
     * Note that if you pass a baton to startCommand and that baton refuses work, then your onFinish
     * function will not run.
     */
    virtual Status startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                RemoteCommandRequestOnAny& request,
                                RemoteCommandCompletionFn&& onFinish,
                                const BatonHandle& baton = nullptr) = 0;
    virtual Status startExhaustCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                       RemoteCommandRequestOnAny& request,
                                       RemoteCommandOnReplyFn&& onReply,
                                       const BatonHandle& baton = nullptr) = 0;

    Future<TaskExecutor::ResponseOnAnyStatus> startCommand(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequestOnAny& request,
        const BatonHandle& baton = nullptr) {
        auto pf = makePromiseFuture<TaskExecutor::ResponseOnAnyStatus>();

        auto status = startCommand(
            cbHandle,
            request,
            [p = std::move(pf.promise)](const TaskExecutor::ResponseOnAnyStatus& rs) mutable {
                p.emplaceValue(rs);
            },
            baton);

        if (!status.isOK()) {
            return status;
        }
        return std::move(pf.future);
    }

    /**
     * Requests cancellation of the network activity associated with "cbHandle" if it has not yet
     * completed.
     *
     * Note that the work involved in onFinish may run locally as a result of invoking this
     * function. Do not hold locks while calling cancelCommand(...).
     */
    virtual void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                               const BatonHandle& baton = nullptr) = 0;

    /**
     * Sets an alarm, which schedules "action" to run no sooner than "when".
     *
     * Returns ErrorCodes::ShutdownInProgress if NetworkInterface::shutdown has already started
     * and true otherwise. If it returns Status::OK(), then the action will be executed by
     * NetworkInterface eventually if no error occurs while waiting for the alarm; otherwise,
     * it will not.
     *
     * "action" should not do anything that requires a lot of computation, or that might block for a
     * long time, as it may execute in a network thread.
     *
     * Any callbacks invoked from setAlarm must observe onNetworkThread to
     * return true. See that method for why.
     */
    virtual Status setAlarm(const TaskExecutor::CallbackHandle& cbHandle,
                            Date_t when,
                            unique_function<void(Status)> action) = 0;

    /**
     * Requests cancellation of the alarm associated with "cbHandle" if it has not yet completed.
     */
    virtual void cancelAlarm(const TaskExecutor::CallbackHandle& cbHandle) = 0;

    /**
     * Schedules the specified action to run as soon as possible on the network interface's
     * execution resource
     */
    virtual Status schedule(unique_function<void(Status)> action) = 0;

    /**
     * Returns true if called from a thread dedicated to networking. I.e. not a
     * calling thread.
     *
     * This is meant to be used to avoid context switches, so callers must be
     * able to rely on this returning true in a callback or completion handler.
     * In the absence of any actual networking thread, always return true.
     */
    virtual bool onNetworkThread() = 0;

    /**
     * Drops all connections to the given host in the connection pool.
     */
    virtual void dropConnections(const HostAndPort& hostAndPort) = 0;

    /**
     * Acquire a connection and subsequently release it.
     * If status is not OK, the connection will be dropped,
     * otherwise the connection will be returned to the pool.
     */
    virtual void testEgress(const HostAndPort& hostAndPort,
                            transport::ConnectSSLMode sslMode,
                            Milliseconds timeout,
                            Status status) = 0;

protected:
    NetworkInterface();
};

}  // namespace executor
}  // namespace mongo
