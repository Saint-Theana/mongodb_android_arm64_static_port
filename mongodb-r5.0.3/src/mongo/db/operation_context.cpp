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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/operation_context.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_key_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/random.h"
#include "mongo/transport/baton.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_tick_source.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(maxTimeAlwaysTimeOut);

MONGO_FAIL_POINT_DEFINE(maxTimeNeverTimeOut);

namespace {

// Enabling the checkForInterruptFail fail point will start a game of random chance on the
// connection specified in the fail point data, generating an interrupt with a given fixed
// probability.  Example invocation:
//
// {configureFailPoint: "checkForInterruptFail",
//  mode: "alwaysOn",
//  data: {threadName: "threadName", chance: .01}}
//
// Both data fields must be specified. In the above example, all interrupt points on the thread with
// name 'threadName' will generate a kill on the current operation with probability p(.01),
// including interrupt points of nested operations. "chance" must be a double between 0 and 1,
// inclusive.
MONGO_FAIL_POINT_DEFINE(checkForInterruptFail);

const auto kNoWaiterThread = stdx::thread::id();
}  // namespace

OperationContext::OperationContext(Client* client, OperationId opId)
    : OperationContext(client, OperationIdSlot(opId)) {}

OperationContext::OperationContext(Client* client, OperationIdSlot&& opIdSlot)
    : _client(client),
      _opId(std::move(opIdSlot)),
      _elapsedTime(client ? client->getServiceContext()->getTickSource()
                          : SystemTickSource::get()) {}

OperationContext::~OperationContext() {
    releaseOperationKey();
}

void OperationContext::setDeadlineAndMaxTime(Date_t when,
                                             Microseconds maxTime,
                                             ErrorCodes::Error timeoutError) {
    invariant(!getClient()->isInDirectClient() || _hasArtificialDeadline);
    invariant(ErrorCodes::isExceededTimeLimitError(timeoutError));
    if (ErrorCodes::mustHaveExtraInfo(timeoutError)) {
        invariant(!ErrorExtraInfo::parserFor(timeoutError));
    }
    uassert(40120,
            "Illegal attempt to change operation deadline",
            _hasArtificialDeadline || !hasDeadline());
    _deadline = when;
    _maxTime = maxTime;
    _timeoutError = timeoutError;
}

Microseconds OperationContext::computeMaxTimeFromDeadline(Date_t when) {
    Microseconds maxTime;
    if (when == Date_t::max()) {
        maxTime = Microseconds::max();
    } else {
        maxTime = when - getServiceContext()->getFastClockSource()->now();
        if (maxTime < Microseconds::zero()) {
            maxTime = Microseconds::zero();
        }
    }
    return maxTime;
}

void OperationContext::setDeadlineByDate(Date_t when, ErrorCodes::Error timeoutError) {
    setDeadlineAndMaxTime(when, computeMaxTimeFromDeadline(when), timeoutError);
}

void OperationContext::setDeadlineAfterNowBy(Microseconds maxTime, ErrorCodes::Error timeoutError) {
    Date_t when;
    if (maxTime < Microseconds::zero()) {
        maxTime = Microseconds::zero();
    }
    if (maxTime == Microseconds::max()) {
        when = Date_t::max();
    } else {
        auto clock = getServiceContext()->getFastClockSource();
        when = clock->now();
        if (maxTime > Microseconds::zero()) {
            when += clock->getPrecision() + maxTime;
        }
    }
    setDeadlineAndMaxTime(when, maxTime, timeoutError);
}

bool OperationContext::hasDeadlineExpired() const {
    if (!hasDeadline()) {
        return false;
    }
    if (MONGO_unlikely(maxTimeNeverTimeOut.shouldFail())) {
        return false;
    }
    if (MONGO_unlikely(maxTimeAlwaysTimeOut.shouldFail())) {
        return true;
    }

    // TODO: Remove once all OperationContexts are properly connected to Clients and ServiceContexts
    // in tests.
    if (MONGO_unlikely(!getClient() || !getServiceContext())) {
        return false;
    }

    const auto now = getServiceContext()->getFastClockSource()->now();
    return now >= getDeadline();
}

ErrorCodes::Error OperationContext::getTimeoutError() const {
    return _timeoutError;
}

Milliseconds OperationContext::getRemainingMaxTimeMillis() const {
    if (!hasDeadline()) {
        return Milliseconds::max();
    }

    return std::max(Milliseconds{0},
                    getDeadline() - getServiceContext()->getFastClockSource()->now());
}

Microseconds OperationContext::getRemainingMaxTimeMicros() const {
    if (!hasDeadline()) {
        return Microseconds::max();
    }
    return _maxTime - getElapsedTime();
}

