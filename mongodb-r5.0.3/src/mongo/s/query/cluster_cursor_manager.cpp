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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"

#include <set>

#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

//
// Helpers to construct a user-friendly error Status from a (nss, cursorId) pair.
//

Status cursorNotFoundStatus(const NamespaceString& nss, CursorId cursorId) {
    return {ErrorCodes::CursorNotFound,
            str::stream() << "Cursor not found (namespace: '" << nss.ns() << "', id: " << cursorId
                          << ")."};
}

Status cursorInUseStatus(const NamespaceString& nss, CursorId cursorId) {
    return {ErrorCodes::CursorInUse,
            str::stream() << "Cursor already in use (namespace: '" << nss.ns()
                          << "', id: " << cursorId << ")."};
}

//
// CursorId is a 64-bit type, made up of a 32-bit prefix and a 32-bit suffix.  The below helpers
// convert between a CursorId and its prefix/suffix.
//

CursorId createCursorId(uint32_t prefix, uint32_t suffix) {
    return (static_cast<uint64_t>(prefix) << 32) | suffix;
}

uint32_t extractPrefixFromCursorId(CursorId cursorId) {
    return static_cast<uint64_t>(cursorId) >> 32;
}

}  // namespace

ClusterCursorManager::PinnedCursor::PinnedCursor(ClusterCursorManager* manager,
                                                 ClusterClientCursorGuard&& cursorGuard,
                                                 const NamespaceString& nss,
                                                 CursorId cursorId)
    : _manager(manager), _cursor(cursorGuard.releaseCursor()), _nss(nss), _cursorId(cursorId) {
    invariant(_manager);
    invariant(_cursorId);  // Zero is not a valid cursor id.
}

ClusterCursorManager::PinnedCursor::~PinnedCursor() {
    if (_cursor) {
        // The underlying cursor has not yet been returned.
        returnAndKillCursor();
    }
}

ClusterCursorManager::PinnedCursor::PinnedCursor(PinnedCursor&& other)
    : _manager(std::move(other._manager)),
      _cursor(std::move(other._cursor)),
      _nss(std::move(other._nss)),
      _cursorId(std::move(other._cursorId)) {}

ClusterCursorManager::PinnedCursor& ClusterCursorManager::PinnedCursor::operator=(
    ClusterCursorManager::PinnedCursor&& other) {
    if (_cursor) {
        // The underlying cursor has not yet been returned.
        returnAndKillCursor();
    }
    _manager = std::move(other._manager);
    _cursor = std::move(other._cursor);
    _nss = std::move(other._nss);
    _cursorId = std::move(other._cursorId);
    return *this;
}

void ClusterCursorManager::PinnedCursor::returnCursor(CursorState cursorState) {
    invariant(_cursor);
    // Note that unpinning a cursor transfers ownership of the underlying ClusterClientCursor object
    // back to the manager.
    _manager->checkInCursor(std::move(_cursor), _nss, _cursorId, cursorState);
    *this = PinnedCursor();
}

CursorId ClusterCursorManager::PinnedCursor::getCursorId() const {
    return _cursorId;
}

GenericCursor ClusterCursorManager::PinnedCursor::toGenericCursor() const {
    invariant(_cursor);
    GenericCursor gc;
    gc.setCursorId(getCursorId());
    gc.setNs(_nss);
    gc.setLsid(_cursor->getLsid());
    gc.setNDocsReturned(_cursor->getNumReturnedSoFar());
    gc.setTailable(_cursor->isTailable());
    gc.setAwaitData(_cursor->isTailableAndAwaitData());
    gc.setOriginatingCommand(_cursor->getOriginatingCommand());
    gc.setLastAccessDate(_cursor->getLastUseDate());
    gc.setCreatedDate(_cursor->getCreatedDate());
    gc.setNBatchesReturned(_cursor->getNBatches());
    return gc;
}

void ClusterCursorManager::PinnedCursor::returnAndKillCursor() {
    invariant(_cursor);

    // Return the cursor as exhausted so that it's deleted immediately.
    returnCursor(CursorState::Exhausted);
}

