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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_mock.h"

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/hello_response.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Helper to create default ReplSettings for tests that represents a one-node replica set.
 */
ReplSettings createReplSettingsForSingleNodeReplSet() {
    ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setReplSetString("mySet/node1:12345");
    return settings;
}

}  // namespace

ReplicationCoordinatorMock::ReplicationCoordinatorMock(ServiceContext* service,
                                                       const ReplSettings& settings)
    : _service(service), _settings(settings) {}

ReplicationCoordinatorMock::ReplicationCoordinatorMock(ServiceContext* service,
                                                       StorageInterface* storage)
    : ReplicationCoordinatorMock(service, createReplSettingsForSingleNodeReplSet()) {
    _storage = storage;
}

ReplicationCoordinatorMock::ReplicationCoordinatorMock(ServiceContext* service)
    : ReplicationCoordinatorMock(service, createReplSettingsForSingleNodeReplSet()) {}

ReplicationCoordinatorMock::~ReplicationCoordinatorMock() {}

void ReplicationCoordinatorMock::startup(OperationContext* opCtx,
                                         StorageEngine::LastShutdownState lastShutdownState) {
    // TODO
}

void ReplicationCoordinatorMock::enterTerminalShutdown() {
    // TODO
}

bool ReplicationCoordinatorMock::enterQuiesceModeIfSecondary(Milliseconds quiesceTime) {
    // TODO
    return true;
}

bool ReplicationCoordinatorMock::inQuiesceMode() const {
    // TODO
    return false;
}

void ReplicationCoordinatorMock::shutdown(OperationContext*) {
    // TODO
}

void ReplicationCoordinatorMock::markAsCleanShutdownIfPossible(OperationContext*) {
    // TODO
}

const ReplSettings& ReplicationCoordinatorMock::getSettings() const {
    return _settings;
}

bool ReplicationCoordinatorMock::isReplEnabled() const {
    return _settings.usingReplSets();
}

ReplicationCoordinator::Mode ReplicationCoordinatorMock::getReplicationMode() const {
    if (_settings.usingReplSets()) {
        return modeReplSet;
    }
    return modeNone;
}

MemberState ReplicationCoordinatorMock::getMemberState() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return _memberState;
}

std::vector<MemberData> ReplicationCoordinatorMock::getMemberData() const {
    MONGO_UNREACHABLE;
    return {};
}

bool ReplicationCoordinatorMock::canAcceptNonLocalWrites() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return _canAcceptNonLocalWrites;
}

void ReplicationCoordinatorMock::setCanAcceptNonLocalWrites(bool canAcceptNonLocalWrites) {
    stdx::lock_guard<Mutex> lk(_mutex);

    _canAcceptNonLocalWrites = canAcceptNonLocalWrites;
}

Status ReplicationCoordinatorMock::waitForMemberState(MemberState expectedState,
                                                      Milliseconds timeout) {
    MONGO_UNREACHABLE;
    return Status::OK();
}

bool ReplicationCoordinatorMock::isInPrimaryOrSecondaryState(OperationContext* opCtx) const {
    return isInPrimaryOrSecondaryState_UNSAFE();
}

bool ReplicationCoordinatorMock::isInPrimaryOrSecondaryState_UNSAFE() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return _memberState.primary() || _memberState.secondary();
}

Seconds ReplicationCoordinatorMock::getSecondaryDelaySecs() const {
    return Seconds(0);
}

void ReplicationCoordinatorMock::clearSyncSourceDenylist() {}

ReplicationCoordinator::StatusAndDuration ReplicationCoordinatorMock::awaitReplication(
    OperationContext* opCtx, const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    return _awaitReplicationReturnValueFunction(opCtx, opTime);
}

void ReplicationCoordinatorMock::setAwaitReplicationReturnValueFunction(
    AwaitReplicationReturnValueFunction returnValueFunction) {
    _awaitReplicationReturnValueFunction = std::move(returnValueFunction);
}

