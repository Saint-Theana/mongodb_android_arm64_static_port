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

#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {

struct CompileCtx;
class PlanStage;
enum class PlanState { ADVANCED, IS_EOF };

/**
 * Provides methods to detach and re-attach to an operation context, which derived classes may
 * override to perform additional actions when these events occur.
 *
 * Parameter 'T' is the typename of the class derived from this class. It's used to implement the
 * curiously recurring template pattern and access the internal state of the derived class.
 */
template <typename T>
class CanSwitchOperationContext {
public:
    CanSwitchOperationContext() = default;

    /**
     * Detaches from the OperationContext and releases any storage-engine state.
     *
     * It is only legal to call this when in a "saved" state. While in the "detached" state, it is
     * only legal to call reattachToOperationContext or the destructor. It is not legal to call
     * detachFromOperationContext() while already in the detached state.
     *
     * Propagates to all children, then calls doDetachFromOperationContext().
     */
    void detachFromOperationContext() {
        invariant(_opCtx);

        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->detachFromOperationContext();
        }

        stage->doDetachFromOperationContext();
        _opCtx = nullptr;
    }

    /**
     * Reattaches to the OperationContext and reacquires any storage-engine state.
     *
     * It is only legal to call this in the "detached" state. On return, the cursor is left in a
     * "saved" state, so callers must still call restoreState to use this object.
     *
     * Propagates to all children, then calls doReattachToOperationContext().
     */
    void attachToOperationContext(OperationContext* opCtx) {
        invariant(opCtx);
        invariant(!_opCtx);

        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->attachToOperationContext(opCtx);
        }

        _opCtx = opCtx;
        stage->doAttachToOperationContext(opCtx);
    }

protected:
    OperationContext* _opCtx{nullptr};
};

/**
 * Provides methods to save and restore the state of the object which derives from this class
 * when corresponding events are generated as a response to a change in the underlying data source.
 * Derived classes may override these methods to perform additional actions when these events occur.
 *
 * Parameter 'T' is the typename of the class derived from this class. It's used to implement the
 * curiously recurring template pattern and access the internal state of the derived class.
 */
template <typename T>
class CanChangeState {
public:
    CanChangeState() = default;

    /**
     * Notifies the stage that the underlying data source may change.
     *
     * It is illegal to call work() or isEOF() when a stage is in the "saved" state. May be called
     * before the first call to open(), before execution of the plan has begun.
     *
     * Propagates to all children, then calls doSaveState().
     */
    void saveState() {
        auto stage = static_cast<T*>(this);
        stage->_commonStats.yields++;

        stage->doSaveState();
        // Save the children in a right to left order so dependent stages (i.e. one using correlated
        // slots) are saved first.
        auto& children = stage->_children;
        for (auto it = children.rbegin(); it != children.rend(); it++) {
            (*it)->saveState();
        }
    }

    /**
     * Notifies the stage that underlying data is stable again and prepares for calls to work().
     *
     * Can only be called while the stage in is the "saved" state.
     *
     * Propagates to all children, then calls doRestoreState().
     *
     * Throws a UserException on failure to restore due to a conflicting event such as a
     * collection drop. May throw a WriteConflictException, in which case the caller may choose to
     * retry.
     */
    void restoreState() {
        auto stage = static_cast<T*>(this);
        stage->_commonStats.unyields++;
        for (auto&& child : stage->_children) {
            child->restoreState();
        }

        stage->doRestoreState();
    }
};

/**
 * Provides methods to obtain execution statistics specific to a plan stage.
 *
 * Parameter 'T' is the typename of the class derived from this class. It's used to implement the
 * curiously recurring template pattern and access the internal state of the derived class.
 */
template <typename T>
class CanTrackStats {
public:
    CanTrackStats(StringData stageType, PlanNodeId nodeId) : _commonStats(stageType, nodeId) {}

    /**
     * Returns a tree of stats. If the stage has any children it must propagate the request for
     * stats to them. If 'includeDebugInfo' is set to 'true' the stage may include some additional
     * debug info, opaque to the caller, which will be available via 'PlanStageStats::debugInfo'
     * member.
     */
    virtual std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const = 0;

