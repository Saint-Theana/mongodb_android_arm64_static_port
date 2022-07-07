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

#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeFindAndModifyPerformsUpdate);

/**
 * If the operation succeeded, then returns either a document to return to the client, or
 * boost::none if no matching document to update/remove was found. If the operation failed, throws.
 */
boost::optional<BSONObj> advanceExecutor(OperationContext* opCtx,
                                         const write_ops::FindAndModifyCommandRequest& request,
                                         PlanExecutor* exec,
                                         bool isRemove) {
    BSONObj value;
    PlanExecutor::ExecState state;
    try {
        state = exec->getNext(&value, nullptr);
    } catch (DBException& exception) {
        auto&& explainer = exec->getPlanExplainer();
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        LOGV2_WARNING(
            23802,
            "Plan executor error during findAndModify: {error}, stats: {stats}, cmd: {cmd}",
            "Plan executor error during findAndModify",
            "error"_attr = exception.toStatus(),
            "stats"_attr = redact(stats),
            "cmd"_attr = request.toBSON(BSONObj() /* commandPassthroughFields */));

        exception.addContext("Plan executor error during findAndModify");
        throw;
    }

    if (PlanExecutor::ADVANCED == state) {
        return {std::move(value)};
    }

    invariant(state == PlanExecutor::IS_EOF);
    return boost::none;
}

void validate(const write_ops::FindAndModifyCommandRequest& request) {
    uassert(ErrorCodes::FailedToParse,
            "Either an update or remove=true must be specified",
            request.getRemove().value_or(false) || request.getUpdate());
    if (request.getRemove().value_or(false)) {
        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both an update and remove=true",
                !request.getUpdate());

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both upsert=true and remove=true ",
                !request.getUpsert() || !*request.getUpsert());

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both new=true and remove=true; 'remove' always returns the deleted "
                "document",
                !request.getNew() || !*request.getNew());

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify arrayFilters and remove=true",
                !request.getArrayFilters());
    }

    if (request.getUpdate() &&
        request.getUpdate()->type() == write_ops::UpdateModification::Type::kPipeline &&
        request.getArrayFilters()) {
        uasserted(ErrorCodes::FailedToParse, "Cannot specify arrayFilters and a pipeline update");
    }
}

