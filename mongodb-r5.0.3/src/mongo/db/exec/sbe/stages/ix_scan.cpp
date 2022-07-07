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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/ix_scan.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/index/index_access_method.h"

namespace mongo::sbe {
IndexScanStage::IndexScanStage(CollectionUUID collUuid,
                               StringData indexName,
                               bool forward,
                               boost::optional<value::SlotId> recordSlot,
                               boost::optional<value::SlotId> recordIdSlot,
                               boost::optional<value::SlotId> snapshotIdSlot,
                               IndexKeysInclusionSet indexKeysToInclude,
                               value::SlotVector vars,
                               boost::optional<value::SlotId> seekKeySlotLow,
                               boost::optional<value::SlotId> seekKeySlotHigh,
                               PlanYieldPolicy* yieldPolicy,
                               PlanNodeId nodeId)
    : PlanStage(seekKeySlotLow ? "ixseek"_sd : "ixscan"_sd, yieldPolicy, nodeId),
      _collUuid(collUuid),
      _indexName(indexName),
      _forward(forward),
      _recordSlot(recordSlot),
      _recordIdSlot(recordIdSlot),
      _snapshotIdSlot(snapshotIdSlot),
      _indexKeysToInclude(indexKeysToInclude),
      _vars(std::move(vars)),
      _seekKeySlotLow(seekKeySlotLow),
      _seekKeySlotHigh(seekKeySlotHigh) {
    // The valid state is when both boundaries, or none is set, or only low key is set.
    invariant((_seekKeySlotLow && _seekKeySlotHigh) || (!_seekKeySlotLow && !_seekKeySlotHigh) ||
              (_seekKeySlotLow && !_seekKeySlotHigh));

    invariant(_indexKeysToInclude.count() == _vars.size());
}

std::unique_ptr<PlanStage> IndexScanStage::clone() const {
    return std::make_unique<IndexScanStage>(_collUuid,
                                            _indexName,
                                            _forward,
                                            _recordSlot,
                                            _recordIdSlot,
                                            _snapshotIdSlot,
                                            _indexKeysToInclude,
                                            _vars,
                                            _seekKeySlotLow,
                                            _seekKeySlotHigh,
                                            _yieldPolicy,
                                            _commonStats.nodeId);
}

void IndexScanStage::prepare(CompileCtx& ctx) {
    if (_recordSlot) {
        _recordAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    if (_recordIdSlot) {
        _recordIdAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    if (_snapshotIdSlot) {
        _snapshotIdAccessor = std::make_unique<value::OwnedValueAccessor>();
    }

    _accessors.resize(_vars.size());
    for (size_t idx = 0; idx < _accessors.size(); ++idx) {
        auto [it, inserted] = _accessorMap.emplace(_vars[idx], &_accessors[idx]);
        uassert(4822821, str::stream() << "duplicate slot: " << _vars[idx], inserted);
    }

    if (_seekKeySlotLow) {
        _seekKeyLowAccessor = ctx.getAccessor(*_seekKeySlotLow);
    }
    if (_seekKeySlotHigh) {
        _seekKeyHiAccessor = ctx.getAccessor(*_seekKeySlotHigh);
        _seekKeyHighHolder = std::make_unique<value::OwnedValueAccessor>();
    }
    _seekKeyLowHolder = std::make_unique<value::OwnedValueAccessor>();

    tassert(5709602, "'_coll' should not be initialized prior to 'acquireCollection()'", !_coll);
    std::tie(_coll, _collName, _catalogEpoch) = acquireCollection(_opCtx, _collUuid);

    auto indexCatalog = _coll->getIndexCatalog();
    auto indexDesc = indexCatalog->findIndexByName(_opCtx, _indexName);
    tassert(4938500,
            str::stream() << "could not find index named '" << _indexName << "' in collection '"
                          << _collName << "'",
            indexDesc);
    _weakIndexCatalogEntry = indexCatalog->getEntryShared(indexDesc);
    auto entry = _weakIndexCatalogEntry.lock();
    tassert(4938503,
            str::stream() << "expected IndexCatalogEntry for index named: " << _indexName,
            static_cast<bool>(entry));
    _ordering = entry->ordering();

    if (_snapshotIdAccessor) {
        _snapshotIdAccessor->reset(
            value::TypeTags::NumberInt64,
            value::bitcastFrom<uint64_t>(_opCtx->recoveryUnit()->getSnapshotId().toNumber()));
    }
}

value::SlotAccessor* IndexScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_recordSlot && *_recordSlot == slot) {
        return _recordAccessor.get();
    }

    if (_recordIdSlot && *_recordIdSlot == slot) {
        return _recordIdAccessor.get();
    }

    if (_snapshotIdSlot && *_snapshotIdSlot == slot) {
        return _snapshotIdAccessor.get();
    }

    if (auto it = _accessorMap.find(slot); it != _accessorMap.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void IndexScanStage::doSaveState() {
    if (slotsAccessible()) {
        if (_recordAccessor) {
            _recordAccessor->makeOwned();
        }
        if (_recordIdAccessor) {
            _recordIdAccessor->makeOwned();
        }
        for (auto& accessor : _accessors) {
            accessor.makeOwned();
        }
    }

    // Seek points are external to the index scan and must be accessible no matter what as long as
    // the index scan is opened.
    if (_open) {
        if (_seekKeyLowHolder) {
            _seekKeyLowHolder->makeOwned();
        }
        if (_seekKeyHighHolder) {
            _seekKeyHighHolder->makeOwned();
        }
    }

    if (_cursor) {
        _cursor->save();
    }

    _coll.reset();
}

void IndexScanStage::restoreCollectionAndIndex() {
    _coll = restoreCollection(_opCtx, _collName, _collUuid, _catalogEpoch);
    auto indexCatalogEntry = _weakIndexCatalogEntry.lock();
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: index '" << _indexName << "' dropped",
            indexCatalogEntry && !indexCatalogEntry->isDropped());
}

void IndexScanStage::doRestoreState() {
    invariant(_opCtx);
    invariant(!_coll);

    // If this stage is not currently open, then there is nothing to restore.
    if (!_open) {
        return;
    }

    restoreCollectionAndIndex();

    if (_cursor) {
        _cursor->restore();
    }

    // Yield is the only time during plan execution that the snapshotId can change. As such, we
    // update it accordingly as part of yield recovery.
    if (_snapshotIdAccessor) {
        _snapshotIdAccessor->reset(
            value::TypeTags::NumberInt64,
            value::bitcastFrom<uint64_t>(_opCtx->recoveryUnit()->getSnapshotId().toNumber()));
    }
}

void IndexScanStage::doDetachFromOperationContext() {
    if (_cursor) {
        _cursor->detachFromOperationContext();
    }
}

void IndexScanStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_cursor) {
        _cursor->reattachToOperationContext(opCtx);
    }
}

void IndexScanStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

void IndexScanStage::doAttachToTrialRunTracker(TrialRunTracker* tracker) {
    _tracker = tracker;
}

void IndexScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    invariant(_opCtx);

    if (_open) {
        tassert(5071006, "reopened IndexScanStage but reOpen=false", reOpen);
        tassert(5071007, "IndexScanStage is open but _coll is null", _coll);
        tassert(5071008, "IndexScanStage is open but don't have _cursor", _cursor);
    } else {
        tassert(5071009, "first open to IndexScanStage but reOpen=true", !reOpen);
        if (!_coll) {
            // We're being opened after 'close()'. We need to re-acquire '_coll' in this case and
            // make some validity checks (the collection has not been dropped, renamed, etc.).
            tassert(5071010, "IndexScanStage is not open but have _cursor", !_cursor);
            restoreCollectionAndIndex();
        }
    }

    _open = true;
    _firstGetNext = true;

    auto entry = _weakIndexCatalogEntry.lock();
    tassert(4938502,
            str::stream() << "expected IndexCatalogEntry for index named: " << _indexName,
            static_cast<bool>(entry));
    if (!_cursor) {
        _cursor = entry->accessMethod()->getSortedDataInterface()->newCursor(_opCtx, _forward);
    }

    if (_seekKeyLowAccessor && _seekKeyHiAccessor) {
        auto [tagLow, valLow] = _seekKeyLowAccessor->getViewOfValue();
        const auto msgTagLow = tagLow;
        uassert(4822851,
                str::stream() << "seek key is wrong type: " << msgTagLow,
                tagLow == value::TypeTags::ksValue);
        _seekKeyLowHolder->reset(false, tagLow, valLow);

        auto [tagHi, valHi] = _seekKeyHiAccessor->getViewOfValue();
        const auto msgTagHi = tagHi;
        uassert(4822852,
                str::stream() << "seek key is wrong type: " << msgTagHi,
                tagHi == value::TypeTags::ksValue);

        _seekKeyHighHolder->reset(false, tagHi, valHi);
    } else if (_seekKeyLowAccessor) {
        auto [tagLow, valLow] = _seekKeyLowAccessor->getViewOfValue();
        const auto msgTagLow = tagLow;
        uassert(4822853,
                str::stream() << "seek key is wrong type: " << msgTagLow,
                tagLow == value::TypeTags::ksValue);
        _seekKeyLowHolder->reset(false, tagLow, valLow);
    } else {
        auto sdi = entry->accessMethod()->getSortedDataInterface();
        KeyString::Builder kb(sdi->getKeyStringVersion(),
                              sdi->getOrdering(),
                              KeyString::Discriminator::kExclusiveBefore);
        kb.appendDiscriminator(KeyString::Discriminator::kExclusiveBefore);

        auto [copyTag, copyVal] = value::makeCopyKeyString(kb.getValueCopy());
        _seekKeyLowHolder->reset(true, copyTag, copyVal);
    }
}

const KeyString::Value& IndexScanStage::getSeekKeyLow() const {
    auto [tag, value] = _seekKeyLowHolder->getViewOfValue();
    return *value::getKeyStringView(value);
}

const KeyString::Value* IndexScanStage::getSeekKeyHigh() const {
    if (!_seekKeyHighHolder) {
        return nullptr;
    }
    auto [tag, value] = _seekKeyHighHolder->getViewOfValue();
    return value::getKeyStringView(value);
}

