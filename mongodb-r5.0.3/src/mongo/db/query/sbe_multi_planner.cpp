/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_multi_planner.h"

#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_ranker_util.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/logv2/log.h"

namespace mongo::sbe {
CandidatePlans MultiPlanner::plan(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) {
    auto candidates =
        collectExecutionStats(std::move(solutions),
                              std::move(roots),
                              trial_period::getTrialPeriodMaxWorks(_opCtx, _collection));
    auto decision = uassertStatusOK(mongo::plan_ranker::pickBestPlan<PlanStageStats>(candidates));
    return finalizeExecutionPlans(std::move(decision), std::move(candidates));
}

CandidatePlans MultiPlanner::finalizeExecutionPlans(
    std::unique_ptr<mongo::plan_ranker::PlanRankingDecision> decision,
    std::vector<plan_ranker::CandidatePlan> candidates) const {
    invariant(decision);

    // Make sure we have at least one plan which hasn't failed.
    uassert(4822873,
            "all candidate plans failed during multi planning",
            std::count_if(candidates.begin(), candidates.end(), [](auto&& candidate) {
                return candidate.status.isOK();
            }) > 0);

    auto&& stats = decision->getStats<sbe::PlanStageStats>();

    const auto winnerIdx = decision->candidateOrder[0];
    tassert(5323801,
            str::stream() << "winner index is out of candidate plans bounds: " << winnerIdx << ", "
                          << candidates.size(),
            winnerIdx < candidates.size());
    tassert(5323802,
            str::stream() << "winner index is out of candidate plan stats bounds: " << winnerIdx
                          << ", " << stats.candidatePlanStats.size(),
            winnerIdx < stats.candidatePlanStats.size());

    auto& winner = candidates[winnerIdx];
    tassert(5323803,
            str::stream() << "winning candidate retruned an error: " << winner.status,
            winner.status.isOK());

    LOGV2_DEBUG(
        4822875, 5, "Winning solution", "bestSolution"_attr = redact(winner.solution->toString()));

    auto explainer =
        plan_explainer_factory::make(winner.root.get(), &winner.data, winner.solution.get());
    LOGV2_DEBUG(4822876, 2, "Winning plan", "planSummary"_attr = explainer->getPlanSummary());

    // Close all candidate plans but the winner.
    for (size_t ix = 1; ix < decision->candidateOrder.size(); ++ix) {
        const auto planIdx = decision->candidateOrder[ix];
        invariant(planIdx < candidates.size());
        candidates[planIdx].root->close();
    }

    // If the winning stage has exited early but has not fetched all results, clear the results
    // queue and reopen the plan stage tree, as we cannot resume such execution tree from where
    // the trial run has stopped, and, as a result, we cannot stash the results returned so far
    // in the plan executor.
    if (!stats.candidatePlanStats[winnerIdx]->common.isEOF && winner.exitedEarly) {
        winner.root->close();
        winner.root->open(false);
        // Clear the results queue.
        winner.results = decltype(winner.results){};
    }

    // Writes a cache entry for the winning plan to the plan cache if possible.
    plan_cache_util::updatePlanCache(
        _opCtx, _collection, _cachingMode, _cq, std::move(decision), candidates);

    return {std::move(candidates), winnerIdx};
}
}  // namespace mongo::sbe