SharedSemiFuture<void> ReplicationCoordinatorMock::awaitReplicationAsyncNoWTimeout(
    const OpTime& opTime, const WriteConcernOptions& writeConcern) {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorMock::stepDown(OperationContext* opCtx,
                                          bool force,
                                          const Milliseconds& waitTime,
                                          const Milliseconds& stepdownTime) {}

bool ReplicationCoordinatorMock::isWritablePrimaryForReportingPurposes() {
    // TODO
    return true;
}

bool ReplicationCoordinatorMock::canAcceptWritesForDatabase(OperationContext* opCtx,
                                                            StringData dbName) {
    stdx::lock_guard<Mutex> lk(_mutex);

    // Return true if we allow writes explicitly even when not in primary state, as in sharding
    // unit tests, so that the op observers can fire but the tests don't have to set all the states
    // as if it's in primary.
    if (_alwaysAllowWrites) {
        return true;
    }
    return dbName == "local" || _memberState.primary();
}

bool ReplicationCoordinatorMock::canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx,
                                                                   StringData dbName) {
    return canAcceptWritesForDatabase(opCtx, dbName);
}

bool ReplicationCoordinatorMock::canAcceptWritesFor(OperationContext* opCtx,
                                                    const NamespaceStringOrUUID& nsOrUUID) {
    // TODO
    return canAcceptWritesForDatabase(opCtx, nsOrUUID.db());
}

bool ReplicationCoordinatorMock::canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                                           const NamespaceStringOrUUID& nsOrUUID) {
    return canAcceptWritesFor(opCtx, nsOrUUID);
}

Status ReplicationCoordinatorMock::checkCanServeReadsFor(OperationContext* opCtx,
                                                         const NamespaceString& ns,
                                                         bool secondaryOk) {
    // TODO
    return Status::OK();
}

Status ReplicationCoordinatorMock::checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                bool secondaryOk) {
    return checkCanServeReadsFor(opCtx, ns, secondaryOk);
}

bool ReplicationCoordinatorMock::shouldRelaxIndexConstraints(OperationContext* opCtx,
                                                             const NamespaceString& ns) {
    return (!canAcceptWritesFor(opCtx, ns) || tenantMigrationRecipientInfo(opCtx));
}

void ReplicationCoordinatorMock::setMyHeartbeatMessage(const std::string& msg) {
    // TODO
}

void ReplicationCoordinatorMock::setMyLastAppliedOpTimeAndWallTime(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<Mutex> lk(_mutex);

    _myLastAppliedOpTime = opTimeAndWallTime.opTime;
    _myLastAppliedWallTime = opTimeAndWallTime.wallTime;
}

void ReplicationCoordinatorMock::setMyLastDurableOpTimeAndWallTime(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<Mutex> lk(_mutex);

    _myLastDurableOpTime = opTimeAndWallTime.opTime;
    _myLastDurableWallTime = opTimeAndWallTime.wallTime;
}

void ReplicationCoordinatorMock::setMyLastAppliedOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<Mutex> lk(_mutex);

    if (opTimeAndWallTime.opTime > _myLastAppliedOpTime) {
        _myLastAppliedOpTime = opTimeAndWallTime.opTime;
        _myLastAppliedWallTime = opTimeAndWallTime.wallTime;
    }
}

void ReplicationCoordinatorMock::setMyLastDurableOpTimeAndWallTimeForward(
    const OpTimeAndWallTime& opTimeAndWallTime) {
    stdx::lock_guard<Mutex> lk(_mutex);

    if (opTimeAndWallTime.opTime > _myLastDurableOpTime) {
        _myLastDurableOpTime = opTimeAndWallTime.opTime;
        _myLastDurableWallTime = opTimeAndWallTime.wallTime;
    }
}

void ReplicationCoordinatorMock::resetMyLastOpTimes() {
    stdx::lock_guard<Mutex> lk(_mutex);

    _myLastDurableOpTime = OpTime();
    _myLastDurableWallTime = Date_t();
}

OpTimeAndWallTime ReplicationCoordinatorMock::getMyLastAppliedOpTimeAndWallTime(
    bool rollbackSafe) const {
    stdx::lock_guard<Mutex> lk(_mutex);
    if (rollbackSafe && _memberState.rollback()) {
        return {};
    }
    return {_myLastAppliedOpTime, _myLastAppliedWallTime};
}

OpTime ReplicationCoordinatorMock::getMyLastAppliedOpTime() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return _myLastAppliedOpTime;
}

OpTimeAndWallTime ReplicationCoordinatorMock::getMyLastDurableOpTimeAndWallTime() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return {_myLastDurableOpTime, _myLastDurableWallTime};
}

