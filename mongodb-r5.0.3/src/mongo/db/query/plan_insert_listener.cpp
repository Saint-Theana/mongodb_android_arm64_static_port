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

#include "mongo/db/query/plan_insert_listener.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/find_common.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo::insert_listener {
namespace {
MONGO_FAIL_POINT_DEFINE(planExecutorHangWhileYieldedInWaitForInserts);
}

bool shouldListenForInserts(OperationContext* opCtx, CanonicalQuery* cq) {
    return cq && cq->getFindCommandRequest().getTailable() &&
        cq->getFindCommandRequest().getAwaitData() && awaitDataState(opCtx).shouldWaitForInserts &&
        opCtx->checkForInterruptNoAssert().isOK() &&
        awaitDataState(opCtx).waitForInsertsDeadline >
        opCtx->getServiceContext()->getPreciseClockSource()->now();
}

bool shouldWaitForInserts(OperationContext* opCtx,
                          CanonicalQuery* cq,
                          PlanYieldPolicy* yieldPolicy) {
    // If this is an awaitData-respecting operation and we have time left and we're not interrupted,
    // we should wait for inserts.
    if (shouldListenForInserts(opCtx, cq)) {
        // We expect awaitData cursors to be yielding.
        invariant(yieldPolicy->canReleaseLocksDuringExecution());

        // For operations with a last committed opTime, we should not wait if the replication
        // coordinator's lastCommittedOpTime has progressed past the client's lastCommittedOpTime.
        // In that case, we will return early so that we can inform the client of the new
        // lastCommittedOpTime immediately.
        if (!clientsLastKnownCommittedOpTime(opCtx).isNull()) {
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            return clientsLastKnownCommittedOpTime(opCtx) >= replCoord->getLastCommittedOpTime();
        }
        return true;
    }
    return false;
}

std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              PlanYieldPolicy* yieldPolicy) {
    // We don't expect to need a capped insert notifier for non-yielding plans.
    invariant(yieldPolicy->canReleaseLocksDuringExecution());

    // We can only wait if we have a collection; otherwise we should retry immediately when
    // we hit EOF.
    auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(opCtx, nss);
    invariant(collection);

    return collection->getCappedInsertNotifier();
}

void waitForInserts(OperationContext* opCtx,
                    PlanYieldPolicy* yieldPolicy,
                    CappedInsertNotifierData* notifierData) {
    invariant(notifierData->notifier);

    // The notifier wait() method will not wait unless the version passed to it matches the
    // current version of the notifier.  Since the version passed to it is the current version
    // of the notifier at the time of the previous EOF, we require two EOFs in a row with no
    // notifier version change in order to wait.  This is sufficient to ensure we never wait
    // when data is available.
    auto curOp = CurOp::get(opCtx);
    curOp->pauseTimer();
    ON_BLOCK_EXIT([curOp] { curOp->resumeTimer(); });

    uint64_t currentNotifierVersion = notifierData->notifier->getVersion();
    auto yieldResult = yieldPolicy->yieldOrInterrupt(opCtx, [opCtx, notifierData] {
        const auto deadline = awaitDataState(opCtx).waitForInsertsDeadline;
        notifierData->notifier->waitUntil(notifierData->lastEOFVersion, deadline);
        if (MONGO_unlikely(planExecutorHangWhileYieldedInWaitForInserts.shouldFail())) {
            LOGV2(4452903,
                  "PlanExecutor - planExecutorHangWhileYieldedInWaitForInserts fail point enabled. "
                  "Blocking until fail point is disabled");
            planExecutorHangWhileYieldedInWaitForInserts.pauseWhileSet();
        }
    });
    notifierData->lastEOFVersion = currentNotifierVersion;

    uassertStatusOK(yieldResult);
}
}  // namespace mongo::insert_listener
