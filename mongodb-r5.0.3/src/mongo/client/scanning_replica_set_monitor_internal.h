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

/**
 * This is an internal header.
 * This should only be included by replica_set_monitor.cpp and unittests.
 * This should never be included by any header.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "mongo/client/read_preference.h"
#include "mongo/client/scanning_replica_set_monitor.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

struct ScanningReplicaSetMonitor::IsMasterReply {
    IsMasterReply() : ok(false) {}
    IsMasterReply(const HostAndPort& host, int64_t latencyMicros, const BSONObj& reply)
        : ok(false), host(host), latencyMicros(latencyMicros) {
        parse(reply);
    }

    /**
     * Never throws. If parsing fails for any reason, sets ok to false.
     */
    void parse(const BSONObj& obj);

    bool ok;      // if false, ignore all other fields
    BSONObj raw;  // Always owned. Other fields are allowed to be a view into this.
    std::string setName;
    bool isMaster;
    bool secondary;
    bool hidden;
    int configVersion{};
    OID electionId;                 // Set if this isMaster reply is from the primary
    HostAndPort primary;            // empty if not present
    std::set<HostAndPort> members;  // both "hosts" and "passives"
    std::set<HostAndPort> passives;
    BSONObj tags;
    int minWireVersion{};
    int maxWireVersion{};

    // remaining fields aren't in isMaster reply, but are known to caller.
    HostAndPort host;
    int64_t latencyMicros;  // ignored if negative
    Date_t lastWriteDate{};
    repl::OpTime opTime{};
};

/**
 * The SetState is the underlying data object behind both the ScanningReplicaSetMonitor and the
 * Refresher
 *
 * Note that the SetState only holds its own lock in init() and drop(). Even those uses can probably
 * be offloaded to the RSM eventually. In all other cases, the RSM and RSM::Refresher use the
 * SetState lock to synchronize.
 */