PlanState IndexScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to get next record from a storage cursor so do not bother saving our internal
    // state in case it yields as the state will be completely overwritten after the call.
    disableSlotAccess();

    if (!_cursor) {
        return trackPlanState(PlanState::IS_EOF);
    }

    checkForInterrupt(_opCtx);

    if (_firstGetNext) {
        _firstGetNext = false;
        _nextRecord = _cursor->seekForKeyString(getSeekKeyLow());
        ++_specificStats.seeks;
    } else {
        _nextRecord = _cursor->nextKeyString();
    }

    ++_specificStats.numReads;
    if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumReads>(1)) {
        // If we're collecting execution stats during multi-planning and reached the end of the
        // trial period because we've performed enough physical reads, bail out from the trial run
        // by raising a special exception to signal a runtime planner that this candidate plan has
        // completed its trial run early. Note that a trial period is executed only once per a
        // PlanStage tree, and once completed never run again on the same tree.
        _tracker = nullptr;
        uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in ixscan");
    }

    if (!_nextRecord) {
        return trackPlanState(PlanState::IS_EOF);
    }

    if (auto seekKeyHigh = getSeekKeyHigh(); seekKeyHigh) {
        auto cmp = _nextRecord->keyString.compare(*seekKeyHigh);

        if (_forward) {
            if (cmp > 0) {
                return trackPlanState(PlanState::IS_EOF);
            }
        } else {
            if (cmp < 0) {
                return trackPlanState(PlanState::IS_EOF);
            }
        }
    }

    // Note: we may in the future want to bump 'keysExamined' for comparisons to a key that result
    // in the stage returning EOF.
    ++_specificStats.keysExamined;
    if (_recordAccessor) {
        _recordAccessor->reset(false,
                               value::TypeTags::ksValue,
                               value::bitcastFrom<KeyString::Value*>(&_nextRecord->keyString));
    }

    if (_recordIdAccessor) {
        _recordIdAccessor->reset(false,
                                 value::TypeTags::RecordId,
                                 value::bitcastFrom<int64_t>(_nextRecord->loc.getLong()));
    }

    if (_accessors.size()) {
        _valuesBuffer.reset();
        readKeyStringValueIntoAccessors(
            _nextRecord->keyString, *_ordering, &_valuesBuffer, &_accessors, _indexKeysToInclude);
    }

    return trackPlanState(PlanState::ADVANCED);
}

void IndexScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();

    _cursor.reset();
    _coll.reset();
    _open = false;
}

std::unique_ptr<PlanStageStats> IndexScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<IndexScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("keysExamined", static_cast<long long>(_specificStats.keysExamined));
        bob.appendNumber("seeks", static_cast<long long>(_specificStats.seeks));
        bob.appendNumber("numReads", static_cast<long long>(_specificStats.numReads));
        if (_recordSlot) {
            bob.appendNumber("recordSlot", static_cast<long long>(*_recordSlot));
        }
        if (_recordIdSlot) {
            bob.appendNumber("recordIdSlot", static_cast<long long>(*_recordIdSlot));
        }
        if (_snapshotIdSlot) {
            bob.appendNumber("snapshotIdSlot", static_cast<long long>(*_snapshotIdSlot));
        }
        if (_seekKeySlotLow) {
            bob.appendNumber("seekKeySlotLow", static_cast<long long>(*_seekKeySlotLow));
        }
        if (_seekKeySlotHigh) {
            bob.appendNumber("seekKeySlotHigh", static_cast<long long>(*_seekKeySlotHigh));
        }
        bob.append("outputSlots", _vars);
        bob.append("indexKeysToInclude", _indexKeysToInclude.to_string());
        ret->debugInfo = bob.obj();
    }

    return ret;
}

const SpecificStats* IndexScanStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> IndexScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    if (_seekKeySlotLow) {
        DebugPrinter::addIdentifier(ret, _seekKeySlotLow.get());
        if (_seekKeySlotHigh) {
            DebugPrinter::addIdentifier(ret, _seekKeySlotHigh.get());
        } else {
            DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
        }
    }

    if (_recordSlot) {
        DebugPrinter::addIdentifier(ret, _recordSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_recordIdSlot) {
        DebugPrinter::addIdentifier(ret, _recordIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    if (_snapshotIdSlot) {
        DebugPrinter::addIdentifier(ret, _snapshotIdSlot.get());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }

    ret.emplace_back(DebugPrinter::Block("[`"));
    size_t varIndex = 0;
    for (size_t keyIndex = 0; keyIndex < _indexKeysToInclude.size(); ++keyIndex) {
        if (!_indexKeysToInclude[keyIndex]) {
            continue;
        }
        if (varIndex) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        invariant(varIndex < _vars.size());
        DebugPrinter::addIdentifier(ret, _vars[varIndex++]);
        ret.emplace_back("=");
        ret.emplace_back(std::to_string(keyIndex));
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _collUuid.toString());
    ret.emplace_back("`\"");

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _indexName);
    ret.emplace_back("`\"");

    ret.emplace_back(_forward ? "true" : "false");

    return ret;
}
}  // namespace mongo::sbe
