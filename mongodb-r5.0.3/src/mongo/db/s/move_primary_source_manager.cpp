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

#include "mongo/db/s/move_primary_source_manager.h"

#include "mongo/client/connpool.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/type_shard_database.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/exit.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangInCloneStage);
MONGO_FAIL_POINT_DEFINE(hangInCleanStaleDataStage);

using namespace shardmetadatautil;

MovePrimarySourceManager::MovePrimarySourceManager(OperationContext* opCtx,
                                                   ShardMovePrimary requestArgs,
                                                   StringData dbname,
                                                   ShardId& fromShard,
                                                   ShardId& toShard)
    : _requestArgs(std::move(requestArgs)),
      _dbname(dbname),
      _fromShard(std::move(fromShard)),
      _toShard(std::move(toShard)),
      _critSecReason(BSON("command"
                          << "movePrimary"
                          << "dbName" << _dbname << "fromShard" << fromShard << "toShard"
                          << toShard)) {}

MovePrimarySourceManager::~MovePrimarySourceManager() {}

NamespaceString MovePrimarySourceManager::getNss() const {
    return _requestArgs.get_shardsvrMovePrimary();
}

Status MovePrimarySourceManager::clone(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCreated);
    auto scopedGuard = makeGuard([&] { cleanupOnError(opCtx); });

    LOGV2(22042,
          "Moving {db} primary from: {fromShard} to: {toShard}",
          "Moving primary for database",
          "db"_attr = _dbname,
          "fromShard"_attr = _fromShard,
          "toShard"_attr = _toShard);

    // Record start in changelog
    auto logChangeCheckedStatus = ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "movePrimary.start",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
        ShardingCatalogClient::kMajorityWriteConcern);

    if (!logChangeCheckedStatus.isOK()) {
        return logChangeCheckedStatus;
    }

    {
        // We use AutoGetDb::ensureDbExists() the first time just in case movePrimary was called
        // before any data was inserted into the database.
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);
        invariant(autoDb.ensureDbExists(), getNss().toString());

        auto dss = DatabaseShardingState::get(opCtx, getNss().toString());
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

        dss->setMovePrimarySourceManager(opCtx, this, dssLock);
    }

    _state = kCloning;

    if (MONGO_unlikely(hangInCloneStage.shouldFail())) {
        LOGV2(4908700, "Hit hangInCloneStage");
        hangInCloneStage.pauseWhileSet(opCtx);
    }

    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto fromShardObj = uassertStatusOK(shardRegistry->getShard(opCtx, _fromShard));
    auto toShardObj = uassertStatusOK(shardRegistry->getShard(opCtx, _toShard));

    BSONObjBuilder cloneCatalogDataCommandBuilder;
    cloneCatalogDataCommandBuilder << "_shardsvrCloneCatalogData" << _dbname << "from"
                                   << fromShardObj->getConnString().toString();


    auto cloneCommandResponse = toShardObj->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        "admin",
        CommandHelpers::appendMajorityWriteConcern(cloneCatalogDataCommandBuilder.obj()),
        Shard::RetryPolicy::kNotIdempotent);

    auto cloneCommandStatus = Shard::CommandResponse::getEffectiveStatus(cloneCommandResponse);
    if (!cloneCommandStatus.isOK()) {
        return cloneCommandStatus;
    }

    auto clonedCollsArray = cloneCommandResponse.getValue().response["clonedColls"];
    for (const auto& elem : clonedCollsArray.Obj()) {
        if (elem.type() == String) {
            _clonedColls.push_back(NamespaceString(elem.String()));
        }
    }

    _state = kCloneCaughtUp;
    scopedGuard.dismiss();
    return Status::OK();
}

