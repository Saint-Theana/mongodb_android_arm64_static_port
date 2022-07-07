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

#include "mongo/db/exec/sbe/stages/makeobj.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/util/str.h"

namespace mongo::sbe {
template <MakeObjOutputType O>
MakeObjStageBase<O>::MakeObjStageBase(std::unique_ptr<PlanStage> input,
                                      value::SlotId objSlot,
                                      boost::optional<value::SlotId> rootSlot,
                                      boost::optional<FieldBehavior> fieldBehavior,
                                      std::vector<std::string> fields,
                                      std::vector<std::string> projectFields,
                                      value::SlotVector projectVars,
                                      bool forceNewObject,
                                      bool returnOldObject,
                                      PlanNodeId planNodeId)
    : PlanStage(O == MakeObjOutputType::object ? "mkobj"_sd : "mkbson"_sd, planNodeId),
      _objSlot(objSlot),
      _rootSlot(rootSlot),
      _fieldBehavior(fieldBehavior),
      _fields(std::move(fields)),
      _projectFields(std::move(projectFields)),
      _projectVars(std::move(projectVars)),
      _forceNewObject(forceNewObject),
      _returnOldObject(returnOldObject) {
    _children.emplace_back(std::move(input));
    invariant(_projectVars.size() == _projectFields.size());
    invariant(static_cast<bool>(rootSlot) == static_cast<bool>(fieldBehavior));
}

template <MakeObjOutputType O>
std::unique_ptr<PlanStage> MakeObjStageBase<O>::clone() const {
    return std::make_unique<MakeObjStageBase<O>>(_children[0]->clone(),
                                                 _objSlot,
                                                 _rootSlot,
                                                 _fieldBehavior,
                                                 _fields,
                                                 _projectFields,
                                                 _projectVars,
                                                 _forceNewObject,
                                                 _returnOldObject,
                                                 _commonStats.nodeId);
}

template <MakeObjOutputType O>
void MakeObjStageBase<O>::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    if (_rootSlot) {
        _root = _children[0]->getAccessor(ctx, *_rootSlot);
    }
    for (auto& p : _fields) {
        auto [it, inserted] = _fieldSet.emplace(p);
        uassert(4822818, str::stream() << "duplicate field: " << p, inserted);
    }

    for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
        auto& p = _projectFields[idx];

        auto [it, inserted] = _projectFieldsMap.emplace(p, idx);
        uassert(4822819, str::stream() << "duplicate field: " << p, inserted);
        _projects.emplace_back(p, _children[0]->getAccessor(ctx, _projectVars[idx]));
    }
    _alreadyProjected.resize(_projectFields.size());

    _compiled = true;
}

template <MakeObjOutputType O>
value::SlotAccessor* MakeObjStageBase<O>::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled && slot == _objSlot) {
        return &_obj;
    } else {
        return _children[0]->getAccessor(ctx, slot);
    }
}

template <MakeObjOutputType O>
void MakeObjStageBase<O>::projectField(value::Object* obj, size_t idx) {
    const auto& p = _projects[idx];

    auto [tag, val] = p.second->getViewOfValue();
    if (tag != value::TypeTags::Nothing) {
        auto [tagCopy, valCopy] = value::copyValue(tag, val);
        obj->push_back(p.first, tagCopy, valCopy);
    }
}

template <MakeObjOutputType O>
void MakeObjStageBase<O>::projectField(UniqueBSONObjBuilder* bob, size_t idx) {
    const auto& p = _projects[idx];

    auto [tag, val] = p.second->getViewOfValue();
    bson::appendValueToBsonObj(*bob, p.first, tag, val);
}

template <MakeObjOutputType O>
void MakeObjStageBase<O>::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
}

