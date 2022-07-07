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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <time.h>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/coll_mod_reply_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/drop_database_gen.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/storage_stats_spec_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/async_request_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/scopeguard.h"
#include "mongo/util/version.h"

namespace mongo {
namespace {

/**
 * Returns a CollMod on the underlying buckets collection of the time-series collection.
 * Returns null if 'origCmd' is not for a time-series collection.
 */
std::unique_ptr<CollMod> makeTimeseriesBucketsCollModCommand(OperationContext* opCtx,
                                                             const CollMod& origCmd) {
    const auto& origNs = origCmd.getNamespace();

    auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, origNs);

    // Return early with null if we are not working with a time-series collection.
    if (!timeseriesOptions) {
        return {};
    }

    auto index = origCmd.getIndex();
    if (index && index->getKeyPattern()) {
        auto bucketsIndexSpecWithStatus = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
            *timeseriesOptions, *index->getKeyPattern());

        uassert(ErrorCodes::IndexNotFound,
                str::stream() << bucketsIndexSpecWithStatus.getStatus().toString()
                              << " Command request: " << redact(origCmd.toBSON({})),
                bucketsIndexSpecWithStatus.isOK());

        index->setKeyPattern(std::move(bucketsIndexSpecWithStatus.getValue()));
    }

    auto ns = origNs.makeTimeseriesBucketsNamespace();
    auto cmd = std::make_unique<CollMod>(ns);
    cmd->setIndex(index);
    cmd->setValidator(origCmd.getValidator());
    cmd->setValidationLevel(origCmd.getValidationLevel());
    cmd->setValidationAction(origCmd.getValidationAction());
    cmd->setViewOn(origCmd.getViewOn());
    cmd->setPipeline(origCmd.getPipeline());
    cmd->setRecordPreImages(origCmd.getRecordPreImages());
    cmd->setExpireAfterSeconds(origCmd.getExpireAfterSeconds());
    cmd->setTimeseries(origCmd.getTimeseries());

    return cmd;
}

/**
 * Returns a CollMod on the view definition of the time-series collection.
 * Returns null if 'origCmd' is not for a time-series collection or if the view definition need not
 * be changed.
 */
std::unique_ptr<CollMod> makeTimeseriesViewCollModCommand(OperationContext* opCtx,
                                                          const CollMod& origCmd) {
    const auto& ns = origCmd.getNamespace();

    auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, ns);

    // Return early with null if we are not working with a time-series collection.
    if (!timeseriesOptions) {
        return {};
    }

    auto& tsMod = origCmd.getTimeseries();
    if (tsMod) {
        auto res =
            timeseries::applyTimeseriesOptionsModifications(*timeseriesOptions, tsMod->toBSON());
        if (res.isOK()) {
            auto& [newOptions, changed] = res.getValue();
            if (changed) {
                auto cmd = std::make_unique<CollMod>(ns);
                constexpr bool asArray = false;
                std::vector<BSONObj> pipeline = {
                    timeseries::generateViewPipeline(newOptions, asArray)};
                cmd->setPipeline(std::move(pipeline));
                return cmd;
            }
        }
    }

    return {};
}


class CmdDropDatabase : public DropDatabaseCmdVersion1Gen<CmdDropDatabase> {
public:
    std::string help() const final {
        return "drop (delete) this database";
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }
    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop database '" << request().getDbName()
                                  << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(NamespaceString(request().getDbName()),
                                                            ActionType::dropDatabase));
        }
        Reply typedRun(OperationContext* opCtx) final {
            auto dbName = request().getDbName();
            // disallow dropping the config database
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
                (dbName == NamespaceString::kConfigDb)) {
                uasserted(ErrorCodes::IllegalOperation,
                          "Cannot drop 'config' database if mongod started "
                          "with --configsvr");
            }

            if ((repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
                 repl::ReplicationCoordinator::modeNone) &&
                (dbName == NamespaceString::kLocalDb)) {
                uasserted(ErrorCodes::IllegalOperation,
                          str::stream() << "Cannot drop '" << dbName
                                        << "' database while replication is active");
            }

            if (request().getCommandParameter() != 1) {
                uasserted(5255100, "Have to pass 1 as 'drop' parameter");
            }

            Status status = dropDatabase(opCtx, dbName.toString());
            if (status != ErrorCodes::NamespaceNotFound) {
                uassertStatusOK(status);
            }
            return {};
        }
    };
} cmdDropDatabase;