struct ScanningReplicaSetMonitor::SetState
    : public std::enable_shared_from_this<ScanningReplicaSetMonitor::SetState> {
    SetState(const SetState&) = delete;
    SetState& operator=(const SetState&) = delete;

public:
    /**
     * Holds the state of a single node in the replicaSet
     */
    struct Node {
        explicit Node(const HostAndPort& host);

        void markFailed(const Status& status);

        bool matches(const ReadPreference pref) const;

        /**
         * Checks if the given tag matches the tag attached to this node.
         *
         * Example:
         *
         * Tag of this node: { "dc": "nyc", "region": "na", "rack": "4" }
         *
         * match: {}
         * match: { "dc": "nyc", "rack": 4 }
         * match: { "region": "na", "dc": "nyc" }
         * not match: { "dc": "nyc", "rack": 2 }
         * not match: { "dc": "sf" }
         */
        bool matches(const BSONObj&) const;

        /**
         *  Returns true if all of the tags in the tag set match node's tags
         */
        bool matches(const TagSet&) const;

        /**
         * Updates this Node based on information in reply. The reply must be from this host.
         */
        void update(const IsMasterReply& reply);

        HostAndPort host;
        bool isUp{false};
        bool isMaster{false};
        int64_t latencyMicros{};
        BSONObj tags;  // owned
        int minWireVersion{};
        int maxWireVersion{};
        Date_t lastWriteDate{};            // from isMasterReply
        Date_t lastWriteDateUpdateTime{};  // set to the local system's time at the time of updating
                                           // lastWriteDate
        Date_t nextPossibleIsMasterCall{};  // time that previous isMaster check ended
        executor::TaskExecutor::CallbackHandle scheduledIsMasterHandle;  //
        repl::OpTime opTime{};                                           // from isMasterReply
    };

    using Nodes = std::vector<Node>;

    struct Waiter {
        Date_t deadline;
        ReadPreferenceSetting criteria;
        std::vector<HostAndPort> excludedHosts;
        Promise<std::vector<HostAndPort>> promise;
    };

    SetState(const MongoURI& uri, ReplicaSetChangeNotifier*, executor::TaskExecutor*);

    bool isUsable() const;

    /**
     * Returns a host matching criteria or an empty host if no known host matches.
     *
     * Note: Uses only local data and does not go over the network.
     */
    std::vector<HostAndPort> getMatchingHosts(
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts = std::vector<HostAndPort>()) const;

    HostAndPort getMatchingHost(
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts = std::vector<HostAndPort>()) const;

    /**
     * Returns the Node with the given host, or NULL if no Node has that host.
     */
    Node* findNode(const HostAndPort& host);

    /**
     * Returns the Node with the given host, or creates one if no Node has that host.
     * Maintains the sorted order of nodes.
     */
    Node* findOrCreateNode(const HostAndPort& host);

    void updateNodeIfInNodes(const IsMasterReply& reply);

    /**
     * Returns the connection string of the nodes that are known the be in the set because we've
     * seen them in the isMaster reply of a PRIMARY.
     */
    ConnectionString confirmedConnectionString() const;

    /**
     * Returns the connection string of the nodes that are believed to be in the set because we've
     * seen them in the isMaster reply of non-PRIMARY nodes in our seed list.
     */
    ConnectionString possibleConnectionString() const;

    /**
     * Call this to notify waiters after a scan processes a valid reply, rescans, or finishes.
     */
    void notify();

    Date_t now() const {
        return executor ? executor->now() : Date_t::now();
    }

    Status makeUnsatisfedReadPrefError(const ReadPreferenceSetting& criteria) const;

    // Tiny enum to convey semantics for rescheduleFefresh()
    enum class SchedulingStrategy {
        kKeepEarlyScan,
        kCancelPreviousScan,
    };

    /**
     * Schedules a refresh via the task executor and cancel any previous refresh.
     * (Task is automatically canceled in the d-tor.)
     */
    void rescheduleRefresh(SchedulingStrategy strategy);

    /**
     *  Notifies all listeners that the ReplicaSet is in use.
     */
    void init();

    /**
     *  Resets the current scan and notifies all listeners that the ReplicaSet isn't in use.
     */
    void drop();

    /**
     * Before unlocking, do `if (kDebugBuild) checkInvariants();`
     */
    void checkInvariants() const;

    /**
     * Wrap the callback and schedule it to run at some time
     *
     * The callback wrapper does the following:
     * * Return before running cb if isDropped is true
     * * Return before running cb if the handle was canceled
     * * Lock before running cb and unlock after
     */
    template <typename Callback>
    auto scheduleWorkAt(Date_t when, Callback&& cb) const;

    const MongoURI setUri;  // URI passed to ctor -- THIS IS NOT UPDATED BY SCANS
    const std::string name;

    ReplicaSetChangeNotifier* const notifier;
    executor::TaskExecutor* const executor;

    bool isDropped = false;

    // You must hold this to access any member below.
    mutable Mutex mutex = MONGO_MAKE_LATCH("SetState::mutex");

    executor::TaskExecutor::CallbackHandle refresherHandle;

    // For starting scans
    std::set<HostAndPort> seedNodes;  // updated whenever a master reports set membership changes
    ConnectionString seedConnStr;     // The connection string from the last time we had valid seeds
    int64_t seedGen = 0;

    bool isMocked = false;  // True if this set is using nodes from MockReplicaSet

    // For tracking scans
    // lastSeenMaster is empty if we have never seen a master or the last scan didn't have one
    HostAndPort lastSeenMaster;
    int consecutiveFailedScans = 0;
    Nodes nodes;                      // maintained sorted and unique by host
    ConnectionString workingConnStr;  // The connection string from our last scan

    // For tracking replies
    OID maxElectionId;      // largest election id observed by this ScanningReplicaSetMonitor
    int configVersion = 0;  // version number of the replica set config.

    // For matching hosts
    int64_t latencyThresholdMicros = 0;
    mutable int roundRobin = 0;  // used when useDeterministicHostSelection is true
    mutable PseudoRandom rand;   // only used for host selection to balance load

    // For scheduling scans
    Seconds refreshPeriod;      // Normal refresh period when not expedited
    bool isExpedited = false;   // True when we are doing more frequent refreshes due to waiters
    std::list<Waiter> waiters;  // Everyone waiting for some ReadPreference to be satisfied
    uint64_t nextScanId = 0;    // The id for the next scan
    ScanStatePtr currentScan;   // NULL if no scan in progress
    Date_t nextScanTime;        // The time at which the next scan is scheduled to start
};

struct ScanningReplicaSetMonitor::ScanState {
    ScanState(const ScanState&) = delete;
    ScanState& operator=(const ScanState&) = delete;

public:
    ScanState() = default;

    /**
     * Adds all hosts in container that aren't in triedHosts to hostsToScan, then shuffles the
     * queue.
     */
    template <typename Container>
    void enqueAllUntriedHosts(const Container& container, PseudoRandom& rand);

    /**
     * Adds all completed hosts back to hostsToScan and shuffles the queue.
     */
    void retryAllTriedHosts(PseudoRandom& rand);

    /**
     * A safe way to clear interrupted scans
     */
    void markHostsToScanAsTried() noexcept;

    // This is only for logging and should not affect behavior otherwise.
    Timer timer;

    // Access to fields is guarded by associated SetState's mutex.
    bool foundUpMaster = false;
    bool foundAnyUpNodes = false;
    std::deque<HostAndPort> hostsToScan;  // Work queue.
    std::set<HostAndPort> possibleNodes;  // Nodes reported by non-primary hosts.
    std::set<HostAndPort> waitingFor;     // Hosts we have dispatched but haven't replied yet.
    std::set<HostAndPort> triedHosts;     // Hosts that have been returned from getNextStep.

    // All responses go here until we find a master.
    typedef std::map<HostAndPort, IsMasterReply> UnconfirmedReplies;
    UnconfirmedReplies unconfirmedReplies;
};

}  // namespace mongo