void makeUpdateRequest(OperationContext* opCtx,
                       const write_ops::FindAndModifyCommandRequest& request,
                       boost::optional<ExplainOptions::Verbosity> explain,
                       UpdateRequest* requestOut) {
    requestOut->setQuery(request.getQuery());
    requestOut->setProj(request.getFields().value_or(BSONObj()));
    invariant(request.getUpdate());
    requestOut->setUpdateModification(*request.getUpdate());
    requestOut->setLegacyRuntimeConstants(
        request.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    requestOut->setLetParameters(request.getLet());
    requestOut->setSort(request.getSort().value_or(BSONObj()));
    requestOut->setHint(request.getHint());
    requestOut->setCollation(request.getCollation().value_or(BSONObj()));
    requestOut->setArrayFilters(request.getArrayFilters().value_or(std::vector<BSONObj>()));
    requestOut->setUpsert(request.getUpsert().value_or(false));
    requestOut->setReturnDocs((request.getNew().value_or(false)) ? UpdateRequest::RETURN_NEW
                                                                 : UpdateRequest::RETURN_OLD);
    requestOut->setMulti(false);
    requestOut->setExplain(explain);

    requestOut->setYieldPolicy(opCtx->inMultiDocumentTransaction()
                                   ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                                   : PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
}

void makeDeleteRequest(OperationContext* opCtx,
                       const write_ops::FindAndModifyCommandRequest& request,
                       bool explain,
                       DeleteRequest* requestOut) {
    requestOut->setQuery(request.getQuery());
    requestOut->setProj(request.getFields().value_or(BSONObj()));
    requestOut->setLegacyRuntimeConstants(
        request.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    requestOut->setLet(request.getLet());
    requestOut->setSort(request.getSort().value_or(BSONObj()));
    requestOut->setHint(request.getHint());
    requestOut->setCollation(request.getCollation().value_or(BSONObj()));
    requestOut->setMulti(false);
    requestOut->setReturnDeleted(true);  // Always return the old value.
    requestOut->setIsExplain(explain);

    requestOut->setYieldPolicy(opCtx->inMultiDocumentTransaction()
                                   ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                                   : PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
}

write_ops::FindAndModifyCommandReply buildResponse(const PlanExecutor* exec,
                                                   bool isRemove,
                                                   const boost::optional<BSONObj>& value) {
    write_ops::FindAndModifyLastError lastError;
    if (isRemove) {
        lastError.setNumDocs(value ? 1 : 0);
    } else {
        const auto updateResult = exec->getUpdateResult();
        lastError.setNumDocs(!updateResult.upsertedId.isEmpty() ? 1 : updateResult.numMatched);
        lastError.setUpdatedExisting(updateResult.numMatched > 0);

        // Note we have to use the upsertedId from the update result here, rather than 'value'
        // because the _id field could have been excluded by a projection.
        if (!updateResult.upsertedId.isEmpty()) {
            lastError.setUpserted(IDLAnyTypeOwned(updateResult.upsertedId.firstElement()));
        }
    }

    write_ops::FindAndModifyCommandReply result;
    result.setLastErrorObject(std::move(lastError));
    result.setValue(value);
    return result;
}

void assertCanWrite_inlock(OperationContext* opCtx, const NamespaceString& nsString) {
    uassert(ErrorCodes::NotWritablePrimary,
            str::stream() << "Not primary while running findAndModify command on collection "
                          << nsString.ns(),
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nsString));

    CollectionShardingState::get(opCtx, nsString)->checkShardVersionOrThrow(opCtx);
}

void recordStatsForTopCommand(OperationContext* opCtx) {
    auto curOp = CurOp::get(opCtx);
    Top::get(opCtx->getClient()->getServiceContext())
        .record(opCtx,
                curOp->getNS(),
                curOp->getLogicalOp(),
                Top::LockType::WriteLocked,
                durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                curOp->isCommand(),
                curOp->getReadWriteType());
}

void checkIfTransactionOnCappedColl(const CollectionPtr& coll, bool inTransaction) {
    if (coll && coll->isCapped()) {
        uassert(
            ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Collection '" << coll->ns()
                          << "' is a capped collection. Writes in transactions are not allowed on "
                             "capped collections.",
            !inTransaction);
    }
}

class CmdFindAndModify : public write_ops::FindAndModifyCmdVersion1Gen<CmdFindAndModify> {
public:
    std::string help() const final {
        return "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: "
               "{processed:true}}, new: true}\n"
               "{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: "
               "{priority:-1}}\n"
               "Either update or remove is required, all other fields have default values.\n"
               "Output is in the \"value\" field\n";
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext* srvContext) const final {
        return Command::AllowedOnSecondary::kNever;
    }

    Command::ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    static void collectMetrics(const Request& request) {
        CmdFindAndModify::_updateMetrics.collectMetrics(request);
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        bool supportsReadMirroring() const final {
            return true;
        }

        NamespaceString ns() const final {
            return this->request().getNamespace();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final;

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) final;

        Reply typedRun(OperationContext* opCtx) final;

        void appendMirrorableRequest(BSONObjBuilder* bob) const final;

    private:
        static write_ops::FindAndModifyCommandReply writeConflictRetryRemove(
            OperationContext* opCtx,
            const NamespaceString& nsString,
            const write_ops::FindAndModifyCommandRequest& request,
            int stmtId,
            CurOp* curOp,
            OpDebug* opDebug,
            bool inTransaction);

        static write_ops::FindAndModifyCommandReply writeConflictRetryUpsert(
            OperationContext* opCtx,
            const NamespaceString& nsString,
            const write_ops::FindAndModifyCommandRequest& request,
            CurOp* curOp,
            OpDebug* opDebug,
            bool inTransaction,
            ParsedUpdate* parsedUpdate);
    };

private:
    // Update related command execution metrics.
    static UpdateMetrics _updateMetrics;
} cmdFindAndModify;

UpdateMetrics CmdFindAndModify::_updateMetrics{"findAndModify"};

write_ops::FindAndModifyCommandReply CmdFindAndModify::Invocation::writeConflictRetryRemove(
    OperationContext* opCtx,
    const NamespaceString& nsString,
    const write_ops::FindAndModifyCommandRequest& request,
    int stmtId,
    CurOp* curOp,
    OpDebug* const opDebug,
    bool inTransaction) {

    auto deleteRequest = DeleteRequest{};
    deleteRequest.setNsString(nsString);
    const bool isExplain = false;
    makeDeleteRequest(opCtx, request, isExplain, &deleteRequest);

    if (opCtx->getTxnNumber()) {
        deleteRequest.setStmtId(stmtId);
    }

    ParsedDelete parsedDelete(opCtx, &deleteRequest);
    uassertStatusOK(parsedDelete.parseRequest());

    AutoGetCollection collection(opCtx, nsString, MODE_IX);

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->enter_inlock(
            nsString.ns().c_str(),
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nsString.db()));
    }

    assertCanWrite_inlock(opCtx, nsString);

    checkIfTransactionOnCappedColl(collection.getCollection(), inTransaction);

    const auto exec = uassertStatusOK(getExecutorDelete(
        opDebug, &collection.getCollection(), &parsedDelete, boost::none /* verbosity */));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    auto docFound =
        advanceExecutor(opCtx, request, exec.get(), request.getRemove().value_or(false));
    // Nothing after advancing the plan executor should throw a WriteConflictException,
    // so the following bookkeeping with execution stats won't end up being done
    // multiple times.

    PlanSummaryStats summaryStats;
    exec->getPlanExplainer().getSummaryStats(&summaryStats);
    if (const auto& coll = collection.getCollection()) {
        CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, summaryStats);
    }
    opDebug->setPlanSummaryMetrics(summaryStats);

    // Fill out OpDebug with the number of deleted docs.
    opDebug->additiveMetrics.ndeleted = docFound ? 1 : 0;

    if (curOp->shouldDBProfile(opCtx)) {
        auto&& explainer = exec->getPlanExplainer();
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
    }
    recordStatsForTopCommand(opCtx);

    if (docFound) {
        ResourceConsumption::DocumentUnitCounter docUnitsReturned;
        docUnitsReturned.observeOne(docFound->objsize());

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
        metricsCollector.incrementDocUnitsReturned(docUnitsReturned);
    }

    return buildResponse(exec.get(), request.getRemove().value_or(false), docFound);
}

