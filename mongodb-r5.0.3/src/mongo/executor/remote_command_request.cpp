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

#include "mongo/executor/remote_command_request.h"

#include <fmt/format.h>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/str.h"

using namespace fmt::literals;

namespace mongo {
namespace executor {
namespace {

// Used to generate unique identifiers for requests so they can be traced throughout the
// asynchronous networking logs
AtomicWord<unsigned long long> requestIdCounter(0);

}  // namespace

constexpr Milliseconds RemoteCommandRequestBase::kNoTimeout;

RemoteCommandRequestBase::RemoteCommandRequestBase(RequestId requestId,
                                                   const std::string& theDbName,
                                                   const BSONObj& theCmdObj,
                                                   const BSONObj& metadataObj,
                                                   OperationContext* opCtx,
                                                   Milliseconds timeoutMillis,
                                                   boost::optional<HedgeOptions> hedgeOptions,
                                                   FireAndForgetMode fireAndForgetMode)
    : id(requestId),
      dbname(theDbName),
      metadata(metadataObj),
      opCtx(opCtx),
      hedgeOptions(hedgeOptions),
      fireAndForgetMode(fireAndForgetMode),
      timeout(timeoutMillis) {
    // If there is a comment associated with the current operation, append it to the command that we
    // are about to dispatch to the shards.
    cmdObj = opCtx && opCtx->getComment() && !theCmdObj["comment"]
        ? theCmdObj.addField(*opCtx->getComment())
        : cmdObj = theCmdObj;

    // maxTimeMSOpOnly is set in the network interface based on the remaining max time attached to
    // the OpCtx.  It should never be specified explicitly.
    uassert(4924403,
            str::stream() << "Command request object should not manually specify "
                          << query_request_helper::kMaxTimeMSOpOnlyField,
            !cmdObj.hasField(query_request_helper::kMaxTimeMSOpOnlyField));

    if (hedgeOptions) {
        operationKey.emplace(UUID::gen());
        cmdObj = cmdObj.addField(BSON("clientOperationKey" << operationKey.get()).firstElement());
    }

    if (opCtx && APIParameters::get(opCtx).getParamsPassed()) {
        BSONObjBuilder bob(std::move(cmdObj));
        APIParameters::get(opCtx).appendInfo(&bob);
        cmdObj = bob.obj();
    }

    _updateTimeoutFromOpCtxDeadline(opCtx);
}

RemoteCommandRequestBase::RemoteCommandRequestBase()
    : id(requestIdCounter.addAndFetch(1)), operationKey(UUID::gen()) {}

void RemoteCommandRequestBase::_updateTimeoutFromOpCtxDeadline(const OperationContext* opCtx) {
    if (!opCtx || !opCtx->hasDeadline()) {
        return;
    }

    const auto opCtxTimeout = opCtx->getRemainingMaxTimeMillis();
    if (timeout == kNoTimeout || opCtxTimeout <= timeout) {
        timeout = opCtxTimeout;
        timeoutCode = opCtx->getTimeoutError();

        if (MONGO_unlikely(maxTimeNeverTimeOut.shouldFail())) {
            // If a mongod or mongos receives a request with a 'maxTimeMS', but the
            // 'maxTimeNeverTimeOut' failpoint is enabled, that server process should not enforce
            // the deadline locally, but should still pass the remaining deadline on to any other
            // servers it contacts as 'maxTimeMSOpOnly'.
            enforceLocalTimeout = false;
        }
    }
}

template <typename T>
RemoteCommandRequestImpl<T>::RemoteCommandRequestImpl() = default;

template <typename T>
RemoteCommandRequestImpl<T>::RemoteCommandRequestImpl(RequestId requestId,
                                                      const T& theTarget,
                                                      const std::string& theDbName,
                                                      const BSONObj& theCmdObj,
                                                      const BSONObj& metadataObj,
                                                      OperationContext* opCtx,
                                                      Milliseconds timeoutMillis,
                                                      boost::optional<HedgeOptions> hedgeOptions,
                                                      FireAndForgetMode fireAndForgetMode)
    : RemoteCommandRequestBase(requestId,
                               theDbName,
                               theCmdObj,
                               metadataObj,
                               opCtx,
                               timeoutMillis,
                               hedgeOptions,
                               fireAndForgetMode),
      target(theTarget) {
    if constexpr (std::is_same_v<T, std::vector<HostAndPort>>) {
        invariant(!theTarget.empty());
    }
}

template <typename T>
RemoteCommandRequestImpl<T>::RemoteCommandRequestImpl(const T& theTarget,
                                                      const std::string& theDbName,
                                                      const BSONObj& theCmdObj,
                                                      const BSONObj& metadataObj,
                                                      OperationContext* opCtx,
                                                      Milliseconds timeoutMillis,
                                                      boost::optional<HedgeOptions> hedgeOptions,
                                                      FireAndForgetMode fireAndForgetMode)
    : RemoteCommandRequestImpl(requestIdCounter.addAndFetch(1),
                               theTarget,
                               theDbName,
                               theCmdObj,
                               metadataObj,
                               opCtx,
                               timeoutMillis,
                               hedgeOptions,
                               fireAndForgetMode) {}

template <typename T>
std::string RemoteCommandRequestImpl<T>::toString() const {
    str::stream out;
    out << "RemoteCommand " << id << " -- target:";
    if constexpr (std::is_same_v<HostAndPort, T>) {
        out << target.toString();
    } else {
        out << "[{}]"_format(fmt::join(target, ", "));
    }
    out << " db:" << dbname;

    if (dateScheduled && timeout != kNoTimeout) {
        out << " expDate:" << (*dateScheduled + timeout).toString();
    }

    if (hedgeOptions) {
        invariant(operationKey);
        out << " hedgeOptions.count: " << hedgeOptions->count;
        out << " operationKey: " << operationKey.get();
    }

    out << " cmd:" << cmdObj.toString();
    return out;
}

template <typename T>
bool RemoteCommandRequestImpl<T>::operator==(const RemoteCommandRequestImpl& rhs) const {
    if (this == &rhs) {
        return true;
    }
    return target == rhs.target && dbname == rhs.dbname &&
        SimpleBSONObjComparator::kInstance.evaluate(cmdObj == rhs.cmdObj) &&
        SimpleBSONObjComparator::kInstance.evaluate(metadata == rhs.metadata) &&
        timeout == rhs.timeout;
}

template <typename T>
bool RemoteCommandRequestImpl<T>::operator!=(const RemoteCommandRequestImpl& rhs) const {
    return !(*this == rhs);
}

template struct RemoteCommandRequestImpl<HostAndPort>;
template struct RemoteCommandRequestImpl<std::vector<HostAndPort>>;

}  // namespace executor
}  // namespace mongo