static const char* repairRemovedMessage =
    "This command has been removed. If you would like to compact your data, use the 'compact' "
    "command. If you would like to rebuild indexes, use the 'reIndex' command. If you need to "
    "recover data, please see the documentation for repairing your database offline: "
    "http://dochub.mongodb.org/core/repair";

class CmdRepairDatabase : public ErrmsgCommandDeprecated {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool maintenanceMode() const {
        return false;
    }

    std::string help() const override {
        return repairRemovedMessage;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    CmdRepairDatabase() : ErrmsgCommandDeprecated("repairDatabase") {}

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) {

        uasserted(ErrorCodes::CommandNotFound, repairRemovedMessage);
        return false;
    }
} cmdRepairDatabase;

/* drop collection */
class CmdDrop : public DropCmdVersion1Gen<CmdDrop> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }
    bool adminOnly() const final {
        return false;
    }
    std::string help() const final {
        return "drop a collection\n{drop : <collectionName>}";
    }
    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return request().getNamespace();
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto ns = request().getNamespace();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop collection '" << ns << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns, ActionType::dropCollection));
        }
        Reply typedRun(OperationContext* opCtx) final {
            if (request().getNamespace().isOplog()) {
                uassert(5255000,
                        "can't drop live oplog while replicating",
                        !repl::ReplicationCoordinator::get(opCtx)->isReplEnabled());
                auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
                invariant(storageEngine);
                // We use the method supportsRecoveryTimestamp() to detect whether we are using
                // the WiredTiger storage engine, which is currently only storage engine that
                // supports the replSetResizeOplog command.
                uassert(
                    5255001,
                    "can't drop oplog on storage engines that support replSetResizeOplog command",
                    !storageEngine->supportsRecoveryTimestamp());
            }

            Reply reply;
            uassertStatusOK(
                dropCollection(opCtx,
                               request().getNamespace(),
                               &reply,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
            return reply;
        }
    };
} cmdDrop;

class CmdDatasize : public ErrmsgCommandDeprecated {
public:
    CmdDatasize() : ErrmsgCommandDeprecated("dataSize", "datasize") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "determine data size for a set of data in a certain range"
               "\nexample: { dataSize:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }"
               "\nmin and max parameters are optional. They must either both be included or both "
               "omitted"
               "\nkeyPattern is an optional parameter indicating an index pattern that would be "
               "useful"
               "for iterating over the min/max bounds. If keyPattern is omitted, it is inferred "
               "from "
               "the structure of min. "
               "\nnote: This command may take a while to run";
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& jsobj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        Timer timer;

        std::string ns = jsobj.firstElement().String();
        BSONObj min = jsobj.getObjectField("min");
        BSONObj max = jsobj.getObjectField("max");
        BSONObj keyPattern = jsobj.getObjectField("keyPattern");
        bool estimate = jsobj["estimate"].trueValue();

        const NamespaceString nss(ns);
        AutoGetCollectionForReadCommand collection(opCtx, nss);

        const auto collDesc =
            CollectionShardingState::get(opCtx, nss)->getCollectionDescription(opCtx);

        if (collDesc.isSharded()) {
            const ShardKeyPattern shardKeyPattern(collDesc.getKeyPattern());
            uassert(ErrorCodes::BadValue,
                    "keyPattern must be empty or must be an object that equals the shard key",
                    keyPattern.isEmpty() ||
                        (SimpleBSONObjComparator::kInstance.evaluate(shardKeyPattern.toBSON() ==
                                                                     keyPattern)));

            uassert(ErrorCodes::BadValue,
                    str::stream() << "min value " << min << " does not have shard key",
                    min.isEmpty() || shardKeyPattern.isShardKey(min));
            min = shardKeyPattern.normalizeShardKey(min);

            uassert(ErrorCodes::BadValue,
                    str::stream() << "max value " << max << " does not have shard key",
                    max.isEmpty() || shardKeyPattern.isShardKey(max));
            max = shardKeyPattern.normalizeShardKey(max);
        }

