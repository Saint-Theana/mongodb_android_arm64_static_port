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

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/stats/server_read_concern_metrics.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {
namespace {

const auto kTermField = "term"_sd;

// Parses the command object to a FindCommandRequest. If the client request did not specify any
// runtime constants, make them available to the query here.
std::unique_ptr<FindCommandRequest> parseCmdObjectToFindCommandRequest(OperationContext* opCtx,
                                                                       NamespaceString nss,
                                                                       BSONObj cmdObj) {
    auto findCommand = query_request_helper::makeFromFindCommand(
        std::move(cmdObj),
        std::move(nss),
        APIParameters::get(opCtx).getAPIStrict().value_or(false));
    return findCommand;
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const FindCommandRequest& findCommand,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    std::unique_ptr<CollatorInterface> collator;
    if (!findCommand.getCollation().isEmpty()) {
        collator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                       ->makeFromBSON(findCommand.getCollation()));
    }

    // Although both 'find' and 'aggregate' commands have an ExpressionContext, some of the data
    // members in the ExpressionContext are used exclusively by the aggregation subsystem. This
    // includes the following fields which here we simply initialize to some meaningless default
    // value:
    //  - explain
    //  - fromMongos
    //  - needsMerge
    //  - bypassDocumentValidation
    //  - mongoProcessInterface
    //  - resolvedNamespaces
    //  - uuid
    //
    // As we change the code to make the find and agg systems more tightly coupled, it would make
    // sense to start initializing these fields for find operations as well.
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx,
        verbosity,
        false,  // fromMongos
        false,  // needsMerge
        findCommand.getAllowDiskUse(),
        false,  // bypassDocumentValidation
        false,  // isMapReduceCommand
        findCommand.getNamespaceOrUUID().nss().value_or(NamespaceString()),
        findCommand.getLegacyRuntimeConstants(),
        std::move(collator),
        nullptr,  // mongoProcessInterface
        StringMap<ExpressionContext::ResolvedNamespace>{},
        boost::none,                             // uuid
        findCommand.getLet(),                    // let
        CurOp::get(opCtx)->dbProfileLevel() > 0  // mayDbProfile
    );
    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    return expCtx;
}

/**
 * A command for running .find() queries.
 */
class FindCmd final : public Command {
public:
    FindCmd() : Command("find") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        // TODO: Parse into a QueryRequest here.
        return std::make_unique<Invocation>(this, opMsgRequest, opMsgRequest.getDatabase());
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "query for documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opQuery;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    /**
     * A find command does not increment the command counter, but rather increments the
     * query counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    bool shouldAffectReadConcernCounter() const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(const FindCmd* definition, const OpMsgRequest& request, StringData dbName)
            : CommandInvocation(definition), _request(request), _dbName(dbName) {
            invariant(_request.body.isOwned());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        }

        bool supportsReadMirroring() const override {
            return true;
        }

        bool canIgnorePrepareConflicts() const override {
            return true;
        }

        bool allowsSpeculativeMajorityReads() const override {
            // Find queries are only allowed to use speculative behavior if the 'allowsSpeculative'
            // flag is passed. The find command will check for this flag internally and fail if
            // necessary.
            return true;
        }

        NamespaceString ns() const override {
            // TODO get the ns from the parsed QueryRequest.
            return NamespaceString(CommandHelpers::parseNsFromCommand(_dbName, _request.body));
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    authSession->isAuthorizedToParseNamespaceElement(_request.body.firstElement()));

            const auto hasTerm = _request.body.hasField(kTermField);
            uassertStatusOK(auth::checkAuthForFind(
                authSession,
                CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(
                    opCtx, CommandHelpers::parseNsOrUUID(_dbName, _request.body)),
                hasTerm));
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            // Acquire locks. The RAII object is optional, because in the case of a view, the locks
            // need to be released.
            boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> ctx;
            ctx.emplace(opCtx,
                        CommandHelpers::parseNsCollectionRequired(_dbName, _request.body),
                        AutoGetCollectionViewMode::kViewsPermitted);
            const auto nss = ctx->getNss();

            // Parse the command BSON to a FindCommandRequest.
            auto findCommand = parseCmdObjectToFindCommandRequest(opCtx, nss, _request.body);