void OperationContext::restoreMaxTimeMS() {
    if (!_storedMaxTime) {
        return;
    }

    auto maxTime = *_storedMaxTime;
    _storedMaxTime = boost::none;

    if (maxTime <= Microseconds::zero()) {
        maxTime = Microseconds::max();
    }

    if (maxTime == Microseconds::max()) {
        _deadline = Date_t::max();
    } else {
        auto clock = getServiceContext()->getFastClockSource();
        _deadline = clock->now() + clock->getPrecision() + maxTime - _elapsedTime.elapsed();
    }
    _maxTime = maxTime;
}

namespace {

// Helper function for checkForInterrupt fail point.  Decides whether the operation currently
// being run by the given Client meet the (probabilistic) conditions for interruption as
// specified in the fail point info.
bool opShouldFail(Client* client, const BSONObj& failPointInfo) {
    // Only target the client with the specified connection number.
    if (client->desc() != failPointInfo["threadName"].valuestrsafe()) {
        return false;
    }

    // Return true with (approx) probability p = "chance".  Recall: 0 <= chance <= 1.
    double next = client->getPrng().nextCanonicalDouble();
    if (next > failPointInfo["chance"].numberDouble()) {
        return false;
    }
    return true;
}

}  // namespace

Status OperationContext::checkForInterruptNoAssert() noexcept {
    // TODO: Remove the MONGO_likely(hasClientAndServiceContext) once all operation contexts are
    // constructed with clients.
    const auto hasClientAndServiceContext = getClient() && getServiceContext();

    if (MONGO_likely(hasClientAndServiceContext) && getClient()->getKilled() &&
        !_isExecutingShutdown) {
        return Status(ErrorCodes::ClientMarkedKilled, "client has been killed");
    }

    if (MONGO_likely(hasClientAndServiceContext) && getServiceContext()->getKillAllOperations() &&
        !_isExecutingShutdown) {
        return Status(ErrorCodes::InterruptedAtShutdown, "interrupted at shutdown");
    }

    if (hasDeadlineExpired()) {
        if (!_hasArtificialDeadline) {
            markKilled(_timeoutError);
        }
        return Status(_timeoutError, "operation exceeded time limit");
    }

    if (_ignoreInterrupts) {
        return Status::OK();
    }

    checkForInterruptFail.executeIf(
        [&](auto&&) {
            LOGV2(20882, "Marking operation as killed for failpoint", "opId"_attr = getOpID());
            markKilled();
        },
        [&](auto&& data) { return opShouldFail(getClient(), data); });

    const auto killStatus = getKillStatus();
    if (killStatus != ErrorCodes::OK) {
        if (killStatus == ErrorCodes::TransactionExceededLifetimeLimitSeconds)
            return Status(
                killStatus,
                "operation was interrupted because the transaction exceeded the configured "
                "'transactionLifetimeLimitSeconds'");

        return Status(killStatus, "operation was interrupted");
    }

    if (_markKillOnClientDisconnect) {
        const auto now = getServiceContext()->getFastClockSource()->now();

        if (now > _lastClientCheck + Milliseconds(500)) {
            _lastClientCheck = now;

            if (!getClient()->session()->isConnected()) {
                markKilled(ErrorCodes::ClientDisconnect);
                return Status(ErrorCodes::ClientDisconnect,
                              "operation was interrupted because a client disconnected");
            }
        }
    }

    return Status::OK();
}

