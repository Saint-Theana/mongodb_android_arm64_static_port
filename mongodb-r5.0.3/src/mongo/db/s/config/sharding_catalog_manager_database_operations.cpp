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

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <pcrecpp.h>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/sharding_ddl_50_upgrade_downgrade.h"
#include "mongo/db/server_options.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"

namespace mongo {
namespace {

/**
 * Selects an optimal shard on which to place a newly created database from the set of available
 * shards. Will return ShardNotFound if shard could not be found.
 */
ShardId selectShardForNewDatabase(OperationContext* opCtx, ShardRegistry* shardRegistry) {
    // Ensure the shard registry contains the most up-to-date list of available shards
    shardRegistry->reload(opCtx);
    auto allShardIds = shardRegistry->getAllShardIds(opCtx);
    uassert(ErrorCodes::ShardNotFound, "No shards found", !allShardIds.empty());
    // TODO SERVER-54231 stop sorting this vector.
    // Ideally it should be shuffled so that the we choose a random candidate based only
    // on shard size and not on their lexical order.
    std::sort(allShardIds.begin(), allShardIds.end());

    ShardId candidateShardId = allShardIds[0];

    auto candidateSize =
        uassertStatusOK(shardutil::retrieveTotalShardSize(opCtx, candidateShardId));

    for (size_t i = 1; i < allShardIds.size(); i++) {
        const ShardId shardId = allShardIds[i];

        const auto currentSize = uassertStatusOK(shardutil::retrieveTotalShardSize(opCtx, shardId));

        if (currentSize < candidateSize) {
            candidateSize = currentSize;
            candidateShardId = shardId;
        }
    }

    return candidateShardId;
}

}  // namespace

DatabaseType ShardingCatalogManager::createDatabase(OperationContext* opCtx,
                                                    StringData dbName,
                                                    const boost::optional<ShardId>& optPrimaryShard,
                                                    bool enableSharding) {
    if (dbName == NamespaceString::kConfigDb) {
        return DatabaseType(
            dbName.toString(), ShardId::kConfigServerId, true, DatabaseVersion::makeFixed());
    }

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Cannot manually create or shard database '" << dbName << "'",
            dbName != NamespaceString::kAdminDb && dbName != NamespaceString::kLocalDb);

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid db name specified: " << dbName,
            NamespaceString::validDBName(dbName, NamespaceString::DollarInDbNameBehavior::Allow));

    // Make sure to force update of any stale metadata
    ON_BLOCK_EXIT([&] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName); });

    DBDirectClient client(opCtx);

    boost::optional<DistLockManager::ScopedLock> dbLock;

    // First perform an optimistic attempt to write the 'sharded' field to the database entry, in
    // case this is the only thing, which is missing. If that doesn't succeed, go through the
    // expensive createDatabase flow.
    while (true) {
        auto response = client.findAndModify([&] {
            write_ops::FindAndModifyCommandRequest findAndModify(DatabaseType::ConfigNS);
            findAndModify.setQuery([&] {
                BSONObjBuilder queryFilterBuilder;
                queryFilterBuilder.append(DatabaseType::name.name(), dbName);
                if (optPrimaryShard) {
                    uassert(ErrorCodes::BadValue,
                            str::stream() << "invalid shard name: " << *optPrimaryShard,
                            optPrimaryShard->isValid());
                    queryFilterBuilder.append(DatabaseType::primary.name(),
                                              optPrimaryShard->toString());
                }
                return queryFilterBuilder.obj();
            }());
            findAndModify.setUpdate(write_ops::UpdateModification::parseFromClassicUpdate(
                BSON("$set" << BSON(DatabaseType::sharded(enableSharding)))));
            findAndModify.setUpsert(false);
            findAndModify.setNew(true);
            return findAndModify;
        }());

        if (response.getLastErrorObject().getNumDocs()) {
            uassert(528120, "Missing value in the response", response.getValue());
            return uassertStatusOK(DatabaseType::fromBSON(*response.getValue()));
        }

        if (dbLock) {
            break;
        }

        // Do another loop, with the db lock held in order to avoid taking the expensive path on
        // concurrent create database operations
        dbLock.emplace(DistLockManager::get(opCtx)->lockDirectLocally(
            opCtx, dbName, DistLockManager::kDefaultLockTimeout));
    }

    // Expensive createDatabase code path
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());

    // Check if a database already exists with the same name (case sensitive), and if so, return the
    // existing entry.
    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(DatabaseType::name(),
                             (std::string) "^" + pcrecpp::RE::QuoteMeta(dbName.toString()) + "$",
                             "i");

    auto dbDoc = client.findOne(DatabaseType::ConfigNS.ns(), {queryBuilder.obj()});
    auto const [primaryShardPtr, database] = [&] {
        if (!dbDoc.isEmpty()) {
            auto actualDb = uassertStatusOK(DatabaseType::fromBSON(dbDoc));

            uassert(ErrorCodes::DatabaseDifferCase,
                    str::stream() << "can't have 2 databases that just differ on case "
                                  << " have: " << actualDb.getName()
                                  << " want to add: " << dbName.toString(),
                    actualDb.getName() == dbName.toString());

            uassert(
                ErrorCodes::NamespaceExists,
                str::stream() << "database already created on a primary which is different from "
                              << *optPrimaryShard,
                !optPrimaryShard || *optPrimaryShard == actualDb.getPrimary());

            // We did a local read of the database entry above and found that the database already
            // exists. However, the data may not be majority committed (a previous createDatabase
            // attempt may have failed with a writeConcern error).
            // Since the current Client doesn't know the opTime of the last write to the database
            // entry, make it wait for the last opTime in the system when we wait for writeConcern.
            replClient.setLastOpToSystemLastOpTime(opCtx);

            return std::make_pair(
                uassertStatusOK(shardRegistry->getShard(opCtx, actualDb.getPrimary())), actualDb);
        } else {
            // The database does not exist. Insert an entry for the new database into the sharding
            // catalog.
            auto const shardPtr = uassertStatusOK(shardRegistry->getShard(
                opCtx,
                optPrimaryShard ? *optPrimaryShard
                                : selectShardForNewDatabase(opCtx, shardRegistry)));

            FixedFCVRegion fcvRegion(opCtx);

            boost::optional<Timestamp> clusterTime;
            if (DatabaseEntryFormat::get(fcvRegion) == DatabaseEntryFormat::kUUIDandTimestamp) {
                const auto now = VectorClock::get(opCtx)->getTime();
                clusterTime = now.clusterTime().asTimestamp();
            }

            // Pick a primary shard for the new database.
            DatabaseType db(dbName.toString(),
                            shardPtr->getId(),
                            enableSharding,
                            DatabaseVersion(UUID::gen(), clusterTime));

            LOGV2(21938,
                  "Registering new database {db} in sharding catalog",
                  "Registering new database in sharding catalog",
                  "db"_attr = db);

            // Do this write with majority writeConcern to guarantee that the shard sees the write
            // when it receives the _flushDatabaseCacheUpdates.
            uassertStatusOK(
                catalogClient->insertConfigDocument(opCtx,
                                                    DatabaseType::ConfigNS,
                                                    db.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));

            return std::make_pair(shardPtr, db);
        }
    }();

    WriteConcernResult unusedResult;
    uassertStatusOK(waitForWriteConcern(opCtx,
                                        replClient.getLastOp(),
                                        ShardingCatalogClient::kMajorityWriteConcern,
                                        &unusedResult));

    // Note, making the primary shard refresh its databaseVersion here is not required for
    // correctness, since either:
    // 1) This is the first time this database is being created. The primary shard will not have a
    //    databaseVersion already cached.
    // 2) The database was dropped and is being re-created. Since dropping a database also sends
    //    _flushDatabaseCacheUpdates to all shards, the primary shard should not have a database
    //    version cached. (Note, it is possible that dropping a database will skip sending
    //    _flushDatabaseCacheUpdates if the config server fails over while dropping the database.)
    // However, routers don't support retrying internally on StaleDbVersion in transactions
    // (SERVER-39704), so if the first operation run against the database is in a transaction, it
    // would fail with StaleDbVersion. Making the primary shard refresh here allows that first
    // transaction to succeed. This allows our transaction passthrough suites and transaction demos
    // to succeed without additional special logic.
    auto cmdResponse = uassertStatusOK(primaryShardPtr->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        BSON("_flushDatabaseCacheUpdates" << dbName),
        Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(cmdResponse.commandStatus);

    return database;
}

