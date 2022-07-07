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

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);
const char kLegacyRuntimeConstantsField[] = "runtimeConstants";

BSONObj appendLegacyRuntimeConstantsToCommandObject(OperationContext* opCtx,
                                                    const BSONObj& origCmdObj) {
    uassert(51196,
            "Cannot specify runtime constants option to a mongos",
            !origCmdObj.getField(kLegacyRuntimeConstantsField));
    auto rtcBSON =
        BSON(kLegacyRuntimeConstantsField << Variables::generateRuntimeConstants(opCtx).toBSON());
    return origCmdObj.addField(rtcBSON.getField(kLegacyRuntimeConstantsField));
}

BSONObj getCollation(const BSONObj& cmdObj) {
    BSONElement collationElement;
    auto status = bsonExtractTypedField(cmdObj, "collation", BSONType::Object, &collationElement);
    if (status.isOK()) {
        return collationElement.Obj();
    } else if (status != ErrorCodes::NoSuchKey) {
        uassertStatusOK(status);
    }

    return BSONObj();
}

boost::optional<BSONObj> getLet(const BSONObj& cmdObj) {
    if (auto letElem = cmdObj.getField("let"_sd); letElem.type() == BSONType::Object) {
        auto bob = BSONObjBuilder();
        bob.appendElementsUnique(letElem.embeddedObject());
        return bob.obj();
    }
    return boost::none;
}

boost::optional<LegacyRuntimeConstants> getLegacyRuntimeConstants(const BSONObj& cmdObj) {
    if (auto rcElem = cmdObj.getField("runtimeConstants"_sd); rcElem.type() == BSONType::Object) {
        IDLParserErrorContext ctx("internalLegacyRuntimeConstants");
        return LegacyRuntimeConstants::parse(ctx, rcElem.embeddedObject());
    }
    return boost::none;
}