// waitForConditionOrInterruptNoAssertUntil returns when:
//
// Normal condvar wait criteria:
// - cv is notified
// - deadline is passed
//
// OperationContext kill criteria:
// - _deadline is passed (artificial deadline or maxTimeMS)
// - markKilled is called (killOp)
//
// Baton criteria:
// - _baton is notified (someone's queuing work for the baton)
// - _baton::run returns (timeout fired / networking is ready / socket disconnected)
//
// We release the lock held by m whenever we call markKilled, since it may trigger
// CancellationSource cancellation which can in turn emplace a SharedPromise which then may acquire
// a mutex.
StatusWith<stdx::cv_status> OperationContext::waitForConditionOrInterruptNoAssertUntil(
    stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept {
    invariant(getClient());

    // If the maxTimeNeverTimeOut failpoint is set, behave as though the operation's deadline does
    // not exist. Under normal circumstances, if the op has an existing deadline which is sooner
    // than the deadline passed into this method, we replace our deadline with the op's. This means
    // that we expect to time out at the same time as the existing deadline expires. If, when we
    // time out, we find that the op's deadline has not expired (as will always be the case if
    // maxTimeNeverTimeOut is set) then we assume that the incongruity is due to a clock mismatch
    // and return _timeoutError regardless. To prevent this behaviour, only consider the op's
    // deadline in the event that the maxTimeNeverTimeOut failpoint is not set.
    bool opHasDeadline = (hasDeadline() && !MONGO_unlikely(maxTimeNeverTimeOut.shouldFail()));

    if (opHasDeadline) {
        deadline = std::min(deadline, getDeadline());
    }

    const auto waitStatus = [&] {
        if (Date_t::max() == deadline) {
            Waitable::wait(_baton.get(), getServiceContext()->getPreciseClockSource(), cv, m);
            return stdx::cv_status::no_timeout;
        }
        return getServiceContext()->getPreciseClockSource()->waitForConditionUntil(
            cv, m, deadline, _baton.get());
    }();

    if (opHasDeadline && waitStatus == stdx::cv_status::timeout && deadline == getDeadline()) {
        // It's possible that the system clock used in stdx::condition_variable::wait_until
        // is slightly ahead of the FastClock used in checkForInterrupt. In this case,
        // we treat the operation as though it has exceeded its time limit, just as if the
        // FastClock and system clock had agreed.
        if (!_hasArtificialDeadline) {
            interruptible_detail::doWithoutLock(m, [&] { markKilled(_timeoutError); });
        }
        return Status(_timeoutError, "operation exceeded time limit");
    }

    return waitStatus;
}

void OperationContext::markKilled(ErrorCodes::Error killCode) {
    invariant(killCode != ErrorCodes::OK);
    if (ErrorCodes::mustHaveExtraInfo(killCode)) {
        invariant(!ErrorExtraInfo::parserFor(killCode));
    }

    if (killCode == ErrorCodes::ClientDisconnect) {
        LOGV2(20883, "Interrupted operation as its client disconnected", "opId"_attr = getOpID());
    }

    if (auto status = ErrorCodes::OK; _killCode.compareAndSwap(&status, killCode)) {
        _cancelSource.cancel();
        if (_baton) {
            _baton->notify();
        }
    }
}

void OperationContext::markKillOnClientDisconnect() {
    if (getClient()->isInDirectClient()) {
        return;
    }

    if (_markKillOnClientDisconnect) {
        return;
    }

    if (getClient() && getClient()->session()) {
        _lastClientCheck = getServiceContext()->getFastClockSource()->now();

        _markKillOnClientDisconnect = true;

        if (_baton) {
            _baton->markKillOnClientDisconnect();
        }
    }
}

void OperationContext::setIsExecutingShutdown() {
    invariant(!_isExecutingShutdown);

    _isExecutingShutdown = true;

    pushIgnoreInterrupts();
}

void OperationContext::setLogicalSessionId(LogicalSessionId lsid) {
    _lsid = std::move(lsid);
}

void OperationContext::setOperationKey(OperationKey opKey) {
    // Only set the opKey once
    invariant(!_opKey);

    _opKey.emplace(std::move(opKey));
    OperationKeyManager::get(_client).add(*_opKey, _opId.getId());
}

void OperationContext::releaseOperationKey() {
    if (_opKey) {
        OperationKeyManager::get(_client).remove(*_opKey);
    }
    _opKey = boost::none;
}

void OperationContext::setTxnNumber(TxnNumber txnNumber) {
    invariant(_lsid);
    _txnNumber = txnNumber;
}

std::unique_ptr<RecoveryUnit> OperationContext::releaseRecoveryUnit() {
    return std::move(_recoveryUnit);
}

WriteUnitOfWork::RecoveryUnitState OperationContext::setRecoveryUnit(
    std::unique_ptr<RecoveryUnit> unit, WriteUnitOfWork::RecoveryUnitState state) {
    _recoveryUnit = std::move(unit);
    WriteUnitOfWork::RecoveryUnitState oldState = _ruState;
    _ruState = state;
    return oldState;
}

void OperationContext::setLockState(std::unique_ptr<Locker> locker) {
    invariant(!_locker);
    invariant(locker);
    _locker = std::move(locker);
}

std::unique_ptr<Locker> OperationContext::swapLockState(std::unique_ptr<Locker> locker, WithLock) {
    invariant(_locker);
    invariant(locker);
    _locker.swap(locker);
    return locker;
}

Date_t OperationContext::getExpirationDateForWaitForValue(Milliseconds waitFor) {
    return getServiceContext()->getPreciseClockSource()->now() + waitFor;
}

bool OperationContext::isIgnoringInterrupts() const {
    return _ignoreInterrupts;
}

}  // namespace mongo