            // Finish the parsing step by using the FindCommandRequest to create a CanonicalQuery.
            const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
            auto expCtx = makeExpressionContext(opCtx, *findCommand, verbosity);
            const bool isExplain = true;
            auto cq = uassertStatusOK(
                CanonicalQuery::canonicalize(opCtx,
                                             std::move(findCommand),
                                             isExplain,
                                             std::move(expCtx),
                                             extensionsCallback,
                                             MatchExpressionParser::kAllowAllSpecialFeatures));

            if (ctx->getView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                ctx.reset();

                // Convert the find command into an aggregation using $match (and other stages, as
                // necessary), if possible.
                const auto& findCommand = cq->getFindCommandRequest();
                auto viewAggregationCommand =
                    uassertStatusOK(query_request_helper::asAggregationCommand(findCommand));

                auto viewAggCmd = OpMsgRequest::fromDBAndBody(_dbName, viewAggregationCommand).body;
                // Create the agg request equivalent of the find operation, with the explain
                // verbosity included.
                auto aggRequest = aggregation_request_helper::parseFromBSON(
                    nss,
                    viewAggCmd,
                    verbosity,
                    APIParameters::get(opCtx).getAPIStrict().value_or(false));

                try {
                    // An empty PrivilegeVector is acceptable because these privileges are only
                    // checked on getMore and explain will not open a cursor.
                    uassertStatusOK(runAggregate(
                        opCtx, nss, aggRequest, viewAggCmd, PrivilegeVector(), result));
                } catch (DBException& error) {
                    if (error.code() == ErrorCodes::InvalidPipelineOperator) {
                        uasserted(ErrorCodes::InvalidPipelineOperator,
                                  str::stream()
                                      << "Unsupported in view pipeline: " << error.what());
                    }
                    throw;
                }
                return;
            }

            // The collection may be NULL. If so, getExecutor() should handle it by returning an
            // execution tree with an EOFStage.
            const auto& collection = ctx->getCollection();

            // Get the execution plan for the query.
            bool permitYield = true;
            auto exec =
                uassertStatusOK(getExecutorFind(opCtx, &collection, std::move(cq), permitYield));