write_ops::FindAndModifyCommandReply CmdFindAndModify::Invocation::writeConflictRetryUpsert(
    OperationContext* opCtx,
    const NamespaceString& nsString,
    const write_ops::FindAndModifyCommandRequest& request,
    CurOp* curOp,
    OpDebug* opDebug,
    bool inTransaction,
    ParsedUpdate* parsedUpdate) {

    AutoGetCollection autoColl(opCtx, nsString, MODE_IX);
    Database* db = autoColl.ensureDbExists();

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->enter_inlock(
            nsString.ns().c_str(),
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nsString.db()));
    }

    assertCanWrite_inlock(opCtx, nsString);

    CollectionPtr createdCollection;
    const CollectionPtr* collectionPtr = &autoColl.getCollection();

    // TODO SERVER-50983: Create abstraction for creating collection when using
    // AutoGetCollection Create the collection if it does not exist when performing an upsert
    // because the update stage does not create its own collection
    if (!*collectionPtr && request.getUpsert() && *request.getUpsert()) {
        assertCanWrite_inlock(opCtx, nsString);

        createdCollection =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nsString);

        // If someone else beat us to creating the collection, do nothing
        if (!createdCollection) {
            uassertStatusOK(userAllowedCreateNS(opCtx, nsString));
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions defaultCollectionOptions;
            uassertStatusOK(db->userCreateNS(opCtx, nsString, defaultCollectionOptions));
            wuow.commit();

            createdCollection =
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nsString);
        }

        invariant(createdCollection);
        collectionPtr = &createdCollection;
    }
    const auto& collection = *collectionPtr;

    checkIfTransactionOnCappedColl(collection, inTransaction);

    const auto exec = uassertStatusOK(
        getExecutorUpdate(opDebug, &collection, parsedUpdate, boost::none /* verbosity */));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    auto docFound =
        advanceExecutor(opCtx, request, exec.get(), request.getRemove().value_or(false));
    // Nothing after advancing the plan executor should throw a WriteConflictException,
    // so the following bookkeeping with execution stats won't end up being done
    // multiple times.

    PlanSummaryStats summaryStats;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summaryStats);
    if (collection) {
        CollectionQueryInfo::get(collection).notifyOfQuery(opCtx, collection, summaryStats);
    }
    auto updateResult = exec->getUpdateResult();
    write_ops_exec::recordUpdateResultInOpDebug(updateResult, opDebug);
    opDebug->setPlanSummaryMetrics(summaryStats);

    if (feature_flags::gFeatureFlagDotsAndDollars.isEnabledAndIgnoreFCV() &&
        updateResult.containsDotsAndDollarsField) {
        // If it's an upsert, increment 'inserts' metric, otherwise increment 'updates'.
        dotsAndDollarsFieldsCounters.incrementForUpsert(!updateResult.upsertedId.isEmpty());
    }

    if (curOp->shouldDBProfile(opCtx)) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
    }
    recordStatsForTopCommand(opCtx);

    if (docFound) {
        ResourceConsumption::DocumentUnitCounter docUnitsReturned;
        docUnitsReturned.observeOne(docFound->objsize());

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
        metricsCollector.incrementDocUnitsReturned(docUnitsReturned);
    }

    return buildResponse(exec.get(), request.getRemove().value_or(false), docFound);
}