OpTime ReplicationCoordinatorMock::getMyLastDurableOpTime() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return _myLastDurableOpTime;
}

Status ReplicationCoordinatorMock::waitUntilMajorityOpTime(mongo::OperationContext* opCtx,
                                                           mongo::repl::OpTime targetOpTime,
                                                           boost::optional<Date_t> deadline) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::waitUntilOpTimeForRead(OperationContext* opCtx,
                                                          const ReadConcernArgs& settings) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                                               const ReadConcernArgs& settings,
                                                               boost::optional<Date_t> deadline) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) {
    return Status::OK();
}

OID ReplicationCoordinatorMock::getElectionId() {
    // TODO
    return OID();
}

OID ReplicationCoordinatorMock::getMyRID() const {
    return OID();
}

int ReplicationCoordinatorMock::getMyId() const {
    return 0;
}

HostAndPort ReplicationCoordinatorMock::getMyHostAndPort() const {
    return HostAndPort();
}

Status ReplicationCoordinatorMock::setFollowerMode(const MemberState& newState) {
    stdx::lock_guard<Mutex> lk(_mutex);

    _memberState = newState;
    return Status::OK();
}

Status ReplicationCoordinatorMock::setFollowerModeRollback(OperationContext* opCtx) {
    return setFollowerMode(MemberState::RS_ROLLBACK);
}

ReplicationCoordinator::ApplierState ReplicationCoordinatorMock::getApplierState() {
    return ApplierState::Running;
}

void ReplicationCoordinatorMock::signalDrainComplete(OperationContext*, long long) {}

void ReplicationCoordinatorMock::signalUpstreamUpdater() {}

StatusWith<BSONObj> ReplicationCoordinatorMock::prepareReplSetUpdatePositionCommand() const {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("replSetUpdatePosition", 1);
    return cmdBuilder.obj();
}

ReplSetConfig ReplicationCoordinatorMock::getConfig() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return _getConfigReturnValue;
}

void ReplicationCoordinatorMock::setGetConfigReturnValue(ReplSetConfig returnValue) {
    stdx::lock_guard<Mutex> lk(_mutex);

    _getConfigReturnValue = std::move(returnValue);
}

void ReplicationCoordinatorMock::processReplSetGetConfig(BSONObjBuilder* result,
                                                         bool commitmentStatus,
                                                         bool includeNewlyAdded) {
    // TODO
}

void ReplicationCoordinatorMock::processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) {}

void ReplicationCoordinatorMock::advanceCommitPoint(
    const OpTimeAndWallTime& committedOptimeAndWallTime, bool fromSyncSource) {}

void ReplicationCoordinatorMock::cancelAndRescheduleElectionTimeout() {}

Status ReplicationCoordinatorMock::processReplSetGetStatus(BSONObjBuilder*,
                                                           ReplSetGetStatusResponseStyle) {
    return Status::OK();
}

void ReplicationCoordinatorMock::appendSecondaryInfoData(BSONObjBuilder* result) {}

void ReplicationCoordinatorMock::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
}

Status ReplicationCoordinatorMock::setMaintenanceMode(OperationContext* opCtx, bool activate) {
    return Status::OK();
}

bool ReplicationCoordinatorMock::getMaintenanceMode() {
    return false;
}

