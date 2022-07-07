/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <queue>

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer_sbe.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/sbe_runtime_planner.h"
#include "mongo/db/query/sbe_stage_builder.h"

namespace mongo {
class PlanExecutorSBE final : public PlanExecutor {
public:
    PlanExecutorSBE(OperationContext* opCtx,
                    std::unique_ptr<CanonicalQuery> cq,
                    sbe::CandidatePlans candidates,
                    const CollectionPtr& collection,
                    bool returnOwnedBson,
                    NamespaceString nss,
                    bool isOpen,
                    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy);

    CanonicalQuery* getCanonicalQuery() const override {
        return _cq.get();
    }

    const NamespaceString& nss() const override {
        return _nss;
    }

    OperationContext* getOpCtx() const override {
        return _opCtx;
    }

    void saveState();
    void restoreState(const RestoreContext& context);

    void detachFromOperationContext();
    void reattachToOperationContext(OperationContext* opCtx);

    ExecState getNext(BSONObj* out, RecordId* dlOut) override;
    ExecState getNextDocument(Document* objOut, RecordId* dlOut) override;

    bool isEOF() override {
        return isMarkedAsKilled() || (_stash.empty() && _root->getCommonStats()->isEOF);
    }

    long long executeCount() override {
        // Using SBE to execute a count command is not yet supported.
        MONGO_UNREACHABLE;
    }

    UpdateResult executeUpdate() override {
        // Using SBE to execute an update command is not yet supported.
        MONGO_UNREACHABLE;
    }
    UpdateResult getUpdateResult() const override {
        // Using SBE to execute an update command is not yet supported.
        MONGO_UNREACHABLE;
    }

    long long executeDelete() override {
        // Using SBE to execute a delete command is not yet supported.
        MONGO_UNREACHABLE;
    }

    void markAsKilled(Status killStatus);

    void dispose(OperationContext* opCtx);

    void enqueue(const BSONObj& obj);

    bool isMarkedAsKilled() const override {
        return !_killStatus.isOK();
    }

    Status getKillStatus() override {
        invariant(isMarkedAsKilled());
        return _killStatus;
    }

    bool isDisposed() const override {
        return _isDisposed;
    }

    Timestamp getLatestOplogTimestamp() const override;
    BSONObj getPostBatchResumeToken() const override;

    /**
     * Even though the leaves of '_root' will acquire AutoGet objects, the caller must acquire a top
     * level AutoGet object outside of this PlanExecutor in order to open a storage transaction and
     * establish a consistent view of the catalog.
     */
    LockPolicy lockPolicy() const override {
        return LockPolicy::kLockExternally;
    }

    const PlanExplainer& getPlanExplainer() const final {
        invariant(_planExplainer);
        return *_planExplainer;
    }

private:
    enum class State { kClosed, kOpened };

    State _state{State::kClosed};

    OperationContext* _opCtx;

    NamespaceString _nss;
    const bool _mustReturnOwnedBson;

    // CompileCtx owns the instance pointed by _env, so we must keep it around.
    const std::unique_ptr<sbe::PlanStage> _root;
    stage_builder::PlanStageData _rootData;
    std::unique_ptr<QuerySolution> _solution;

    sbe::value::SlotAccessor* _result{nullptr};
    sbe::value::SlotAccessor* _resultRecordId{nullptr};
    sbe::value::TypeTags _tagLastRecordId{sbe::value::TypeTags::Nothing};
    sbe::value::Value _valLastRecordId{0};
    sbe::RuntimeEnvironment::Accessor* _oplogTs{nullptr};

    boost::optional<sbe::value::SlotId> _resumeRecordIdSlot;

    std::queue<std::pair<BSONObj, boost::optional<RecordId>>> _stash;
    // If we are returning owned result (i.e. value is moved out of the result accessor) then its
    // lifetime must extend up to the next getNext (or saveState).
    BSONObj _lastGetNext;

    // If _killStatus has a non-OK value, then we have been killed and the value represents the
    // reason for the kill.
    Status _killStatus = Status::OK();

    std::unique_ptr<CanonicalQuery> _cq;

    std::unique_ptr<PlanYieldPolicySBE> _yieldPolicy;

    std::unique_ptr<PlanExplainer> _planExplainer;

    bool _isDisposed{false};
};

/**
 * Executes getNext() on the 'root' PlanStage and used 'resultSlot' and 'recordIdSlot' to access the
 * fetched document and it's record id, which are stored in 'out' and 'dlOut' parameters
 * respectively, if they not null pointers.
 *
 * This common logic can be used by various consumers which need to fetch data using an SBE
 * PlanStage tree, such as PlanExecutor or RuntimePlanner.
 */
sbe::PlanState fetchNext(sbe::PlanStage* root,
                         sbe::value::SlotAccessor* resultSlot,
                         sbe::value::SlotAccessor* recordIdSlot,
                         BSONObj* out,
                         RecordId* dlOut,
                         bool returnOwnedBson);
}  // namespace mongo
