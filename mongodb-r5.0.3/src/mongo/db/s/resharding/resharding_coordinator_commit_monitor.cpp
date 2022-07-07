/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include <boost/optional.hpp>
#include <fmt/format.h>

#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/request_types/resharding_operation_time_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/testing_proctor.h"

using namespace fmt::literals;

namespace mongo {
namespace resharding {

namespace {

MONGO_FAIL_POINT_DEFINE(failQueryingRecipients);
MONGO_FAIL_POINT_DEFINE(hangBeforeQueryingRecipients);

BSONObj makeCommandObj(const NamespaceString& ns) {
    auto command = _shardsvrReshardingOperationTime(ns);
    command.setDbName("admin");
    return command.toBSON({});
}

auto makeRequests(const BSONObj& cmdObj, const std::vector<ShardId>& recipientShards) {
    invariant(!recipientShards.empty(), "The list of recipient shards cannot be empty");
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& recipient : recipientShards) {
        requests.emplace_back(recipient, cmdObj);
    }
    return requests;
}

static constexpr auto kRemainingOperationTimeFieldName = "remainingMillis"_sd;

boost::optional<Milliseconds> extractOperationRemainingTime(const BSONObj& obj) {
    if (const auto field = obj.getField(kRemainingOperationTimeFieldName); field.ok()) {
        const auto remainingTimeMillis =
            uassertStatusOK(field.parseIntegerElementToNonNegativeLong());
        return Milliseconds(remainingTimeMillis);
    }
    return boost::none;
}

}  // namespace

CoordinatorCommitMonitor::CoordinatorCommitMonitor(
    NamespaceString ns,
    std::vector<ShardId> recipientShards,
    CoordinatorCommitMonitor::TaskExecutorPtr executor,
    CancellationToken cancelToken,
    Milliseconds maxDelayBetweenQueries)
    : _ns(std::move(ns)),
      _recipientShards(std::move(recipientShards)),
      _executor(std::move(executor)),
      _cancelToken(std::move(cancelToken)),
      _threshold(Milliseconds(gRemainingReshardingOperationTimeThresholdMillis.load())),
      _maxDelayBetweenQueries(maxDelayBetweenQueries) {}


SemiFuture<void> CoordinatorCommitMonitor::waitUntilRecipientsAreWithinCommitThreshold() const {
    return _makeFuture()
        .onError([](Status status) {
            if (ErrorCodes::isCancellationError(status.code())) {
                LOGV2_DEBUG(5392003,
                            kDiagnosticLogLevel,
                            "The resharding commit monitor is interrupted",
                            "error"_attr = status);
            } else {
                LOGV2_WARNING(5392004,
                              "Stopped the resharding commit monitor due to an error",
                              "error"_attr = status);
            }
            return Status::OK();
        })
        .semi();
}

void CoordinatorCommitMonitor::setNetworkExecutorForTest(TaskExecutorPtr networkExecutor) {
    invariant(TestingProctor::instance().isEnabled(),
              "Using a separate executor for networking is a test-only feature");
    _networkExecutor = std::move(networkExecutor);
}

Milliseconds CoordinatorCommitMonitor::_queryMaxRemainingOperationTimeForRecipients() const {
    const auto cmdObj = makeCommandObj(_ns);
    const auto requests = makeRequests(cmdObj, _recipientShards);

    LOGV2_DEBUG(5392001,
                kDiagnosticLogLevel,
                "Querying recipient shards for the remaining operation time",
                "namespace"_attr = _ns);

    auto opCtx = cc().makeOperationContext();
    auto executor = _networkExecutor ? _networkExecutor : _executor;
    AsyncRequestsSender ars(opCtx.get(),
                            executor,
                            "admin",
                            requests,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            Shard::RetryPolicy::kIdempotent);

    hangBeforeQueryingRecipients.pauseWhileSet();

    auto maxRemainingTime = Milliseconds(0);
    while (!ars.done()) {
        iassert(ErrorCodes::CallbackCanceled,
                "The resharding commit monitor has been canceled",
                !_cancelToken.isCanceled());

        auto response = ars.next();
        const auto errorContext =
            "Failed command: {} on {}"_format(cmdObj.toString(), response.shardId.toString());

        const auto shardResponse =
            uassertStatusOKWithContext(std::move(response.swResponse), errorContext);
        const auto status = getStatusFromCommandResult(shardResponse.data);
        uassertStatusOKWithContext(status, errorContext);

        if (const auto remainingTime = extractOperationRemainingTime(shardResponse.data);
            remainingTime && remainingTime.get() > maxRemainingTime) {
            // A recipient shard does not report the remaining operation time when there is no data
            // to copy and no oplog entry to apply.
            maxRemainingTime = remainingTime.get();
        }
    }

    failQueryingRecipients.execute([](const BSONObj&) {
        iasserted(Status(ErrorCodes::FailPointEnabled, "Querying resharding recipients failed"));
    });

    LOGV2_DEBUG(5392002,
                kDiagnosticLogLevel,
                "Finished querying recipient shards for the remaining operation time",
                "namespace"_attr = _ns,
                "remainingTime"_attr = maxRemainingTime);

    return maxRemainingTime;
}

ExecutorFuture<void> CoordinatorCommitMonitor::_makeFuture() const {
    return ExecutorFuture<void>(_executor)
        .then([this] { return _queryMaxRemainingOperationTimeForRecipients(); })
        .onError([](Status status) {
            if (ErrorCodes::isCancellationError(status.code()))
                // Do not retry on cancellation errors.
                iasserted(status);

            // Absorbs any exception thrown by the query phase, except for cancellation errors, and
            // retries. The intention is to handle short term issues with querying recipients (e.g.,
            // network hiccups and connection timeouts).
            LOGV2_WARNING(5392006,
                          "Encountered an error while querying recipients, will retry shortly",
                          "error"_attr = status);
            return Milliseconds::max();
        })
        .then([this, anchor = shared_from_this()](Milliseconds maxRemainingTime) {
            // Check if all recipient shards are within the commit threshold.
            if (maxRemainingTime <= _threshold)
                return ExecutorFuture<void>(_executor);

            // The following ensures that the monitor would never sleep for more than a predefined
            // maximum delay between querying recipient shards. Thus, it can handle very large,
            // and potentially inaccurate estimates of the remaining operation time.
            auto sleepTime = std::min(maxRemainingTime - _threshold, _maxDelayBetweenQueries);
            return _executor->sleepFor(sleepTime, _cancelToken)
                .then([this, anchor = std::move(anchor)] {
                    // We are not canceled yet, so schedule new queries against recipient shards.
                    return _makeFuture();
                });
        });
}

}  // namespace resharding
}  // namespace mongo