ClusterCursorManager::ClusterCursorManager(ClockSource* clockSource)
    : _clockSource(clockSource),
      _randomSeed(SecureRandom().nextInt64()),
      _pseudoRandom(_randomSeed) {
    invariant(_clockSource);
}

ClusterCursorManager::~ClusterCursorManager() {
    invariant(_cursorIdPrefixToNamespaceMap.empty());
    invariant(_namespaceToContainerMap.empty());
}

void ClusterCursorManager::shutdown(OperationContext* opCtx) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _inShutdown = true;
    }
    killAllCursors(opCtx);
}

StatusWith<CursorId> ClusterCursorManager::registerCursor(
    OperationContext* opCtx,
    std::unique_ptr<ClusterClientCursor> cursor,
    const NamespaceString& nss,
    CursorType cursorType,
    CursorLifetime cursorLifetime,
    UserNameIterator authenticatedUsers) {
    // Read the clock out of the lock.
    const auto now = _clockSource->now();

    stdx::unique_lock<Latch> lk(_mutex);
    _log.push({LogEvent::Type::kRegisterAttempt, boost::none, now, nss});

    if (_inShutdown) {
        lk.unlock();
        cursor->kill(opCtx);
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot register new cursors as we are in the process of shutting down");
    }

    invariant(cursor);
    cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

    // Find the CursorEntryContainer for this namespace.  If none exists, create one.
    auto nsToContainerIt = _namespaceToContainerMap.find(nss);
    if (nsToContainerIt == _namespaceToContainerMap.end()) {
        uint32_t containerPrefix = 0;
        do {
            // The server has always generated positive values for CursorId (which is a signed
            // type), so we use std::abs() here on the prefix for consistency with this historical
            // behavior. If the random number generated is INT_MIN, calling std::abs on it is
            // undefined behavior on 2's complement systems so we need to generate a new number.
            int32_t randomNumber = 0;
            do {
                randomNumber = _pseudoRandom.nextInt32();
            } while (randomNumber == std::numeric_limits<int32_t>::min());
            containerPrefix = static_cast<uint32_t>(std::abs(randomNumber));
        } while (_cursorIdPrefixToNamespaceMap.count(containerPrefix) > 0);
        _cursorIdPrefixToNamespaceMap[containerPrefix] = nss;

        auto emplaceResult =
            _namespaceToContainerMap.emplace(nss, CursorEntryContainer(containerPrefix));
        invariant(emplaceResult.second);
        invariant(_namespaceToContainerMap.size() == _cursorIdPrefixToNamespaceMap.size());

        nsToContainerIt = emplaceResult.first;
    } else {
        invariant(!nsToContainerIt->second.entryMap.empty());  // If exists, shouldn't be empty.
    }
    CursorEntryContainer& container = nsToContainerIt->second;

    // Generate a CursorId (which can't be the invalid value zero).
    CursorEntryMap& entryMap = container.entryMap;
    CursorId cursorId = 0;
    do {
        const uint32_t cursorSuffix = static_cast<uint32_t>(_pseudoRandom.nextInt32());
        cursorId = createCursorId(container.containerPrefix, cursorSuffix);
    } while (cursorId == 0 || entryMap.count(cursorId) > 0);

    // Create a new CursorEntry and register it in the CursorEntryContainer's map.
    auto emplaceResult = entryMap.emplace(cursorId,
                                          CursorEntry(std::move(cursor),
                                                      cursorType,
                                                      cursorLifetime,
                                                      now,
                                                      authenticatedUsers,
                                                      opCtx->getOperationKey()));
    invariant(emplaceResult.second);
    _log.push({LogEvent::Type::kRegisterComplete, cursorId, now, nss});

    return cursorId;
}

