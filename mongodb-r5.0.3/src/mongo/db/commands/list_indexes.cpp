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

#include <memory>
#include <utility>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/list_indexes.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/list_indexes_gen.h"
#include "mongo/db/query/cursor_request.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

/**
 * Returns index specs, with resolved namespace, from the catalog for this listIndexes request.
 */
using IndexSpecsWithNamespaceString = std::pair<std::list<BSONObj>, NamespaceString>;
IndexSpecsWithNamespaceString getIndexSpecsWithNamespaceString(OperationContext* opCtx,
                                                               const ListIndexes& cmd) {
    const auto& origNssOrUUID = cmd.getNamespaceOrUUID();

    // Since time-series collections don't have UUIDs, we skip the time-series lookup
    // if the target collection is specified as a UUID.
    if (const auto& origNss = origNssOrUUID.nss()) {
        if (auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, *origNss)) {
            auto bucketsNss = origNss->makeTimeseriesBucketsNamespace();
            AutoGetCollectionForReadCommandMaybeLockFree autoColl(opCtx, bucketsNss);

            const CollectionPtr& coll = autoColl.getCollection();
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "ns does not exist: " << bucketsNss,
                    coll);

            return std::make_pair(
                timeseries::createTimeseriesIndexesFromBucketsIndexes(
                    *timeseriesOptions,
                    listIndexesInLock(opCtx, coll, bucketsNss, cmd.getIncludeBuildUUIDs())),
                *origNss);
        }
    }

    AutoGetCollectionForReadCommandMaybeLockFree autoColl(opCtx, origNssOrUUID);

    const auto& nss = autoColl.getNss();
    const CollectionPtr& coll = autoColl.getCollection();
    uassert(
        ErrorCodes::NamespaceNotFound, str::stream() << "ns does not exist: " << nss.ns(), coll);

    return std::make_pair(listIndexesInLock(opCtx, coll, nss, cmd.getIncludeBuildUUIDs()), nss);
}

/**
 * Lists the indexes for a given collection.
 * If 'includeBuildUUIDs' is true, then the index build uuid is also returned alongside the index
 * spec for in-progress index builds only.
 *
 * Format:
 * {
 *   listIndexes: <collection name>,
 *   includeBuildUUIDs: <boolean>,
 * }
 *
 * Return format:
 * {
 *   indexes: [
 *     <index>,
 *     ...
 *   ]
 * }
 *
 * Where '<index>' is the index spec if either the index is ready or 'includeBuildUUIDs' is false.
 * If the index is in-progress and 'includeBuildUUIDs' is true then '<index>' has the following
 * format:
 * {
 *   spec: <index spec>,
 *   buildUUID: <index build uuid>
 * }
 */

class CmdListIndexes final : public ListIndexesCmdVersion1Gen<CmdListIndexes> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kOptIn;
    }
    bool maintenanceOk() const final {
        return false;
    }
    bool adminOnly() const final {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    std::string help() const final {
        return "list indexes for a collection";
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            auto nss = request().getNamespaceOrUUID();
            if (nss.uuid()) {
                // UUID requires opCtx to resolve, settle on just the dbname.
                return NamespaceString(request().getDbName(), "");
            }
            invariant(nss.nss());
            return nss.nss().get();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());

            auto& cmd = request();
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                authzSession->isAuthorizedToParseNamespaceElement(request().getNamespaceOrUUID()));

            const auto nss = CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(
                opCtx, cmd.getNamespaceOrUUID());

            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to list indexes on collection:" << nss.ns(),
                    authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(nss), ActionType::listIndexes));
        }

        ListIndexesReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            auto indexSpecsWithNss = getIndexSpecsWithNamespaceString(opCtx, request());
            const auto& indexList = indexSpecsWithNss.first;
            const auto& nss = indexSpecsWithNss.second;
            return ListIndexesReply(_makeCursor(opCtx, indexList, nss));
        }

    private:
        /**
         * Constructs a cursor that iterates the index specs found in 'indexSpecsWithNss'.
         * This function does not hold any locks because it does not access in-memory
         * or on-disk data.
         */
        ListIndexesReplyCursor _makeCursor(OperationContext* opCtx,
                                           const std::list<BSONObj>& indexList,
                                           const NamespaceString& nss) {
            auto& cmd = request();

            long long batchSize = std::numeric_limits<long long>::max();
            if (cmd.getCursor() && cmd.getCursor()->getBatchSize()) {
                batchSize = *cmd.getCursor()->getBatchSize();
            }

            auto expCtx = make_intrusive<ExpressionContext>(
                opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);

            auto ws = std::make_unique<WorkingSet>();
            auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

            for (auto&& indexSpec : indexList) {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->keyData.clear();
                member->recordId = RecordId();
                member->resetDocument(SnapshotId(), indexSpec.getOwned());
                member->transitionToOwnedObj();
                root->pushBack(id);
            }

            auto exec = uassertStatusOK(
                plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(root),
                                            &CollectionPtr::null,
                                            PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                            false, /* whether returned BSON must be owned */
                                            nss));

            std::vector<mongo::ListIndexesReplyItem> firstBatch;
            int bytesBuffered = 0;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj nextDoc;
                PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
                if (state == PlanExecutor::IS_EOF) {
                    break;
                }
                invariant(state == PlanExecutor::ADVANCED);

                // If we can't fit this result inside the current batch, then we stash it for
                // later.
                if (!FindCommon::haveSpaceForNext(nextDoc, objCount, bytesBuffered)) {
                    exec->enqueue(nextDoc);
                    break;
                }

                try {
                    firstBatch.push_back(ListIndexesReplyItem::parse(
                        IDLParserErrorContext("ListIndexesReplyItem"), nextDoc));
                } catch (const DBException& exc) {
                    LOGV2_ERROR(5254500,
                                "Could not parse catalog entry while replying to listIndexes",
                                "entry"_attr = nextDoc,
                                "error"_attr = exc);
                    uasserted(5254501,
                              "Could not parse catalog entry while replying to listIndexes");
                }
                bytesBuffered += nextDoc.objsize();
            }

            if (exec->isEOF()) {
                return ListIndexesReplyCursor(0 /* cursorId */, nss, std::move(firstBatch));
            }

            exec->saveState();
            exec->detachFromOperationContext();

            // Global cursor registration must be done without holding any locks.
            auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                opCtx,
                {std::move(exec),
                 nss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                 APIParameters::get(opCtx),
                 opCtx->getWriteConcern(),
                 repl::ReadConcernArgs::get(opCtx),
                 cmd.toBSON({}),
                 {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::listIndexes)}});

            pinnedCursor->incNBatches();
            pinnedCursor->incNReturnedSoFar(firstBatch.size());

            return ListIndexesReplyCursor(
                pinnedCursor.getCursor()->cursorid(), nss, std::move(firstBatch));
        }
    };
} cmdListIndexes;

}  // namespace
}  // namespace mongo