Status MovePrimarySourceManager::enterCriticalSection(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloneCaughtUp);
    auto scopedGuard = makeGuard([&] { cleanupOnError(opCtx); });

    // Mark the shard as running a critical operation that requires recovery on crash.
    auto startMetadataOpStatus = ShardingStateRecovery::startMetadataOp(opCtx);
    if (!startMetadataOpStatus.isOK()) {
        return startMetadataOpStatus;
    }

    {
        // The critical section must be entered with the database X lock in order to ensure there
        // are no writes which could have entered and passed the database version check just before
        // we entered the critical section, but will potentially complete after we left it.
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);

        if (!autoDb.getDb()) {
            uasserted(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "The database " << getNss().toString()
                                    << " was dropped during the movePrimary operation.");
        }

        auto dss = DatabaseShardingState::get(opCtx, getNss().toString());
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

        // IMPORTANT: After this line, the critical section is in place and needs to be signaled
        dss->enterCriticalSectionCatchUpPhase(opCtx, dssLock, _critSecReason);
    }

    _state = kCriticalSection;

    // Persist a signal to secondaries that we've entered the critical section. This will cause
    // secondaries to refresh their routing table when next accessed, which will block behind the
    // critical section. This ensures causal consistency by preventing a stale mongos with a cluster
    // time inclusive of the move primary config commit update from accessing secondary data.
    // Note: this write must occur after the critSec flag is set, to ensure the secondary refresh
    // will stall behind the flag.
    Status signalStatus =
        updateShardDatabasesEntry(opCtx,
                                  BSON(ShardDatabaseType::name() << getNss().toString()),
                                  BSONObj(),
                                  BSON(ShardDatabaseType::enterCriticalSectionCounter() << 1),
                                  false /*upsert*/);
    if (!signalStatus.isOK()) {
        return {
            ErrorCodes::OperationFailed,
            str::stream() << "Failed to persist critical section signal for secondaries due to: "
                          << signalStatus.toString()};
    }

    LOGV2(22043, "movePrimary successfully entered critical section");

    scopedGuard.dismiss();

    return Status::OK();
}

Status MovePrimarySourceManager::commitOnConfig(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    auto scopedGuard = makeGuard([&] { cleanupOnError(opCtx); });

    {
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_X);

        if (!autoDb.getDb()) {
            uasserted(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "The database " << getNss().toString()
                                    << " was dropped during the movePrimary operation.");
        }

        auto dss = DatabaseShardingState::get(opCtx, getNss().toString());
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

        // Read operations must begin to wait on the critical section just before we send the
        // commit operation to the config server
        dss->enterCriticalSectionCommitPhase(opCtx, dssLock, _critSecReason);
    }

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    auto commitStatus = [&]() {
        try {
            return _commitOnConfig(opCtx);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }();

    if (!commitStatus.isOK()) {
        // Need to get the latest optime in case the refresh request goes to a secondary --
        // otherwise the read won't wait for the write that _commitOnConfig may have
        // done
        LOGV2(22044,
              "Error occurred while committing the movePrimary. Performing a majority write "
              "against the config server to obtain its latest optime: {error}",
              "Error occurred while committing the movePrimary. Performing a majority write "
              "against the config server to obtain its latest optime",
              "error"_attr = redact(commitStatus));

        Status validateStatus = ShardingLogging::get(opCtx)->logChangeChecked(
            opCtx,
            "movePrimary.validating",
            getNss().ns(),
            _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
            ShardingCatalogClient::kMajorityWriteConcern);

        if ((ErrorCodes::isInterruption(validateStatus.code()) ||
             ErrorCodes::isShutdownError(validateStatus.code()) ||
             validateStatus == ErrorCodes::CallbackCanceled) &&
            globalInShutdownDeprecated()) {
            // Since the server is already doing a clean shutdown, this call will just join the
            // previous shutdown call
            shutdown(waitForShutdown());
        }

        // If we failed to get the latest config optime because we stepped down as primary, then it
        // is safe to fail without crashing because the new primary will fetch the latest optime
        // when it recovers the sharding state recovery document, as long as we also clear the
        // metadata for this database, forcing subsequent callers to do a full refresh. Check if
        // this node can accept writes for this collection as a proxy for it being primary.
        if (!validateStatus.isOK()) {
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            AutoGetDb autoDb(opCtx, getNss().toString(), MODE_IX);

            if (!autoDb.getDb()) {
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "The database " << getNss().toString()
                                        << " was dropped during the movePrimary operation.");
            }

            if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, getNss())) {
                auto dss = DatabaseShardingState::get(opCtx, getNss().db());
                dss->clearDatabaseInfo(opCtx);
                uassertStatusOK(validateStatus.withContext(
                    str::stream() << "Unable to verify movePrimary commit for database: "
                                  << getNss().ns()
                                  << " because the node's replication role changed. Version "
                                     "was cleared for: "
                                  << getNss().ns()
                                  << ", so it will get a full refresh when accessed again."));
            }
        }

        // We would not be able to guarantee our next database refresh would pick up the write for
        // the movePrimary commit (if it happened), because we were unable to get the latest config
        // OpTime.
        fassert(50762,
                validateStatus.withContext(
                    str::stream() << "Failed to commit movePrimary for database " << getNss().ns()
                                  << " due to " << redact(commitStatus)
                                  << ". Updating the optime with a write before clearing the "
                                  << "version also failed"));

        // If we can validate but the commit still failed, return the status.
        return commitStatus;
    }

    _state = kCloneCompleted;

    _cleanup(opCtx);

    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "movePrimary.commit",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
        ShardingCatalogClient::kMajorityWriteConcern));

    scopedGuard.dismiss();

    _state = kNeedCleanStaleData;

    return Status::OK();
}