StatusWith<ClusterCursorManager::PinnedCursor> ClusterCursorManager::checkOutCursor(
    const NamespaceString& nss,
    CursorId cursorId,
    OperationContext* opCtx,
    AuthzCheckFn authChecker,
    AuthCheck checkSessionAuth) {
    const auto now = _clockSource->now();

    stdx::lock_guard<Latch> lk(_mutex);
    _log.push({LogEvent::Type::kCheckoutAttempt, cursorId, now, nss});

    if (_inShutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot check out cursor as we are in the process of shutting down");
    }

    CursorEntry* entry = _getEntry(lk, nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    // Check if the user is coauthorized to access this cursor.
    auto authCheckStatus = authChecker(entry->getAuthenticatedUsers());
    if (!authCheckStatus.isOK()) {
        return authCheckStatus.withContext(str::stream()
                                           << "cursor id " << cursorId
                                           << " was not created by the authenticated user");
    }

    if (checkSessionAuth == kCheckSession) {
        const auto cursorPrivilegeStatus = checkCursorSessionPrivilege(opCtx, entry->getLsid());
        if (!cursorPrivilegeStatus.isOK()) {
            return cursorPrivilegeStatus;
        }
    }

    if (entry->getOperationUsingCursor()) {
        return cursorInUseStatus(nss, cursorId);
    }

    auto cursorGuard = entry->releaseCursor(opCtx);

    // We use pinning of a cursor as a proxy for active, user-initiated use of a cursor.  Therefore,
    // we pass down to the logical session cache and vivify the record (updating last use).
    if (cursorGuard->getLsid()) {
        auto vivifyCursorStatus =
            LogicalSessionCache::get(opCtx)->vivify(opCtx, cursorGuard->getLsid().get());
        if (!vivifyCursorStatus.isOK()) {
            return vivifyCursorStatus;
        }
    }
    cursorGuard->reattachToOperationContext(opCtx);

    _log.push({LogEvent::Type::kCheckoutComplete, cursorId, now, nss});
    return PinnedCursor(this, std::move(cursorGuard), nss, cursorId);
}

void ClusterCursorManager::checkInCursor(std::unique_ptr<ClusterClientCursor> cursor,
                                         const NamespaceString& nss,
                                         CursorId cursorId,
                                         CursorState cursorState) {
    invariant(cursor);
    // Read the clock out of the lock.
    const auto now = _clockSource->now();

    // Detach the cursor from the operation which had checked it out.
    OperationContext* opCtx = cursor->getCurrentOperationContext();
    invariant(opCtx);
    cursor->detachFromOperationContext();
    cursor->setLastUseDate(now);

    stdx::unique_lock<Latch> lk(_mutex);
    _log.push({LogEvent::Type::kCheckInAttempt, cursorId, now, nss});

    CursorEntry* entry = _getEntry(lk, nss, cursorId);
    invariant(entry);

    // killPending will be true if killCursor() was called while the cursor was in use.
    const bool killPending = entry->isKillPending();

    entry->setLastActive(now);
    entry->returnCursor(std::move(cursor));

    if (cursorState == CursorState::NotExhausted && !killPending) {
        _log.push({LogEvent::Type::kCheckInCompleteCursorSaved, cursorId, now, nss});
        // The caller may need the cursor again.
        return;
    }

    // After detaching the cursor, the entry will be destroyed.
    entry = nullptr;
    detachAndKillCursor(std::move(lk), opCtx, nss, cursorId);
}

Status ClusterCursorManager::checkAuthForKillCursors(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     CursorId cursorId,
                                                     AuthzCheckFn authChecker) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto entry = _getEntry(lk, nss, cursorId);

    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    // Note that getAuthenticatedUsers() is thread-safe, so it's okay to call even if there's
    // an operation using the cursor.
    return authChecker(entry->getAuthenticatedUsers());
}

void ClusterCursorManager::killOperationUsingCursor(WithLock, CursorEntry* entry) {
    invariant(entry->getOperationUsingCursor());
    // Interrupt any operation currently using the cursor.
    OperationContext* opUsingCursor = entry->getOperationUsingCursor();
    stdx::lock_guard<Client> lk(*opUsingCursor->getClient());
    opUsingCursor->getServiceContext()->killOperation(lk, opUsingCursor, ErrorCodes::CursorKilled);

    // Don't delete the cursor, as an operation is using it. It will be cleaned up when the
    // operation is done.
}

