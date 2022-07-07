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

#include "mongo/util/assert_util.h"
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/db/exec/collection_scan.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/repl/optime.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo {

using std::unique_ptr;
using std::vector;

// static
const char* CollectionScan::kStageType = "COLLSCAN";

CollectionScan::CollectionScan(ExpressionContext* expCtx,
                               const CollectionPtr& collection,
                               const CollectionScanParams& params,
                               WorkingSet* workingSet,
                               const MatchExpression* filter)
    : RequiresCollectionStage(kStageType, expCtx, collection),
      _workingSet(workingSet),
      _filter((filter && !filter->isTriviallyTrue()) ? filter : nullptr),
      _params(params) {
    // Explain reports the direction of the collection scan.
    _specificStats.direction = params.direction;
    _specificStats.minRecord = params.minRecord;
    _specificStats.maxRecord = params.maxRecord;
    _specificStats.tailable = params.tailable;
    if (params.minRecord || params.maxRecord) {
        // The 'minRecord' and 'maxRecord' parameters are used for a special optimization that
        // applies only to forwards scans of the oplog and scans on collections clustered by _id.
        invariant(!params.resumeAfterRecordId);
        if (collection->ns().isOplog()) {
            invariant(params.direction == CollectionScanParams::FORWARD);
        } else {
            invariant(collection->isClustered());
        }
    }
    LOGV2_DEBUG(5400802,
                5,
                "collection scan bounds",
                "min"_attr = (!_params.minRecord) ? "none" : _params.minRecord->toString(),
                "max"_attr = (!_params.maxRecord) ? "none" : _params.maxRecord->toString());
    invariant(!_params.shouldTrackLatestOplogTimestamp || collection->ns().isOplog());

    if (params.assertTsHasNotFallenOffOplog) {
        invariant(params.shouldTrackLatestOplogTimestamp);
        invariant(params.direction == CollectionScanParams::FORWARD);
    }

    if (params.resumeAfterRecordId) {
        // The 'resumeAfterRecordId' parameter is used for resumable collection scans, which we
        // only support in the forward direction.
        invariant(params.direction == CollectionScanParams::FORWARD);
    }
}

PlanStage::StageState CollectionScan::doWork(WorkingSetID* out) {
    if (_commonStats.isEOF) {
        return PlanStage::IS_EOF;
    }

    boost::optional<Record> record;
    const bool needToMakeCursor = !_cursor;
    try {
        if (needToMakeCursor) {
            const bool forward = _params.direction == CollectionScanParams::FORWARD;

            if (forward && _params.shouldWaitForOplogVisibility) {
                // Forward, non-tailable scans from the oplog need to wait until all oplog entries
                // before the read begins to be visible. This isn't needed for reverse scans because
                // we only hide oplog entries from forward scans, and it isn't necessary for tailing
                // cursors because they ignore EOF and will eventually see all writes. Forward,
                // non-tailable scans are the only case where a meaningful EOF will be seen that
                // might not include writes that finished before the read started. This also must be
                // done before we create the cursor as that is when we establish the endpoint for
                // the cursor. Also call abandonSnapshot to make sure that we are using a fresh
                // storage engine snapshot while waiting. Otherwise, we will end up reading from the
                // snapshot where the oplog entries are not yet visible even after the wait.
                invariant(!_params.tailable && collection()->ns().isOplog());

                opCtx()->recoveryUnit()->abandonSnapshot();
                collection()->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(opCtx());
            }

            _cursor = collection()->getCursor(opCtx(), forward);

            if (!_lastSeenId.isNull()) {
                invariant(_params.tailable);
                // Seek to where we were last time. If it no longer exists, mark us as dead since we
                // want to signal an error rather than silently dropping data from the stream.
                //
                // Note that we want to return the record *after* this one since we have already
                // returned this one. This is possible in the tailing case. Notably, tailing is the
                // only time we'd need to create a cursor after already getting a record out of it
                // and updating our _lastSeenId.
                if (!_cursor->seekExact(_lastSeenId)) {
                    uasserted(ErrorCodes::CappedPositionLost,
                              str::stream() << "CollectionScan died due to failure to restore "
                                            << "tailable cursor position. "
                                            << "Last seen record id: " << _lastSeenId);
                }
            }

            if (_params.resumeAfterRecordId && !_params.resumeAfterRecordId->isNull()) {
                invariant(!_params.tailable);
                invariant(_lastSeenId.isNull());
                // Seek to where we are trying to resume the scan from. Signal a KeyNotFound error
                // if the record no longer exists.
                //
                // Note that we want to return the record *after* this one since we have already
                // returned this one prior to the resume.
                auto recordIdToSeek = *_params.resumeAfterRecordId;
                if (!_cursor->seekExact(recordIdToSeek)) {
                    uasserted(
                        ErrorCodes::KeyNotFound,
                        str::stream()
                            << "Failed to resume collection scan: the recordId from which we are "
                            << "attempting to resume no longer exists in the collection. "
                            << "recordId: " << recordIdToSeek);
                }
            }

            return PlanStage::NEED_TIME;
        }

        if (_lastSeenId.isNull() && _params.direction == CollectionScanParams::FORWARD &&
            _params.minRecord) {
            // Seek to the approximate start location.
            record = _cursor->seekNear(*_params.minRecord);
        }

        if (_lastSeenId.isNull() && _params.direction == CollectionScanParams::BACKWARD &&
            _params.maxRecord) {
            // Seek to the approximate start location (at the end).
            record = _cursor->seekNear(*_params.maxRecord);
        }

        if (!record) {
            record = _cursor->next();
        }
    } catch (const WriteConflictException&) {
        // Leave us in a state to try again next time.
        if (needToMakeCursor)
            _cursor.reset();
        *out = WorkingSet::INVALID_ID;
        return PlanStage::NEED_YIELD;
    }

    if (!record) {
        // We hit EOF. If we are tailable and have already seen data, leave us in a state to pick up
        // where we left off on the next call to work(). Otherwise, the EOF is permanent.
        if (_params.tailable && !_lastSeenId.isNull()) {
            _cursor.reset();
        } else {
            _commonStats.isEOF = true;
        }
        return PlanStage::IS_EOF;
    }

    _lastSeenId = record->id;
    if (_params.assertTsHasNotFallenOffOplog) {
        assertTsHasNotFallenOffOplog(*record);
    }
    if (_params.shouldTrackLatestOplogTimestamp) {
        setLatestOplogEntryTimestamp(*record);
    }

    WorkingSetID id = _workingSet->allocate();
    WorkingSetMember* member = _workingSet->get(id);
    member->recordId = record->id;
    member->resetDocument(opCtx()->recoveryUnit()->getSnapshotId(), record->data.releaseToBson());
    _workingSet->transitionToRecordIdAndObj(id);

    return returnIfMatches(member, id, out);
}

void CollectionScan::setLatestOplogEntryTimestamp(const Record& record) {
    auto tsElem = record.data.toBson()[repl::OpTime::kTimestampFieldName];
    uassert(ErrorCodes::Error(4382100),
            str::stream() << "CollectionScan was asked to track latest operation time, "
                             "but found a result without a valid 'ts' field: "
                          << record.data.toBson().toString(),
            tsElem.type() == BSONType::bsonTimestamp);
    LOGV2_DEBUG(550450,
                5,
                "Setting _latestOplogEntryTimestamp to the max of the timestamp of the current "
                "latest oplog entry and the timestamp of the current record",
                "latestOplogEntryTimestamp"_attr = _latestOplogEntryTimestamp,
                "currentRecordTimestamp"_attr = tsElem.timestamp());
    _latestOplogEntryTimestamp = std::max(_latestOplogEntryTimestamp, tsElem.timestamp());
}

void CollectionScan::assertTsHasNotFallenOffOplog(const Record& record) {
    // If the first entry we see in the oplog is the replset initialization, then it doesn't matter
    // if its timestamp is later than the timestamp that should not have fallen off the oplog; no
    // events earlier can have fallen off this oplog. Otherwise, verify that the timestamp of the
    // first observed oplog entry is earlier than or equal to timestamp that should not have fallen
    // off the oplog.
    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(record.data.toBson()));
    invariant(_specificStats.docsTested == 0);
    const bool isNewRS =
        oplogEntry.getObject().binaryEqual(BSON("msg" << repl::kInitiatingSetMsg)) &&
        oplogEntry.getOpType() == repl::OpTypeEnum::kNoop;
    uassert(ErrorCodes::OplogQueryMinTsMissing,
            "Specified timestamp has already fallen off the oplog",
            isNewRS || oplogEntry.getTimestamp() <= *_params.assertTsHasNotFallenOffOplog);
    // We don't need to check this assertion again after we've confirmed the first oplog event.
    _params.assertTsHasNotFallenOffOplog = boost::none;
}