void CmdFindAndModify::Invocation::doCheckAuthorization(OperationContext* opCtx) const {
    std::vector<Privilege> privileges;
    const auto& request = this->request();

    ActionSet actions;
    actions.addAction(ActionType::find);

    if (request.getUpdate()) {
        actions.addAction(ActionType::update);
    }
    if (request.getUpsert().value_or(false)) {
        actions.addAction(ActionType::insert);
    }
    if (request.getRemove().value_or(false)) {
        actions.addAction(ActionType::remove);
    }
    if (request.getBypassDocumentValidation().value_or(false)) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    ResourcePattern resource(
        CommandHelpers::resourcePatternForNamespace(request.getNamespace().toString()));
    uassert(17138,
            "Invalid target namespace " + resource.toString(),
            resource.isExactNamespacePattern());
    privileges.push_back(Privilege(resource, actions));

    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to find and modify on database'"
                          << this->request().getDbName() << "'",
            AuthorizationSession::get(opCtx->getClient())->isAuthorizedForPrivileges(privileges));
}

void CmdFindAndModify::Invocation::explain(OperationContext* opCtx,
                                           ExplainOptions::Verbosity verbosity,
                                           rpc::ReplyBuilderInterface* result) {

    const BSONObj& cmdObj = this->request().toBSON(BSONObj() /* commandPassthroughFields */);
    validate(this->request());

    const NamespaceString& nsString = this->request().getNamespace();
    uassertStatusOK(userAllowedWriteNS(opCtx, nsString));
    auto const curOp = CurOp::get(opCtx);
    OpDebug* const opDebug = &curOp->debug();
    const std::string dbName = this->request().getDbName().toString();

    if (this->request().getRemove().value_or(false)) {
        auto deleteRequest = DeleteRequest{};
        deleteRequest.setNsString(nsString);
        const bool isExplain = true;
        makeDeleteRequest(opCtx, this->request(), isExplain, &deleteRequest);

        ParsedDelete parsedDelete(opCtx, &deleteRequest);
        uassertStatusOK(parsedDelete.parseRequest());

        // Explain calls of the findAndModify command are read-only, but we take write
        // locks so that the timing information is more accurate.
        AutoGetCollection collection(opCtx, nsString, MODE_IX);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName << " does not exist",
                collection.getDb());

        CollectionShardingState::get(opCtx, nsString)->checkShardVersionOrThrow(opCtx);

        const auto exec = uassertStatusOK(
            getExecutorDelete(opDebug, &collection.getCollection(), &parsedDelete, verbosity));

        auto bodyBuilder = result->getBodyBuilder();
        Explain::explainStages(
            exec.get(), collection.getCollection(), verbosity, BSONObj(), cmdObj, &bodyBuilder);
    } else {
        auto updateRequest = UpdateRequest();
        updateRequest.setNamespaceString(nsString);
        makeUpdateRequest(opCtx, this->request(), verbosity, &updateRequest);

        const ExtensionsCallbackReal extensionsCallback(opCtx, &updateRequest.getNamespaceString());
        ParsedUpdate parsedUpdate(opCtx, &updateRequest, extensionsCallback);
        uassertStatusOK(parsedUpdate.parseRequest());

        // Explain calls of the findAndModify command are read-only, but we take write
        // locks so that the timing information is more accurate.
        AutoGetCollection collection(opCtx, nsString, MODE_IX);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName << " does not exist",
                collection.getDb());

        CollectionShardingState::get(opCtx, nsString)->checkShardVersionOrThrow(opCtx);

        const auto exec = uassertStatusOK(
            getExecutorUpdate(opDebug, &collection.getCollection(), &parsedUpdate, verbosity));

        auto bodyBuilder = result->getBodyBuilder();
        Explain::explainStages(
            exec.get(), collection.getCollection(), verbosity, BSONObj(), cmdObj, &bodyBuilder);
    }
}