BSONObj getShardKey(OperationContext* opCtx,
                    const ChunkManager& chunkMgr,
                    const NamespaceString& nss,
                    const BSONObj& query,
                    const BSONObj& collation,
                    const boost::optional<ExplainOptions::Verbosity> verbosity,
                    const boost::optional<BSONObj>& let,
                    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {
    auto expCtx = makeExpressionContextWithDefaultsForTargeter(
        opCtx, nss, collation, verbosity, let, runtimeConstants);

    BSONObj shardKey =
        uassertStatusOK(chunkMgr.getShardKeyPattern().extractShardKeyFromQuery(expCtx, query));
    uassert(ErrorCodes::ShardKeyNotFound,
            "Query for sharded findAndModify must contain the shard key",
            !shardKey.isEmpty());
    return shardKey;
}

void updateShardKeyValueOnWouldChangeOwningShardError(OperationContext* opCtx,
                                                      const NamespaceString nss,
                                                      Status responseStatus,
                                                      const BSONObj& cmdObj,
                                                      BSONObjBuilder* result) {
    BSONObjBuilder extraInfoBuilder;
    responseStatus.extraInfo()->serialize(&extraInfoBuilder);
    auto extraInfo = extraInfoBuilder.obj();
    auto wouldChangeOwningShardExtraInfo =
        WouldChangeOwningShardInfo::parseFromCommandError(extraInfo);

    try {
        auto matchedDocOrUpserted = documentShardKeyUpdateUtil::updateShardKeyForDocument(
            opCtx, nss, wouldChangeOwningShardExtraInfo);
        auto upserted = matchedDocOrUpserted && wouldChangeOwningShardExtraInfo.getShouldUpsert();
        auto updatedExistingDocument = matchedDocOrUpserted && !upserted;

        BSONObjBuilder lastErrorObjBuilder(result->subobjStart("lastErrorObject"));
        lastErrorObjBuilder.appendNumber("n", matchedDocOrUpserted ? 1 : 0);
        lastErrorObjBuilder.appendBool("updatedExisting", updatedExistingDocument);
        if (upserted) {
            lastErrorObjBuilder.appendAs(wouldChangeOwningShardExtraInfo.getPostImage()["_id"],
                                         "upserted");
        }
        lastErrorObjBuilder.doneFast();

        auto shouldReturnPostImage = cmdObj.getBoolField("new");
        if (updatedExistingDocument) {
            result->append("value",
                           shouldReturnPostImage ? wouldChangeOwningShardExtraInfo.getPostImage()
                                                 : wouldChangeOwningShardExtraInfo.getPreImage());
        } else if (upserted && shouldReturnPostImage) {
            result->append("value", wouldChangeOwningShardExtraInfo.getPostImage());
        } else {
            result->appendNull("value");
        }
        result->append("ok", 1.0);
    } catch (DBException& e) {
        if (e.code() == ErrorCodes::DuplicateKey &&
            e.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id")) {
            e.addContext(documentShardKeyUpdateUtil::kDuplicateKeyErrorContext);
        }
        e.addContext("findAndModify");
        throw;
    }
}

class FindAndModifyCmd : public BasicCommand {
public:
    FindAndModifyCmd()
        : BasicCommand("findAndModify", "findandmodify"), _updateMetrics{"findAndModify"} {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        return {{level != repl::ReadConcernLevel::kLocalReadConcern &&
                     level != repl::ReadConcernLevel::kSnapshotReadConcern,
                 {ErrorCodes::InvalidOptions, "read concern not supported"}},
                {{ErrorCodes::InvalidOptions, "default read concern not permitted"}}};
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        bool update = cmdObj["update"].trueValue();
        bool upsert = cmdObj["upsert"].trueValue();
        bool remove = cmdObj["remove"].trueValue();

        ActionSet actions;
        actions.addAction(ActionType::find);
        if (update) {
            actions.addAction(ActionType::update);
        }
        if (upsert) {
            actions.addAction(ActionType::insert);
        }
        if (remove) {
            actions.addAction(ActionType::remove);
        }
        if (shouldBypassDocumentValidationForCommand(cmdObj)) {
            actions.addAction(ActionType::bypassDocumentValidation);
        }

        std::string ns = CommandHelpers::parseNsFromCommand(dbname, cmdObj);
        ResourcePattern resource(CommandHelpers::resourcePatternForNamespace(ns));
        uassert(17137,
                "Invalid target namespace " + resource.toString(),
                resource.isExactNamespacePattern());
        out->push_back(Privilege(resource, actions));
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        std::string dbName = request.getDatabase().toString();
        const BSONObj& cmdObj = request.body;
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

        const auto cm =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        std::shared_ptr<Shard> shard;
        if (cm.isSharded()) {
            const BSONObj query = cmdObj.getObjectField("query");
            const BSONObj collation = getCollation(cmdObj);
            const auto let = getLet(cmdObj);
            const auto rc = getLegacyRuntimeConstants(cmdObj);
            const BSONObj shardKey =
                getShardKey(opCtx, cm, nss, query, collation, verbosity, let, rc);
            const auto chunk = cm.findIntersectingChunk(shardKey, collation);

            shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getShardId()));
        } else {
            shard =
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cm.dbPrimary()));
        }

        const auto explainCmd = ClusterExplain::wrapAsExplain(
            appendLegacyRuntimeConstantsToCommandObject(opCtx, cmdObj), verbosity);

        // Time how long it takes to run the explain command on the shard.
        Timer timer;
        BSONObjBuilder bob;

        if (cm.isSharded()) {
            _runCommand(opCtx,
                        shard->getId(),
                        cm.getVersion(shard->getId()),
                        boost::none,
                        nss,
                        applyReadWriteConcern(opCtx, false, false, explainCmd),
                        &bob);
        } else {
            _runCommand(opCtx,
                        shard->getId(),
                        boost::make_optional(!cm.dbVersion().isFixed(), ChunkVersion::UNSHARDED()),
                        cm.dbVersion(),
                        nss,
                        applyReadWriteConcern(opCtx, false, false, explainCmd),
                        &bob);
        }

        const auto millisElapsed = timer.millis();

        executor::RemoteCommandResponse response(bob.obj(), Milliseconds(millisElapsed));

        // We fetch an arbitrary host from the ConnectionString, since
        // ClusterExplain::buildExplainResult() doesn't use the given HostAndPort.
        AsyncRequestsSender::Response arsResponse{
            shard->getId(), response, shard->getConnString().getServers().front()};

        auto bodyBuilder = result->getBodyBuilder();
        return ClusterExplain::buildExplainResult(opCtx,
                                                  {arsResponse},
                                                  ClusterExplain::kSingleShard,
                                                  millisElapsed,
                                                  cmdObj,
                                                  &bodyBuilder);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

        // Collect metrics.
        _updateMetrics.collectMetrics(cmdObj);

        // Technically, findAndModify should only be creating database if upsert is true, but this
        // would require that the parsing be pulled into this function.
        cluster::createDatabase(opCtx, nss.db());

        // Append mongoS' runtime constants to the command object before forwarding it to the shard.
        auto cmdObjForShard = appendLegacyRuntimeConstantsToCommandObject(opCtx, cmdObj);

        const auto cm = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
        if (cm.isSharded()) {
            const BSONObj query = cmdObjForShard.getObjectField("query");
            const BSONObj collation = getCollation(cmdObjForShard);
            const auto let = getLet(cmdObjForShard);
            const auto rc = getLegacyRuntimeConstants(cmdObjForShard);
            const BSONObj shardKey =
                getShardKey(opCtx, cm, nss, query, collation, boost::none, let, rc);

            auto chunk = cm.findIntersectingChunk(shardKey, collation);

            _runCommand(opCtx,
                        chunk.getShardId(),
                        cm.getVersion(chunk.getShardId()),
                        boost::none,
                        nss,
                        applyReadWriteConcern(opCtx, this, cmdObjForShard),
                        &result);
        } else {
            _runCommand(opCtx,
                        cm.dbPrimary(),
                        boost::make_optional(!cm.dbVersion().isFixed(), ChunkVersion::UNSHARDED()),
                        cm.dbVersion(),
                        nss,
                        applyReadWriteConcern(opCtx, this, cmdObjForShard),
                        &result);
        }

        return true;
    }