Status ShardingCatalogManager::commitMovePrimary(OperationContext* opCtx,
                                                 const StringData dbname,
                                                 const ShardId& toShard) {

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // TODO (SERVER-53283): Remove once version 5.0 has been released.
    Lock::SharedLock lock(opCtx->lockState(), _kDatabaseOpLock);

    // Must use local read concern because we will perform subsequent writes.
    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            DatabaseType::ConfigNS,
                                            BSON(DatabaseType::name << dbname),
                                            BSON(DatabaseType::name << -1),
                                            1));

    const auto databasesVector = std::move(findResponse.docs);
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max database version for database '" << dbname
                          << "', but found no databases",
            !databasesVector.empty());

    const auto dbType = uassertStatusOK(DatabaseType::fromBSON(databasesVector.front()));

    if (dbType.getPrimary() == toShard) {
        // The primary has already been set to the destination shard. It's likely that there was a
        // network error and the shard resent the command.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return Status::OK();
    }

    auto newDbType = dbType;
    newDbType.setPrimary(toShard);

    auto const currentDatabaseVersion = dbType.getVersion();

    newDbType.setVersion(currentDatabaseVersion.makeUpdated());

    auto updateQueryBuilder = BSONObjBuilder(BSON(DatabaseType::name << dbname));
    updateQueryBuilder.append(DatabaseType::version.name(), currentDatabaseVersion.toBSON());

    auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        DatabaseType::ConfigNS,
        updateQueryBuilder.obj(),
        newDbType.toBSON(),
        false,
        ShardingCatalogClient::kLocalWriteConcern);

    if (!updateStatus.isOK()) {
        LOGV2(21940,
              "Error committing movePrimary for {db}: {error}",
              "Error committing movePrimary",
              "db"_attr = dbname,
              "error"_attr = redact(updateStatus.getStatus()));
        return updateStatus.getStatus();
    }

    // If this assertion is tripped, it means that the request sent fine, but no documents were
    // updated. This is likely because the database version was changed in between the query and
    // the update, so no documents were found to change. This shouldn't happen however, because we
    // are holding the dist lock during the movePrimary operation.
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to update primary shard for database '" << dbname
                          << " with version " << currentDatabaseVersion.getLastMod(),
            updateStatus.getValue());

    // Ensure the next attempt to retrieve the database or any of its collections will do a full
    // reload
    Grid::get(opCtx)->catalogCache()->purgeDatabase(dbname);

    return Status::OK();
}

}  // namespace mongo