Status ReplicationCoordinatorMock::processReplSetSyncFrom(OperationContext* opCtx,
                                                          const HostAndPort& target,
                                                          BSONObjBuilder* resultObj) {
    // TODO
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
    // TODO
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetReconfig(OperationContext* opCtx,
                                                          const ReplSetReconfigArgs& args,
                                                          BSONObjBuilder* resultObj) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::doReplSetReconfig(OperationContext* opCtx,
                                                     GetNewConfigFn getNewConfig,
                                                     bool force) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::doOptimizedReconfig(OperationContext* opCtx,
                                                       GetNewConfigFn getNewConfig) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::awaitConfigCommitment(OperationContext* opCtx,
                                                         bool waitForOplogCommitment) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetInitiate(OperationContext* opCtx,
                                                          const BSONObj& configObj,
                                                          BSONObjBuilder* resultObj) {
    return Status::OK();
}

Status ReplicationCoordinatorMock::processReplSetUpdatePosition(const UpdatePositionArgs& updates) {
    // TODO
    return Status::OK();
}

bool ReplicationCoordinatorMock::buildsIndexes() {
    // TODO
    return true;
}

std::vector<HostAndPort> ReplicationCoordinatorMock::getHostsWrittenTo(const OpTime& op,
                                                                       bool durablyWritten) {
    return std::vector<HostAndPort>();
}

Status ReplicationCoordinatorMock::checkIfWriteConcernCanBeSatisfied(
    const WriteConcernOptions& writeConcern) const {
    return Status::OK();
}

Status ReplicationCoordinatorMock::checkIfCommitQuorumCanBeSatisfied(
    const CommitQuorumOptions& commitQuorum) const {
    return Status::OK();
}

bool ReplicationCoordinatorMock::isCommitQuorumSatisfied(
    const CommitQuorumOptions& commitQuorum, const std::vector<mongo::HostAndPort>& members) const {
    return true;
}

WriteConcernOptions ReplicationCoordinatorMock::getGetLastErrorDefault() {
    return WriteConcernOptions();
}

Status ReplicationCoordinatorMock::checkReplEnabledForCommand(BSONObjBuilder* result) {
    // TODO
    return Status::OK();
}

HostAndPort ReplicationCoordinatorMock::chooseNewSyncSource(const OpTime& lastOpTimeFetched) {
    return HostAndPort();
}

void ReplicationCoordinatorMock::denylistSyncSource(const HostAndPort& host, Date_t until) {}

void ReplicationCoordinatorMock::resetLastOpTimesFromOplog(OperationContext* opCtx) {
    stdx::lock_guard<Mutex> lk(_mutex);

    _resetLastOpTimesCalled = true;
}

bool ReplicationCoordinatorMock::lastOpTimesWereReset() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return _resetLastOpTimesCalled;
}

ChangeSyncSourceAction ReplicationCoordinatorMock::shouldChangeSyncSource(
    const HostAndPort& currentSource,
    const rpc::ReplSetMetadata& replMetadata,
    const rpc::OplogQueryMetadata& oqMetadata,
    const OpTime& previousOpTimeFetched,
    const OpTime& lastOpTimeFetched) {
    MONGO_UNREACHABLE;
}

OpTime ReplicationCoordinatorMock::getLastCommittedOpTime() const {
    return OpTime();
}

OpTimeAndWallTime ReplicationCoordinatorMock::getLastCommittedOpTimeAndWallTime() const {
    return {OpTime(), Date_t()};
}

Status ReplicationCoordinatorMock::processReplSetRequestVotes(
    OperationContext* opCtx,
    const ReplSetRequestVotesArgs& args,
    ReplSetRequestVotesResponse* response) {
    return Status::OK();
}

void ReplicationCoordinatorMock::prepareReplMetadata(const BSONObj& metadataRequestObj,
                                                     const OpTime& lastOpTimeFromClient,
                                                     BSONObjBuilder* builder) const {}

Status ReplicationCoordinatorMock::processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                                      ReplSetHeartbeatResponse* response) {
    return Status::OK();
}

bool ReplicationCoordinatorMock::getWriteConcernMajorityShouldJournal() {
    return true;
}

long long ReplicationCoordinatorMock::getTerm() const {
    stdx::lock_guard<Mutex> lk(_mutex);

    return _term;
}

Status ReplicationCoordinatorMock::updateTerm(OperationContext* opCtx, long long term) {
    stdx::lock_guard<Mutex> lk(_mutex);

    _term = term;
    return Status::OK();
}

void ReplicationCoordinatorMock::clearCommittedSnapshot() {}

OpTime ReplicationCoordinatorMock::getCurrentCommittedSnapshotOpTime() const {
    return OpTime();
}

void ReplicationCoordinatorMock::waitUntilSnapshotCommitted(OperationContext* opCtx,
                                                            const Timestamp& untilSnapshot) {}

void ReplicationCoordinatorMock::createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) {
    return;
}

WriteConcernOptions ReplicationCoordinatorMock::populateUnsetWriteConcernOptionsSyncMode(
    WriteConcernOptions wc) {
    if (wc.syncMode == WriteConcernOptions::SyncMode::UNSET) {
        if (wc.wMode == WriteConcernOptions::kMajority) {
            wc.syncMode = WriteConcernOptions::SyncMode::JOURNAL;
        } else {
            wc.syncMode = WriteConcernOptions::SyncMode::NONE;
        }
    }
    return wc;
}