Status ClusterCursorManager::killCursor(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        CursorId cursorId) {
    invariant(opCtx);

    const auto now = _clockSource->now();
    stdx::unique_lock<Latch> lk(_mutex);

    _log.push({LogEvent::Type::kKillCursorAttempt, cursorId, now, nss});

    CursorEntry* entry = _getEntry(lk, nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    // Interrupt any operation currently using the cursor, unless if it's the current operation.
    OperationContext* opUsingCursor = entry->getOperationUsingCursor();
    if (opUsingCursor) {
        // The caller shouldn't need to call killCursor on their own cursor.
        invariant(opUsingCursor != opCtx, "Cannot call killCursor() on your own cursor");
        killOperationUsingCursor(lk, entry);
        return Status::OK();
    }

    // No one is using the cursor, so we destroy it.
    detachAndKillCursor(std::move(lk), opCtx, nss, cursorId);

    // We no longer hold the lock here.

    return Status::OK();
}

void ClusterCursorManager::detachAndKillCursor(stdx::unique_lock<Latch> lk,
                                               OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               CursorId cursorId) {
    auto detachedCursorGuard = _detachCursor(lk, opCtx, nss, cursorId);
    invariant(detachedCursorGuard.getStatus());

    // Deletion of the cursor can happen out of the lock.
    lk.unlock();
    detachedCursorGuard.getValue()->kill(opCtx);
}

std::size_t ClusterCursorManager::killMortalCursorsInactiveSince(OperationContext* opCtx,
                                                                 Date_t cutoff) {
    const auto now = _clockSource->now();
    stdx::unique_lock<Latch> lk(_mutex);

    auto pred = [cutoff](CursorId cursorId, const CursorEntry& entry) -> bool {
        if (entry.getLifetimeType() == CursorLifetime::Immortal ||
            entry.getOperationUsingCursor() ||
            (entry.getLsid() && !enableTimeoutOfInactiveSessionCursors.load())) {
            return false;
        }

        bool res = entry.getLastActive() <= cutoff;

        if (res) {
            LOGV2(22837,
                  "Cursor timed out",
                  "cursorId"_attr = cursorId,
                  "idleSince"_attr = entry.getLastActive().toString());
        }

        return res;
    };

    return killCursorsSatisfying(std::move(lk), opCtx, std::move(pred), now);
}

void ClusterCursorManager::killAllCursors(OperationContext* opCtx) {
    const auto now = _clockSource->now();
    stdx::unique_lock<Latch> lk(_mutex);
    auto pred = [](CursorId, const CursorEntry&) -> bool { return true; };

    killCursorsSatisfying(std::move(lk), opCtx, std::move(pred), now);
}

std::size_t ClusterCursorManager::killCursorsSatisfying(
    stdx::unique_lock<Latch> lk,
    OperationContext* opCtx,
    std::function<bool(CursorId, const CursorEntry&)> pred,
    Date_t now) {
    invariant(opCtx);
    invariant(lk.owns_lock());
    std::size_t nKilled = 0;

    _log.push(
        {LogEvent::Type::kRemoveCursorsSatisfyingPredicateAttempt, boost::none, now, boost::none});

    std::vector<ClusterClientCursorGuard> cursorsToDestroy;
    auto nsContainerIt = _namespaceToContainerMap.begin();
    while (nsContainerIt != _namespaceToContainerMap.end()) {
        auto&& entryMap = nsContainerIt->second.entryMap;
        auto cursorIdEntryIt = entryMap.begin();
        while (cursorIdEntryIt != entryMap.end()) {
            auto cursorId = cursorIdEntryIt->first;
            auto& entry = cursorIdEntryIt->second;

            if (!pred(cursorId, entry)) {
                ++cursorIdEntryIt;
                continue;
            }

            ++nKilled;

            if (entry.getOperationUsingCursor()) {
                // Mark the OperationContext using the cursor as killed, and move on.
                killOperationUsingCursor(lk, &entry);
                ++cursorIdEntryIt;
                continue;
            }

            _log.push({LogEvent::Type::kCursorMarkedForDeletionBySatisfyingPredicate,
                       cursorId,
                       // While we collected 'now' above, we ran caller-provided predicates which
                       // may have been expensive. To avoid re-reading from the clock while the
                       // lock is held, we do not provide a value for 'now' in this log entry.
                       boost::none,
                       nsContainerIt->first});

            cursorsToDestroy.push_back(entry.releaseCursor(opCtx));

            // Destroy the entry and set the iterator to the next element.
            entryMap.erase(cursorIdEntryIt++);
        }

        if (entryMap.empty()) {
            nsContainerIt = eraseContainer(nsContainerIt);
        } else {
            ++nsContainerIt;
        }
    }

    _log.push({LogEvent::Type::kRemoveCursorsSatisfyingPredicateComplete,
               boost::none,
               // While we collected 'now' above, we ran caller-provided predicates which may have
               // been expensive. To avoid re-reading from the clock while the lock is held, we do
               // not provide a value for 'now' in this log entry.
               boost::none,
               boost::none});

    // Ensure cursors are killed outside the lock, as killing may require waiting for callbacks to
    // finish.
    lk.unlock();

    for (auto&& cursorGuard : cursorsToDestroy) {
        invariant(cursorGuard);
        cursorGuard->kill(opCtx);
    }

    return nKilled;
}

ClusterCursorManager::Stats ClusterCursorManager::stats() const {
    stdx::lock_guard<Latch> lk(_mutex);

    Stats stats;

    for (auto& nsContainerPair : _namespaceToContainerMap) {
        for (auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            const CursorEntry& entry = cursorIdEntryPair.second;

            if (entry.isKillPending()) {
                // Killed cursors do not count towards the number of pinned cursors or the number of
                // open cursors.
                continue;
            }

            if (entry.getOperationUsingCursor()) {
                ++stats.cursorsPinned;
            }

            switch (entry.getCursorType()) {
                case CursorType::SingleTarget:
                    ++stats.cursorsSingleTarget;
                    break;
                case CursorType::MultiTarget:
                    ++stats.cursorsMultiTarget;
                    break;
            }
        }
    }

    return stats;
}

void ClusterCursorManager::appendActiveSessions(LogicalSessionIdSet* lsids) const {
    stdx::lock_guard<Latch> lk(_mutex);

    for (const auto& nsContainerPair : _namespaceToContainerMap) {
        for (const auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            const CursorEntry& entry = cursorIdEntryPair.second;

            if (entry.isKillPending()) {
                // Don't include sessions for killed cursors.
                continue;
            }

            auto lsid = entry.getLsid();
            if (lsid) {
                lsids->insert(*lsid);
            }
        }
    }
}

GenericCursor ClusterCursorManager::CursorEntry::cursorToGenericCursor(
    CursorId cursorId, const NamespaceString& ns) const {
    invariant(_cursor);
    GenericCursor gc;
    gc.setCursorId(cursorId);
    gc.setNs(ns);
    gc.setCreatedDate(_cursor->getCreatedDate());
    gc.setLastAccessDate(_cursor->getLastUseDate());
    gc.setLsid(_cursor->getLsid());
    gc.setNDocsReturned(_cursor->getNumReturnedSoFar());
    gc.setTailable(_cursor->isTailable());
    gc.setAwaitData(_cursor->isTailableAndAwaitData());
    gc.setOriginatingCommand(_cursor->getOriginatingCommand());
    gc.setNoCursorTimeout(getLifetimeType() == CursorLifetime::Immortal);
    gc.setNBatchesReturned(_cursor->getNBatches());
    return gc;
}

std::vector<GenericCursor> ClusterCursorManager::getIdleCursors(
    const OperationContext* opCtx, MongoProcessInterface::CurrentOpUserMode userMode) const {
    std::vector<GenericCursor> cursors;

    stdx::lock_guard<Latch> lk(_mutex);

    AuthorizationSession* ctxAuth = AuthorizationSession::get(opCtx->getClient());

    for (const auto& nsContainerPair : _namespaceToContainerMap) {
        for (const auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {

            const CursorEntry& entry = cursorIdEntryPair.second;
            // If auth is enabled, and userMode is allUsers, check if the current user has
            // permission to see this cursor.
            if (ctxAuth->getAuthorizationManager().isAuthEnabled() &&
                userMode == MongoProcessInterface::CurrentOpUserMode::kExcludeOthers &&
                !ctxAuth->isCoauthorizedWith(entry.getAuthenticatedUsers())) {
                continue;
            }
            if (entry.isKillPending() || entry.getOperationUsingCursor()) {
                // Don't include sessions for killed or pinned cursors.
                continue;
            }

            cursors.emplace_back(
                entry.cursorToGenericCursor(cursorIdEntryPair.first, nsContainerPair.first));
        }
    }

    return cursors;
}

std::pair<Status, int> ClusterCursorManager::killCursorsWithMatchingSessions(
    OperationContext* opCtx, const SessionKiller::Matcher& matcher) {
    auto eraser = [&](ClusterCursorManager& mgr, CursorId id) {
        auto cursorNss = getNamespaceForCursorId(id);
        if (!cursorNss) {
            // The cursor manager couldn't find a namespace associated with 'id'. This means the
            // cursor must have already been killed, so we have no more work to do.
            return;
        }
        uassertStatusOK(mgr.killCursor(opCtx, *cursorNss, id));
        LOGV2(22838, "Killing cursor as part of killing session(s)", "cursorId"_attr = id);
    };

    auto bySessionCursorKiller = makeKillCursorsBySessionAdaptor(opCtx, matcher, std::move(eraser));
    bySessionCursorKiller(*this);
    return std::make_pair(bySessionCursorKiller.getStatus(),
                          bySessionCursorKiller.getCursorsKilled());
}

stdx::unordered_set<CursorId> ClusterCursorManager::getCursorsForSession(
    LogicalSessionId lsid) const {
    stdx::lock_guard<Latch> lk(_mutex);

    stdx::unordered_set<CursorId> cursorIds;

    for (auto&& nsContainerPair : _namespaceToContainerMap) {
        for (auto&& [cursorId, entry] : nsContainerPair.second.entryMap) {
            if (entry.isKillPending()) {
                // Don't include sessions for killed cursors.
                continue;
            }

            auto cursorLsid = entry.getLsid();
            if (lsid == cursorLsid) {
                cursorIds.insert(cursorId);
            }
        }
    }

    return cursorIds;
}

stdx::unordered_set<CursorId> ClusterCursorManager::getCursorsForOpKeys(
    std::vector<OperationKey> opKeys) const {
    stdx::lock_guard<Latch> lk(_mutex);

    stdx::unordered_set<CursorId> cursorIds;

    // While we could maintain a cached mapping of OperationKey to CursorID to increase performance,
    // this approach was chosen given that 1) mongos will not have as many open cursors as a shard
    // and 2) mongos performance has historically not been a bottleneck.
    for (auto&& opKey : opKeys) {
        for (auto&& nsContainerPair : _namespaceToContainerMap) {
            for (auto&& [cursorId, entry] : nsContainerPair.second.entryMap) {
                if (entry.isKillPending()) {
                    // Don't include any killed cursors.
                    continue;
                }

                if (opKey == entry.getOperationKey()) {
                    cursorIds.insert(cursorId);
                }
            }
        }
    }

    return cursorIds;
}

boost::optional<NamespaceString> ClusterCursorManager::getNamespaceForCursorId(
    CursorId cursorId) const {
    stdx::lock_guard<Latch> lk(_mutex);

    const auto it = _cursorIdPrefixToNamespaceMap.find(extractPrefixFromCursorId(cursorId));
    if (it == _cursorIdPrefixToNamespaceMap.end()) {
        return boost::none;
    }
    return it->second;
}

auto ClusterCursorManager::_getEntry(WithLock, NamespaceString const& nss, CursorId cursorId)
    -> CursorEntry* {

    auto nsToContainerIt = _namespaceToContainerMap.find(nss);
    if (nsToContainerIt == _namespaceToContainerMap.end()) {
        return nullptr;
    }
    CursorEntryMap& entryMap = nsToContainerIt->second.entryMap;
    auto entryMapIt = entryMap.find(cursorId);
    if (entryMapIt == entryMap.end()) {
        return nullptr;
    }

    return &entryMapIt->second;
}

auto ClusterCursorManager::eraseContainer(NssToCursorContainerMap::iterator it)
    -> NssToCursorContainerMap::iterator {
    auto&& container = it->second;
    auto&& entryMap = container.entryMap;
    invariant(entryMap.empty());

    // This was the last cursor remaining in the given namespace.  Erase all state associated
    // with this namespace.
    size_t numDeleted = _cursorIdPrefixToNamespaceMap.erase(container.containerPrefix);
    if (numDeleted != 1) {
        LOGV2_ERROR(
            4786901,
            "Error attempting to erase CursorEntryContainer for nss {nss} and containerPrefix"
            "{prefix}. Could not find containerPrefix in map from cursor ID prefix to nss. "
            "Expected 'numDeleted' to be 1, but got {actualNumDeleted}",
            "Error attempting to erase CursorEntryContainer. Could not find containerPrefix in map "
            "from cursor id prefix to namespace string.",
            "nss"_attr = it->first,
            "prefix"_attr = container.containerPrefix,
            "actualNumDeleted"_attr = numDeleted);
        logCursorManagerInfo();
        MONGO_UNREACHABLE;
    }
    const auto nssRemoved = it->first;
    _namespaceToContainerMap.erase(it++);
    _log.push({LogEvent::Type::kNamespaceEntryMapErased,
               boost::none,
               boost::none,
               std::move(nssRemoved)});

    invariant(_namespaceToContainerMap.size() == _cursorIdPrefixToNamespaceMap.size());
    return it;
}

StatusWith<ClusterClientCursorGuard> ClusterCursorManager::_detachCursor(WithLock lk,
                                                                         OperationContext* opCtx,
                                                                         const NamespaceString& nss,
                                                                         CursorId cursorId) {
    _log.push({LogEvent::Type::kDetachAttempt, cursorId, boost::none, nss});
    CursorEntry* entry = _getEntry(lk, nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    if (entry->getOperationUsingCursor()) {
        return cursorInUseStatus(nss, cursorId);
    }

    // Transfer ownership away from the entry.
    ClusterClientCursorGuard cursor = entry->releaseCursor(opCtx);

    // Destroy the entry.
    auto nsToContainerIt = _namespaceToContainerMap.find(nss);
    invariant(nsToContainerIt != _namespaceToContainerMap.end());
    CursorEntryMap& entryMap = nsToContainerIt->second.entryMap;
    size_t eraseResult = entryMap.erase(cursorId);
    invariant(1 == eraseResult);
    if (entryMap.empty()) {
        eraseContainer(nsToContainerIt);
    }

    _log.push({LogEvent::Type::kDetachComplete, cursorId, boost::none, nss});

    return std::move(cursor);
}

void ClusterCursorManager::logCursorManagerInfo() const {
    LOGV2_ERROR_OPTIONS(4786900,
                        logv2::LogTruncation::Disabled,
                        "Dumping cursor manager contents. "
                        "NSS -> Container map: {nssToContainer} "
                        "Cursor ID Prefix -> NSS map: {cursorIdToNss} "
                        "Internal log: {internalLog}",
                        "Dumping cursor manager contents.",
                        "{nssToContainer}"_attr = dumpNssToContainerMap(),
                        "{cursorIdToNss}"_attr = dumpCursorIdToNssMap(),
                        "{internalLog}"_attr = dumpInternalLog());
}

std::string ClusterCursorManager::LogEvent::typeToString(ClusterCursorManager::LogEvent::Type t) {
    const static stdx::unordered_map<LogEvent::Type, std::string> kMap = {
        {Type::kRegisterAttempt, "registerAttempt"},
        {Type::kRegisterComplete, "registerComplete"},
        {Type::kCheckoutAttempt, "checkoutAttempt"},
        {Type::kCheckoutComplete, "checkoutComplete"},
        {Type::kCheckInAttempt, "checkInAttempt"},
        {Type::kCheckInCompleteCursorSaved, "checkInCompleteCursorSaved"},
        {Type::kDetachAttempt, "detachAttempt"},
        {Type::kDetachComplete, "detachComplete"},
        {Type::kNamespaceEntryMapErased, "namespaceEntryMapErased"},
        {Type::kRemoveCursorsSatisfyingPredicateAttempt, "killCursorsSatisfyingPredicateAttempt"},
        {Type::kRemoveCursorsSatisfyingPredicateComplete, "killCursorsSatisfyingPredicateComplete"},
        {Type::kKillCursorAttempt, "killCursorAttempt"},
    };

    if (auto it = kMap.find(t); it != kMap.end()) {
        return it->second;
    }
    return "unknown " + std::to_string(static_cast<int>(t));
}

BSONObj ClusterCursorManager::dumpNssToContainerMap() const {
    BSONObjBuilder bob;
    // Record an object for the NSS -> Container map.
    {
        BSONObjBuilder nssToContainer(bob.subobjStart("nssToContainer"));
        for (auto&& [nss, cursorContainer] : _namespaceToContainerMap) {
            BSONObjBuilder nssBob(nssToContainer.subobjStart(nss.toString()));
            nssBob.appendNumber("containerPrefix",
                                static_cast<long long>(cursorContainer.containerPrefix));
            BSONArrayBuilder cursors(nssBob.subarrayStart("cursors"));
            for (auto&& [cursorId, cursorEntry] : cursorContainer.entryMap) {
                BSONObjBuilder cursorBob(cursors.subobjStart());
                cursorBob.appendNumber("id", cursorId);
                cursorBob.append("lastActive", cursorEntry.getLastActive());
            }
        }
    }
    return bob.obj();
}

BSONObj ClusterCursorManager::dumpCursorIdToNssMap() const {
    BSONObjBuilder bob;

    // Record an array for the Cursor ID Prefix -> NSS map.
    {
        BSONArrayBuilder cursorIdPrefixToNss(bob.subarrayStart("cursorIdPrefixToNss"));
        for (auto&& [cursorIdPrefix, nss] : _cursorIdPrefixToNamespaceMap) {
            BSONObjBuilder bob(cursorIdPrefixToNss.subobjStart());
            bob.appendNumber("cursorIdPrefix", static_cast<long long>(cursorIdPrefix));
            bob.append("nss", nss.toString());
        }
    }

    return bob.obj();
}

BSONObj ClusterCursorManager::dumpInternalLog() const {
    BSONObjBuilder bob;
    // Dump the internal log maintained by the ClusterCursorManager.
    {
        BSONArrayBuilder logBuilder(bob.subarrayStart("log"));
        size_t i = _log.start;
        while (i != _log.end) {
            BSONObjBuilder bob(logBuilder.subobjStart());
            const auto& logEntry = _log.events[i];
            if (logEntry.cursorId) {
                bob.appendNumber("cursorId", *logEntry.cursorId);
            }
            bob.append("type", LogEvent::typeToString(logEntry.type));

            if (logEntry.time) {
                bob.append("approxTime", *logEntry.time);
            }

            if (logEntry.nss) {
                bob.append("nss", logEntry.nss->toString());
            }

            i = (i + 1) % _log.events.size();
        }
    }
    return bob.obj();
}
}  // namespace mongo