        long long numRecords = 0;
        if (collection) {
            numRecords = collection->numRecords(opCtx);
        }

        if (numRecords == 0) {
            result.appendNumber("size", 0);
            result.appendNumber("numObjects", 0);
            result.append("millis", timer.millis());
            return true;
        }

        result.appendBool("estimate", estimate);

        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
        if (min.isEmpty() && max.isEmpty()) {
            if (estimate) {
                result.appendNumber("size", static_cast<long long>(collection->dataSize(opCtx)));
                result.appendNumber("numObjects", numRecords);
                result.append("millis", timer.millis());
                return 1;
            }
            exec = InternalPlanner::collectionScan(
                opCtx, &collection.getCollection(), PlanYieldPolicy::YieldPolicy::NO_YIELD);
        } else if (min.isEmpty() || max.isEmpty()) {
            errmsg = "only one of min or max specified";
            return false;
        } else {
            if (keyPattern.isEmpty()) {
                // if keyPattern not provided, try to infer it from the fields in 'min'
                keyPattern = Helpers::inferKeyPattern(min);
            }

            const IndexDescriptor* idx =
                collection->getIndexCatalog()->findShardKeyPrefixedIndex(opCtx,
                                                                         keyPattern,
                                                                         true);  // requireSingleKey

            if (idx == nullptr) {
                errmsg = "couldn't find valid index containing key pattern";
                return false;
            }
            // If both min and max non-empty, append MinKey's to make them fit chosen index
            KeyPattern kp(idx->keyPattern());
            min = Helpers::toKeyFormat(kp.extendRangeBound(min, false));
            max = Helpers::toKeyFormat(kp.extendRangeBound(max, false));

            exec = InternalPlanner::indexScan(opCtx,
                                              &collection.getCollection(),
                                              idx,
                                              min,
                                              max,
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              PlanYieldPolicy::YieldPolicy::NO_YIELD);
        }

        long long avgObjSize = collection->dataSize(opCtx) / numRecords;

        long long maxSize = jsobj["maxSize"].numberLong();
        long long maxObjects = jsobj["maxObjects"].numberLong();

        long long size = 0;
        long long numObjects = 0;

        try {
            RecordId loc;
            while (PlanExecutor::ADVANCED == exec->getNext(static_cast<BSONObj*>(nullptr), &loc)) {
                if (estimate)
                    size += avgObjSize;
                else
                    size += collection->getRecordStore()->dataFor(opCtx, loc).size();

                numObjects++;

                if ((maxSize && size > maxSize) || (maxObjects && numObjects > maxObjects)) {
                    result.appendBool("maxReached", true);
                    break;
                }
            }
        } catch (DBException& exception) {
            LOGV2_WARNING(23801,
                          "Internal error while reading {namespace}",
                          "Internal error while reading",
                          "namespace"_attr = ns);
            exception.addContext("Executor error while reading during dataSize command");
            throw;
        }

        StringBuilder os;
        os << "Finding size for ns: " << ns;
        if (!min.isEmpty()) {
            os << " between " << min << " and " << max;
        }

        result.appendNumber("size", size);
        result.appendNumber("numObjects", numObjects);
        result.append("millis", timer.millis());
        return true;
    }
} cmdDatasize;