            auto bodyBuilder = result->getBodyBuilder();
            // Got the execution tree. Explain it.
            Explain::explainStages(
                exec.get(), collection, verbosity, BSONObj(), _request.body, &bodyBuilder);
        }

        /**
         * Runs a query using the following steps:
         *   --Parsing.
         *   --Acquire locks.
         *   --Plan query, obtaining an executor that can run it.
         *   --Generate the first batch.
         *   --Save state for getMore, transferring ownership of the executor to a ClientCursor.
         *   --Generate response to send to the client.
         */
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            // Although it is a command, a find command gets counted as a query.
            globalOpCounters.gotQuery();

            const BSONObj& cmdObj = _request.body;

            // Parse the command BSON to a FindCommandRequest. Pass in the parsedNss in case cmdObj
            // does not have a UUID.
            auto parsedNss = NamespaceString{CommandHelpers::parseNsFromCommand(_dbName, cmdObj)};
            const bool isExplain = false;
            const bool isOplogNss = (parsedNss == NamespaceString::kRsOplogNamespace);
            auto findCommand =
                parseCmdObjectToFindCommandRequest(opCtx, std::move(parsedNss), cmdObj);

            // Only allow speculative majority for internal commands that specify the correct flag.
            uassert(ErrorCodes::ReadConcernMajorityNotEnabled,
                    "Majority read concern is not enabled.",
                    !(repl::ReadConcernArgs::get(opCtx).isSpeculativeMajority() &&
                      !findCommand->getAllowSpeculativeMajorityRead()));

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "It is illegal to open a tailable cursor in a transaction",
                    !(opCtx->inMultiDocumentTransaction() && findCommand->getTailable()));

            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "The 'readOnce' option is not supported within a transaction.",
                    !txnParticipant || !opCtx->inMultiDocumentTransaction() ||
                        !findCommand->getReadOnce());

            // Validate term before acquiring locks, if provided.
            auto term = findCommand->getTerm();
            if (term) {
                // Note: updateTerm returns ok if term stayed the same.
                uassertStatusOK(replCoord->updateTerm(opCtx, *term));
            }

            // The presence of a term in the request indicates that this is an internal replication
            // oplog read request.
            if (term && isOplogNss) {
                // We do not want to take tickets for internal (replication) oplog reads. Stalling
                // on ticket acquisition can cause complicated deadlocks. Primaries may depend on
                // data reaching secondaries in order to proceed; and secondaries may get stalled
                // replicating because of an inability to acquire a read ticket.
                opCtx->lockState()->skipAcquireTicket();
            }


            // Acquire locks. If the query is on a view, we release our locks and convert the query
            // request into an aggregation command.
            boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> ctx;
            ctx.emplace(opCtx,
                        CommandHelpers::parseNsOrUUID(_dbName, _request.body),
                        AutoGetCollectionViewMode::kViewsPermitted);
            const auto& nss = ctx->getNss();

            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "UUID " << findCommand->getNamespaceOrUUID().uuid().get()
                                  << " specified in query request not found",
                    ctx || !findCommand->getNamespaceOrUUID().uuid());

            // Set the namespace if a collection was found, as opposed to nothing or a view.
            if (ctx) {
                query_request_helper::refreshNSS(ctx->getNss(), findCommand.get());
            }

            // Check whether we are allowed to read from this node after acquiring our locks.
            uassertStatusOK(replCoord->checkCanServeReadsFor(
                opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

            // Fill out curop information.
            //
            // We pass negative values for 'ntoreturn' and 'ntoskip' to indicate that these values
            // should be omitted from the log line. Limit and skip information is already present in
            // the find command parameters, so these fields are redundant.
            const int ntoreturn = -1;
            const int ntoskip = -1;
            beginQueryOp(opCtx, nss, _request.body, ntoreturn, ntoskip);

            // Finish the parsing step by using the FindCommandRequest to create a CanonicalQuery.
            const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
            auto expCtx = makeExpressionContext(opCtx, *findCommand, boost::none /* verbosity */);
            auto cq = uassertStatusOK(
                CanonicalQuery::canonicalize(opCtx,
                                             std::move(findCommand),
                                             isExplain,
                                             std::move(expCtx),
                                             extensionsCallback,
                                             MatchExpressionParser::kAllowAllSpecialFeatures));

            if (ctx->getView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                ctx.reset();

                // Convert the find command into an aggregation using $match (and other stages, as
                // necessary), if possible.
                const auto& findCommand = cq->getFindCommandRequest();
                auto viewAggregationCommand =
                    uassertStatusOK(query_request_helper::asAggregationCommand(findCommand));

                BSONObj aggResult = CommandHelpers::runCommandDirectly(
                    opCtx, OpMsgRequest::fromDBAndBody(_dbName, std::move(viewAggregationCommand)));
                auto status = getStatusFromCommandResult(aggResult);
                if (status.code() == ErrorCodes::InvalidPipelineOperator) {
                    uasserted(ErrorCodes::InvalidPipelineOperator,
                              str::stream() << "Unsupported in view pipeline: " << status.reason());
                }
                uassertStatusOK(status);
                result->getBodyBuilder().appendElements(aggResult);
                return;
            }

            const auto& collection = ctx->getCollection();

            if (cq->getFindCommandRequest().getReadOnce()) {
                // The readOnce option causes any storage-layer cursors created during plan
                // execution to assume read data will not be needed again and need not be cached.
                opCtx->recoveryUnit()->setReadOnce(true);
            }

            // Get the execution plan for the query.
            bool permitYield = true;
            auto exec =
                uassertStatusOK(getExecutorFind(opCtx, &collection, std::move(cq), permitYield));

            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
            }

            if (!collection) {
                // No collection. Just fill out curop indicating that there were zero results and
                // there is no ClientCursor id, and then return.
                const long long numResults = 0;
                const CursorId cursorId = 0;
                endQueryOp(opCtx, collection, *exec, numResults, cursorId);
                auto bodyBuilder = result->getBodyBuilder();
                appendCursorResponseObject(cursorId, nss.ns(), BSONArray(), &bodyBuilder);
                return;
            }

            FindCommon::waitInFindBeforeMakingBatch(opCtx, *exec->getCanonicalQuery());

            const FindCommandRequest& originalFC =
                exec->getCanonicalQuery()->getFindCommandRequest();

            // Stream query results, adding them to a BSONArray as we go.
            CursorResponseBuilder::Options options;
            options.isInitialResponse = true;
            if (!opCtx->inMultiDocumentTransaction()) {
                options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
            }
            CursorResponseBuilder firstBatch(result, options);
            BSONObj obj;
            PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
            std::uint64_t numResults = 0;
            bool stashedResult = false;
            ResourceConsumption::DocumentUnitCounter docUnitsReturned;

            try {
                while (!FindCommon::enoughForFirstBatch(originalFC, numResults) &&
                       PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
                    // If we can't fit this result inside the current batch, then we stash it for
                    // later.
                    if (!FindCommon::haveSpaceForNext(obj, numResults, firstBatch.bytesUsed())) {
                        exec->enqueue(obj);
                        stashedResult = true;
                        break;
                    }

                    // If this executor produces a postBatchResumeToken, add it to the response.
                    firstBatch.setPostBatchResumeToken(exec->getPostBatchResumeToken());

                    // Add result to output buffer.
                    firstBatch.append(obj);
                    numResults++;
                    docUnitsReturned.observeOne(obj.objsize());
                }
            } catch (DBException& exception) {
                firstBatch.abandon();

                auto&& explainer = exec->getPlanExplainer();
                auto&& [stats, _] =
                    explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
                LOGV2_WARNING(23798,
                              "Plan executor error during find command: {error}, "
                              "stats: {stats}, cmd: {cmd}",
                              "Plan executor error during find command",
                              "error"_attr = exception.toStatus(),
                              "stats"_attr = redact(stats),
                              "cmd"_attr = cmdObj);

                exception.addContext("Executor error during find command");
                throw;
            }

            // For empty batches, or in the case where the final result was added to the batch
            // rather than being stashed, we update the PBRT to ensure that it is the most recent
            // available.
            if (!stashedResult) {
                firstBatch.setPostBatchResumeToken(exec->getPostBatchResumeToken());
            }

            // Set up the cursor for getMore.
            CursorId cursorId = 0;
            if (shouldSaveCursor(opCtx, collection, state, exec.get())) {
                ClientCursorPin pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                    opCtx,
                    {std::move(exec),
                     nss,
                     AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                     APIParameters::get(opCtx),
                     opCtx->getWriteConcern(),
                     repl::ReadConcernArgs::get(opCtx),
                     _request.body,
                     {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)}});
                cursorId = pinnedCursor.getCursor()->cursorid();

                invariant(!exec);
                PlanExecutor* cursorExec = pinnedCursor.getCursor()->getExecutor();

                // State will be restored on getMore.
                cursorExec->saveState();
                cursorExec->detachFromOperationContext();

                // We assume that cursors created through a DBDirectClient are always used from
                // their original OperationContext, so we do not need to move time to and from the
                // cursor.
                if (!opCtx->getClient()->isInDirectClient()) {
                    pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(
                        opCtx->getRemainingMaxTimeMicros());
                }
                pinnedCursor.getCursor()->setNReturnedSoFar(numResults);
                pinnedCursor.getCursor()->incNBatches();

                // Fill out curop based on the results.
                endQueryOp(opCtx, collection, *cursorExec, numResults, cursorId);
            } else {
                endQueryOp(opCtx, collection, *exec, numResults, cursorId);
            }

            // Generate the response object to send to the client.
            firstBatch.done(cursorId, nss.ns());

            // Increment this metric once we have generated a response and we know it will return
            // documents.
            auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
            metricsCollector.incrementDocUnitsReturned(docUnitsReturned);
            query_request_helper::validateCursorResponse(result->getBodyBuilder().asTempObj());
        }

        void appendMirrorableRequest(BSONObjBuilder* bob) const override {
            // Filter the keys that can be mirrored
            static const auto kMirrorableKeys = [] {
                BSONObjBuilder keyBob;
                keyBob.append("find", 1);
                keyBob.append("filter", 1);
                keyBob.append("skip", 1);
                keyBob.append("limit", 1);
                keyBob.append("sort", 1);
                keyBob.append("hint", 1);
                keyBob.append("collation", 1);
                keyBob.append("min", 1);
                keyBob.append("max", 1);
                return keyBob.obj();
            }();

            _request.body.filterFieldsUndotted(bob, kMirrorableKeys, true);

            // Tell the find to only return a single batch
            bob->append("batchSize", 1);
            bob->append("singleBatch", true);
        }

    private:
        const OpMsgRequest _request;
        const StringData _dbName;
    };

} findCmd;

}  // namespace
}  // namespace mongo
