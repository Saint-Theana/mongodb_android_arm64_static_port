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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/vm/vm.h"

#include <boost/algorithm/string.hpp>
#include <pcre.h>

#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/js_function.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/sort_spec.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/datetime.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/summation.h"

MONGO_FAIL_POINT_DEFINE(failOnPoisonedFieldLookup);

namespace mongo {
namespace sbe {
namespace vm {

/*
 * This table must be kept in sync with Instruction::Tags. It encodes how the instruction affects
 * the stack; i.e. push(+1), pop(-1), or no effect.
 */
int Instruction::stackOffset[Instruction::Tags::lastInstruction] = {
    1,   // pushConstVal
    1,   // pushAccessVal
    1,   // pushMoveVal
    1,   // pushLocalVal
    -1,  // pop
    0,   // swap

    -1,  // add
    -1,  // sub
    -1,  // mul
    -1,  // div
    -1,  // idiv
    -1,  // mod
    0,   // negate
    0,   // numConvert

    0,  // logicNot

    -1,  // less
    -1,  // lessEq
    -1,  // greater
    -1,  // greaterEq
    -1,  // eq
    -1,  // neq
    -1,  // cmp3w

    -2,  // collLess
    -2,  // collLessEq
    -2,  // collGreater
    -2,  // collGreaterEq
    -2,  // collEq
    -2,  // collNeq
    -2,  // collCmp3w

    -1,  // fillEmpty
    -1,  // getField
    -1,  // getElement
    -1,  // collComparisonKey

    -1,  // aggSum
    -1,  // aggMin
    -1,  // aggMax
    -1,  // aggFirst
    -1,  // aggLast

    -1,  // aggCollMin
    -1,  // aggCollMax

    0,  // exists
    0,  // isNull
    0,  // isObject
    0,  // isArray
    0,  // isString
    0,  // isNumber
    0,  // isBinData
    0,  // isDate
    0,  // isNaN
    0,  // isRecordId
    0,  // isMinKey
    0,  // isMaxKey
    0,  // typeMatch

    0,  // function is special, the stack offset is encoded in the instruction itself
    0,  // functionSmall is special, the stack offset is encoded in the instruction itself

    0,   // jmp
    -1,  // jmpTrue
    0,   // jmpNothing

    -1,  // fail
};

namespace {
template <typename T>
T readFromMemory(const uint8_t* ptr) noexcept {
    static_assert(!IsEndian<T>::value);

    T val;
    memcpy(&val, ptr, sizeof(T));
    return val;
}

template <typename T>
size_t writeToMemory(uint8_t* ptr, const T val) noexcept {
    static_assert(!IsEndian<T>::value);

    memcpy(ptr, &val, sizeof(T));
    return sizeof(T);
}
}  // namespace

void CodeFragment::adjustStackSimple(const Instruction& i) {
    _stackSize += Instruction::stackOffset[i.tag];
}

void CodeFragment::fixup(int offset) {
    for (auto fixUp : _fixUps) {
        auto ptr = instrs().data() + fixUp.offset;
        int newOffset = readFromMemory<int>(ptr) + offset;
        writeToMemory(ptr, newOffset);
    }
}

void CodeFragment::removeFixup(FrameId frameId) {
    _fixUps.erase(std::remove_if(_fixUps.begin(),
                                 _fixUps.end(),
                                 [frameId](const auto& f) { return f.frameId == frameId; }),
                  _fixUps.end());
}

void CodeFragment::copyCodeAndFixup(const CodeFragment& from) {
    for (auto fixUp : from._fixUps) {
        fixUp.offset += _instrs.size();
        _fixUps.push_back(fixUp);
    }

    _instrs.insert(_instrs.end(), from._instrs.begin(), from._instrs.end());
}

void CodeFragment::append(std::unique_ptr<CodeFragment> code) {
    // Fixup before copying.
    code->fixup(_stackSize);

    copyCodeAndFixup(*code);

    _stackSize += code->_stackSize;
}

void CodeFragment::append(std::unique_ptr<CodeFragment> lhs, std::unique_ptr<CodeFragment> rhs) {
    invariant(lhs->stackSize() == rhs->stackSize());

    // Fixup before copying.
    lhs->fixup(_stackSize);
    rhs->fixup(_stackSize);

    copyCodeAndFixup(*lhs);
    copyCodeAndFixup(*rhs);

    _stackSize += lhs->_stackSize;
}

void CodeFragment::appendConstVal(value::TypeTags tag, value::Value val) {
    Instruction i;
    i.tag = Instruction::pushConstVal;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(tag) + sizeof(val));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, tag);
    offset += writeToMemory(offset, val);
}

void CodeFragment::appendAccessVal(value::SlotAccessor* accessor) {
    Instruction i;
    i.tag = Instruction::pushAccessVal;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(accessor));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, accessor);
}

void CodeFragment::appendMoveVal(value::SlotAccessor* accessor) {
    Instruction i;
    i.tag = Instruction::pushMoveVal;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(accessor));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, accessor);
}

void CodeFragment::appendLocalVal(FrameId frameId, int stackOffset) {
    Instruction i;
    i.tag = Instruction::pushLocalVal;
    adjustStackSimple(i);

    auto fixUpOffset = _instrs.size() + sizeof(Instruction);
    _fixUps.push_back(FixUp{frameId, fixUpOffset});

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(stackOffset));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, stackOffset);
}

void CodeFragment::appendAdd() {
    appendSimpleInstruction(Instruction::add);
}

void CodeFragment::appendNumericConvert(value::TypeTags targetTag) {
    Instruction i;
    i.tag = Instruction::numConvert;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(targetTag));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, targetTag);
}

void CodeFragment::appendSub() {
    appendSimpleInstruction(Instruction::sub);
}

void CodeFragment::appendMul() {
    appendSimpleInstruction(Instruction::mul);
}

void CodeFragment::appendDiv() {
    appendSimpleInstruction(Instruction::div);
}

void CodeFragment::appendIDiv() {
    appendSimpleInstruction(Instruction::idiv);
}

void CodeFragment::appendMod() {
    appendSimpleInstruction(Instruction::mod);
}

void CodeFragment::appendNegate() {
    appendSimpleInstruction(Instruction::negate);
}

void CodeFragment::appendNot() {
    appendSimpleInstruction(Instruction::logicNot);
}

void CodeFragment::appendSimpleInstruction(Instruction::Tags tag) {
    Instruction i;
    i.tag = tag;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction));

    offset += writeToMemory(offset, i);
}

void CodeFragment::appendGetField() {
    appendSimpleInstruction(Instruction::getField);
}

void CodeFragment::appendGetElement() {
    appendSimpleInstruction(Instruction::getElement);
}

void CodeFragment::appendCollComparisonKey() {
    appendSimpleInstruction(Instruction::collComparisonKey);
}

void CodeFragment::appendSum() {
    appendSimpleInstruction(Instruction::aggSum);
}

void CodeFragment::appendMin() {
    appendSimpleInstruction(Instruction::aggMin);
}

void CodeFragment::appendMax() {
    appendSimpleInstruction(Instruction::aggMax);
}

void CodeFragment::appendFirst() {
    appendSimpleInstruction(Instruction::aggFirst);
}

void CodeFragment::appendLast() {
    appendSimpleInstruction(Instruction::aggLast);
}

void CodeFragment::appendCollMin() {
    appendSimpleInstruction(Instruction::aggCollMin);
}

void CodeFragment::appendCollMax() {
    appendSimpleInstruction(Instruction::aggCollMax);
}

void CodeFragment::appendExists() {
    appendSimpleInstruction(Instruction::exists);
}

void CodeFragment::appendIsNull() {
    appendSimpleInstruction(Instruction::isNull);
}

void CodeFragment::appendIsObject() {
    appendSimpleInstruction(Instruction::isObject);
}

void CodeFragment::appendIsArray() {
    appendSimpleInstruction(Instruction::isArray);
}

void CodeFragment::appendIsString() {
    appendSimpleInstruction(Instruction::isString);
}

void CodeFragment::appendIsNumber() {
    appendSimpleInstruction(Instruction::isNumber);
}

void CodeFragment::appendIsBinData() {
    appendSimpleInstruction(Instruction::isBinData);
}

void CodeFragment::appendIsDate() {
    appendSimpleInstruction(Instruction::isDate);
}

void CodeFragment::appendIsNaN() {
    appendSimpleInstruction(Instruction::isNaN);
}

void CodeFragment::appendIsRecordId() {
    appendSimpleInstruction(Instruction::isRecordId);
}

void CodeFragment::appendTypeMatch(uint32_t typeMask) {
    Instruction i;
    i.tag = Instruction::typeMatch;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(typeMask));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, typeMask);
}

void CodeFragment::appendFunction(Builtin f, ArityType arity) {
    Instruction i;
    const bool isSmallArity = (arity <= std::numeric_limits<SmallArityType>::max());
    i.tag = isSmallArity ? Instruction::functionSmall : Instruction::function;

    // Account for consumed arguments
    _stackSize -= arity;
    // and the return value.
    _stackSize += 1;

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(f) +
                                (isSmallArity ? sizeof(SmallArityType) : sizeof(ArityType)));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, f);
    offset += isSmallArity ? writeToMemory(offset, static_cast<SmallArityType>(arity))
                           : writeToMemory(offset, arity);
}

void CodeFragment::appendJump(int jumpOffset) {
    Instruction i;
    i.tag = Instruction::jmp;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(jumpOffset));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, jumpOffset);
}

void CodeFragment::appendJumpTrue(int jumpOffset) {
    Instruction i;
    i.tag = Instruction::jmpTrue;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(jumpOffset));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, jumpOffset);
}

void CodeFragment::appendJumpNothing(int jumpOffset) {
    Instruction i;
    i.tag = Instruction::jmpNothing;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(jumpOffset));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, jumpOffset);
}