Status ReplicationCoordinatorMock::stepUpIfEligible(bool skipDryRun) {
    return Status::OK();
}

void ReplicationCoordinatorMock::alwaysAllowWrites(bool allowWrites) {
    stdx::lock_guard<Mutex> lk(_mutex);

    _alwaysAllowWrites = allowWrites;
}

Status ReplicationCoordinatorMock::abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) {
    return Status::OK();
}

void ReplicationCoordinatorMock::incrementNumCatchUpOpsIfCatchingUp(long numOps) {
    return;
}

void ReplicationCoordinatorMock::signalDropPendingCollectionsRemovedFromStorage() {}

boost::optional<Timestamp> ReplicationCoordinatorMock::getRecoveryTimestamp() {
    if (_storage) {
        return _storage->getRecoveryTimestamp(getServiceContext());
    }
    return boost::none;
}

bool ReplicationCoordinatorMock::setContainsArbiter() const {
    return false;
}

bool ReplicationCoordinatorMock::replSetContainsNewlyAddedMembers() const {
    return false;
}

void ReplicationCoordinatorMock::attemptToAdvanceStableTimestamp() {
    return;
}

void ReplicationCoordinatorMock::finishRecoveryIfEligible(OperationContext* opCtx) {
    return;
}

void ReplicationCoordinatorMock::updateAndLogStateTransitionMetrics(
    const ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
    const size_t numOpsKilled,
    const size_t numOpsRunning) const {
    return;
}

TopologyVersion ReplicationCoordinatorMock::getTopologyVersion() const {
    return TopologyVersion(repl::instanceId, 0);
}

void ReplicationCoordinatorMock::incrementTopologyVersion() {
    return;
}

SharedSemiFuture<std::shared_ptr<const HelloResponse>>
ReplicationCoordinatorMock::getHelloResponseFuture(
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion) {
    auto response =
        awaitHelloResponse(nullptr, horizonParams, clientTopologyVersion, Date_t::now());
    return SharedSemiFuture<std::shared_ptr<const HelloResponse>>(
        std::shared_ptr<const HelloResponse>(response));
}

std::shared_ptr<const HelloResponse> ReplicationCoordinatorMock::awaitHelloResponse(
    OperationContext* opCtx,
    const SplitHorizon::Parameters& horizonParams,
    boost::optional<TopologyVersion> clientTopologyVersion,
    boost::optional<Date_t> deadline) {
    auto config = getConfig();
    auto response = std::make_shared<HelloResponse>();
    response->setReplSetVersion(config.getConfigVersion());
    response->setIsWritablePrimary(true);
    response->setIsSecondary(false);
    if (config.getNumMembers() > 0) {
        response->setMe(config.getMemberAt(0).getHostAndPort());
    } else {
        response->setMe(HostAndPort::parseThrowing("localhost:27017"));
    }

    response->setElectionId(OID::gen());
    response->setTopologyVersion(TopologyVersion(repl::instanceId, 0));
    return response;
}

StatusWith<OpTime> ReplicationCoordinatorMock::getLatestWriteOpTime(OperationContext* opCtx) const
    noexcept {
    return getMyLastAppliedOpTime();
}

HostAndPort ReplicationCoordinatorMock::getCurrentPrimaryHostAndPort() const {
    return HostAndPort();
}

void ReplicationCoordinatorMock::cancelCbkHandle(
    executor::TaskExecutor::CallbackHandle activeHandle) {
    MONGO_UNREACHABLE;
}

BSONObj ReplicationCoordinatorMock::runCmdOnPrimaryAndAwaitResponse(
    OperationContext* opCtx,
    const std::string& dbName,
    const BSONObj& cmdObj,
    OnRemoteCmdScheduledFn onRemoteCmdScheduled,
    OnRemoteCmdCompleteFn onRemoteCmdComplete) {
    return BSON("ok" << 1);
}
void ReplicationCoordinatorMock::restartScheduledHeartbeats_forTest() {
    return;
}

void ReplicationCoordinatorMock::recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) {
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
