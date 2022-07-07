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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(skipWriteConflictRetries);

AtomicWord<bool> WriteConflictException::trace(false);

WriteConflictException::WriteConflictException()
    : DBException(Status(ErrorCodes::WriteConflict,
                         "WriteConflict error: this operation conflicted with another operation. "
                         "Please retry your operation or multi-document transaction.")) {
    if (trace.load()) {
        printStackTrace();
    }
}

void WriteConflictException::logAndBackoff(int attempt, StringData operation, StringData ns) {
    mongo::logAndBackoff(4640401,
                         ::mongo::logv2::LogComponent::kWrite,
                         logv2::LogSeverity::Debug(1),
                         static_cast<size_t>(attempt),
                         "Caught WriteConflictException",
                         "operation"_attr = operation,
                         "ns"_attr = ns);
}
}  // namespace mongo