    /**
     * Get stats specific to this stage. Some stages may not have specific stats, in which
     * case they return nullptr. The pointer is *not* owned by the caller.
     *
     * The returned pointer is only valid when the corresponding stage is also valid.
     * It must not exist past the stage. If you need the stats to outlive the stage,
     * use the getStats(...) method above.
     */
    virtual const SpecificStats* getSpecificStats() const = 0;

    /**
     * Get the CommonStats for this stage. The pointer is *not* owned by the caller.
     *
     * The returned pointer is only valid when the corresponding stage is also valid.
     * It must not exist past the stage. If you need the stats to outlive the stage,
     * use the getStats(...) method above.
     */
    const CommonStats* getCommonStats() const {
        return &_commonStats;
    }

    /**
     * Populates plan 'summary' object by walking through the entire PlanStage tree and for each
     * node whose plan node ID equals to the given 'nodeId', or if 'nodeId' is 'kEmptyPlanNodeId',
     * invoking 'accumulate(summary)' on the SpecificStats instance obtained by calling
     * 'getSpecificStats()'.
     */
    void accumulate(PlanNodeId nodeId, PlanSummaryStats& summary) const {
        if (auto stats = getSpecificStats();
            stats && (nodeId == kEmptyPlanNodeId || _commonStats.nodeId == nodeId)) {
            stats->accumulate(summary);
        }

        auto stage = static_cast<const T*>(this);
        for (auto&& child : stage->_children) {
            child->accumulate(nodeId, summary);
        }
    }

    void detachFromTrialRunTracker() {
        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->detachFromTrialRunTracker();
        }

        stage->doDetachFromTrialRunTracker();
    }

    void attachToTrialRunTracker(TrialRunTracker* tracker) {
        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->attachToTrialRunTracker(tracker);
        }

        stage->doAttachToTrialRunTracker(tracker);
    }

    /**
     * Force this stage to collect timing info during its execution. Must not be called after
     * execution has started.
     */
    void markShouldCollectTimingInfo() {
        invariant(!_commonStats.executionTimeMillis || *_commonStats.executionTimeMillis == 0);
        _commonStats.executionTimeMillis.emplace(0);

        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->markShouldCollectTimingInfo();
        }
    }

    void disableSlotAccess(bool recursive = false) {
        auto stage = static_cast<T*>(this);
        stage->_slotsAccessible = false;
        if (recursive) {
            for (auto& child : stage->_children) {
                child->disableSlotAccess(true);
            }
        }
    }

protected:
    PlanState trackPlanState(PlanState state) {
        if (state == PlanState::IS_EOF) {
            _commonStats.isEOF = true;
            _slotsAccessible = false;
        } else {
            invariant(state == PlanState::ADVANCED);
            _commonStats.advances++;
            _slotsAccessible = true;
        }
        return state;
    }


    void trackClose() {
        _commonStats.closes++;
        _slotsAccessible = false;
    }

    bool slotsAccessible() const {
        return _slotsAccessible;
    }

    /**
     * Returns an optional timer which is used to collect time spent executing the current stage.
     * May return boost::none if it is not necessary to collect timing info.
     */
    boost::optional<ScopedTimer> getOptTimer(OperationContext* opCtx) {
        if (_commonStats.executionTimeMillis && opCtx) {
            return {{opCtx->getServiceContext()->getFastClockSource(),
                     _commonStats.executionTimeMillis.get_ptr()}};
        }

        return boost::none;
    }

    CommonStats _commonStats;

private:
    /**
     * In general, accessors can be accesed only after getNext returns a row. It is most definitely
     * not OK to access accessors in ANY other state; e.g. closed, not yet opened, after EOF. We
     * need this tracker to support unfortunate consequences of the internal yielding feature. Once
     * that feature is retired we can then simply revisit all stages and simplify them.
     */
    bool _slotsAccessible{false};
};

/**
 * Provides a method which can be used to check if the current operation has been interrupted.
 * Maintains an internal state to maintain the interrupt check period. Also responsible for
 * triggering yields if this object has been configured with a yield policy.
 */
class CanInterrupt {
public:
    /**
     * This object will always be responsible for interrupt checking, but it can also optionally be
     * responsible for yielding. In order to enable yielding, the caller should pass a non-null
     * 'PlanYieldPolicy' pointer. Yielding may be disabled by providing a nullptr.
     */
    explicit CanInterrupt(PlanYieldPolicy* yieldPolicy) : _yieldPolicy(yieldPolicy) {}

