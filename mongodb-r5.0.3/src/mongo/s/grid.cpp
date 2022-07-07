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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/grid.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/query/cluster_cursor_manager.h"

namespace mongo {
namespace {
const auto grid = ServiceContext::declareDecoration<Grid>();
}  // namespace

Grid::Grid() = default;

Grid::~Grid() = default;

Grid* Grid::get(ServiceContext* serviceContext) {
    return &grid(serviceContext);
}

Grid* Grid::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

void Grid::init(std::unique_ptr<ShardingCatalogClient> catalogClient,
                std::unique_ptr<CatalogCache> catalogCache,
                std::unique_ptr<ShardRegistry> shardRegistry,
                std::unique_ptr<ClusterCursorManager> cursorManager,
                std::unique_ptr<BalancerConfiguration> balancerConfig,
                std::unique_ptr<executor::TaskExecutorPool> executorPool,
                executor::NetworkInterface* network) {
    invariant(!_catalogClient);
    invariant(!_catalogCache);
    invariant(!_shardRegistry);
    invariant(!_cursorManager);
    invariant(!_balancerConfig);
    invariant(!_executorPool);
    invariant(!_network);

    _catalogClient = std::move(catalogClient);
    _catalogCache = std::move(catalogCache);
    _shardRegistry = std::move(shardRegistry);
    _cursorManager = std::move(cursorManager);
    _balancerConfig = std::move(balancerConfig);
    _executorPool = std::move(executorPool);
    _network = network;

    _shardRegistry->init(grid.owner(this));
}

bool Grid::isShardingInitialized() const {
    return _shardingInitialized.load();
}

void Grid::setShardingInitialized() {
    invariant(!_shardingInitialized.load());
    _shardingInitialized.store(true);
}

Grid::CustomConnectionPoolStatsFn Grid::getCustomConnectionPoolStatsFn() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _customConnectionPoolStatsFn;
}

void Grid::setCustomConnectionPoolStatsFn(CustomConnectionPoolStatsFn statsFn) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_customConnectionPoolStatsFn || !statsFn);
    _customConnectionPoolStatsFn = std::move(statsFn);
}

bool Grid::allowLocalHost() const {
    return _allowLocalShard;
}

void Grid::setAllowLocalHost(bool allow) {
    _allowLocalShard = allow;
}

repl::ReadConcernArgs Grid::readConcernWithConfigTime(
    repl::ReadConcernLevel readConcernLevel) const {
    return ReadConcernArgs(configOpTime(), readConcernLevel);
}

ReadPreferenceSetting Grid::readPreferenceWithConfigTime(
    const ReadPreferenceSetting& readPreference) const {
    ReadPreferenceSetting readPrefToReturn(readPreference);
    readPrefToReturn.minClusterTime = configOpTime().getTimestamp();
    return readPrefToReturn;
}

// TODO SERVER-50675: directly use VectorClock's configTime once 5.0 becomes last-lts.
repl::OpTime Grid::configOpTime() const {
    invariant(serverGlobalParams.clusterRole != ClusterRole::ConfigServer);

    auto configTime = [this] {
        stdx::lock_guard<Latch> lk(_mutex);
        return _configOpTime;
    }();

    const auto& fcv = serverGlobalParams.featureCompatibility;
    if (fcv.isVersionInitialized() &&
        fcv.isGreaterThanOrEqualTo(ServerGlobalParams::FeatureCompatibility::Version::kVersion47)) {
        const auto currentTime = VectorClock::get(grid.owner(this))->getTime();
        const auto vcConfigTimeTs = currentTime.configTime().asTimestamp();
        if (!vcConfigTimeTs.isNull() && vcConfigTimeTs >= configTime.getTimestamp()) {
            // TODO SERVER-44097: investigate why not using a term (e.g. with a LogicalTime)
            // can lead - upon CSRS stepdowns - to a last applied opTime lower than the
            // previous primary's committed opTime
            configTime =
                mongo::repl::OpTime(vcConfigTimeTs, mongo::repl::OpTime::kUninitializedTerm);
        }
    }

    return configTime;
}

boost::optional<repl::OpTime> Grid::advanceConfigOpTime(OperationContext* opCtx,
                                                        repl::OpTime opTime,
                                                        StringData what) {
    const auto prevOpTime = _advanceConfigOpTime(opTime);
    if (prevOpTime && prevOpTime->getTerm() != mongo::repl::OpTime::kUninitializedTerm &&
        opTime.getTerm() != mongo::repl::OpTime::kUninitializedTerm &&
        prevOpTime->getTerm() != opTime.getTerm()) {
        std::string clientAddr = "(unknown)";
        if (opCtx && opCtx->getClient()) {
            clientAddr = opCtx->getClient()->clientAddress(true);
        }
        LOGV2(22792,
              "Received {reason} {clientAddress} indicating config server"
              " term has increased, previous opTime {prevOpTime}, now {opTime}",
              "Term advanced for config server",
              "opTime"_attr = opTime,
              "prevOpTime"_attr = prevOpTime,
              "reason"_attr = what,
              "clientAddress"_attr = clientAddr);
    }
    return prevOpTime;
}

boost::optional<repl::OpTime> Grid::_advanceConfigOpTime(const repl::OpTime& opTime) {
    invariant(serverGlobalParams.clusterRole != ClusterRole::ConfigServer);
    auto vectorClock = VectorClock::get(grid.owner(this));
    if (vectorClock->isEnabled()) {
        vectorClock->gossipInConfigOpTime(opTime);
    }
    stdx::lock_guard<Latch> lk(_mutex);
    if (_configOpTime < opTime) {
        repl::OpTime prev = _configOpTime;
        _configOpTime = opTime;
        return prev;
    }
    return boost::none;
}

void Grid::clearForUnitTests() {
    _catalogCache.reset();
    _catalogClient.reset();
    _shardRegistry.reset();
    _cursorManager.reset();
    _balancerConfig.reset();
    _executorPool.reset();
    _network = nullptr;

    _configOpTime = repl::OpTime();
}

}  // namespace mongo
