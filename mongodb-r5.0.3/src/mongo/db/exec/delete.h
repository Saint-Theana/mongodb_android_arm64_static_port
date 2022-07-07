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

#pragma once

#include "mongo/db/exec/requires_collection_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/storage/remove_saver.h"

namespace mongo {

class CanonicalQuery;
class OpDebug;
class OperationContext;
class PlanExecutor;

struct DeleteStageParams {
    DeleteStageParams()
        : isMulti(false),
          fromMigrate(false),
          isExplain(false),
          returnDeleted(false),
          canonicalQuery(nullptr),
          opDebug(nullptr) {}

    // Should we delete all documents returned from the child (a "multi delete"), or at most one
    // (a "single delete")?
    bool isMulti;

    // Is this delete part of a migrate operation that is essentially like a no-op
    // when the cluster is observed by an external client.
    bool fromMigrate;

    // Are we explaining a delete command rather than actually executing it?
    bool isExplain;

    // Should we return the document we just deleted?
    bool returnDeleted;

    // The stmtId for this particular delete.
    StmtId stmtId = kUninitializedStmtId;

    // The parsed query predicate for this delete. Not owned here.
    CanonicalQuery* canonicalQuery;

    // The user-requested sort specification. Currently used just for findAndModify.
    BSONObj sort;

    // Optional. When not null, delete metrics are recorded here.
    OpDebug* opDebug;

    // Optional. When not null, send document about to be deleted to removeSaver.
    // RemoveSaver is called before actual deletes are executed.
    // Note: the differentiating factor between this and returnDeleted is that the caller will get
    // the deleted document after it was already deleted. That means that if the caller would have
    // to use the removeSaver at that point, they miss the document if the process dies before it
    // reaches the removeSaver. However, this is still best effort since the RemoveSaver
    // operates on a different persistence system from the the database storage engine.
    std::unique_ptr<RemoveSaver> removeSaver;
};

/**
 * This stage delete documents by RecordId that are returned from its child. If the deleted
 * document was requested to be returned, then ADVANCED is returned after deleting a document.
 * Otherwise, NEED_TIME is returned after deleting a document.
 *
 * Callers of work() must be holding a write lock (and, for replicated deletes, callers must have
 * had the replication coordinator approve the write).
 */
class DeleteStage final : public RequiresMutableCollectionStage {
    DeleteStage(const DeleteStage&) = delete;
    DeleteStage& operator=(const DeleteStage&) = delete;

public:
    static constexpr StringData kStageType = "DELETE"_sd;

    DeleteStage(ExpressionContext* expCtx,
                std::unique_ptr<DeleteStageParams> params,
                WorkingSet* ws,
                const CollectionPtr& collection,
                PlanStage* child);

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_DELETE;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

protected:
    void doSaveStateRequiresCollection() final {}

    void doRestoreStateRequiresCollection() final;

private:
    /**
     * Stores 'idToRetry' in '_idRetrying' so the delete can be retried during the next call to
     * work(). Always returns NEED_YIELD and sets 'out' to WorkingSet::INVALID_ID.
     */
    StageState prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out);

    std::unique_ptr<DeleteStageParams> _params;

    // Not owned by us.
    WorkingSet* _ws;

    // If not WorkingSet::INVALID_ID, we use this rather than asking our child what to do next.
    WorkingSetID _idRetrying;

    // If not WorkingSet::INVALID_ID, we return this member to our caller.
    WorkingSetID _idReturning;

    // Stats
    DeleteStats _specificStats;
};

}  // namespace mongo