write_ops::FindAndModifyCommandReply CmdFindAndModify::Invocation::typedRun(
    OperationContext* opCtx) {
    const auto& req = request();

    validate(req);

    const NamespaceString& nsString = req.getNamespace();
    uassertStatusOK(userAllowedWriteNS(opCtx, nsString));
    auto const curOp = CurOp::get(opCtx);
    OpDebug* const opDebug = &curOp->debug();

    // Collect metrics.
    CmdFindAndModify::collectMetrics(req);

    boost::optional<DisableDocumentValidation> maybeDisableValidation;
    if (req.getBypassDocumentValidation().value_or(false)) {
        maybeDisableValidation.emplace(opCtx);
    }

    const auto inTransaction = opCtx->inMultiDocumentTransaction();
    uassert(50781,
            str::stream() << "Cannot write to system collection " << nsString.ns()
                          << " within a transaction.",
            !(inTransaction && nsString.isSystem()));

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    uassert(50777,
            str::stream() << "Cannot write to unreplicated collection " << nsString.ns()
                          << " within a transaction.",
            !(inTransaction && replCoord->isOplogDisabledFor(opCtx, nsString)));


    const auto stmtId = 0;
    if (opCtx->getTxnNumber() && !inTransaction) {
        const auto txnParticipant = TransactionParticipant::get(opCtx);
        if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
            RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
            auto findAndModifyReply = parseOplogEntryForFindAndModify(opCtx, req, *entry);

            // Make sure to wait for writeConcern on the opTime that will include this
            // write. Needs to set to the system last opTime to get the latest term in an
            // event when an election happened after the actual write.
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            replClient.setLastOpToSystemLastOpTime(opCtx);

            return findAndModifyReply;
        }
    }

    // Although usually the PlanExecutor handles WCE internally, it will throw WCEs when it
    // is executing a findAndModify. This is done to ensure that we can always match,
    // modify, and return the document under concurrency, if a matching document exists.
    return writeConflictRetry(opCtx, "findAndModify", nsString.ns(), [&] {
        if (req.getRemove().value_or(false)) {
            return CmdFindAndModify::Invocation::writeConflictRetryRemove(
                opCtx, nsString, req, stmtId, curOp, opDebug, inTransaction);
        } else {
            if (MONGO_unlikely(hangBeforeFindAndModifyPerformsUpdate.shouldFail())) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeFindAndModifyPerformsUpdate,
                    opCtx,
                    "hangBeforeFindAndModifyPerformsUpdate");
            }

            // Nested retry loop to handle concurrent conflicting upserts with equality
            // match.
            int retryAttempts = 0;
            for (;;) {
                auto updateRequest = UpdateRequest();
                updateRequest.setNamespaceString(nsString);
                const auto verbosity = boost::none;
                makeUpdateRequest(opCtx, req, verbosity, &updateRequest);

                if (opCtx->getTxnNumber()) {
                    updateRequest.setStmtIds({stmtId});
                }

                const ExtensionsCallbackReal extensionsCallback(
                    opCtx, &updateRequest.getNamespaceString());
                ParsedUpdate parsedUpdate(opCtx, &updateRequest, extensionsCallback);
                uassertStatusOK(parsedUpdate.parseRequest());

                try {
                    return CmdFindAndModify::Invocation::writeConflictRetryUpsert(
                        opCtx, nsString, req, curOp, opDebug, inTransaction, &parsedUpdate);
                } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
                    if (!parsedUpdate.hasParsedQuery()) {
                        uassertStatusOK(parsedUpdate.parseQueryToCQ());
                    }

                    if (!write_ops_exec::shouldRetryDuplicateKeyException(
                            parsedUpdate, *ex.extraInfo<DuplicateKeyErrorInfo>())) {
                        throw;
                    }

                    ++retryAttempts;
                    logAndBackoff(4721200,
                                  ::mongo::logv2::LogComponent::kWrite,
                                  logv2::LogSeverity::Debug(1),
                                  retryAttempts,
                                  "Caught DuplicateKey exception during findAndModify upsert",
                                  "namespace"_attr = nsString.ns());
                }
            }
        }
    });
}

void CmdFindAndModify::Invocation::appendMirrorableRequest(BSONObjBuilder* bob) const {
    const auto& req = request();

    bob->append(FindCommandRequest::kCommandName, req.getNamespace().coll());

    if (!req.getQuery().isEmpty()) {
        bob->append(FindCommandRequest::kFilterFieldName, req.getQuery());
    }
    if (req.getSort()) {
        bob->append(write_ops::FindAndModifyCommandRequest::kSortFieldName, *req.getSort());
    }
    if (req.getCollation()) {
        bob->append(write_ops::FindAndModifyCommandRequest::kCollationFieldName,
                    *req.getCollation());
    }

    // Prevent the find from returning multiple documents since we can
    bob->append("batchSize", 1);
    bob->append("singleBatch", true);
}

}  // namespace
}  // namespace mongo