template <>
void MakeObjStageBase<MakeObjOutputType::object>::produceObject() {
    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    resetAlreadyProjected();

    _obj.reset(tag, val);

    if (_root) {
        auto [tag, val] = _root->getViewOfValue();

        if (tag == value::TypeTags::bsonObject) {
            size_t nFieldsNeededIfInclusion = _projects.size() + _fieldSet.size();
            auto be = value::bitcastTo<const char*>(val);
            auto size = ConstDataView(be).read<LittleEndian<uint32_t>>();
            auto end = be + size;
            // Simple heuristic to determine number of fields.
            obj->reserve(size / 16);
            // Skip document length.
            be += 4;
            while (*be != 0) {
                auto sv = bson::fieldNameView(be);
                auto key = StringMapHasher{}.hashed_key(StringData(sv));

                bool projected = false;

                // This is an extremely hot path and our benchmarks have shown that
                // checking whether the projected fields map is empty before doing the lookup
                // can make a big impact for the common case where there are no projected fields.
                if (!_projectFieldsMap.empty()) {
                    if (auto it = _projectFieldsMap.find(key); it != _projectFieldsMap.end()) {
                        projectField(obj, it->second);
                        _alreadyProjected[it->second] = true;
                        --nFieldsNeededIfInclusion;
                        projected = true;
                    }
                }

                if (!projected && !isFieldRestricted(key)) {
                    auto [tag, val] = bson::convertFrom<true>(be, end, sv.size());
                    auto [copyTag, copyVal] = value::copyValue(tag, val);
                    obj->push_back(sv, copyTag, copyVal);
                    --nFieldsNeededIfInclusion;
                }

                if (nFieldsNeededIfInclusion == 0 && _fieldBehavior == FieldBehavior::keep) {
                    return;
                }

                be = bson::advance(be, sv.size());
            }
        } else if (tag == value::TypeTags::Object) {
            size_t nFieldsNeededIfInclusion = _projectFieldsMap.size() + _fieldSet.size();
            auto objRoot = value::getObjectView(val);
            obj->reserve(objRoot->size());
            for (size_t idx = 0; idx < objRoot->size(); ++idx) {
                auto sv = objRoot->field(idx);
                auto key = StringMapHasher{}.hashed_key(StringData(sv));

                bool projected = false;

                // This is an extremely hot path and our benchmarks have shown that
                // checking whether the projected fields map is empty before doing the lookup
                // can make a big impact for the common case where there are no projected fields.
                if (!_projectFieldsMap.empty()) {
                    if (auto it = _projectFieldsMap.find(key); it != _projectFieldsMap.end()) {
                        projectField(obj, it->second);
                        _alreadyProjected[it->second] = true;
                        --nFieldsNeededIfInclusion;
                        projected = true;
                    }
                }

                if (!projected && !isFieldRestricted(key)) {
                    auto [tag, val] = objRoot->getAt(idx);
                    auto [copyTag, copyVal] = value::copyValue(tag, val);
                    obj->push_back(sv, copyTag, copyVal);
                    --nFieldsNeededIfInclusion;
                }

                if (nFieldsNeededIfInclusion == 0 && _fieldBehavior == FieldBehavior::keep) {
                    return;
                }
            }
        } else {
            for (size_t idx = 0; idx < _projects.size(); ++idx) {
                if (!_alreadyProjected[idx]) {
                    projectField(obj, idx);
                }
            }
            // If the result is non empty object return it.
            if (obj->size() || _forceNewObject) {
                return;
            }
            // Now we have to make a decision - return Nothing or the original _root.
            if (!_returnOldObject) {
                _obj.reset(false, value::TypeTags::Nothing, 0);
            } else {
                // _root is not an object return it unmodified.
                _obj.reset(false, tag, val);
            }
            return;
        }
    }
    for (size_t idx = 0; idx < _projects.size(); ++idx) {
        if (!_alreadyProjected[idx]) {
            projectField(obj, idx);
        }
    }
}