private:
    static void _runCommand(OperationContext* opCtx,
                            const ShardId& shardId,
                            const boost::optional<ChunkVersion>& shardVersion,
                            const boost::optional<DatabaseVersion>& dbVersion,
                            const NamespaceString& nss,
                            const BSONObj& cmdObj,
                            BSONObjBuilder* result) {
        bool isRetryableWrite = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);
        const auto response = [&] {
            std::vector<AsyncRequestsSender::Request> requests;
            BSONObj filteredCmdObj = CommandHelpers::filterCommandRequestForPassthrough(cmdObj);
            BSONObj cmdObjWithVersions(std::move(filteredCmdObj));
            if (dbVersion) {
                cmdObjWithVersions = appendDbVersionIfPresent(cmdObjWithVersions, *dbVersion);
            }
            if (shardVersion) {
                cmdObjWithVersions = appendShardVersion(cmdObjWithVersions, *shardVersion);
            }
            requests.emplace_back(shardId, cmdObjWithVersions);

            MultiStatementTransactionRequestsSender ars(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                nss.db().toString(),
                requests,
                kPrimaryOnlyReadPreference,
                isRetryableWrite ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);

            auto response = ars.next();
            invariant(ars.done());

            return uassertStatusOK(std::move(response.swResponse));
        }();

        uassertStatusOK(response.status);

        const auto responseStatus = getStatusFromCommandResult(response.data);
        if (ErrorCodes::isNeedRetargettingError(responseStatus.code()) ||
            ErrorCodes::isSnapshotError(responseStatus.code()) ||
            responseStatus.code() == ErrorCodes::StaleDbVersion) {
            // Command code traps this exception and re-runs
            uassertStatusOK(responseStatus.withContext("findAndModify"));
        }

        if (responseStatus.code() == ErrorCodes::WouldChangeOwningShard) {
            if (isRetryableWrite) {
                RouterOperationContextSession routerSession(opCtx);
                try {
                    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
                    readConcernArgs =
                        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                    // Re-run the findAndModify command that will change the shard key value in a
                    // transaction. We call _runCommand recursively, and this second time through
                    // since it will be run as a transaction it will take the other code path to
                    // updateShardKeyValueOnWouldChangeOwningShardError.  We ensure the retried
                    // operation does not include WC inside the transaction by stripping it from the
                    // cmdObj.  The transaction commit will still use the WC, because it uses the WC
                    // from the opCtx (which has been set previously in Strategy).
                    documentShardKeyUpdateUtil::startTransactionForShardKeyUpdate(opCtx);
                    _runCommand(opCtx,
                                shardId,
                                shardVersion,
                                dbVersion,
                                nss,
                                stripWriteConcern(cmdObj),
                                result);
                    uassertStatusOK(getStatusFromCommandResult(result->asTempObj()));
                    auto commitResponse =
                        documentShardKeyUpdateUtil::commitShardKeyUpdateTransaction(opCtx);

                    uassertStatusOK(getStatusFromCommandResult(commitResponse));
                    if (auto wcErrorElem = commitResponse["writeConcernError"]) {
                        appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
                    }
                } catch (DBException& e) {
                    if (e.code() != ErrorCodes::DuplicateKey ||
                        (e.code() == ErrorCodes::DuplicateKey &&
                         !e.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id"))) {
                        e.addContext(documentShardKeyUpdateUtil::kNonDuplicateKeyErrorContext);
                    };

                    auto txnRouterForAbort = TransactionRouter::get(opCtx);
                    if (txnRouterForAbort)
                        txnRouterForAbort.implicitlyAbortTransaction(opCtx, e.toStatus());

                    throw;
                }
            } else {
                updateShardKeyValueOnWouldChangeOwningShardError(
                    opCtx, nss, responseStatus, cmdObj, result);
            }

            return;
        }

        // First append the properly constructed writeConcernError. It will then be skipped in
        // appendElementsUnique.
        if (auto wcErrorElem = response.data["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
        }

        result->appendElementsUnique(
            CommandHelpers::filterCommandReplyForPassthrough(response.data));
    }

    // Update related command execution metrics.
    UpdateMetrics _updateMetrics;
} findAndModifyCmd;

}  // namespace
}  // namespace mongo