    /**
     * Checks for interrupt if necessary. If yielding has been enabled for this object, then also
     * performs a yield if necessary.
     */
    void checkForInterrupt(OperationContext* opCtx) {
        invariant(opCtx);

        if (!_yieldPolicy) {
            // Yielding has been disabled, but interrupt checking can never be disabled (all
            // SBE operations must be interruptible). When yielding is enabled, it is responsible
            // for interrupt checking, but when disabled we do it ourselves.
            if (--_interruptCounter == 0) {
                _interruptCounter = kInterruptCheckPeriod;
                opCtx->checkForInterrupt();
            }
        } else if (_yieldPolicy->shouldYieldOrInterrupt(opCtx)) {
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(opCtx));
        }
    }

protected:
    PlanYieldPolicy* const _yieldPolicy{nullptr};

private:
    static const int kInterruptCheckPeriod = 128;
    int _interruptCounter = kInterruptCheckPeriod;
};

/**
 * This is an abstract base class of all plan stages in SBE.
 */
class PlanStage : public CanSwitchOperationContext<PlanStage>,
                  public CanChangeState<PlanStage>,
                  public CanTrackStats<PlanStage>,
                  public CanInterrupt {
public:
    PlanStage(StringData stageType, PlanYieldPolicy* yieldPolicy, PlanNodeId nodeId)
        : CanTrackStats{stageType, nodeId}, CanInterrupt{yieldPolicy} {}

    PlanStage(StringData stageType, PlanNodeId nodeId) : PlanStage(stageType, nullptr, nodeId) {}

    virtual ~PlanStage() = default;

    /**
     * The idiomatic C++ pattern of object cloning. Plan stages must be fully copyable as every
     * thread in parallel execution needs its own private copy.
     */
    virtual std::unique_ptr<PlanStage> clone() const = 0;

    /**
     * Prepare this SBE PlanStage tree for execution. Must be called once, and must be called
     * prior to open(), getNext(), close(), saveState(), or restoreState(),
     */
    virtual void prepare(CompileCtx& ctx) = 0;

    /**
     * Returns a slot accessor for a given slot id. This method is only called during the prepare
     * phase.
     */
    virtual value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) = 0;

    /**
     * Opens the plan tree and makes it ready for subsequent open(), getNext(), and close() calls.
     * The expectation is that a plan stage acquires resources (e.g. memory buffers) during the open
     * call and avoids resource acquisition in getNext().
     *
     * When reOpen flag is true then the plan stage should reinitizalize already acquired resources
     * (e.g. re-hash, re-sort, re-seek, etc).
     */
    virtual void open(bool reOpen) = 0;

    /**
     * Moves to the next position. If the end is reached then return EOF otherwise ADVANCED. Callers
     * are not required to call getNext until EOF. They can stop consuming results at any time. Once
     * EOF is reached it will stay at EOF unless reopened.
     */
    virtual PlanState getNext() = 0;

    /**
     * The mirror method to open(). It releases any acquired resources.
     */
    virtual void close() = 0;

    virtual std::vector<DebugPrinter::Block> debugPrint() const {
        auto stats = getCommonStats();
        std::string str = str::stream() << '[' << stats->nodeId << "] " << stats->stageType;
        return {DebugPrinter::Block(str)};
    }

    friend class CanSwitchOperationContext<PlanStage>;
    friend class CanChangeState<PlanStage>;
    friend class CanTrackStats<PlanStage>;

protected:
    // Derived classes can optionally override these methods.
    virtual void doSaveState() {}
    virtual void doRestoreState() {}
    virtual void doDetachFromOperationContext() {}
    virtual void doAttachToOperationContext(OperationContext* opCtx) {}
    virtual void doDetachFromTrialRunTracker() {}
    virtual void doAttachToTrialRunTracker(TrialRunTracker* tracker) {}

    std::vector<std::unique_ptr<PlanStage>> _children;
};

template <typename T, typename... Args>
inline std::unique_ptr<PlanStage> makeS(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}
}  // namespace sbe
}  // namespace mongo