class CollectionStats : public ErrmsgCommandDeprecated {
public:
    CollectionStats() : ErrmsgCommandDeprecated("collStats", "collstats") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool maintenanceOk() const override {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "{ collStats:\"blog.posts\" , scale : 1 } scale divides sizes e.g. for KB use 1024\n"
               "    avgObjSize - in bytes";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::collStats);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& jsobj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, jsobj));

        if (nss.coll().empty()) {
            errmsg = "No collection name specified";
            return false;
        }

        result.append("ns", nss.ns());
        auto spec = StorageStatsSpec::parse(IDLParserErrorContext("collStats"), jsobj);
        Status status = appendCollectionStorageStats(opCtx, nss, spec, &result);
        if (!status.isOK() && status.code() != ErrorCodes::NamespaceNotFound) {
            errmsg = status.reason();
            return false;
        }

        return true;
    }
} cmdCollectionStats;

class CollectionModCommand : public BasicCommandWithRequestParser<CollectionModCommand> {
public:
    using Request = CollMod;
    using Reply = CollModReply;

    CollectionModCommand() : BasicCommandWithRequestParser() {}

    virtual const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return "Sets collection options.\n"
               "Example: { collMod: 'foo', viewOn: 'bar'} "
               "Example: { collMod: 'foo', index: {keyPattern: {a: 1}, expireAfterSeconds: 600} "
               "Example: { collMod: 'foo', index: {name: 'bar', expireAfterSeconds: 600} }\n";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        return auth::checkAuthForCollMod(AuthorizationSession::get(client), nss, cmdObj, false);
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const std::string& db,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) final {
        const auto* cmd = &requestParser.request();

        // If the target namespace refers to a time-series collection, we will redirect the
        // collection modification request to the underlying bucket collection.
        // Aliasing collMod on a time-series collection in this manner has a few advantages:
        // - It supports modifying the expireAfterSeconds setting (which is also a collection
        //   creation option).
        // - It avoids any accidental changes to critical view-specific properties of the
        //   time-series collection, which are important for maintaining the view-bucket
        //   relationship.
        //
        // 'timeseriesBucketsCmd' is null if the request namespace does not refer to a time-series
        // collection. Otherwise, transforms the user time-series collMod request to one on the
        // underlying bucket collection.
        auto timeseriesBucketsCmd =
            makeTimeseriesBucketsCollModCommand(opCtx, requestParser.request());
        if (timeseriesBucketsCmd) {
            // We additionally create a special, limited collMod command for the view definition
            // itself if the pipeline needs to be updated to reflect changed timeseries options.
            // This operation is completed first. In the case that we get a partial update where
            // only one of the two collMod operations fully completes (e.g. replication rollback),
            // having the view pipeline update without updating the timeseries options on the
            // buckets collection will result in sub-optimal performance, but correct behavior.
            // If the timeseries options were updated without updating the view pipeline, we could
            // end up with incorrect query behavior (namely data missing from some queries).
            auto timeseriesViewCmd =
                makeTimeseriesViewCollModCommand(opCtx, requestParser.request());
            if (timeseriesViewCmd) {
                uassertStatusOK(collMod(opCtx,
                                        timeseriesViewCmd->getNamespace(),
                                        timeseriesViewCmd->toBSON(BSONObj()),
                                        &result));
            }
            cmd = timeseriesBucketsCmd.get();
        }

        uassertStatusOK(collMod(opCtx, cmd->getNamespace(), cmd->toBSON(BSONObj()), &result));
        return true;
    }

    void validateResult(const BSONObj& resultObj) final {
        auto reply = Reply::parse(IDLParserErrorContext("CollModReply"), resultObj);
        coll_mod_reply_validation::validateReply(reply);
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::CollMod::kAuthorizationContract;
    }
} collectionModCommand;

class DBStats : public ErrmsgCommandDeprecated {
public:
    DBStats() : ErrmsgCommandDeprecated("dbStats", "dbstats") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool maintenanceOk() const override {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "Get stats on a database. Not instantaneous. Slower for databases with large "
               ".ns files.\n"
               "Example: { dbStats:1, scale:1 }";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::dbStats);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& jsobj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        int scale = 1;
        if (jsobj["scale"].isNumber()) {
            scale = jsobj["scale"].numberInt();
            if (scale <= 0) {
                errmsg = "scale has to be > 0";
                return false;
            }
        } else if (jsobj["scale"].trueValue()) {
            errmsg = "scale has to be a number > 0";
            return false;
        }

