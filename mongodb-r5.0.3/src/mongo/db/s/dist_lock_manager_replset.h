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

#include "mongo/db/s/dist_lock_catalog.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"

namespace mongo {

class ReplSetDistLockManager : public DistLockManager {
public:
    // How frequently should the dist lock pinger thread run and write liveness information about
    // this instance of the dist lock manager
    static const Seconds kDistLockPingInterval;

    // How long should the lease on a distributed lock last
    static const Minutes kDistLockExpirationTime;

    ReplSetDistLockManager(ServiceContext* service,
                           StringData processID,
                           std::unique_ptr<DistLockCatalog> catalog,
                           Milliseconds pingInterval,
                           Milliseconds lockExpiration);

    virtual ~ReplSetDistLockManager();

    void startUp() override;
    void shutDown(OperationContext* opCtx) override;

    std::string getProcessID() override;

    Status lockDirect(OperationContext* opCtx,
                      StringData name,
                      StringData whyMessage,
                      Milliseconds waitFor) override;

    Status tryLockDirectWithLocalWriteConcern(OperationContext* opCtx,
                                              StringData name,
                                              StringData whyMessage) override;

    void unlock(Interruptible* intr, StringData name) override;

    void unlockAll(OperationContext* opCtx) override;

    /**
     * Indicates to the dist lock manager that a step-up has occurred with the specified term. This
     * in turn requests that the dist lock manager performs a recovery, freeing all locks it might
     * have previously held, before it attempts to acquire any new ones.
     */
    void onStepUp(long long term);
    void markRecovered_forTest();

private:
    /**
     * Queue a lock to be unlocked asynchronously with retry until it doesn't error.
     */
    SharedSemiFuture<void> _queueUnlock(const OID& lockSessionID, const std::string& name);

    /**
     * Periodically pings and checks if there are locks queued that needs unlocking.
     */
    void doTask();

    /**
     * Returns true if shutDown was called.
     */
    bool isShutDown();

    /**
     * Returns true if the current process that owns the lock has no fresh pings since
     * the lock expiration threshold.
     */
    StatusWith<bool> isLockExpired(OperationContext* opCtx,
                                   const LocksType lockDoc,
                                   const Milliseconds& lockExpiration);

    long long _waitForRecovery(OperationContext* opCtx);

    /**
     * Data structure for storing information about distributed lock pings.
     */
    struct DistLockPingInfo {
        DistLockPingInfo();
        DistLockPingInfo(StringData processId,
                         Date_t lastPing,
                         Date_t configLocalTime,
                         OID lockSessionId,
                         OID electionId);

        // the process processId of the last known owner of the lock.
        std::string processId;

        // the ping value from the last owner of the lock.
        Date_t lastPing;

        // the config server local time when this object was updated.
        Date_t configLocalTime;

        // last known owner of the lock.
        OID lockSessionId;

        // the election id of the config server when this object was updated.
        // Note: unused by legacy dist lock.
        OID electionId;
    };

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (F) Self synchronizing.
    // (M) Must hold _mutex for access.
    // (I) Immutable, no synchronization needed.
    // (S) Can only be called inside startUp/shutDown.
    //

    ServiceContext* const _serviceContext;  // (F)

    const std::string _processID;                     // (I)
    const std::unique_ptr<DistLockCatalog> _catalog;  // (I)
    const Milliseconds _pingInterval;                 // (I)
    const Milliseconds _lockExpiration;               // (I)

    Mutex _mutex = MONGO_MAKE_LATCH("ReplSetDistLockManager::_mutex");
    std::unique_ptr<stdx::thread> _execThread;  // (S)

    // Contains the list of locks queued for unlocking. Among regular unlocks of a properly acquired
    // locks, cases when an unlock operation can be queued include:
    // 1. First attempt on unlocking resulted in an error.
    // 2. Attempting to grab or overtake a lock resulted in an error where we are uncertain
    //    whether the modification was actually applied or not, and call unlock to make
    //    sure that it was cleaned up.
    struct UnlockRequest {
        UnlockRequest(OID lockId, std::string name)
            : lockId(std::move(lockId)), name(std::move(name)) {}

        OID lockId;
        std::string name;

        // Will be signaled when the unlock request has completed
        SharedPromise<void> unlockCompleted;
    };
    std::deque<UnlockRequest> _unlockList;  // (M)

    bool _isShutDown = false;              // (M)
    stdx::condition_variable _shutDownCV;  // (M)

    // Map of lockName to last ping information.
    stdx::unordered_map<std::string, DistLockPingInfo> _pingHistory;  // (M)

    // Tracks the state of post step-up recovery.
    enum Recovery {
        kMustRecover,
        kMustWaitForRecovery,
        kRecovered,
    };
    Recovery _recoveryState;
    long long _recoveryTerm{-1};
    stdx::condition_variable _waitForRecoveryCV;
};

}  // namespace mongo