ByteCode::~ByteCode() {
    auto size = _argStackOwned.size();
    invariant(_argStackTags.size() == size);
    invariant(_argStackVals.size() == size);
    for (size_t i = 0; i < size; ++i) {
        if (_argStackOwned[i]) {
            value::releaseValue(_argStackTags[i], _argStackVals[i]);
        }
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::getField(value::TypeTags objTag,
                                                                   value::Value objValue,
                                                                   value::TypeTags fieldTag,
                                                                   value::Value fieldValue) {
    if (!value::isString(fieldTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto fieldStr = value::getStringView(fieldTag, fieldValue);

    if (MONGO_unlikely(failOnPoisonedFieldLookup.shouldFail())) {
        uassert(4623399, "Lookup of $POISON", fieldStr != "POISON");
    }

    if (objTag == value::TypeTags::Object) {
        auto [tag, val] = value::getObjectView(objValue)->getField(fieldStr);
        return {false, tag, val};
    } else if (objTag == value::TypeTags::bsonObject) {
        auto be = value::bitcastTo<const char*>(objValue);
        auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        // Skip document length.
        be += 4;
        while (*be != 0) {
            auto sv = bson::fieldNameView(be);

            if (sv == fieldStr) {
                auto [tag, val] = bson::convertFrom<true>(be, end, sv.size());
                return {false, tag, val};
            }

            be = bson::advance(be, sv.size());
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::getElement(value::TypeTags arrTag,
                                                                     value::Value arrValue,
                                                                     value::TypeTags idxTag,
                                                                     value::Value idxValue) {
    // We need to ensure that 'size_t' is wide enough to store 32-bit index.
    static_assert(sizeof(size_t) >= sizeof(int32_t), "size_t must be at least 32-bits");

    if (!value::isArray(arrTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    if (idxTag != value::TypeTags::NumberInt32) {
        return {false, value::TypeTags::Nothing, 0};
    }

    const auto idxInt32 = value::bitcastTo<int32_t>(idxValue);
    const bool isNegative = idxInt32 < 0;

    size_t idx = 0;
    if (isNegative) {
        // Upcast 'idxInt32' to 'int64_t' prevent overflow during the sign change.
        idx = static_cast<size_t>(-static_cast<int64_t>(idxInt32));
    } else {
        idx = static_cast<size_t>(idxInt32);
    }

    if (arrTag == value::TypeTags::Array) {
        // If `arr` is an SBE array, use Array::getAt() to retrieve the element at index `idx`.
        auto arrayView = value::getArrayView(arrValue);

        size_t convertedIdx = idx;
        if (isNegative) {
            if (idx > arrayView->size()) {
                return {false, value::TypeTags::Nothing, 0};
            }
            convertedIdx = arrayView->size() - idx;
        }

        auto [tag, val] = value::getArrayView(arrValue)->getAt(convertedIdx);
        return {false, tag, val};
    } else if (arrTag == value::TypeTags::bsonArray || arrTag == value::TypeTags::ArraySet) {
        value::ArrayEnumerator enumerator(arrTag, arrValue);

        if (!isNegative) {
            // Loop through array until we meet element at position 'idx'.
            size_t i = 0;
            while (i < idx && !enumerator.atEnd()) {
                i++;
                enumerator.advance();
            }
            // If the array didn't have an element at index `idx`, return Nothing.
            if (enumerator.atEnd()) {
                return {false, value::TypeTags::Nothing, 0};
            }
            auto [tag, val] = enumerator.getViewOfValue();
            return {false, tag, val};
        }

        // For negative indexes we use two pointers approach. We start two array enumerators at the
        // distance of 'idx' and move them at the same time. Once one of the enumerators reaches the
        // end of the array, the second one points to the element at position '-idx'.
        //
        // First, move one of the enumerators 'idx' elements forward.
        size_t i = 0;
        while (i < idx && !enumerator.atEnd()) {
            enumerator.advance();
            i++;
        }

        if (i != idx) {
            // Array is too small to have an element at the requested index.
            return {false, value::TypeTags::Nothing, 0};
        }

        // Initiate second enumerator at the start of the array. Now the distance between
        // 'enumerator' and 'windowEndEnumerator' is exactly 'idx' elements. Move both enumerators
        // until the first one reaches the end of the array.
        value::ArrayEnumerator windowEndEnumerator(arrTag, arrValue);
        while (!enumerator.atEnd() && !windowEndEnumerator.atEnd()) {
            enumerator.advance();
            windowEndEnumerator.advance();
        }
        invariant(enumerator.atEnd());
        invariant(!windowEndEnumerator.atEnd());

        auto [tag, val] = windowEndEnumerator.getViewOfValue();
        return {false, tag, val};
    } else {
        // Earlier in this function we bailed out if the `arrTag` wasn't Array, ArraySet or
        // bsonArray, so it should be impossible to reach this point.
        MONGO_UNREACHABLE
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggSum(value::TypeTags accTag,
                                                                 value::Value accValue,
                                                                 value::TypeTags fieldTag,
                                                                 value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        accTag = value::TypeTags::NumberInt64;
        accValue = value::bitcastFrom<int64_t>(0);
    }

    return genericAdd(accTag, accValue, fieldTag, fieldValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggMin(value::TypeTags accTag,
                                                                 value::Value accValue,
                                                                 value::TypeTags fieldTag,
                                                                 value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    auto [tag, val] = genericCompare<std::less<>>(accTag, accValue, fieldTag, fieldValue);

    if (tag == value::TypeTags::Boolean && value::bitcastTo<bool>(val)) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    } else {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggCollMin(value::TypeTags accTag,
                                                                     value::Value accValue,
                                                                     value::TypeTags collTag,
                                                                     value::Value collValue,
                                                                     value::TypeTags fieldTag,
                                                                     value::Value fieldValue) {
    // Skip aggregation step if we don't have the input or if the collation is Nothing or an
    // unexpected type.
    if (fieldTag == value::TypeTags::Nothing || collTag != value::TypeTags::collator) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    auto collator = value::getCollatorView(collValue);

    auto [tag, val] = genericCompare<std::less<>>(accTag, accValue, fieldTag, fieldValue, collator);

    if (tag == value::TypeTags::Boolean && value::bitcastTo<bool>(val)) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    } else {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggMax(value::TypeTags accTag,
                                                                 value::Value accValue,
                                                                 value::TypeTags fieldTag,
                                                                 value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    auto [tag, val] = genericCompare<std::greater<>>(accTag, accValue, fieldTag, fieldValue);

    if (tag == value::TypeTags::Boolean && value::bitcastTo<bool>(val)) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    } else {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggCollMax(value::TypeTags accTag,
                                                                     value::Value accValue,
                                                                     value::TypeTags collTag,
                                                                     value::Value collValue,
                                                                     value::TypeTags fieldTag,
                                                                     value::Value fieldValue) {
    // Skip aggregation step if we don't have the input or if the collation is Nothing or an
    // unexpected type.
    if (fieldTag == value::TypeTags::Nothing || collTag != value::TypeTags::collator) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    auto collator = value::getCollatorView(collValue);

    auto [tag, val] =
        genericCompare<std::greater<>>(accTag, accValue, fieldTag, fieldValue, collator);

    if (tag == value::TypeTags::Boolean && value::bitcastTo<bool>(val)) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    } else {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggFirst(value::TypeTags accTag,
                                                                   value::Value accValue,
                                                                   value::TypeTags fieldTag,
                                                                   value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    // Disregard the next value, always return the first one.
    auto [tag, val] = value::copyValue(accTag, accValue);
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggLast(value::TypeTags accTag,
                                                                  value::Value accValue,
                                                                  value::TypeTags fieldTag,
                                                                  value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    // Disregard the accumulator, always return the next value.
    auto [tag, val] = value::copyValue(fieldTag, fieldValue);
    return {true, tag, val};
}


bool hasSeparatorAt(size_t idx, StringData input, StringData separator) {
    return (idx + separator.size() <= input.size()) &&
        input.substr(idx, separator.size()) == separator;
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinSplit(ArityType arity) {
    auto [ownedSeparator, tagSeparator, valSeparator] = getFromStack(1);
    auto [ownedInput, tagInput, valInput] = getFromStack(0);

    if (!value::isString(tagSeparator) || !value::isString(tagInput)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto input = value::getStringView(tagInput, valInput);
    auto separator = value::getStringView(tagSeparator, valSeparator);

    auto [tag, val] = value::makeNewArray();
    auto arr = value::getArrayView(val);
    value::ValueGuard guard{tag, val};

    size_t splitPos;
    while ((splitPos = input.find(separator)) != std::string::npos) {
        auto [tag, val] = value::makeNewString(input.substr(0, splitPos));
        arr->push_back(tag, val);

        splitPos += separator.size();
        input = input.substr(splitPos);
    }

    // This is the last string.
    {
        auto [tag, val] = value::makeNewString(input);
        arr->push_back(tag, val);
    }

    guard.reset();
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDropFields(ArityType arity) {
    auto [ownedSeparator, tagInObj, valInObj] = getFromStack(0);

    // We operate only on objects.
    if (!value::isObject(tagInObj)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Build the set of fields to drop.
    std::set<std::string, std::less<>> restrictFieldsSet;
    for (ArityType idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);

        if (!value::isString(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        restrictFieldsSet.emplace(value::getStringView(tag, val));
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    if (tagInObj == value::TypeTags::bsonObject) {
        auto be = value::bitcastTo<const char*>(valInObj);
        auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        // Skip document length.
        be += 4;
        while (*be != 0) {
            auto sv = bson::fieldNameView(be);

            if (restrictFieldsSet.count(sv) == 0) {
                auto [tag, val] = bson::convertFrom<false>(be, end, sv.size());
                obj->push_back(sv, tag, val);
            }

            be = bson::advance(be, sv.size());
        }
    } else if (tagInObj == value::TypeTags::Object) {
        auto objRoot = value::getObjectView(valInObj);
        for (size_t idx = 0; idx < objRoot->size(); ++idx) {
            StringData sv(objRoot->field(idx));

            if (restrictFieldsSet.count(sv) == 0) {

                auto [tag, val] = objRoot->getAt(idx);
                auto [copyTag, copyVal] = value::copyValue(tag, val);
                obj->push_back(sv, copyTag, copyVal);
            }
        }
    }

    guard.reset();
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewArray(ArityType arity) {
    auto [tag, val] = value::makeNewArray();
    value::ValueGuard guard{tag, val};

    auto arr = value::getArrayView(val);

    for (ArityType idx = 0; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);
        auto [tagCopy, valCopy] = value::copyValue(tag, val);
        arr->push_back(tagCopy, valCopy);
    }

    guard.reset();
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewObj(ArityType arity) {
    std::vector<value::TypeTags> typeTags;
    std::vector<value::Value> values;
    std::vector<std::string> names;

    for (ArityType idx = 0; idx < arity; idx += 2) {
        {
            auto [owned, tag, val] = getFromStack(idx);

            if (!value::isString(tag)) {
                return {false, value::TypeTags::Nothing, 0};
            }

            names.emplace_back(value::getStringView(tag, val));
        }
        {
            auto [owned, tag, val] = getFromStack(idx + 1);
            typeTags.push_back(tag);
            values.push_back(val);
        }
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    for (size_t idx = 0; idx < typeTags.size(); ++idx) {
        auto [tagCopy, valCopy] = value::copyValue(typeTags[idx], values[idx]);
        obj->push_back(names[idx], tagCopy, valCopy);
    }

    guard.reset();
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinKeyStringToString(
    ArityType arity) {
    auto [owned, tagInKey, valInKey] = getFromStack(0);

    // We operate only on keys.
    if (tagInKey != value::TypeTags::ksValue) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto key = value::getKeyStringView(valInKey);

    auto [tagStr, valStr] = value::makeNewString(key->toString());

    return {true, tagStr, valStr};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewKeyString(ArityType arity) {
    auto [_, tagInVersion, valInVersion] = getFromStack(0);

    if (!value::isNumber(tagInVersion) ||
        !(value::numericCast<int64_t>(tagInVersion, valInVersion) == 0 ||
          value::numericCast<int64_t>(tagInVersion, valInVersion) == 1)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    KeyString::Version version =
        static_cast<KeyString::Version>(value::numericCast<int64_t>(tagInVersion, valInVersion));

    auto [__, tagInOrdering, valInOrdering] = getFromStack(1);
    if (!value::isNumber(tagInOrdering)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto orderingBits = value::numericCast<int32_t>(tagInOrdering, valInOrdering);
    BSONObjBuilder bb;
    for (size_t i = 0; i < Ordering::kMaxCompoundIndexKeys; ++i) {
        bb.append(""_sd, (orderingBits & (1 << i)) ? 1 : 0);
    }

    KeyString::HeapBuilder kb{version, Ordering::make(bb.done())};

    for (size_t idx = 2; idx < arity - 1u; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        if (value::isNumber(tag)) {
            auto num = value::numericCast<int64_t>(tag, val);
            kb.appendNumberLong(num);
        } else if (value::isString(tag)) {
            auto str = value::getStringView(tag, val);
            kb.appendString(str);
        } else {
            uasserted(4822802, "unsuppored key string type");
        }
    }

    auto [___, tagInDisrim, valInDiscrim] = getFromStack(arity - 1);
    if (!value::isNumber(tagInDisrim)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto discrimNum = value::numericCast<int64_t>(tagInDisrim, valInDiscrim);
    if (discrimNum < 0 || discrimNum > 2) {
        return {false, value::TypeTags::Nothing, 0};
    }

    kb.appendDiscriminator(static_cast<KeyString::Discriminator>(discrimNum));

    return {true,
            value::TypeTags::ksValue,
            value::bitcastFrom<KeyString::Value*>(new KeyString::Value(kb.release()))};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAbs(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericAbs(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCeil(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericCeil(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinFloor(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericFloor(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinTrunc(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericTrunc(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinExp(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericExp(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinLn(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericLn(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinLog10(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericLog10(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinSqrt(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericSqrt(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToArray(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto [_, tagField, valField] = getFromStack(1);

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArray();
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};

    invariant(ownAgg && tagAgg == value::TypeTags::Array);
    auto arr = value::getArrayView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    auto [tagCopy, valCopy] = value::copyValue(tagField, valField);
    arr->push_back(tagCopy, valCopy);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToSet(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto [_, tagField, valField] = getFromStack(1);

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArraySet();
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};

    invariant(ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    auto [tagCopy, valCopy] = value::copyValue(tagField, valField);
    arr->push_back(tagCopy, valCopy);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollAddToSet(ArityType arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto [ownColl, tagColl, valColl] = getFromStack(1);
    auto [_, tagField, valField] = getFromStack(2);

    // If the collator is Nothing or if it's some unexpected type, don't push back the value
    // and just return the accumulator.
    if (tagColl != value::TypeTags::collator) {
        topStack(false, value::TypeTags::Nothing, 0);
        return {ownAgg, tagAgg, valAgg};
    }

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        ownAgg = true;
        std::tie(tagAgg, valAgg) = value::makeNewArraySet(value::getCollatorView(valColl));
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};

    invariant(ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // Push back the value. Note that array will ignore Nothing.
    auto [tagCopy, valCopy] = value::copyValue(tagField, valField);
    arr->push_back(tagCopy, valCopy);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinRunJsPredicate(ArityType arity) {
    invariant(arity == 2);

    auto [predicateOwned, predicateType, predicateValue] = getFromStack(0);
    auto [inputOwned, inputType, inputValue] = getFromStack(1);

    if (predicateType != value::TypeTags::jsFunction || !value::isObject(inputType)) {
        return {false, value::TypeTags::Nothing, value::bitcastFrom<int64_t>(0)};
    }

    BSONObj obj;
    if (inputType == value::TypeTags::Object) {
        BSONObjBuilder objBuilder;
        bson::convertToBsonObj(objBuilder, value::getObjectView(inputValue));
        obj = objBuilder.obj();
    } else if (inputType == value::TypeTags::bsonObject) {
        obj = BSONObj(value::getRawPointerView(inputValue));
    } else {
        MONGO_UNREACHABLE;
    }

    auto predicate = value::getJsFunctionView(predicateValue);
    auto predicateResult = predicate->runAsPredicate(obj);
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(predicateResult)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinReplaceOne(ArityType arity) {
    invariant(arity == 3);

    auto [ownedInputStr, typeTagInputStr, valueInputStr] = getFromStack(0);
    auto [ownedFindStr, typeTagFindStr, valueFindStr] = getFromStack(1);
    auto [ownedReplacementStr, typeTagReplacementStr, valueReplacementStr] = getFromStack(2);

    if (!value::isString(typeTagInputStr) || !value::isString(typeTagFindStr) ||
        !value::isString(typeTagReplacementStr)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto input = value::getStringView(typeTagInputStr, valueInputStr);
    auto find = value::getStringView(typeTagFindStr, valueFindStr);
    auto replacement = value::getStringView(typeTagReplacementStr, valueReplacementStr);

    // If find string is empty, return nothing, since an empty find will match every position in a
    // string.
    if (find.empty()) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // If find string is not found, return the original string.
    size_t startIndex = input.find(find);
    if (startIndex == std::string::npos) {
        topStack(false, value::TypeTags::Nothing, 0);
        return {ownedInputStr, typeTagInputStr, valueInputStr};
    }

    StringBuilder output;
    size_t endIndex = startIndex + find.size();
    output << input.substr(0, startIndex);
    output << replacement;
    output << input.substr(endIndex);

    auto strData = output.stringData();
    auto [outputStrTypeTag, outputStrValue] = sbe::value::makeNewString(strData);
    return {true, outputStrTypeTag, outputStrValue};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDoubleDoubleSum(ArityType arity) {
    invariant(arity >= 1);

    value::TypeTags resultTag = value::TypeTags::NumberInt32;
    bool haveDate = false;

    // Sweep across all tags and pick the result type.
    for (ArityType idx = 0; idx < arity; ++idx) {
        auto [own, tag, val] = getFromStack(idx);
        if (tag == value::TypeTags::Date) {
            if (haveDate) {
                uassert(4848404, "only one date allowed in an $add expression", !haveDate);
            }
            // Date is a simple 64 bit integer.
            haveDate = true;
            tag = value::TypeTags::NumberInt64;
        }
        if (value::isNumber(tag)) {
            resultTag = value::getWidestNumericalType(resultTag, tag);
        } else if (tag == value::TypeTags::Nothing || tag == value::TypeTags::Null) {
            // What to do about null and nothing?
            return {false, value::TypeTags::Nothing, 0};
        } else {
            // What to do about non-numeric types like arrays and objects?
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    if (resultTag == value::TypeTags::NumberDecimal) {
        Decimal128 sum;
        for (ArityType idx = 0; idx < arity; ++idx) {
            auto [own, tag, val] = getFromStack(idx);
            if (tag == value::TypeTags::Date) {
                sum = sum.add(Decimal128(value::bitcastTo<int64_t>(val)));
            } else {
                sum = sum.add(value::numericCast<Decimal128>(tag, val));
            }
        }
        if (haveDate) {
            return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(sum.toLong())};
        } else {
            auto [tag, val] = value::makeCopyDecimal(sum);
            return {true, tag, val};
        }
    } else {
        DoubleDoubleSummation sum;
        for (ArityType idx = 0; idx < arity; ++idx) {
            auto [own, tag, val] = getFromStack(idx);
            if (tag == value::TypeTags::NumberInt32) {
                sum.addInt(value::numericCast<int32_t>(tag, val));
            } else if (tag == value::TypeTags::NumberInt64) {
                sum.addLong(value::numericCast<int64_t>(tag, val));
            } else if (tag == value::TypeTags::NumberDouble) {
                sum.addDouble(value::numericCast<double>(tag, val));
            } else if (tag == value::TypeTags::Date) {
                sum.addLong(value::bitcastTo<int64_t>(val));
            }
        }
        if (haveDate) {
            uassert(ErrorCodes::Overflow, "date overflow in $add", sum.fitsLong());
            return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(sum.getLong())};
        } else {
            switch (resultTag) {
                case value::TypeTags::NumberInt32: {
                    auto result = sum.getLong();
                    if (sum.fitsLong() && result >= std::numeric_limits<int32_t>::min() &&
                        result <= std::numeric_limits<int32_t>::max()) {
                        return {false,
                                value::TypeTags::NumberInt32,
                                value::bitcastFrom<int32_t>(result)};
                    }
                    // Fall through to the larger type.
                }
                case value::TypeTags::NumberInt64: {
                    if (sum.fitsLong()) {
                        return {false,
                                value::TypeTags::NumberInt64,
                                value::bitcastFrom<int64_t>(sum.getLong())};
                    }
                    // Fall through to the larger type.
                }
                case value::TypeTags::NumberDouble: {
                    return {false,
                            value::TypeTags::NumberDouble,
                            value::bitcastFrom<double>(sum.getDouble())};
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}

/**
 * A helper for the builtinDate method. The formal parameters yearOrWeekYear and monthOrWeek carry
 * values depending on wether the date is a year-month-day or ISOWeekYear.
 */
using DateFn = std::function<Date_t(
    TimeZone, long long, long long, long long, long long, long long, long long, long long)>;
std::tuple<bool, value::TypeTags, value::Value> builtinDateHelper(
    DateFn computeDateFn,
    std::tuple<bool, value::TypeTags, value::Value> tzdb,
    std::tuple<bool, value::TypeTags, value::Value> yearOrWeekYear,
    std::tuple<bool, value::TypeTags, value::Value> monthOrWeek,
    std::tuple<bool, value::TypeTags, value::Value> day,
    std::tuple<bool, value::TypeTags, value::Value> hour,
    std::tuple<bool, value::TypeTags, value::Value> minute,
    std::tuple<bool, value::TypeTags, value::Value> second,
    std::tuple<bool, value::TypeTags, value::Value> millisecond,
    std::tuple<bool, value::TypeTags, value::Value> timezone) {

    auto [ownedTzdb, typeTagTzdb, valueTzdb] = tzdb;
    auto [ownedYearOrWeekYear, typeTagYearOrWeekYear, valueYearOrWeekYear] = yearOrWeekYear;
    auto [ownedMonthOrWeek, typeTagMonthOrWeek, valueMonthOrWeek] = monthOrWeek;
    auto [ownedDay, typeTagDay, valueDay] = day;
    auto [ownedHr, typeTagHr, valueHr] = hour;
    auto [ownedMin, typeTagMin, valueMin] = minute;
    auto [ownedSec, typeTagSec, valueSec] = second;
    auto [ownedMillis, typeTagMillis, valueMillis] = millisecond;
    auto [ownedTz, typeTagTz, valueTz] = timezone;

    if (typeTagTzdb != value::TypeTags::timeZoneDB || !value::isNumber(typeTagYearOrWeekYear) ||
        !value::isNumber(typeTagMonthOrWeek) || !value::isNumber(typeTagDay) ||
        !value::isNumber(typeTagHr) || !value::isNumber(typeTagMin) ||
        !value::isNumber(typeTagSec) || !value::isNumber(typeTagMillis) ||
        !value::isString(typeTagTz)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto timeZoneDB = value::getTimeZoneDBView(valueTzdb);
    invariant(timeZoneDB);

    auto tzString = value::getStringView(typeTagTz, valueTz);
    const auto tz = tzString == "" ? timeZoneDB->utcZone() : timeZoneDB->getTimeZone(tzString);

    auto date =
        computeDateFn(tz,
                      value::numericCast<int64_t>(typeTagYearOrWeekYear, valueYearOrWeekYear),
                      value::numericCast<int64_t>(typeTagMonthOrWeek, valueMonthOrWeek),
                      value::numericCast<int64_t>(typeTagDay, valueDay),
                      value::numericCast<int64_t>(typeTagHr, valueHr),
                      value::numericCast<int64_t>(typeTagMin, valueMin),
                      value::numericCast<int64_t>(typeTagSec, valueSec),
                      value::numericCast<int64_t>(typeTagMillis, valueMillis));
    return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(date.asInt64())};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDate(ArityType arity) {
    auto timeZoneDBTuple = getFromStack(0);
    auto yearTuple = getFromStack(1);
    auto monthTuple = getFromStack(2);
    auto dayTuple = getFromStack(3);
    auto hourTuple = getFromStack(4);
    auto minuteTuple = getFromStack(5);
    auto secondTuple = getFromStack(6);
    auto millisTuple = getFromStack(7);
    auto timezoneTuple = getFromStack(8);

    return builtinDateHelper(
        [](TimeZone tz,
           long long year,
           long long month,
           long long day,
           long long hour,
           long long min,
           long long sec,
           long long millis) -> Date_t {
            return tz.createFromDateParts(year, month, day, hour, min, sec, millis);
        },
        timeZoneDBTuple,
        yearTuple,
        monthTuple,
        dayTuple,
        hourTuple,
        minuteTuple,
        secondTuple,
        millisTuple,
        timezoneTuple);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDateDiff(ArityType arity) {
    invariant(arity == 5 || arity == 6);  // 6th parameter is 'startOfWeek'.

    auto [timezoneDBOwn, timezoneDBTag, timezoneDBValue] = getFromStack(0);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBValue);

    // Get startDate.
    auto [startDateOwn, startDateTag, startDateValue] = getFromStack(1);
    if (!coercibleToDate(startDateTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto startDate = getDate(startDateTag, startDateValue);

    // Get endDate.
    auto [endDateOwn, endDateTag, endDateValue] = getFromStack(2);
    if (!coercibleToDate(endDateTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto endDate = getDate(endDateTag, endDateValue);

    // Get unit.
    auto [unitOwn, unitTag, unitValue] = getFromStack(3);
    if (!value::isString(unitTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto unitString = value::getStringView(unitTag, unitValue);
    if (!isValidTimeUnit(unitString)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto unit = parseTimeUnit(unitString);

    // Get timezone.
    auto [timezoneOwn, timezoneTag, timezoneValue] = getFromStack(4);
    if (!isValidTimezone(timezoneTag, timezoneValue, timezoneDB)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezone = getTimezone(timezoneTag, timezoneValue, timezoneDB);

    // Get startOfWeek, if 'startOfWeek' parameter was passed and time unit is the week.
    DayOfWeek startOfWeek{kStartOfWeekDefault};
    if (6 == arity) {
        auto [startOfWeekOwn, startOfWeekTag, startOfWeekValue] = getFromStack(5);
        if (!value::isString(startOfWeekTag)) {
            return {false, value::TypeTags::Nothing, 0};
        }
        if (TimeUnit::week == unit) {
            auto startOfWeekString = value::getStringView(startOfWeekTag, startOfWeekValue);
            if (!isValidDayOfWeek(startOfWeekString)) {
                return {false, value::TypeTags::Nothing, 0};
            }
            startOfWeek = parseDayOfWeek(startOfWeekString);
        }
    }
    auto result = dateDiff(startDate, endDate, unit, timezone, startOfWeek);
    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDateWeekYear(ArityType arity) {
    auto timeZoneDBTuple = getFromStack(0);
    auto yearTuple = getFromStack(1);
    auto weekTuple = getFromStack(2);
    auto dayTuple = getFromStack(3);
    auto hourTuple = getFromStack(4);
    auto minuteTuple = getFromStack(5);
    auto secondTuple = getFromStack(6);
    auto millisTuple = getFromStack(7);
    auto timezoneTuple = getFromStack(8);

    return builtinDateHelper(
        [](TimeZone tz,
           long long year,
           long long month,
           long long day,
           long long hour,
           long long min,
           long long sec,
           long long millis) -> Date_t {
            return tz.createFromIso8601DateParts(year, month, day, hour, min, sec, millis);
        },
        timeZoneDBTuple,
        yearTuple,
        weekTuple,
        dayTuple,
        hourTuple,
        minuteTuple,
        secondTuple,
        millisTuple,
        timezoneTuple);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDateToParts(ArityType arity) {
    auto [timezoneDBOwn, timezoneDBTag, timezoneDBVal] = getFromStack(0);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBVal);
    auto [dateOwn, dateTag, dateVal] = getFromStack(1);

    // Get timezone.
    auto [timezoneOwn, timezoneTag, timezoneVal] = getFromStack(2);
    if (!value::isString(timezoneTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    TimeZone timezone = getTimezone(timezoneTag, timezoneVal, timezoneDB);

    // Get date.
    if (dateTag != value::TypeTags::Date && dateTag != value::TypeTags::Timestamp &&
        dateTag != value::TypeTags::ObjectId && dateTag != value::TypeTags::bsonObjectId) {
        return {false, value::TypeTags::Nothing, 0};
    }
    Date_t date = getDate(dateTag, dateVal);

    // Get date parts.
    auto dateParts = timezone.dateParts(date);
    auto [dateObjTag, dateObjVal] = value::makeNewObject();
    value::ValueGuard guard{dateObjTag, dateObjVal};
    auto dateObj = value::getObjectView(dateObjVal);
    dateObj->push_back("year", value::TypeTags::NumberInt32, dateParts.year);
    dateObj->push_back("month", value::TypeTags::NumberInt32, dateParts.month);
    dateObj->push_back("day", value::TypeTags::NumberInt32, dateParts.dayOfMonth);
    dateObj->push_back("hour", value::TypeTags::NumberInt32, dateParts.hour);
    dateObj->push_back("minute", value::TypeTags::NumberInt32, dateParts.minute);
    dateObj->push_back("second", value::TypeTags::NumberInt32, dateParts.second);
    dateObj->push_back("millisecond", value::TypeTags::NumberInt32, dateParts.millisecond);
    guard.reset();
    return {true, dateObjTag, dateObjVal};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsoDateToParts(ArityType arity) {
    auto [timezoneDBOwn, timezoneDBTag, timezoneDBVal] = getFromStack(0);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBVal);
    auto [dateOwn, dateTag, dateVal] = getFromStack(1);

    // Get timezone.
    auto [timezoneOwn, timezoneTag, timezoneVal] = getFromStack(2);
    if (!value::isString(timezoneTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    TimeZone timezone = getTimezone(timezoneTag, timezoneVal, timezoneDB);

    // Get date.
    if (dateTag != value::TypeTags::Date && dateTag != value::TypeTags::Timestamp &&
        dateTag != value::TypeTags::ObjectId && dateTag != value::TypeTags::bsonObjectId) {
        return {false, value::TypeTags::Nothing, 0};
    }
    Date_t date = getDate(dateTag, dateVal);

    // Get date parts.
    auto dateParts = timezone.dateIso8601Parts(date);
    auto [dateObjTag, dateObjVal] = value::makeNewObject();
    value::ValueGuard guard{dateObjTag, dateObjVal};
    auto dateObj = value::getObjectView(dateObjVal);
    dateObj->push_back("isoWeekYear", value::TypeTags::NumberInt32, dateParts.year);
    dateObj->push_back("isoWeek", value::TypeTags::NumberInt32, dateParts.weekOfYear);
    dateObj->push_back("isoDayOfWeek", value::TypeTags::NumberInt32, dateParts.dayOfWeek);
    dateObj->push_back("hour", value::TypeTags::NumberInt32, dateParts.hour);
    dateObj->push_back("minute", value::TypeTags::NumberInt32, dateParts.minute);
    dateObj->push_back("second", value::TypeTags::NumberInt32, dateParts.second);
    dateObj->push_back("millisecond", value::TypeTags::NumberInt32, dateParts.millisecond);
    guard.reset();
    return {true, dateObjTag, dateObjVal};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDayOfYear(ArityType arity) {
    invariant(arity == 3);

    auto [timezoneDBOwn, timezoneDBTag, timezoneDBValue] = getFromStack(0);
    auto [dateOwn, dateTag, dateValue] = getFromStack(1);
    auto [timezoneOwn, timezoneTag, timezoneValue] = getFromStack(2);
    return genericDayOfYear(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDayOfMonth(ArityType arity) {
    invariant(arity == 3);

    auto [timezoneDBOwn, timezoneDBTag, timezoneDBValue] = getFromStack(0);
    auto [dateOwn, dateTag, dateValue] = getFromStack(1);
    auto [timezoneOwn, timezoneTag, timezoneValue] = getFromStack(2);
    return genericDayOfMonth(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDayOfWeek(ArityType arity) {
    invariant(arity == 3);

    auto [timezoneDBOwn, timezoneDBTag, timezoneDBValue] = getFromStack(0);
    auto [dateOwn, dateTag, dateValue] = getFromStack(1);
    auto [timezoneOwn, timezoneTag, timezoneValue] = getFromStack(2);
    return genericDayOfWeek(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinBitTestPosition(ArityType arity) {
    invariant(arity == 3);

    auto [ownedMask, maskTag, maskValue] = getFromStack(0);
    auto [ownedInput, valueTag, value] = getFromStack(1);

    // Carries a flag to indicate the desired testing behavior this was invoked under. The testing
    // behavior is used to determine if we need to bail out of the bit position comparison early in
    // the depending if a bit is found to be set or unset.
    auto [_, tagBitTestBehavior, valueBitTestBehavior] = getFromStack(2);
    invariant(tagBitTestBehavior == value::TypeTags::NumberInt32);

    if (!value::isArray(maskTag) || !value::isBinData(valueTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto bitPositions = value::getArrayView(maskValue);
    auto binDataSize = static_cast<int64_t>(value::getBSONBinDataSize(valueTag, value));
    auto binData = value::getBSONBinData(valueTag, value);
    auto bitTestBehavior = BitTestBehavior{value::bitcastTo<int32_t>(valueBitTestBehavior)};

    auto isBitSet = false;
    for (size_t idx = 0; idx < bitPositions->size(); ++idx) {
        auto [tagBitPosition, valueBitPosition] = bitPositions->getAt(idx);
        auto bitPosition = value::bitcastTo<int64_t>(valueBitPosition);
        if (bitPosition >= binDataSize * 8) {
            // If position to test is longer than the data to test against, zero-extend.
            isBitSet = false;
        } else {
            // Convert the bit position to a byte position within a byte. Note that byte positions
            // start at position 0 in the document's value BinData array representation, and bit
            // positions start at the least significant bit.
            auto byteIdx = bitPosition / 8;
            auto currentBit = bitPosition % 8;
            auto currentByte = binData[byteIdx];

            isBitSet = currentByte & (1 << currentBit);
        }

        // Bail out early if we succeed with the any case or fail with the all case. To do this, we
        // negate a test to determine if we need to continue looping over the bit position list. So
        // the first part of the disjunction checks when a bit is set and the test is invoked by the
        // AllSet or AnyClear expressions. The second test checks if a bit isn't set and we are
        // checking the AllClear or the AnySet cases.
        if (!((isBitSet &&
               (bitTestBehavior == BitTestBehavior::AllSet ||
                bitTestBehavior == BitTestBehavior::AnyClear)) ||
              (!isBitSet &&
               (bitTestBehavior == BitTestBehavior::AllClear ||
                bitTestBehavior == BitTestBehavior::AnySet)))) {
            return {false,
                    value::TypeTags::Boolean,
                    value::bitcastFrom<bool>(bitTestBehavior == BitTestBehavior::AnyClear ||
                                             bitTestBehavior == BitTestBehavior::AnySet)};
        }
    }
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(bitTestBehavior == BitTestBehavior::AllSet ||
                                     bitTestBehavior == BitTestBehavior::AllClear)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinBitTestZero(ArityType arity) {
    invariant(arity == 2);
    auto [maskOwned, maskTag, maskValue] = getFromStack(0);
    auto [inputOwned, inputTag, inputValue] = getFromStack(1);

    if ((maskTag != value::TypeTags::NumberInt32 && maskTag != value::TypeTags::NumberInt64) ||
        (inputTag != value::TypeTags::NumberInt32 && inputTag != value::TypeTags::NumberInt64)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto maskNum = value::numericCast<int64_t>(maskTag, maskValue);
    auto inputNum = value::numericCast<int64_t>(inputTag, inputValue);
    auto result = (maskNum & inputNum) == 0;
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinBitTestMask(ArityType arity) {
    invariant(arity == 2);
    auto [maskOwned, maskTag, maskValue] = getFromStack(0);
    auto [inputOwned, inputTag, inputValue] = getFromStack(1);

    if ((maskTag != value::TypeTags::NumberInt32 && maskTag != value::TypeTags::NumberInt64) ||
        (inputTag != value::TypeTags::NumberInt32 && inputTag != value::TypeTags::NumberInt64)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto maskNum = value::numericCast<int64_t>(maskTag, maskValue);
    auto inputNum = value::numericCast<int64_t>(inputTag, inputValue);
    auto result = (maskNum & inputNum) == maskNum;
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinBsonSize(ArityType arity) {
    auto [_, tagOperand, valOperand] = getFromStack(0);

    if (tagOperand == value::TypeTags::Object) {
        BSONObjBuilder objBuilder;
        bson::convertToBsonObj(objBuilder, value::getObjectView(valOperand));
        int32_t sz = objBuilder.done().objsize();
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(sz)};
    } else if (tagOperand == value::TypeTags::bsonObject) {
        auto beginObj = value::getRawPointerView(valOperand);
        int32_t sz = ConstDataView(beginObj).read<LittleEndian<int32_t>>();
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(sz)};
    }
    return {false, value::TypeTags::Nothing, 0};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinToUpper(ArityType arity) {
    auto [_, operandTag, operandVal] = getFromStack(0);

    if (value::isString(operandTag)) {
        auto [strTag, strVal] = value::copyValue(operandTag, operandVal);
        auto buf = value::getRawStringView(strTag, strVal);
        auto range = std::make_pair(buf, buf + value::getStringLength(strTag, strVal));
        boost::to_upper(range);
        return {true, strTag, strVal};
    }
    return {false, value::TypeTags::Nothing, 0};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinToLower(ArityType arity) {
    auto [_, operandTag, operandVal] = getFromStack(0);

    if (value::isString(operandTag)) {
        auto [strTag, strVal] = value::copyValue(operandTag, operandVal);
        auto buf = value::getRawStringView(strTag, strVal);
        auto range = std::make_pair(buf, buf + value::getStringLength(strTag, strVal));
        boost::to_lower(range);
        return {true, strTag, strVal};
    }
    return {false, value::TypeTags::Nothing, 0};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCoerceToString(ArityType arity) {
    auto [operandOwn, operandTag, operandVal] = getFromStack(0);

    if (value::isString(operandTag)) {
        topStack(false, value::TypeTags::Nothing, 0);
        return {operandOwn, operandTag, operandVal};
    }

    if (operandTag == value::TypeTags::bsonSymbol) {
        // Values of type StringBig and Values of type bsonSymbol have identical representations,
        // so we can simply take ownership of the argument, change the type tag to StringBig, and
        // return it.
        topStack(false, value::TypeTags::Nothing, 0);
        return {operandOwn, value::TypeTags::StringBig, operandVal};
    }

    switch (operandTag) {
        case value::TypeTags::NumberInt32: {
            std::string str = str::stream() << value::bitcastTo<int32_t>(operandVal);
            auto [strTag, strVal] = value::makeNewString(str);
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberInt64: {
            std::string str = str::stream() << value::bitcastTo<int64_t>(operandVal);
            auto [strTag, strVal] = value::makeNewString(str);
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberDouble: {
            std::string str = str::stream() << value::bitcastTo<double>(operandVal);
            auto [strTag, strVal] = value::makeNewString(str);
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberDecimal: {
            std::string str = value::bitcastTo<Decimal128>(operandVal).toString();
            auto [strTag, strVal] = value::makeNewString(str);
            return {true, strTag, strVal};
        }
        case value::TypeTags::Date: {
            std::string str = str::stream()
                << TimeZoneDatabase::utcZone().formatDate(
                       kISOFormatString,
                       Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(operandVal)));
            auto [strTag, strVal] = value::makeNewString(str);
            return {true, strTag, strVal};
        }
        case value::TypeTags::Timestamp: {
            Timestamp ts{value::bitcastTo<uint64_t>(operandVal)};
            auto [strTag, strVal] = value::makeNewString(ts.toString());
            return {true, strTag, strVal};
        }
        case value::TypeTags::Null: {
            auto [strTag, strVal] = value::makeNewString("");
            return {true, strTag, strVal};
        }
        default:
            break;
    }
    return {false, value::TypeTags::Nothing, 0};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAcos(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAcos(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAcosh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAcosh(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAsin(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAsin(operandTag, operandValue);
}
std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAsinh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAsinh(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAtan(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAtan(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAtanh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAtanh(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAtan2(ArityType arity) {
    auto [owned1, operandTag1, operandValue1] = getFromStack(0);
    auto [owned2, operandTag2, operandValue2] = getFromStack(1);
    return genericAtan2(operandTag1, operandValue1, operandTag2, operandValue2);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCos(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericCos(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCosh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericCosh(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDegreesToRadians(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericDegreesToRadians(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinRadiansToDegrees(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericRadiansToDegrees(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinSin(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericSin(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinSinh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericSinh(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinTan(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericTan(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinTanh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericTanh(operandTag, operandValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinRound(ArityType arity) {
    invariant(arity == 1);
    auto [owned, tag, val] = getFromStack(0);

    // Round 'val' to the closest integer, with ties rounding to the closest even integer.
    // If 'val' is +Inf, -Inf, or NaN, this function will simply return 'val' as-is.
    switch (tag) {
        case value::TypeTags::NumberInt32:
        case value::TypeTags::NumberInt64:
            // The value is already an integer, so just return it as-is.
            return {false, tag, val};
        case value::TypeTags::NumberDouble: {
            // std::nearbyint()'s behavior relies on a thread-local "rounding mode", so
            // we use boost::numeric::RoundEven<double>::nearbyint() instead. We should
            // switch over to roundeven() once it becomes available in the standard library.
            // (See https://en.cppreference.com/w/c/experimental/fpext1 for details.)
            auto operand = value::bitcastTo<double>(val);
            auto rounded = boost::numeric::RoundEven<double>::nearbyint(operand);
            return {false, tag, value::bitcastFrom<double>(rounded)};
        }
        case value::TypeTags::NumberDecimal: {
            auto operand = value::bitcastTo<Decimal128>(val);
            auto rounded = operand.round(Decimal128::RoundingMode::kRoundTiesToEven);
            if (operand.isEqual(rounded)) {
                // If the output of rounding is equal to the input, then we can just take
                // ownership of 'operand' and return it. (This is more efficient than calling
                // makeCopyDecimal(), which would allocate memory on the heap.)
                topStack(false, value::TypeTags::Nothing, 0);
                return {owned, tag, val};
            }

            auto [tag, val] = value::makeCopyDecimal(rounded);
            return {true, tag, val};
        }
        default:
            return {false, value::TypeTags::Nothing, 0};
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinConcat(ArityType arity) {
    StringBuilder result;
    for (ArityType idx = 0; idx < arity; ++idx) {
        auto [_, tag, value] = getFromStack(idx);
        if (!value::isString(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }
        result << sbe::value::getStringView(tag, value);
    }

    auto [strTag, strValue] = sbe::value::makeNewString(result.str());
    return {true, strTag, strValue};
}

std::pair<value::TypeTags, value::Value> ByteCode::genericIsMember(value::TypeTags lhsTag,
                                                                   value::Value lhsVal,
                                                                   value::TypeTags rhsTag,
                                                                   value::Value rhsVal,
                                                                   CollatorInterface* collator) {
    if (!value::isArray(rhsTag)) {
        return {value::TypeTags::Nothing, 0};
    }

    if (rhsTag == value::TypeTags::ArraySet) {
        auto arrSet = value::getArraySetView(rhsVal);

        if (CollatorInterface::collatorsMatch(collator, arrSet->getCollator())) {
            auto& values = arrSet->values();
            return {value::TypeTags::Boolean,
                    value::bitcastFrom<bool>(values.find({lhsTag, lhsVal}) != values.end())};
        }
    }

    auto rhsArr = value::ArrayEnumerator{rhsTag, rhsVal};
    while (!rhsArr.atEnd()) {
        auto [rhsTag, rhsVal] = rhsArr.getViewOfValue();
        auto [tag, val] = value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal, collator);
        if (tag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(val) == 0) {
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(true)};
        }
        rhsArr.advance();
    }
    return {value::TypeTags::Boolean, value::bitcastFrom<bool>(false)};
}

std::pair<value::TypeTags, value::Value> ByteCode::genericIsMember(value::TypeTags lhsTag,
                                                                   value::Value lhsVal,
                                                                   value::TypeTags rhsTag,
                                                                   value::Value rhsVal,
                                                                   value::TypeTags collTag,
                                                                   value::Value collVal) {
    if (collTag != value::TypeTags::collator) {
        return {value::TypeTags::Nothing, 0};
    }

    auto collator = value::getCollatorView(collVal);

    return genericIsMember(lhsTag, lhsVal, rhsTag, rhsVal, collator);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsMember(ArityType arity) {
    invariant(arity == 2);

    auto [ownedInput, inputTag, inputVal] = getFromStack(0);
    auto [ownedArr, arrTag, arrVal] = getFromStack(1);

    auto [resultTag, resultVal] = genericIsMember(inputTag, inputVal, arrTag, arrVal);
    return {false, resultTag, resultVal};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollIsMember(ArityType arity) {
    invariant(arity == 3);

    auto [ownedColl, collTag, collVal] = getFromStack(0);
    auto [ownedInput, inputTag, inputVal] = getFromStack(1);
    auto [ownedArr, arrTag, arrVal] = getFromStack(2);

    auto [resultTag, resultVal] =
        genericIsMember(inputTag, inputVal, arrTag, arrVal, collTag, collVal);

    return {false, resultTag, resultVal};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinIndexOfBytes(ArityType arity) {
    auto [strOwn, strTag, strVal] = getFromStack(0);
    auto [substrOwn, substrTag, substrVal] = getFromStack(1);
    if ((!value::isString(strTag)) || (!value::isString(substrTag))) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto str = value::getStringView(strTag, strVal);
    auto substring = value::getStringView(substrTag, substrVal);
    int64_t startIndex = 0, endIndex = str.size();

    if (arity >= 3) {
        auto [startOwn, startTag, startVal] = getFromStack(2);
        if (startTag != value::TypeTags::NumberInt64) {
            return {false, value::TypeTags::Nothing, 0};
        }
        startIndex = value::bitcastTo<int64_t>(startVal);
        // Check index is positive.
        if (startIndex < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        // Check for valid bounds.
        if (static_cast<size_t>(startIndex) > str.size()) {
            return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
        }
    }
    if (arity >= 4) {
        auto [endOwn, endTag, endVal] = getFromStack(3);
        if (endTag != value::TypeTags::NumberInt64) {
            return {false, value::TypeTags::Nothing, 0};
        }
        endIndex = value::bitcastTo<int64_t>(endVal);
        // Check index is positive.
        if (endIndex < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        // Check for valid bounds.
        if (endIndex < startIndex) {
            return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
        }
    }
    auto index = str.substr(startIndex, endIndex - startIndex).find(substring);
    if (index != std::string::npos) {
        return {
            false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(startIndex + index)};
    }
    return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinIndexOfCP(ArityType arity) {
    auto [strOwn, strTag, strVal] = getFromStack(0);
    auto [substrOwn, substrTag, substrVal] = getFromStack(1);
    if ((!value::isString(strTag)) || (!value::isString(substrTag))) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto str = value::getStringView(strTag, strVal);
    auto substr = value::getStringView(substrTag, substrVal);
    int64_t startCodePointIndex = 0, endCodePointIndexArg = str.size();

    if (arity >= 3) {
        auto [startOwn, startTag, startVal] = getFromStack(2);
        if (startTag != value::TypeTags::NumberInt64) {
            return {false, value::TypeTags::Nothing, 0};
        }
        startCodePointIndex = value::bitcastTo<int64_t>(startVal);
        // Check index is positive.
        if (startCodePointIndex < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        // Check for valid bounds.
        if (static_cast<size_t>(startCodePointIndex) > str.size()) {
            return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
        }
    }
    if (arity >= 4) {
        auto [endOwn, endTag, endVal] = getFromStack(3);
        if (endTag != value::TypeTags::NumberInt64) {
            return {false, value::TypeTags::Nothing, 0};
        }
        endCodePointIndexArg = value::bitcastTo<int64_t>(endVal);
        // Check index is positive.
        if (endCodePointIndexArg < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        // Check for valid bounds.
        if (endCodePointIndexArg < startCodePointIndex) {
            return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
        }
    }

    // Handle edge case if both string and substring are empty strings.
    if (startCodePointIndex == 0 && str.empty() && substr.empty()) {
        return {true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)};
    }

    // Need to get byte indexes for start and end indexes.
    int64_t startByteIndex = 0, byteIndex = 0, codePointIndex;
    for (codePointIndex = 0; static_cast<size_t>(byteIndex) < str.size(); codePointIndex++) {
        if (codePointIndex == startCodePointIndex) {
            startByteIndex = byteIndex;
        }
        uassert(5075307,
                "$indexOfCP found bad UTF-8 in the input",
                !str::isUTF8ContinuationByte(str[byteIndex]));
        byteIndex += str::getCodePointLength(str[byteIndex]);
    }

    int64_t endCodePointIndex = std::min(codePointIndex, endCodePointIndexArg);
    byteIndex = startByteIndex;
    for (codePointIndex = startCodePointIndex; codePointIndex < endCodePointIndex;
         ++codePointIndex) {
        if (str.substr(byteIndex, substr.size()).compare(substr) == 0) {
            return {
                false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(codePointIndex)};
        }
        byteIndex += str::getCodePointLength(str[byteIndex]);
    }
    return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsTimeUnit(ArityType arity) {
    invariant(arity == 1);
    auto [timeUnitOwn, timeUnitTag, timeUnitValue] = getFromStack(0);
    if (!value::isString(timeUnitTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(
                isValidTimeUnit(value::getStringView(timeUnitTag, timeUnitValue)))};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsDayOfWeek(ArityType arity) {
    invariant(arity == 1);
    auto [dayOfWeekOwn, dayOfWeekTag, dayOfWeekValue] = getFromStack(0);
    if (!value::isString(dayOfWeekTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(
                isValidDayOfWeek(value::getStringView(dayOfWeekTag, dayOfWeekValue)))};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsTimezone(ArityType arity) {
    auto [timezoneDBOwn, timezoneDBTag, timezoneDBVal] = getFromStack(0);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBVal);
    auto [timezoneOwn, timezoneTag, timezoneVal] = getFromStack(1);
    if (!value::isString(timezoneTag)) {
        return {false, value::TypeTags::Boolean, false};
    }
    auto timezoneStr = value::getStringView(timezoneTag, timezoneVal);
    if (timezoneDB->isTimeZoneIdentifier(timezoneStr)) {
        return {false, value::TypeTags::Boolean, true};
    }
    return {false, value::TypeTags::Boolean, false};
}

namespace {
std::tuple<bool, value::TypeTags, value::Value> setUnion(
    const std::vector<value::TypeTags>& argTags,
    const std::vector<value::Value>& argVals,
    const CollatorInterface* collator = nullptr) {
    auto [resTag, resVal] = value::makeNewArraySet(collator);
    value::ValueGuard resGuard{resTag, resVal};
    auto resView = value::getArraySetView(resVal);

    for (size_t idx = 0; idx < argVals.size(); ++idx) {
        auto argTag = argTags[idx];
        auto argVal = argVals[idx];

        auto arrIter = value::ArrayEnumerator{argTag, argVal};
        while (!arrIter.atEnd()) {
            auto [elTag, elVal] = arrIter.getViewOfValue();
            auto [copyTag, copyVal] = value::copyValue(elTag, elVal);
            resView->push_back(copyTag, copyVal);
            arrIter.advance();
        }
    }
    resGuard.reset();
    return {true, resTag, resVal};
}

std::tuple<bool, value::TypeTags, value::Value> setIntersection(
    const std::vector<value::TypeTags>& argTags,
    const std::vector<value::Value>& argVals,
    const CollatorInterface* collator = nullptr) {
    auto intersectionMap =
        value::ValueMapType<size_t>{0, value::ValueHash(collator), value::ValueEq(collator)};

    auto [resTag, resVal] = value::makeNewArraySet(collator);
    value::ValueGuard resGuard{resTag, resVal};

    for (size_t idx = 0; idx < argVals.size(); ++idx) {
        auto tag = argTags[idx];
        auto val = argVals[idx];

        bool atLeastOneCommonElement = false;
        auto enumerator = value::ArrayEnumerator{tag, val};
        while (!enumerator.atEnd()) {
            auto [elTag, elVal] = enumerator.getViewOfValue();
            if (idx == 0) {
                intersectionMap[{elTag, elVal}] = 1;
            } else {
                if (auto it = intersectionMap.find({elTag, elVal}); it != intersectionMap.end()) {
                    if (it->second == idx) {
                        it->second++;
                        atLeastOneCommonElement = true;
                    }
                }
            }
            enumerator.advance();
        }

        if (idx > 0 && !atLeastOneCommonElement) {
            resGuard.reset();
            return {true, resTag, resVal};
        }
    }

    auto resView = value::getArraySetView(resVal);
    for (auto&& [item, counter] : intersectionMap) {
        if (counter == argVals.size()) {
            auto [elTag, elVal] = item;
            auto [copyTag, copyVal] = value::copyValue(elTag, elVal);
            resView->push_back(copyTag, copyVal);
        }
    }

    resGuard.reset();
    return {true, resTag, resVal};
}

std::tuple<bool, value::TypeTags, value::Value> setDifference(
    value::TypeTags lhsTag,
    value::Value lhsVal,
    value::TypeTags rhsTag,
    value::Value rhsVal,
    const CollatorInterface* collator = nullptr) {
    auto [resTag, resVal] = value::makeNewArraySet(collator);
    value::ValueGuard resGuard{resTag, resVal};
    auto resView = value::getArraySetView(resVal);

    value::ValueSetType setValuesSecondArg(0, value::ValueHash(collator), value::ValueEq(collator));
    auto rhsIter = value::ArrayEnumerator(rhsTag, rhsVal);
    while (!rhsIter.atEnd()) {
        auto [elTag, elVal] = rhsIter.getViewOfValue();
        setValuesSecondArg.insert({elTag, elVal});
        rhsIter.advance();
    }

    auto lhsIter = value::ArrayEnumerator(lhsTag, lhsVal);
    while (!lhsIter.atEnd()) {
        auto [elTag, elVal] = lhsIter.getViewOfValue();
        if (setValuesSecondArg.count({elTag, elVal}) == 0) {
            auto [copyTag, copyVal] = value::copyValue(elTag, elVal);
            resView->push_back(copyTag, copyVal);
        }
        lhsIter.advance();
    }

    resGuard.reset();
    return {true, resTag, resVal};
}
}  // namespace

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetUnion(ArityType arity) {
    invariant(arity >= 1);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;
    for (size_t idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setUnion(argTags, argVals, value::getCollatorView(collVal));
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetUnion(ArityType arity) {
    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 0; idx < arity; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setUnion(argTags, argVals);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetIntersection(
    ArityType arity) {
    invariant(arity >= 1);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setIntersection(argTags, argVals, value::getCollatorView(collVal));
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetIntersection(ArityType arity) {
    std::vector<value::TypeTags> argTags;
    std::vector<value::Value> argVals;

    for (size_t idx = 0; idx < arity; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        if (!value::isArray(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        argTags.push_back(tag);
        argVals.push_back(val);
    }

    return setIntersection(argTags, argVals);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollSetDifference(
    ArityType arity) {
    invariant(arity == 3);

    auto [_, collTag, collVal] = getFromStack(0);
    if (collTag != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(1);
    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(2);

    if (!value::isArray(lhsTag) || !value::isArray(rhsTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    return setDifference(lhsTag, lhsVal, rhsTag, rhsVal, value::getCollatorView(collVal));
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinSetDifference(ArityType arity) {
    invariant(arity == 2);

    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(1);

    if (!value::isArray(lhsTag) || !value::isArray(rhsTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    return setDifference(lhsTag, lhsVal, rhsTag, rhsVal);
}

namespace {
/**
 * A helper function to create the result object {"match" : .., "idx" : ..., "captures" :
 * ...} from the result of pcre_exec().
 */
std::tuple<bool, value::TypeTags, value::Value> buildRegexMatchResultObject(
    StringData inputString,
    const std::vector<int>& capturesBuffer,
    size_t numCaptures,
    uint32_t& startBytePos,
    uint32_t& codePointPos) {

    auto verifyBounds = [&inputString](auto startPos, auto limitPos, auto isCapture) {
        // If a capture group was not matched, then the 'startPos' and 'limitPos' will both be -1.
        // These bounds cannot occur for a match on the full string.
        if (startPos == -1 && limitPos == -1 && isCapture) {
            return true;
        }
        if (startPos == -1 || limitPos == -1) {
            LOGV2_ERROR(5073412,
                        "Unexpected error occurred while executing regexFind.",
                        "startPos"_attr = startPos,
                        "limitPos"_attr = limitPos);
            return false;
        }
        if (startPos < 0 || static_cast<size_t>(startPos) > inputString.size() || limitPos < 0 ||
            static_cast<size_t>(limitPos) > inputString.size() || startPos > limitPos) {
            LOGV2_ERROR(5073413,
                        "Unexpected error occurred while executing regexFind.",
                        "startPos"_attr = startPos,
                        "limitPos"_attr = limitPos);
            return false;
        }
        return true;
    };

    // Extract the matched string: its start and (end+1) indices are in the first two elements of
    // capturesBuffer.
    if (!verifyBounds(capturesBuffer[0], capturesBuffer[1], false)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto matchStartIdx = capturesBuffer[0];
    auto matchedString = inputString.substr(matchStartIdx, capturesBuffer[1] - matchStartIdx);
    auto [matchedTag, matchedVal] = value::makeNewString(matchedString);
    value::ValueGuard matchedGuard{matchedTag, matchedVal};

    // We iterate through the input string's contents preceding the match index, in order to convert
    // the byte offset to a code point offset.
    for (auto byteIdx = startBytePos; byteIdx < static_cast<uint32_t>(matchStartIdx);
         ++codePointPos) {
        byteIdx += str::getCodePointLength(inputString[byteIdx]);
    }
    startBytePos = matchStartIdx;

    auto [arrTag, arrVal] = value::makeNewArray();
    value::ValueGuard arrGuard{arrTag, arrVal};
    auto arrayView = value::getArrayView(arrVal);
    // The next '2 * numCaptures' entries (after the first two entries) of 'capturesBuffer'
    // hold the (start, limit) pairs of indexes, for each of the capture groups. We skip the first
    // two elements and start iteration from 3rd element so that we only construct the strings for
    // capture groups.
    for (size_t i = 0; i < numCaptures; ++i) {
        const auto start = capturesBuffer[2 * (i + 1)];
        const auto limit = capturesBuffer[2 * (i + 1) + 1];
        if (!verifyBounds(start, limit, true)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        if (start == -1 && limit == -1) {
            arrayView->push_back(value::TypeTags::Null, 0);
        } else {
            auto captureString = inputString.substr(start, limit - start);
            auto [tag, val] = value::makeNewString(captureString);
            arrayView->push_back(tag, val);
        }
    }

    auto [resTag, resVal] = value::makeNewObject();
    value::ValueGuard resGuard{resTag, resVal};
    auto resObjectView = value::getObjectView(resVal);
    resObjectView->reserve(3);
    matchedGuard.reset();
    resObjectView->push_back("match", matchedTag, matchedVal);
    resObjectView->push_back(
        "idx", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(codePointPos));
    arrGuard.reset();
    resObjectView->push_back("captures", arrTag, arrVal);
    resGuard.reset();
    return {true, resTag, resVal};
}

/**
 * A helper function to extract the next match in the subject string using the compiled regex
 * pattern.
 * - pcre: The wrapper object containing the compiled pcre expression
 * - inputString: The subject string.
 * - capturesBuffer: Array to be populated with the found matched string and capture groups.
 * - startBytePos: The position from where the search should start given in bytes.
 * - codePointPos: The same position in terms of code points.
 * - isMatch: Boolean flag to mark if the caller function is $regexMatch, in which case the result
 * returned is true/false.
 */
std::tuple<bool, value::TypeTags, value::Value> pcreNextMatch(value::PcreRegex* pcre,
                                                              StringData inputString,
                                                              std::vector<int>& capturesBuffer,
                                                              uint32_t& startBytePos,
                                                              uint32_t& codePointPos,
                                                              bool isMatch = false) {
    auto execResult = pcre->execute(inputString, startBytePos, capturesBuffer);

    auto numCaptures = pcre->getNumberCaptures();
    if (execResult < -1 || execResult > static_cast<int>(numCaptures) + 1) {
        LOGV2_ERROR(5073414,
                    "Error occurred while executing regular expression.",
                    "execResult"_attr = execResult);
        return {false, value::TypeTags::Nothing, 0};
    }

    if (isMatch) {
        // $regexMatch returns true or false.
        bool match = (execResult != PCRE_ERROR_NOMATCH);
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(match)};
    } else {
        // $regexFind and $regexFindAll build result object or return null.
        if (execResult == PCRE_ERROR_NOMATCH) {
            return {false, value::TypeTags::Null, 0};
        }
        return buildRegexMatchResultObject(
            inputString, capturesBuffer, numCaptures, startBytePos, codePointPos);
    }
}

/**
 * A helper function to extract the first match in the subject string using the compiled regex
 * pattern. See 'pcreNextMatch' function for parameters description.
 */
std::tuple<bool, value::TypeTags, value::Value> pcreFirstMatch(
    value::PcreRegex* pcre,
    StringData inputString,
    bool isMatch = false,
    std::vector<int>* capturesBuffer = nullptr,
    uint32_t* startBytePos = nullptr,
    uint32_t* codePointPos = nullptr) {
    std::vector<int> tmpCapturesBuffer;
    uint32_t tmpStartBytePos = 0;
    uint32_t tmpCodePointPos = 0;

    capturesBuffer = capturesBuffer ? capturesBuffer : &tmpCapturesBuffer;
    startBytePos = startBytePos ? startBytePos : &tmpStartBytePos;
    codePointPos = codePointPos ? codePointPos : &tmpCodePointPos;

    // The first two-thirds of the capturesBuffer is used to pass back captured substrings' start
    // and (end+1) indexes. The remaining third of the vector is used as workspace by pcre_exec()
    // while matching capturing subpatterns, and is not available for passing back information.
    auto numCaptures = pcre->getNumberCaptures();
    capturesBuffer->resize((1 + numCaptures) * 3);

    return pcreNextMatch(pcre, inputString, *capturesBuffer, *startBytePos, *codePointPos, isMatch);
}

/**
 * A helper function with common logic for $regexMatch and $regexFind functions. Both extract only
 * the first match to a regular expression, but return different result objects.
 */
std::tuple<bool, value::TypeTags, value::Value> genericPcreRegexSingleMatch(
    value::TypeTags typeTagPcreRegex,
    value::Value valuePcreRegex,
    value::TypeTags typeTagInputStr,
    value::Value valueInputStr,
    bool isMatch) {
    if (!value::isStringOrSymbol(typeTagInputStr) || !value::isPcreRegex(typeTagPcreRegex)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto inputString = value::getStringOrSymbolView(typeTagInputStr, valueInputStr);
    auto pcreRegex = value::getPcreRegexView(valuePcreRegex);

    return pcreFirstMatch(pcreRegex, inputString, isMatch);
}

std::pair<value::TypeTags, value::Value> collComparisonKey(value::TypeTags tag,
                                                           value::Value val,
                                                           const CollatorInterface* collator) {
    using namespace std::literals;

    // This function should only be called if 'collator' is non-null and 'tag' is a collatable type.
    invariant(collator);
    invariant(value::isCollatableType(tag));

    // For strings, call CollatorInterface::getComparisonKey() to obtain the comparison key.
    if (value::isString(tag)) {
        return value::makeNewString(
            collator->getComparisonKey(value::getStringView(tag, val)).getKeyData());
    }

    // For collatable types other than strings (such as arrays and objects), we take the slow
    // path and round-trip the value through BSON.
    BSONObjBuilder input;
    bson::appendValueToBsonObj<BSONObjBuilder>(input, ""_sd, tag, val);

    BSONObjBuilder output;
    CollationIndexKey::collationAwareIndexKeyAppend(input.obj().firstElement(), collator, &output);

    BSONObj outputView = output.done();
    auto ptr = outputView.objdata();
    auto be = ptr + 4;
    auto end = ptr + ConstDataView(ptr).read<LittleEndian<uint32_t>>();
    return bson::convertFrom<false>(be, end, 0);
}

}  // namespace

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexCompile(ArityType arity) {
    invariant(arity == 2);

    auto [patternOwned, patternTypeTag, patternValue] = getFromStack(0);
    auto [optionsOwned, optionsTypeTag, optionsValue] = getFromStack(1);

    if (!value::isString(patternTypeTag) || !value::isString(optionsTypeTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto pattern = value::getStringView(patternTypeTag, patternValue);
    auto options = value::getStringView(optionsTypeTag, optionsValue);

    if (pattern.find('\0', 0) != std::string::npos || options.find('\0', 0) != std::string::npos) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [pcreTag, pcreValue] = value::makeNewPcreRegex(pattern, options);
    return {true, pcreTag, pcreValue};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexMatch(ArityType arity) {
    invariant(arity == 2);
    auto [ownedPcreRegex, typeTagPcreRegex, valuePcreRegex] = getFromStack(0);
    auto [ownedInputStr, typeTagInputStr, valueInputStr] = getFromStack(1);

    return genericPcreRegexSingleMatch(
        typeTagPcreRegex, valuePcreRegex, typeTagInputStr, valueInputStr, true);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexFind(ArityType arity) {
    invariant(arity == 2);
    auto [ownedPcreRegex, typeTagPcreRegex, valuePcreRegex] = getFromStack(0);
    auto [ownedInputStr, typeTagInputStr, valueInputStr] = getFromStack(1);

    return genericPcreRegexSingleMatch(
        typeTagPcreRegex, valuePcreRegex, typeTagInputStr, valueInputStr, false);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexFindAll(ArityType arity) {
    invariant(arity == 2);
    auto [ownedPcre, typeTagPcreRegex, valuePcreRegex] = getFromStack(0);
    auto [ownedStr, typeTagInputStr, valueInputStr] = getFromStack(1);

    if (!value::isString(typeTagInputStr) || typeTagPcreRegex != value::TypeTags::pcreRegex) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto inputString = value::getStringView(typeTagInputStr, valueInputStr);
    auto pcre = value::getPcreRegexView(valuePcreRegex);

    std::vector<int> capturesBuffer;
    uint32_t startBytePos = 0;
    uint32_t codePointPos = 0;
    bool isFirstMatch = true;

    // Prepare the result array of matching objects.
    auto [arrTag, arrVal] = value::makeNewArray();
    value::ValueGuard arrGuard{arrTag, arrVal};
    auto arrayView = value::getArrayView(arrVal);

    int resultSize = 0;
    do {
        auto [_, matchTag, matchVal] = [&]() {
            if (isFirstMatch) {
                isFirstMatch = false;
                return pcreFirstMatch(
                    pcre, inputString, false, &capturesBuffer, &startBytePos, &codePointPos);
            }
            return pcreNextMatch(pcre, inputString, capturesBuffer, startBytePos, codePointPos);
        }();
        value::ValueGuard matchGuard{matchTag, matchVal};

        if (matchTag == value::TypeTags::Null) {
            break;
        }
        if (matchTag != value::TypeTags::Object) {
            return {false, value::TypeTags::Nothing, 0};
        }

        resultSize += getApproximateSize(matchTag, matchVal);
        uassert(5126606,
                "$regexFindAll: the size of buffer to store output exceeded the 64MB limit",
                resultSize <= mongo::BufferMaxSize);

        matchGuard.reset();
        arrayView->push_back(matchTag, matchVal);

        // Move indexes after the current matched string to prepare for the next search.
        auto [mstrTag, mstrVal] = value::getObjectView(matchVal)->getField("match");
        auto matchString = value::getStringView(mstrTag, mstrVal);
        if (matchString.empty()) {
            startBytePos += str::getCodePointLength(inputString[startBytePos]);
            ++codePointPos;
        } else {
            startBytePos += matchString.size();
            for (size_t byteIdx = 0; byteIdx < matchString.size(); ++codePointPos) {
                byteIdx += str::getCodePointLength(matchString[byteIdx]);
            }
        }
    } while (startBytePos < inputString.size());

    arrGuard.reset();
    return {true, arrTag, arrVal};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinShardFilter(ArityType arity) {
    invariant(arity == 2);

    auto [ownedFilter, filterTag, filterValue] = getFromStack(0);
    auto [ownedShardKey, shardKeyTag, shardKeyValue] = getFromStack(1);

    if (filterTag != value::TypeTags::shardFilterer || shardKeyTag != value::TypeTags::bsonObject) {
        if (filterTag == value::TypeTags::shardFilterer &&
            shardKeyTag == value::TypeTags::Nothing) {
            LOGV2_WARNING(5071200,
                          "No shard key found in document, it may have been inserted manually "
                          "into shard",
                          "keyPattern"_attr =
                              value::getShardFiltererView(filterValue)->getKeyPattern());
        }
        return {false, value::TypeTags::Nothing, 0};
    }

    BSONObj keyAsUnownedBson{sbe::value::bitcastTo<const char*>(shardKeyValue)};
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(
                value::getShardFiltererView(filterValue)->keyBelongsToMe(keyAsUnownedBson))};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinShardHash(ArityType arity) {
    invariant(arity == 1);

    auto [ownedShardKey, shardKeyTag, shardKeyValue] = getFromStack(0);

    // Compute the shard key hash value by round-tripping it through BSONObj as it is currently the
    // only way to do it if we do not want to duplicate the hash computation code.
    // TODO SERVER-55622
    BSONObjBuilder input;
    bson::appendValueToBsonObj<BSONObjBuilder>(input, ""_sd, shardKeyTag, shardKeyValue);
    auto hashVal =
        BSONElementHasher::hash64(input.obj().firstElement(), BSONElementHasher::DEFAULT_HASH_SEED);
    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<decltype(hashVal)>(hashVal)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinExtractSubArray(ArityType arity) {
    // We need to ensure that 'size_t' is wide enough to store 32-bit index.
    static_assert(sizeof(size_t) >= sizeof(int32_t), "size_t must be at least 32-bits");

    auto [arrayOwned, arrayTag, arrayValue] = getFromStack(0);
    auto [limitOwned, limitTag, limitValue] = getFromStack(1);

    if (!value::isArray(arrayTag) || limitTag != value::TypeTags::NumberInt32) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto limit = value::bitcastTo<int32_t>(limitValue);

    auto absWithSign = [](int32_t value) -> std::pair<bool, size_t> {
        if (value < 0) {
            // Upcast 'value' to 'int64_t' prevent overflow during the sign change.
            return {true, -static_cast<int64_t>(value)};
        }
        return {false, value};
    };

    size_t start = 0;
    bool isNegativeStart = false;
    size_t length = 0;
    if (arity == 2) {
        std::tie(isNegativeStart, start) = absWithSign(limit);
        length = start;
        if (!isNegativeStart) {
            start = 0;
        }
    } else {
        if (limit < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        length = limit;

        auto [skipOwned, skipTag, skipValue] = getFromStack(2);
        if (skipTag != value::TypeTags::NumberInt32) {
            return {false, value::TypeTags::Nothing, 0};
        }

        auto skip = value::bitcastTo<int32_t>(skipValue);
        std::tie(isNegativeStart, start) = absWithSign(skip);
    }

    auto [resultTag, resultValue] = value::makeNewArray();
    value::ValueGuard resultGuard{resultTag, resultValue};
    auto resultView = value::getArrayView(resultValue);

    if (arrayTag == value::TypeTags::Array) {
        auto arrayView = value::getArrayView(arrayValue);
        auto arraySize = arrayView->size();

        auto convertedStart = [&]() -> size_t {
            if (isNegativeStart) {
                if (start > arraySize) {
                    return 0;
                } else {
                    return arraySize - start;
                }
            } else {
                return std::min(start, arraySize);
            }
        }();

        size_t end = convertedStart + std::min(length, arraySize - convertedStart);

        for (size_t i = convertedStart; i < end; i++) {
            auto [tag, value] = arrayView->getAt(i);
            auto [copyTag, copyValue] = value::copyValue(tag, value);
            resultView->push_back(copyTag, copyValue);
        }
    } else {
        auto advance = [](value::ArrayEnumerator& enumerator, size_t offset) {
            size_t i = 0;
            while (i < offset && !enumerator.atEnd()) {
                i++;
                enumerator.advance();
            }
        };

        value::ArrayEnumerator startEnumerator{arrayTag, arrayValue};
        if (isNegativeStart) {
            value::ArrayEnumerator windowEndEnumerator{arrayTag, arrayValue};
            advance(windowEndEnumerator, start);

            while (!startEnumerator.atEnd() && !windowEndEnumerator.atEnd()) {
                startEnumerator.advance();
                windowEndEnumerator.advance();
            }
            invariant(windowEndEnumerator.atEnd());
        } else {
            advance(startEnumerator, start);
        }

        size_t i = 0;
        while (i < length && !startEnumerator.atEnd()) {
            auto [tag, value] = startEnumerator.getViewOfValue();
            auto [copyTag, copyValue] = value::copyValue(tag, value);
            resultView->push_back(copyTag, copyValue);

            i++;
            startEnumerator.advance();
        }
    }

    resultGuard.reset();
    return {true, resultTag, resultValue};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsArrayEmpty(ArityType arity) {
    invariant(arity == 1);
    auto [arrayOwned, arrayType, arrayValue] = getFromStack(0);

    if (!value::isArray(arrayType)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    if (arrayType == value::TypeTags::Array) {
        auto arrayView = value::getArrayView(arrayValue);
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(arrayView->size() == 0)};
    } else if (arrayType == value::TypeTags::bsonArray || arrayType == value::TypeTags::ArraySet) {
        value::ArrayEnumerator enumerator(arrayType, arrayValue);
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(enumerator.atEnd())};
    } else {
        // Earlier in this function we bailed out if the `arrayType` wasn't Array, ArraySet or
        // bsonArray, so it should be impossible to reach this point.
        MONGO_UNREACHABLE
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinHasNullBytes(ArityType arity) {
    invariant(arity == 1);
    auto [strOwned, strType, strValue] = getFromStack(0);

    if (!value::isString(strType)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto stringView = value::getStringView(strType, strValue);
    auto hasNullBytes = stringView.find('\0') != std::string::npos;

    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(hasNullBytes)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetRegexPattern(ArityType arity) {
    invariant(arity == 1);
    auto [regexOwned, regexType, regexValue] = getFromStack(0);

    if (regexType != value::TypeTags::bsonRegex) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto regex = value::getBsonRegexView(regexValue);
    auto [strType, strValue] = value::makeNewString(regex.pattern);

    return {true, strType, strValue};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetRegexFlags(ArityType arity) {
    invariant(arity == 1);
    auto [regexOwned, regexType, regexValue] = getFromStack(0);

    if (regexType != value::TypeTags::bsonRegex) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto regex = value::getBsonRegexView(regexValue);
    auto [strType, strValue] = value::makeNewString(regex.flags);

    return {true, strType, strValue};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinGenerateSortKey(ArityType arity) {
    invariant(arity == 2);

    auto [ssOwned, ssTag, ssVal] = getFromStack(0);
    auto [objOwned, objTag, objVal] = getFromStack(1);
    if (ssTag != value::TypeTags::sortSpec || !value::isObject(objTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto ss = value::getSortSpecView(ssVal);

    auto obj = [objTag = objTag, objVal = objVal]() {
        if (objTag == value::TypeTags::bsonObject) {
            return BSONObj{value::bitcastTo<const char*>(objVal)};
        } else if (objTag == value::TypeTags::Object) {
            BSONObjBuilder objBuilder;
            bson::convertToBsonObj(objBuilder, value::getObjectView(objVal));
            return objBuilder.obj();
        } else {
            MONGO_UNREACHABLE_TASSERT(5037004);
        }
    }();

    return {true,
            value::TypeTags::ksValue,
            value::bitcastFrom<KeyString::Value*>(new KeyString::Value(ss->generateSortKey(obj)))};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinReverseArray(ArityType arity) {
    invariant(arity == 1);
    auto [inputOwned, inputType, inputVal] = getFromStack(0);

    if (!value::isArray(inputType)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [resultTag, resultVal] = value::makeNewArray();
    auto resultView = value::getArrayView(resultVal);
    value::ValueGuard resultGuard{resultTag, resultVal};

    if (inputType == value::TypeTags::Array) {
        auto inputView = value::getArrayView(inputVal);
        size_t inputSize = inputView->size();
        resultView->reserve(inputSize);
        for (size_t i = 0; i < inputSize; i++) {
            auto [origTag, origVal] = inputView->getAt(inputSize - 1 - i);
            auto [copyTag, copyVal] = copyValue(origTag, origVal);
            resultView->push_back(copyTag, copyVal);
        }

        resultGuard.reset();
        return {true, resultTag, resultVal};
    } else if (inputType == value::TypeTags::bsonArray || inputType == value::TypeTags::ArraySet) {
        value::ArrayEnumerator enumerator{inputType, inputVal};

        // Using intermediate vector since bsonArray and ArraySet don't
        // support reverse iteration.
        std::vector<std::pair<value::TypeTags, value::Value>> inputContents;

        if (inputType == value::TypeTags::ArraySet) {
            // Reserve space to avoid resizing on push_back calls.
            auto arraySetView = value::getArraySetView(inputVal);
            inputContents.reserve(arraySetView->size());
            resultView->reserve(arraySetView->size());
        }

        while (!enumerator.atEnd()) {
            inputContents.push_back(enumerator.getViewOfValue());
            enumerator.advance();
        }

        // Run through the array backwards and copy into the result array.
        for (auto it = inputContents.rbegin(); it != inputContents.rend(); ++it) {
            auto [copyTag, copyVal] = copyValue(it->first, it->second);
            resultView->push_back(copyTag, copyVal);
        }

        resultGuard.reset();
        return {true, resultTag, resultVal};
    } else {
        // Earlier in this function we bailed out if the `inputType` wasn't
        // Array, ArraySet or bsonArray, so it should be impossible to reach
        // this point.
        MONGO_UNREACHABLE;
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDateAdd(ArityType arity) {
    invariant(arity == 5);

    auto [timezoneDBOwn, timezoneDBTag, timezoneDBVal] = getFromStack(0);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBVal);

    auto [startDateOwn, startDateTag, startDateVal] = getFromStack(1);
    if (!coercibleToDate(startDateTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto startDate = getDate(startDateTag, startDateVal);

    auto [unitOwn, unitTag, unitVal] = getFromStack(2);
    if (!value::isString(unitTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    std::string unitStr{value::getStringView(unitTag, unitVal)};
    if (!isValidTimeUnit(unitStr)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto unit = parseTimeUnit(unitStr);

    auto [amountOwn, amountTag, amountVal] = getFromStack(3);
    if (amountTag != value::TypeTags::NumberInt64) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto amount = value::bitcastTo<int64_t>(amountVal);

    auto [timezoneOwn, timezoneTag, timezoneVal] = getFromStack(4);
    if (!value::isString(timezoneTag) || !isValidTimezone(timezoneTag, timezoneVal, timezoneDB)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezone = getTimezone(timezoneTag, timezoneVal, timezoneDB);

    auto resDate = dateAdd(startDate, unit, amount, timezone);
    return {
        false, value::TypeTags::Date, value::bitcastFrom<int64_t>(resDate.toMillisSinceEpoch())};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinFtsMatch(ArityType arity) {
    invariant(arity == 2);

    auto [matcherOwn, matcherTag, matcherVal] = getFromStack(0);
    auto [inputOwn, inputTag, inputVal] = getFromStack(1);

    if (matcherTag != value::TypeTags::ftsMatcher || !value::isObject(inputTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto obj = [inputTag = inputTag, inputVal = inputVal]() {
        if (inputTag == value::TypeTags::bsonObject) {
            return BSONObj{value::bitcastTo<const char*>(inputVal)};
        }

        invariant(inputTag == value::TypeTags::Object);
        BSONObjBuilder builder;
        bson::convertToBsonObj(builder, value::getObjectView(inputVal));
        return builder.obj();
    }();

    const bool matches = value::getFtsMatcherView(matcherVal)->matches(obj);
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(matches)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::dispatchBuiltin(Builtin f,
                                                                          ArityType arity) {
    switch (f) {
        case Builtin::dateDiff:
            return builtinDateDiff(arity);
        case Builtin::dateParts:
            return builtinDate(arity);
        case Builtin::datePartsWeekYear:
            return builtinDateWeekYear(arity);
        case Builtin::dateToParts:
            return builtinDateToParts(arity);
        case Builtin::isoDateToParts:
            return builtinIsoDateToParts(arity);
        case Builtin::dayOfYear:
            return builtinDayOfYear(arity);
        case Builtin::dayOfMonth:
            return builtinDayOfMonth(arity);
        case Builtin::dayOfWeek:
            return builtinDayOfWeek(arity);
        case Builtin::split:
            return builtinSplit(arity);
        case Builtin::regexMatch:
            return builtinRegexMatch(arity);
        case Builtin::replaceOne:
            return builtinReplaceOne(arity);
        case Builtin::dropFields:
            return builtinDropFields(arity);
        case Builtin::newArray:
            return builtinNewArray(arity);
        case Builtin::newObj:
            return builtinNewObj(arity);
        case Builtin::ksToString:
            return builtinKeyStringToString(arity);
        case Builtin::newKs:
            return builtinNewKeyString(arity);
        case Builtin::abs:
            return builtinAbs(arity);
        case Builtin::ceil:
            return builtinCeil(arity);
        case Builtin::floor:
            return builtinFloor(arity);
        case Builtin::trunc:
            return builtinTrunc(arity);
        case Builtin::exp:
            return builtinExp(arity);
        case Builtin::ln:
            return builtinLn(arity);
        case Builtin::log10:
            return builtinLog10(arity);
        case Builtin::sqrt:
            return builtinSqrt(arity);
        case Builtin::addToArray:
            return builtinAddToArray(arity);
        case Builtin::addToSet:
            return builtinAddToSet(arity);
        case Builtin::collAddToSet:
            return builtinCollAddToSet(arity);
        case Builtin::doubleDoubleSum:
            return builtinDoubleDoubleSum(arity);
        case Builtin::bitTestZero:
            return builtinBitTestZero(arity);
        case Builtin::bitTestMask:
            return builtinBitTestMask(arity);
        case Builtin::bitTestPosition:
            return builtinBitTestPosition(arity);
        case Builtin::bsonSize:
            return builtinBsonSize(arity);
        case Builtin::toUpper:
            return builtinToUpper(arity);
        case Builtin::toLower:
            return builtinToLower(arity);
        case Builtin::coerceToString:
            return builtinCoerceToString(arity);
        case Builtin::acos:
            return builtinAcos(arity);
        case Builtin::acosh:
            return builtinAcosh(arity);
        case Builtin::asin:
            return builtinAsin(arity);
        case Builtin::asinh:
            return builtinAsinh(arity);
        case Builtin::atan:
            return builtinAtan(arity);
        case Builtin::atanh:
            return builtinAtanh(arity);
        case Builtin::atan2:
            return builtinAtan2(arity);
        case Builtin::cos:
            return builtinCos(arity);
        case Builtin::cosh:
            return builtinCosh(arity);
        case Builtin::degreesToRadians:
            return builtinDegreesToRadians(arity);
        case Builtin::radiansToDegrees:
            return builtinRadiansToDegrees(arity);
        case Builtin::sin:
            return builtinSin(arity);
        case Builtin::sinh:
            return builtinSinh(arity);
        case Builtin::tan:
            return builtinTan(arity);
        case Builtin::tanh:
            return builtinTanh(arity);
        case Builtin::round:
            return builtinRound(arity);
        case Builtin::concat:
            return builtinConcat(arity);
        case Builtin::isMember:
            return builtinIsMember(arity);
        case Builtin::collIsMember:
            return builtinCollIsMember(arity);
        case Builtin::indexOfBytes:
            return builtinIndexOfBytes(arity);
        case Builtin::indexOfCP:
            return builtinIndexOfCP(arity);
        case Builtin::isDayOfWeek:
            return builtinIsDayOfWeek(arity);
        case Builtin::isTimeUnit:
            return builtinIsTimeUnit(arity);
        case Builtin::isTimezone:
            return builtinIsTimezone(arity);
        case Builtin::setUnion:
            return builtinSetUnion(arity);
        case Builtin::setIntersection:
            return builtinSetIntersection(arity);
        case Builtin::setDifference:
            return builtinSetDifference(arity);
        case Builtin::collSetUnion:
            return builtinCollSetUnion(arity);
        case Builtin::collSetIntersection:
            return builtinCollSetIntersection(arity);
        case Builtin::collSetDifference:
            return builtinCollSetDifference(arity);
        case Builtin::runJsPredicate:
            return builtinRunJsPredicate(arity);
        case Builtin::regexCompile:
            return builtinRegexCompile(arity);
        case Builtin::regexFind:
            return builtinRegexFind(arity);
        case Builtin::regexFindAll:
            return builtinRegexFindAll(arity);
        case Builtin::shardFilter:
            return builtinShardFilter(arity);
        case Builtin::shardHash:
            return builtinShardHash(arity);
        case Builtin::extractSubArray:
            return builtinExtractSubArray(arity);
        case Builtin::isArrayEmpty:
            return builtinIsArrayEmpty(arity);
        case Builtin::reverseArray:
            return builtinReverseArray(arity);
        case Builtin::dateAdd:
            return builtinDateAdd(arity);
        case Builtin::hasNullBytes:
            return builtinHasNullBytes(arity);
        case Builtin::getRegexPattern:
            return builtinGetRegexPattern(arity);
        case Builtin::getRegexFlags:
            return builtinGetRegexFlags(arity);
        case Builtin::ftsMatch:
            return builtinFtsMatch(arity);
        case Builtin::generateSortKey:
            return builtinGenerateSortKey(arity);
    }

    MONGO_UNREACHABLE;
}

std::tuple<uint8_t, value::TypeTags, value::Value> ByteCode::run(const CodeFragment* code) {
    auto pcPointer = code->instrs().data();
    auto pcEnd = pcPointer + code->instrs().size();

    for (;;) {
        if (pcPointer == pcEnd) {
            break;
        } else {
            Instruction i = readFromMemory<Instruction>(pcPointer);
            pcPointer += sizeof(i);
            switch (i.tag) {
                case Instruction::pushConstVal: {
                    auto tag = readFromMemory<value::TypeTags>(pcPointer);
                    pcPointer += sizeof(tag);
                    auto val = readFromMemory<value::Value>(pcPointer);
                    pcPointer += sizeof(val);

                    pushStack(false, tag, val);

                    break;
                }
                case Instruction::pushAccessVal: {
                    auto accessor = readFromMemory<value::SlotAccessor*>(pcPointer);
                    pcPointer += sizeof(accessor);

                    auto [tag, val] = accessor->getViewOfValue();
                    pushStack(false, tag, val);

                    break;
                }
                case Instruction::pushMoveVal: {
                    auto accessor = readFromMemory<value::SlotAccessor*>(pcPointer);
                    pcPointer += sizeof(accessor);

                    auto [tag, val] = accessor->copyOrMoveValue();
                    pushStack(true, tag, val);

                    break;
                }
                case Instruction::pushLocalVal: {
                    auto stackOffset = readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(stackOffset);

                    auto [owned, tag, val] = getFromStack(stackOffset);

                    pushStack(false, tag, val);

                    break;
                }
                case Instruction::pop: {
                    auto [owned, tag, val] = getFromStack(0);
                    popStack();

                    if (owned) {
                        value::releaseValue(tag, val);
                    }

                    break;
                }
                case Instruction::swap: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(1);

                    // Swap values only if they are not physically same. This is necessary for the
                    // "swap and pop" idiom for returning a value from the top of the stack (used
                    // by ELocalBind). For example, consider the case where a series of swap, pop,
                    // swap, pop... instructions are executed and the value at stack[0] and
                    // stack[1] are physically identical, but stack[1] is owned and stack[0] is
                    // not. After swapping them, the 'pop' instruction would free the owned one and
                    // leave the unowned value dangling. The only exception to this is shallow
                    // values (values which fit directly inside a 64 bit Value and don't need
                    // to be freed explicitly).
                    if (!(rhsTag == lhsTag && rhsVal == lhsVal)) {
                        setStack(0, lhsOwned, lhsTag, lhsVal);
                        setStack(1, rhsOwned, rhsTag, rhsVal);
                    } else {
                        // See explanation above.
                        tassert(
                            56123,
                            "Attempting to swap two identical values when top of stack is owned",
                            !rhsOwned || isShallowType(rhsTag));
                    }

                    break;
                }
                case Instruction::add: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericAdd(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::sub: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericSub(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::mul: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericMul(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::div: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericDiv(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::idiv: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericIDiv(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::mod: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericMod(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::negate: {
                    auto [owned, tag, val] = getFromStack(0);

                    auto [resultOwned, resultTag, resultVal] = genericSub(
                        value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0), tag, val);

                    topStack(resultOwned, resultTag, resultVal);

                    if (owned) {
                        value::releaseValue(resultTag, resultVal);
                    }

                    break;
                }
                case Instruction::numConvert: {
                    auto tag = readFromMemory<value::TypeTags>(pcPointer);
                    pcPointer += sizeof(tag);

                    auto [owned, lhsTag, lhsVal] = getFromStack(0);

                    auto [rhsOwned, rhsTag, rhsVal] = genericNumConvert(lhsTag, lhsVal, tag);

                    topStack(rhsOwned, rhsTag, rhsVal);

                    if (owned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }

                    break;
                }
                case Instruction::logicNot: {
                    auto [owned, tag, val] = getFromStack(0);

                    auto [resultTag, resultVal] = genericNot(tag, val);

                    topStack(false, resultTag, resultVal);

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::less: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] = genericCompare<std::less<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::collLess: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);

                    auto [tag, val] = genericCompare<std::less<>>(
                        lhsTag, lhsVal, rhsTag, rhsVal, collTag, collVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    break;
                }
                case Instruction::lessEq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] =
                        genericCompare<std::less_equal<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::collLessEq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);

                    auto [tag, val] = genericCompare<std::less_equal<>>(
                        lhsTag, lhsVal, rhsTag, rhsVal, collTag, collVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    break;
                }
                case Instruction::greater: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] =
                        genericCompare<std::greater<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::collGreater: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);

                    auto [tag, val] = genericCompare<std::greater<>>(
                        lhsTag, lhsVal, rhsTag, rhsVal, collTag, collVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    break;
                }
                case Instruction::greaterEq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] =
                        genericCompare<std::greater_equal<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::collGreaterEq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);

                    auto [tag, val] = genericCompare<std::greater_equal<>>(
                        lhsTag, lhsVal, rhsTag, rhsVal, collTag, collVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    break;
                }
                case Instruction::eq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] =
                        genericCompare<std::equal_to<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::collEq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);

                    auto [tag, val] = genericCompare<std::equal_to<>>(
                        lhsTag, lhsVal, rhsTag, rhsVal, collTag, collVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    break;
                }
                case Instruction::neq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] =
                        genericCompare<std::equal_to<>>(lhsTag, lhsVal, rhsTag, rhsVal);
                    std::tie(tag, val) = genericNot(tag, val);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::collNeq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);

                    auto [tag, val] = genericCompare<std::equal_to<>>(
                        lhsTag, lhsVal, rhsTag, rhsVal, collTag, collVal);
                    std::tie(tag, val) = genericNot(tag, val);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    break;
                }
                case Instruction::cmp3w: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] = compare3way(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::collCmp3w: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);

                    auto [tag, val] = compare3way(lhsTag, lhsVal, rhsTag, rhsVal, collTag, collVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    break;
                }
                case Instruction::fillEmpty: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    if (lhsTag == value::TypeTags::Nothing) {
                        topStack(rhsOwned, rhsTag, rhsVal);

                        if (lhsOwned) {
                            value::releaseValue(lhsTag, lhsVal);
                        }
                    } else {
                        if (rhsOwned) {
                            value::releaseValue(rhsTag, rhsVal);
                        }
                    }
                    break;
                }
                case Instruction::getField: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = getField(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::getElement: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = getElement(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::collComparisonKey: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    if (lhsTag != value::TypeTags::Nothing && rhsTag == value::TypeTags::collator) {
                        // If lhs is a collatable type, call collComparisonKey() to obtain the
                        // comparison key. If lhs is not a collatable type, we can just leave it
                        // on the stack as-is.
                        if (value::isCollatableType(lhsTag)) {
                            auto collator = value::getCollatorView(rhsVal);
                            auto [tag, val] = collComparisonKey(lhsTag, lhsVal, collator);
                            topStack(true, tag, val);
                        } else {
                            // Set 'lhsOwned' to false so that lhs doesn't get released below.
                            lhsOwned = false;
                        }
                    } else {
                        // If lhs was Nothing or rhs wasn't Collator, return Nothing.
                        topStack(false, value::TypeTags::Nothing, 0);
                    }

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggSum: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggSum(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggMin: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggMin(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggCollMin: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] =
                        aggCollMin(lhsTag, lhsVal, collTag, collVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggMax: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggMax(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggCollMax: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [collOwned, collTag, collVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] =
                        aggCollMax(lhsTag, lhsVal, collTag, collVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (collOwned) {
                        value::releaseValue(collTag, collVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggFirst: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggFirst(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggLast: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggLast(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::exists: {
                    auto [owned, tag, val] = getFromStack(0);

                    topStack(false,
                             value::TypeTags::Boolean,
                             value::bitcastFrom<bool>(tag != value::TypeTags::Nothing));

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isNull: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(tag == value::TypeTags::Null));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isObject: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(value::isObject(tag)));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isArray: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(value::isArray(tag)));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isString: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(value::isString(tag)));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isNumber: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(value::isNumber(tag)));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isBinData: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(value::isBinData(tag)));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isDate: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(tag == value::TypeTags::Date));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isNaN: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(value::isNaN(tag, val)));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isRecordId: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(value::isRecordId(tag)));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isMinKey: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(tag == value::TypeTags::MinKey));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isMaxKey: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false,
                                 value::TypeTags::Boolean,
                                 value::bitcastFrom<bool>(tag == value::TypeTags::MaxKey));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::typeMatch: {
                    auto typeMask = readFromMemory<uint32_t>(pcPointer);
                    pcPointer += sizeof(typeMask);

                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        bool matches = static_cast<bool>(getBSONTypeMask(tag) & typeMask);
                        topStack(
                            false, value::TypeTags::Boolean, value::bitcastFrom<bool>(matches));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::function:
                case Instruction::functionSmall: {
                    auto f = readFromMemory<Builtin>(pcPointer);
                    pcPointer += sizeof(f);
                    ArityType arity{0};
                    if (i.tag == Instruction::function) {
                        arity = readFromMemory<ArityType>(pcPointer);
                        pcPointer += sizeof(ArityType);
                    } else {
                        arity = readFromMemory<SmallArityType>(pcPointer);
                        pcPointer += sizeof(SmallArityType);
                    }

                    auto [owned, tag, val] = dispatchBuiltin(f, arity);

                    for (ArityType cnt = 0; cnt < arity; ++cnt) {
                        auto [owned, tag, val] = getFromStack(0);
                        popStack();
                        if (owned) {
                            value::releaseValue(tag, val);
                        }
                    }

                    pushStack(owned, tag, val);

                    break;
                }
                case Instruction::jmp: {
                    auto jumpOffset = readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(jumpOffset);

                    pcPointer += jumpOffset;
                    break;
                }
                case Instruction::jmpTrue: {
                    auto jumpOffset = readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(jumpOffset);

                    auto [owned, tag, val] = getFromStack(0);
                    popStack();

                    if (tag == value::TypeTags::Boolean && value::bitcastTo<bool>(val)) {
                        pcPointer += jumpOffset;
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::jmpNothing: {
                    auto jumpOffset = readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(jumpOffset);

                    auto [owned, tag, val] = getFromStack(0);
                    if (tag == value::TypeTags::Nothing) {
                        pcPointer += jumpOffset;
                    }
                    break;
                }
                case Instruction::fail: {
                    auto [ownedCode, tagCode, valCode] = getFromStack(1);
                    invariant(tagCode == value::TypeTags::NumberInt64);

                    auto [ownedMsg, tagMsg, valMsg] = getFromStack(0);
                    invariant(value::isString(tagMsg));

                    ErrorCodes::Error code{
                        static_cast<ErrorCodes::Error>(value::bitcastTo<int64_t>(valCode))};
                    std::string message{value::getStringView(tagMsg, valMsg)};

                    uasserted(code, message);

                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }
    uassert(
        4822801, "The evaluation stack must hold only a single value", _argStackOwned.size() == 1);

    auto owned = _argStackOwned[0];
    auto tag = _argStackTags[0];
    auto val = _argStackVals[0];

    _argStackOwned.clear();
    _argStackTags.clear();
    _argStackVals.clear();

    return {owned, tag, val};
}

bool ByteCode::runPredicate(const CodeFragment* code) {
    auto [owned, tag, val] = run(code);

    bool pass = (tag == value::TypeTags::Boolean) && value::bitcastTo<bool>(val);

    if (owned) {
        value::releaseValue(tag, val);
    }

    return pass;
}
}  // namespace vm
}  // namespace sbe
}  // namespace mongo