namespace {
bool atEndOfRangeInclusive(const CollectionScanParams& params, const WorkingSetMember& member) {
    if (params.direction == CollectionScanParams::FORWARD) {
        return params.maxRecord && member.recordId > *params.maxRecord;
    } else {
        return params.minRecord && member.recordId < *params.minRecord;
    }
}
}  // namespace

PlanStage::StageState CollectionScan::returnIfMatches(WorkingSetMember* member,
                                                      WorkingSetID memberID,
                                                      WorkingSetID* out) {
    ++_specificStats.docsTested;

    // The 'minRecord' and 'maxRecord' bounds are always inclusive, even if the query predicate is
    // an exclusive inequality like $gt or $lt. In such cases, we rely on '_filter' to either
    // exclude or include the endpoints as required by the user's query.
    if (atEndOfRangeInclusive(_params, *member)) {
        _workingSet->free(memberID);
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    if (Filter::passes(member, _filter)) {
        if (_params.stopApplyingFilterAfterFirstMatch) {
            _filter = nullptr;
        }
        *out = memberID;
        return PlanStage::ADVANCED;
    } else {
        _workingSet->free(memberID);
        return PlanStage::NEED_TIME;
    }
}

bool CollectionScan::isEOF() {
    return _commonStats.isEOF;
}

void CollectionScan::doSaveStateRequiresCollection() {
    if (_cursor) {
        _cursor->save();
    }
}

void CollectionScan::doRestoreStateRequiresCollection() {
    if (_cursor) {
        const bool couldRestore = _cursor->restore();
        uassert(ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. "
                    << "Last seen record id: " << _lastSeenId,
                couldRestore);
    }
}

void CollectionScan::doDetachFromOperationContext() {
    if (_cursor)
        _cursor->detachFromOperationContext();
}

void CollectionScan::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(opCtx());
}

unique_ptr<PlanStageStats> CollectionScan::getStats() {
    // Add a BSON representation of the filter to the stats tree, if there is one.
    if (nullptr != _filter) {
        BSONObjBuilder bob;
        _filter->serialize(&bob);
        _commonStats.filter = bob.obj();
    }

    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_COLLSCAN);
    ret->specific = std::make_unique<CollectionScanStats>(_specificStats);
    return ret;
}

const SpecificStats* CollectionScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