template <>
void MakeObjStageBase<MakeObjOutputType::bsonObject>::produceObject() {
    UniqueBSONObjBuilder bob;
    resetAlreadyProjected();

    auto finish = [this, &bob]() {
        bob.doneFast();
        char* data = bob.bb().release().release();
        _obj.reset(value::TypeTags::bsonObject, value::bitcastFrom<char*>(data));
    };

    if (_root) {
        auto [tag, val] = _root->getViewOfValue();

        if (tag == value::TypeTags::bsonObject) {
            size_t nFieldsNeededIfInclusion = _projects.size() + _fieldSet.size();
            auto be = value::bitcastTo<const char*>(val);
            // Skip document length.
            be += 4;
            while (*be != 0) {
                auto sv = bson::fieldNameView(be);
                auto key = StringMapHasher{}.hashed_key(StringData(sv));

                bool projected = false;

                // This is an extremely hot path and our benchmarks have shown that
                // checking whether the projected fields map is empty before doing the lookup
                // can make a big impact for the common case where there are no projected fields.
                if (!_projectFieldsMap.empty()) {
                    if (auto it = _projectFieldsMap.find(key); it != _projectFieldsMap.end()) {
                        projected = true;
                        projectField(&bob, it->second);
                        _alreadyProjected[it->second] = true;
                        --nFieldsNeededIfInclusion;
                    }
                }

                if (!projected && !isFieldRestricted(key)) {
                    bob.append(BSONElement(be, sv.size() + 1, -1, BSONElement::CachedSizeTag{}));
                    --nFieldsNeededIfInclusion;
                }

                if (nFieldsNeededIfInclusion == 0 && _fieldBehavior == FieldBehavior::keep) {
                    finish();
                    return;
                }

                be = bson::advance(be, sv.size());
            }
        } else if (tag == value::TypeTags::Object) {
            auto objRoot = value::getObjectView(val);
            size_t nFieldsNeededIfInclusion = _projectFieldsMap.size() + _fieldSet.size();
            for (size_t idx = 0; idx < objRoot->size(); ++idx) {
                auto key = StringMapHasher{}.hashed_key(StringData(objRoot->field(idx)));

                bool projected = false;

                // This is an extremely hot path and our benchmarks have shown that
                // checking whether the projected fields map is empty before doing the lookup
                // can make a big impact for the common case where there are no projected fields.
                if (!_projectFieldsMap.empty()) {
                    if (auto it = _projectFieldsMap.find(key); it != _projectFieldsMap.end()) {
                        projected = true;
                        projectField(&bob, it->second);
                        _alreadyProjected[it->second] = true;
                        --nFieldsNeededIfInclusion;
                    }
                }

                if (!projected && !isFieldRestricted(key)) {
                    auto [tag, val] = objRoot->getAt(idx);
                    bson::appendValueToBsonObj(bob, objRoot->field(idx), tag, val);
                    --nFieldsNeededIfInclusion;
                }

                if (nFieldsNeededIfInclusion == 0 && _fieldBehavior == FieldBehavior::keep) {
                    finish();
                    return;
                }
            }
        } else {
            for (size_t idx = 0; idx < _projects.size(); ++idx) {
                if (!_alreadyProjected[idx]) {
                    projectField(&bob, idx);
                }
            }
            // If the result is non empty object return it.
            if (!bob.asTempObj().isEmpty() || _forceNewObject) {
                finish();
                return;
            }
            // Now we have to make a decision - return Nothing or the original _root.
            if (!_returnOldObject) {
                _obj.reset(false, value::TypeTags::Nothing, 0);
            } else {
                // _root is not an object return it unmodified.
                _obj.reset(false, tag, val);
            }

            return;
        }
    }
    for (size_t idx = 0; idx < _projects.size(); ++idx) {
        if (!_alreadyProjected[idx]) {
            projectField(&bob, idx);
        }
    }
    finish();
}

template <MakeObjOutputType O>
PlanState MakeObjStageBase<O>::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call getNext() on our child so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the getNext() call.
    disableSlotAccess();
    auto state = _children[0]->getNext();

    if (state == PlanState::ADVANCED) {
        produceObject();
    }
    return trackPlanState(state);
}

template <MakeObjOutputType O>
void MakeObjStageBase<O>::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

template <MakeObjOutputType O>
std::unique_ptr<PlanStageStats> MakeObjStageBase<O>::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("objSlot", static_cast<long long>(_objSlot));
        if (_rootSlot) {
            bob.appendNumber("rootSlot", static_cast<long long>(*_rootSlot));
        }
        if (_fieldBehavior) {
            bob.append("fieldBehavior", *_fieldBehavior == FieldBehavior::drop ? "drop" : "keep");
        }
        bob.append("fields", _fields);
        bob.append("projectFields", _projectFields);
        bob.append("projectSlots", _projectVars);
        bob.append("forceNewObject", _forceNewObject);
        bob.append("returnOldObject", _returnOldObject);
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

template <MakeObjOutputType O>
const SpecificStats* MakeObjStageBase<O>::getSpecificStats() const {
    return nullptr;
}

template <MakeObjOutputType O>
std::vector<DebugPrinter::Block> MakeObjStageBase<O>::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _objSlot);

    if (_rootSlot) {
        DebugPrinter::addIdentifier(ret, *_rootSlot);

        ret.emplace_back(DebugPrinter::Block("[`"));
        for (size_t idx = 0; idx < _fields.size(); ++idx) {
            if (idx) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }

            DebugPrinter::addIdentifier(ret, _fields[idx]);
        }
        ret.emplace_back(DebugPrinter::Block("`]"));

        ret.emplace_back(*_fieldBehavior == FieldBehavior::drop ? "drop" : "keep");
    }

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _projectFields[idx]);
        ret.emplace_back("=");
        DebugPrinter::addIdentifier(ret, _projectVars[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(_forceNewObject ? "true" : "false");
    ret.emplace_back(_returnOldObject ? "true" : "false");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

template <MakeObjOutputType O>
void MakeObjStageBase<O>::doSaveState() {
    if (!slotsAccessible()) {
        return;
    }

    _obj.makeOwned();
}

// Explicit template instantiations.
template class MakeObjStageBase<MakeObjOutputType::object>;
template class MakeObjStageBase<MakeObjOutputType::bsonObject>;
}  // namespace mongo::sbe