        const std::string ns = parseNs(dbname, jsobj);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid db name: " << ns,
                NamespaceString::validDBName(ns, NamespaceString::DollarInDbNameBehavior::Allow));

        // TODO (Kal): OldClientContext legacy, needs to be removed
        {
            CurOp::get(opCtx)->ensureStarted();
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setNS_inlock(dbname);
        }

        AutoGetDb autoDb(opCtx, ns, MODE_IS);

        result.append("db", ns);

        Database* db = autoDb.getDb();
        if (!db) {
            // TODO: This preserves old behaviour where we used to create an empty database
            // metadata even when the database is accessed for read. Without this several
            // unit-tests will fail, which are fairly easy to fix. If backwards compatibility
            // is not needed for the missing DB case, we can just do the same that's done in
            // CollectionStats.
            result.appendNumber("collections", 0);
            result.appendNumber("views", 0);
            result.appendNumber("objects", 0);
            result.append("avgObjSize", 0);
            result.appendNumber("dataSize", 0);
            result.appendNumber("storageSize", 0);
            result.appendNumber("totalSize", 0);
            result.appendNumber("indexes", 0);
            result.appendNumber("indexSize", 0);
            result.appendNumber("scaleFactor", scale);
            result.appendNumber("fileSize", 0);
            if (!getGlobalServiceContext()->getStorageEngine()->isEphemeral()) {
                result.appendNumber("fsUsedSize", 0);
                result.appendNumber("fsTotalSize", 0);
            }
        } else {
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                // TODO: OldClientContext legacy, needs to be removed
                CurOp::get(opCtx)->enter_inlock(
                    dbname.c_str(), CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(dbname));
            }

            db->getStats(opCtx, &result, scale);
        }

        return true;
    }
} cmdDBStats;

// Provides the means to asynchronously run `buildinfo` commands.
class BuildInfoExecutor final : public AsyncRequestExecutor {
public:
    BuildInfoExecutor() : AsyncRequestExecutor("BuildInfoExecutor") {}

    Status handleRequest(std::shared_ptr<RequestExecutionContext> rec) {
        auto result = rec->getReplyBuilder()->getBodyBuilder();
        VersionInfoInterface::instance().appendBuildInfo(&result);
        appendStorageEngineList(rec->getOpCtx()->getServiceContext(), &result);
        return Status::OK();
    }

    static BuildInfoExecutor* get(ServiceContext* svc);
};

const auto getBuildInfoExecutor = ServiceContext::declareDecoration<BuildInfoExecutor>();
BuildInfoExecutor* BuildInfoExecutor::get(ServiceContext* svc) {
    return const_cast<BuildInfoExecutor*>(&getBuildInfoExecutor(svc));
}

const auto buildInfoExecutorRegisterer = ServiceContext::ConstructorActionRegisterer{
    "BuildInfoExecutor",
    [](ServiceContext* ctx) { getBuildInfoExecutor(ctx).start(); },
    [](ServiceContext* ctx) { getBuildInfoExecutor(ctx).stop(); }};

class CmdBuildInfo : public BasicCommand {
public:
    CmdBuildInfo() : BasicCommand("buildInfo", "buildinfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const override {
        return false;
    }

    virtual bool adminOnly() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    std::string help() const override {
        return "get version #, etc.\n"
               "{ buildinfo:1 }";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        VersionInfoInterface::instance().appendBuildInfo(&result);
        appendStorageEngineList(opCtx->getServiceContext(), &result);
        return true;
    }

    Future<void> runAsync(std::shared_ptr<RequestExecutionContext> rec, std::string) override {
        auto opCtx = rec->getOpCtx();
        return BuildInfoExecutor::get(opCtx->getServiceContext())->schedule(std::move(rec));
    }

} cmdBuildInfo;

}  // namespace
}  // namespace mongo