Status MovePrimarySourceManager::_commitOnConfig(OperationContext* opCtx) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kMajorityReadConcern,
                                            DatabaseType::ConfigNS,
                                            BSON(DatabaseType::name << _dbname),
                                            BSON(DatabaseType::name << -1),
                                            1));

    const auto databasesVector = std::move(findResponse.docs);
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max database version for database '" << _dbname
                          << "', but found no databases",
            !databasesVector.empty());

    const auto dbType = uassertStatusOK(DatabaseType::fromBSON(databasesVector.front()));

    if (dbType.getPrimary() == _toShard) {
        return Status::OK();
    }

    auto newDbType = dbType;
    newDbType.setPrimary(_toShard);

    auto const currentDatabaseVersion = dbType.getVersion();

    newDbType.setVersion(currentDatabaseVersion.makeUpdated());

    auto updateQueryBuilder = BSONObjBuilder(BSON(DatabaseType::name << _dbname));
    updateQueryBuilder.append(DatabaseType::version.name(), currentDatabaseVersion.toBSON());

    auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        DatabaseType::ConfigNS,
        updateQueryBuilder.obj(),
        newDbType.toBSON(),
        false,
        ShardingCatalogClient::kMajorityWriteConcern);

    if (!updateStatus.isOK()) {
        LOGV2(5448803,
              "Error committing movePrimary for {db}: {error}",
              "Error committing movePrimary",
              "db"_attr = _dbname,
              "error"_attr = redact(updateStatus.getStatus()));
        return updateStatus.getStatus();
    }

    return Status::OK();
}

Status MovePrimarySourceManager::cleanStaleData(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kNeedCleanStaleData);

    if (MONGO_unlikely(hangInCleanStaleDataStage.shouldFail())) {
        LOGV2(4908701, "Hit hangInCleanStaleDataStage");
        hangInCleanStaleDataStage.pauseWhileSet(opCtx);
    }

    // Only drop the cloned (unsharded) collections.
    DBDirectClient client(opCtx);
    for (auto& coll : _clonedColls) {
        BSONObj dropCollResult;
        client.runCommand(_dbname.toString(), BSON("drop" << coll.coll()), dropCollResult);
        Status dropStatus = getStatusFromCommandResult(dropCollResult);
        if (!dropStatus.isOK()) {
            LOGV2(22045,
                  "Failed to drop cloned collection {namespace} in movePrimary: {error}",
                  "Failed to drop cloned collection in movePrimary",
                  "namespace"_attr = coll,
                  "error"_attr = redact(dropStatus));
        }
    }

    _state = kDone;
    return Status::OK();
}


void MovePrimarySourceManager::cleanupOnError(OperationContext* opCtx) {
    if (_state == kDone) {
        return;
    }

    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "movePrimary.error",
        _dbname.toString(),
        _buildMoveLogEntry(_dbname.toString(), _fromShard.toString(), _toShard.toString()),
        ShardingCatalogClient::kMajorityWriteConcern);

    try {
        _cleanup(opCtx);
    } catch (const ExceptionForCat<ErrorCategory::NotPrimaryError>& ex) {
        BSONObjBuilder requestArgsBSON;
        _requestArgs.serialize(&requestArgsBSON);
        LOGV2_WARNING(22046,
                      "Failed to clean up movePrimary with request parameters {request} due to: "
                      "{error}",
                      "Failed to clean up movePrimary",
                      "request"_attr = redact(requestArgsBSON.obj()),
                      "error"_attr = redact(ex));
    }
}

void MovePrimarySourceManager::_cleanup(OperationContext* opCtx) {
    invariant(_state != kDone);

    {
        // Unregister from the database's sharding state if we're still registered.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb autoDb(opCtx, getNss().toString(), MODE_IX);

        auto dss = DatabaseShardingState::get(opCtx, getNss().db());
        dss->clearMovePrimarySourceManager(opCtx);
        dss->clearDatabaseInfo(opCtx);
        // Leave the critical section if we're still registered.
        dss->exitCriticalSection(opCtx, _critSecReason);
    }

    if (_state == kCriticalSection || _state == kCloneCompleted) {
        // Clear the 'minOpTime recovery' document so that the next time a node from this shard
        // becomes a primary, it won't have to recover the config server optime.
        ShardingStateRecovery::endMetadataOp(opCtx);
    }

    // If we're in the kCloneCompleted state, then we need to do the last step of cleaning up
    // now-stale data on the old primary. Otherwise, indicate that we're done.
    if (_state != kCloneCompleted) {
        _state = kDone;
    }

    return;
}

}  // namespace mongo
