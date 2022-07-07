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

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/util/make_data_structure.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/util/str.h"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

namespace mongo::stage_builder {
namespace {
struct ExpressionVisitorContext {
    struct VarsFrame {
        std::deque<Variables::Id> variablesToBind;

        // Slots that have been used to bind $let variables. This list is necessary to know which
        // slots to remove from the environment when the $let goes out of scope.
        std::set<sbe::value::SlotId> slotsForLetVariables;

        template <class... Args>
        VarsFrame(Args&&... args)
            : variablesToBind{std::forward<Args>(args)...}, slotsForLetVariables{} {}
    };

    ExpressionVisitorContext(StageBuilderState& state,
                             EvalStage inputStage,
                             sbe::value::SlotId rootSlot,
                             PlanNodeId planNodeId)
        : state(state), rootSlot(rootSlot), planNodeId(planNodeId) {
        evalStack.emplaceFrame(std::move(inputStage));
    }

    void ensureArity(size_t arity) {
        invariant(evalStack.topFrame().exprsCount() >= arity);
    }

    EvalStage extractCurrentEvalStage() {
        return evalStack.topFrame().extractStage();
    }

    void setCurrentStage(EvalStage stage) {
        evalStack.topFrame().setStage(std::move(stage));
    }

    std::unique_ptr<sbe::EExpression> popExpr() {
        return evalStack.topFrame().popExpr().extractExpr();
    }

    void pushExpr(std::unique_ptr<sbe::EExpression> expr) {
        evalStack.topFrame().pushExpr(std::move(expr));
    }

    void pushExpr(std::unique_ptr<sbe::EExpression> expr, EvalStage stage) {
        pushExpr(std::move(expr));
        evalStack.topFrame().setStage(std::move(stage));
    }

    std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> popFrame() {
        auto [expr, stage] = evalStack.popFrame();
        return {expr.extractExpr(), std::move(stage)};
    }

    sbe::value::SlotVector getLexicalEnvironment() {
        sbe::value::SlotVector lexicalEnvironment;
        for (const auto& [_, slot] : environment) {
            lexicalEnvironment.push_back(slot);
        }
        return lexicalEnvironment;
    }

    std::tuple<sbe::value::SlotId, std::unique_ptr<sbe::EExpression>, EvalStage> done() {
        invariant(evalStack.framesCount() == 1);
        auto [expr, stage] = popFrame();
        return {state.slotId(), std::move(expr), stageOrLimitCoScan(std::move(stage), planNodeId)};
    }

    StageBuilderState& state;

    EvalStack<> evalStack;

    sbe::value::SlotId rootSlot;

    // The lexical environment for the expression being traversed. A variable reference takes the
    // form "$$variable_name" in MQL's concrete syntax and gets transformed into a numeric
    // identifier (Variables::Id) in the AST. During this translation, we directly translate any
    // such variable to an SBE slot using this mapping.
    std::map<Variables::Id, sbe::value::SlotId> environment;
    std::stack<VarsFrame> varsFrameStack;

    // The id of the QuerySolutionNode to which the expression we are converting to SBE is attached.
    const PlanNodeId planNodeId;
};

std::pair<sbe::value::SlotId, EvalStage> generateTraverseHelper(
    EvalStage inputStage,
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    size_t level,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    // The field we will be traversing at the current nested level.
    auto fieldSlot{slotIdGenerator->generate()};
    // The result coming from the 'in' branch of the traverse plan stage.
    auto outputSlot{slotIdGenerator->generate()};

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldSlot'.
    inputStage = makeProject(std::move(inputStage),
                             planNodeId,
                             fieldSlot,
                             makeFunction("getField"_sd,
                                          sbe::makeE<sbe::EVariable>(inputSlot),
                                          sbe::makeE<sbe::EConstant>(fp.getFieldName(level))));

    EvalStage innerBranch;
    if (level == fp.getPathLength() - 1) {
        innerBranch = makeProject(makeLimitCoScanStage(planNodeId),
                                  planNodeId,
                                  outputSlot,
                                  sbe::makeE<sbe::EVariable>(fieldSlot));
    } else {
        // Generate nested traversal.
        auto [slot, stage] = generateTraverseHelper(makeLimitCoScanStage(planNodeId),
                                                    fieldSlot,
                                                    fp,
                                                    level + 1,
                                                    planNodeId,
                                                    slotIdGenerator);
        innerBranch =
            makeProject(std::move(stage), planNodeId, outputSlot, sbe::makeE<sbe::EVariable>(slot));
    }

    // The final traverse stage for the current nested level.
    return {outputSlot,
            makeTraverse(std::move(inputStage),
                         std::move(innerBranch),
                         fieldSlot,
                         outputSlot,
                         outputSlot,
                         nullptr,
                         nullptr,
                         planNodeId,
                         1)};
}

/**
 * For the given MatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the comparison expression.
 */
std::pair<sbe::value::SlotId, EvalStage> generateTraverse(
    EvalStage inputStage,
    sbe::value::SlotId inputSlot,
    bool expectsDocumentInputOnly,
    const FieldPath& fp,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    if (expectsDocumentInputOnly) {
        // When we know for sure that 'inputSlot' will be a document and _not_ an array (such as
        // when traversing the root document), we can generate a simpler expression.
        return generateTraverseHelper(
            std::move(inputStage), inputSlot, fp, 0, planNodeId, slotIdGenerator);
    } else {
        // The general case: the value in the 'inputSlot' may be an array that will require
        // traversal.
        auto outputSlot{slotIdGenerator->generate()};
        auto [innerBranchOutputSlot, innerBranch] =
            generateTraverseHelper(makeLimitCoScanStage(planNodeId),
                                   inputSlot,
                                   fp,
                                   0,  // level
                                   planNodeId,
                                   slotIdGenerator);
        return {outputSlot,
                makeTraverse(std::move(inputStage),
                             std::move(innerBranch),
                             inputSlot,
                             outputSlot,
                             innerBranchOutputSlot,
                             nullptr,
                             nullptr,
                             planNodeId,
                             1)};
    }
}

/**
 * Generates an EExpression that converts the input to upper or lower case.
 */
void generateStringCaseConversionExpression(ExpressionVisitorContext* _context,
                                            const std::string& caseConversionFunction) {
    auto frameId = _context->state.frameId();
    auto str = sbe::makeEs(_context->popExpr());
    sbe::EVariable inputRef(frameId, 0);
    uint32_t typeMask = (getBSONTypeMask(sbe::value::TypeTags::StringSmall) |
                         getBSONTypeMask(sbe::value::TypeTags::StringBig) |
                         getBSONTypeMask(sbe::value::TypeTags::bsonString) |
                         getBSONTypeMask(sbe::value::TypeTags::bsonSymbol) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberDecimal) |
                         getBSONTypeMask(sbe::value::TypeTags::Date) |
                         getBSONTypeMask(sbe::value::TypeTags::Timestamp));
    auto checkValidTypeExpr = sbe::makeE<sbe::ETypeMatch>(inputRef.clone(), typeMask);
    auto checkNullorMissing = generateNullOrMissing(inputRef);
    auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");

    auto caseConversionExpr = sbe::makeE<sbe::EIf>(
        std::move(checkValidTypeExpr),
        makeFunction(caseConversionFunction, makeFunction("coerceToString", inputRef.clone())),
        sbe::makeE<sbe::EFail>(ErrorCodes::Error{5066300},
                               str::stream() << "$" << caseConversionFunction
                                             << " input type is not supported"));

    auto totalCaseConversionExpr =
        sbe::makeE<sbe::EIf>(std::move(checkNullorMissing),
                             sbe::makeE<sbe::EConstant>(emptyStrTag, emptyStrVal),
                             std::move(caseConversionExpr));
    _context->pushExpr(
        sbe::makeE<sbe::ELocalBind>(frameId, std::move(str), std::move(totalCaseConversionExpr)));
}

void buildArrayAccessByConstantIndex(ExpressionVisitorContext* context,
                                     const std::string& exprName,
                                     int32_t index) {
    context->ensureArity(1);

    auto array = context->popExpr();

    auto frameId = context->state.frameId();
    auto binds = sbe::makeEs(std::move(array));
    sbe::EVariable arrayRef{frameId, 0};

    auto indexExpr = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                sbe::value::bitcastFrom<int32_t>(index));
    auto argumentIsNotArray = makeNot(makeFunction("isArray", arrayRef.clone()));
    auto resultExpr = buildMultiBranchConditional(
        CaseValuePair{generateNullOrMissing(arrayRef),
                      sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
        CaseValuePair{std::move(argumentIsNotArray),
                      sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126704},
                                             exprName + " argument must be an array")},
        makeFunction("getElement", arrayRef.clone(), std::move(indexExpr)));

    context->pushExpr(
        sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(resultExpr)));
}

/**
 * Generate an EExpression representing a Regex function result upon null argument(s) depending on
 * the type of the function: $regexMatch - false, $regexFind - null, $RegexFindAll - [].
 */
std::unique_ptr<sbe::EExpression> generateRegexNullResponse(StringData exprName) {
    if (exprName.toString().compare(std::string("regexMatch")) == 0) {
        return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                          sbe::value::bitcastFrom<bool>(false));
    } else if (exprName.toString().compare("regexFindAll") == 0) {
        auto [arrTag, arrVal] = sbe::value::makeNewArray();
        return sbe::makeE<sbe::EConstant>(arrTag, arrVal);
    }
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0);
}

class ExpressionPreVisitor final : public ExpressionVisitor {
public:
    ExpressionPreVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(ExpressionConstant* expr) final {}
    void visit(ExpressionAbs* expr) final {}
    void visit(ExpressionAdd* expr) final {}
    void visit(ExpressionAllElementsTrue* expr) final {}
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {}
    void visit(ExpressionArray* expr) final {}
    void visit(ExpressionArrayElemAt* expr) final {}
    void visit(ExpressionFirst* expr) final {}
    void visit(ExpressionLast* expr) final {}
    void visit(ExpressionObjectToArray* expr) final {}
    void visit(ExpressionArrayToObject* expr) final {}
    void visit(ExpressionBsonSize* expr) final {}
    void visit(ExpressionCeil* expr) final {}
    void visit(ExpressionCoerceToBool* expr) final {}
    void visit(ExpressionCompare* expr) final {}
    void visit(ExpressionConcat* expr) final {}
    void visit(ExpressionConcatArrays* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionCond* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionDateDiff* expr) final {}
    void visit(ExpressionDateFromString* expr) final {}
    void visit(ExpressionDateFromParts* expr) final {}
    void visit(ExpressionDateToParts* expr) final {}
    void visit(ExpressionDateToString* expr) final {}
    void visit(ExpressionDateTrunc* expr) final {}
    void visit(ExpressionDivide* expr) final {}
    void visit(ExpressionExp* expr) final {}
    void visit(ExpressionFieldPath* expr) final {}
    void visit(ExpressionFilter* expr) final {}
    void visit(ExpressionFloor* expr) final {}
    void visit(ExpressionIfNull* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionIn* expr) final {}
    void visit(ExpressionIndexOfArray* expr) final {}
    void visit(ExpressionIndexOfBytes* expr) final {}
    void visit(ExpressionIndexOfCP* expr) final {}
    void visit(ExpressionIsNumber* expr) final {}
    void visit(ExpressionLet* expr) final {
        _context->varsFrameStack.push(ExpressionVisitorContext::VarsFrame{
            std::begin(expr->getOrderedVariableIds()), std::end(expr->getOrderedVariableIds())});
    }
    void visit(ExpressionLn* expr) final {}
    void visit(ExpressionLog* expr) final {}
    void visit(ExpressionLog10* expr) final {}
    void visit(ExpressionMap* expr) final {}
    void visit(ExpressionMeta* expr) final {}
    void visit(ExpressionMod* expr) final {}
    void visit(ExpressionMultiply* expr) final {}
    void visit(ExpressionNot* expr) final {}
    void visit(ExpressionObject* expr) final {}
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {}
    void visit(ExpressionRange* expr) final {}
    void visit(ExpressionReduce* expr) final {}
    void visit(ExpressionReplaceOne* expr) final {}
    void visit(ExpressionReplaceAll* expr) final {}
    void visit(ExpressionSetDifference* expr) final {}
    void visit(ExpressionSetEquals* expr) final {}
    void visit(ExpressionSetIntersection* expr) final {}
    void visit(ExpressionSetIsSubset* expr) final {}
    void visit(ExpressionSetUnion* expr) final {}
    void visit(ExpressionSize* expr) final {}
    void visit(ExpressionReverseArray* expr) final {}
    void visit(ExpressionSlice* expr) final {}
    void visit(ExpressionIsArray* expr) final {}
    void visit(ExpressionRound* expr) final {}
    void visit(ExpressionSplit* expr) final {}
    void visit(ExpressionSqrt* expr) final {}
    void visit(ExpressionStrcasecmp* expr) final {}
    void visit(ExpressionSubstrBytes* expr) final {}
    void visit(ExpressionSubstrCP* expr) final {}
    void visit(ExpressionStrLenBytes* expr) final {}
    void visit(ExpressionBinarySize* expr) final {}
    void visit(ExpressionStrLenCP* expr) final {}
    void visit(ExpressionSubtract* expr) final {}
    void visit(ExpressionSwitch* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionTestApiVersion* expr) final {}
    void visit(ExpressionToLower* expr) final {}
    void visit(ExpressionToUpper* expr) final {}
    void visit(ExpressionTrim* expr) final {}
    void visit(ExpressionTrunc* expr) final {}
    void visit(ExpressionType* expr) final {}
    void visit(ExpressionZip* expr) final {}
    void visit(ExpressionConvert* expr) final {}
    void visit(ExpressionRegexFind* expr) final {}
    void visit(ExpressionRegexFindAll* expr) final {}
    void visit(ExpressionRegexMatch* expr) final {}
    void visit(ExpressionCosine* expr) final {}
    void visit(ExpressionSine* expr) final {}
    void visit(ExpressionTangent* expr) final {}
    void visit(ExpressionArcCosine* expr) final {}
    void visit(ExpressionArcSine* expr) final {}
    void visit(ExpressionArcTangent* expr) final {}
    void visit(ExpressionArcTangent2* expr) final {}
    void visit(ExpressionHyperbolicArcTangent* expr) final {}
    void visit(ExpressionHyperbolicArcCosine* expr) final {}
    void visit(ExpressionHyperbolicArcSine* expr) final {}
    void visit(ExpressionHyperbolicTangent* expr) final {}
    void visit(ExpressionHyperbolicCosine* expr) final {}
    void visit(ExpressionHyperbolicSine* expr) final {}
    void visit(ExpressionDegreesToRadians* expr) final {}
    void visit(ExpressionRadiansToDegrees* expr) final {}
    void visit(ExpressionDayOfMonth* expr) final {}
    void visit(ExpressionDayOfWeek* expr) final {}
    void visit(ExpressionDayOfYear* expr) final {}
    void visit(ExpressionHour* expr) final {}
    void visit(ExpressionMillisecond* expr) final {}
    void visit(ExpressionMinute* expr) final {}
    void visit(ExpressionMonth* expr) final {}
    void visit(ExpressionSecond* expr) final {}
    void visit(ExpressionWeek* expr) final {}
    void visit(ExpressionIsoWeekYear* expr) final {}
    void visit(ExpressionIsoDayOfWeek* expr) final {}
    void visit(ExpressionIsoWeek* expr) final {}
    void visit(ExpressionYear* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(ExpressionTests::Testable* expr) final {}
    void visit(ExpressionInternalJsEmit* expr) final {}
    void visit(ExpressionInternalFindSlice* expr) final {}
    void visit(ExpressionInternalFindPositional* expr) final {}
    void visit(ExpressionInternalFindElemMatch* expr) final {}
    void visit(ExpressionFunction* expr) final {}
    void visit(ExpressionRandom* expr) final {}
    void visit(ExpressionToHashedIndexKey* expr) final {}
    void visit(ExpressionDateAdd* expr) final {}
    void visit(ExpressionDateSubtract* expr) final {}
    void visit(ExpressionGetField* expr) final {}
    void visit(ExpressionSetField* expr) final {}

private:
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        invariant(logicOp == sbe::EPrimBinary::logicOr || logicOp == sbe::EPrimBinary::logicAnd);

        if (expr->getChildren().size() < 2) {
            // All this bookkeeping is only necessary for short circuiting, so we can skip it if we
            // don't have two or more branches.
            return;
        }

        _context->evalStack.emplaceFrame(EvalStage{});
    }

    ExpressionVisitorContext* _context;
};

class ExpressionInVisitor final : public ExpressionVisitor {
public:
    ExpressionInVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(ExpressionConstant* expr) final {}
    void visit(ExpressionAbs* expr) final {}
    void visit(ExpressionAdd* expr) final {}
    void visit(ExpressionAllElementsTrue* expr) final {}
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {}
    void visit(ExpressionArray* expr) final {}
    void visit(ExpressionArrayElemAt* expr) final {}
    void visit(ExpressionFirst* expr) final {}
    void visit(ExpressionLast* expr) final {}
    void visit(ExpressionObjectToArray* expr) final {}
    void visit(ExpressionArrayToObject* expr) final {}
    void visit(ExpressionBsonSize* expr) final {}
    void visit(ExpressionCeil* expr) final {}
    void visit(ExpressionCoerceToBool* expr) final {}
    void visit(ExpressionCompare* expr) final {}
    void visit(ExpressionConcat* expr) final {}
    void visit(ExpressionConcatArrays* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionCond* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionDateDiff* expr) final {}
    void visit(ExpressionDateFromString* expr) final {}
    void visit(ExpressionDateFromParts* expr) final {}
    void visit(ExpressionDateToParts* expr) final {}
    void visit(ExpressionDateToString* expr) final {}
    void visit(ExpressionDateTrunc*) final {}
    void visit(ExpressionDivide* expr) final {}
    void visit(ExpressionExp* expr) final {}
    void visit(ExpressionFieldPath* expr) final {}
    void visit(ExpressionFilter* expr) final {
        // This visitor executes after visiting the expression that will evaluate to the array for
        // filtering and before visiting the filter condition expression.
        auto variableId = expr->getVariableId();
        invariant(_context->environment.find(variableId) == _context->environment.end());

        auto currentElementSlot = _context->state.slotId();
        _context->environment.insert({variableId, currentElementSlot});

        // Push new frame to provide clean context for sub-tree generated by filter predicate.
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionFloor* expr) final {}
    void visit(ExpressionIfNull* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionIn* expr) final {}
    void visit(ExpressionIndexOfArray* expr) final {}
    void visit(ExpressionIndexOfBytes* expr) final {}
    void visit(ExpressionIndexOfCP* expr) final {}
    void visit(ExpressionIsNumber* expr) final {}
    void visit(ExpressionLet* expr) final {
        // This visitor fires after each variable definition in a $let expression. The top of the
        // _context's expression stack will be an expression defining the variable initializer. We
        // use a separate frame stack ('varsFrameStack') to keep track of which variable we are
        // visiting, so we can appropriately bind the initializer.
        invariant(!_context->varsFrameStack.empty());
        auto& currentFrame = _context->varsFrameStack.top();

        invariant(!currentFrame.variablesToBind.empty());

        auto varToBind = currentFrame.variablesToBind.front();
        currentFrame.variablesToBind.pop_front();

        // We create two bindings. First, the initializer result is bound to a slot when this
        // ProjectStage executes.
        auto slotToBind = _context->state.slotId();
        _context->setCurrentStage(makeProject(_context->extractCurrentEvalStage(),
                                              _context->planNodeId,
                                              slotToBind,
                                              _context->popExpr()));
        currentFrame.slotsForLetVariables.insert(slotToBind);

        // Second, we bind this variables AST-level name (with type Variable::Id) to the SlotId that
        // will be used for compilation and execution. Once this "stage builder" finishes, these
        // Variable::Id bindings will no longer be relevant.
        invariant(_context->environment.find(varToBind) == _context->environment.end());
        _context->environment.insert({varToBind, slotToBind});
    }
    void visit(ExpressionLn* expr) final {}
    void visit(ExpressionLog* expr) final {}
    void visit(ExpressionLog10* expr) final {}
    void visit(ExpressionMap* expr) final {}
    void visit(ExpressionMeta* expr) final {}
    void visit(ExpressionMod* expr) final {}
    void visit(ExpressionMultiply* expr) final {}
    void visit(ExpressionNot* expr) final {}
    void visit(ExpressionObject* expr) final {}
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {}
    void visit(ExpressionRange* expr) final {}
    void visit(ExpressionReduce* expr) final {}
    void visit(ExpressionReplaceOne* expr) final {}
    void visit(ExpressionReplaceAll* expr) final {}
    void visit(ExpressionSetDifference* expr) final {}
    void visit(ExpressionSetEquals* expr) final {}
    void visit(ExpressionSetIntersection* expr) final {}
    void visit(ExpressionSetIsSubset* expr) final {}
    void visit(ExpressionSetUnion* expr) final {}
    void visit(ExpressionSize* expr) final {}
    void visit(ExpressionReverseArray* expr) final {}
    void visit(ExpressionSlice* expr) final {}
    void visit(ExpressionIsArray* expr) final {}
    void visit(ExpressionRound* expr) final {}
    void visit(ExpressionSplit* expr) final {}
    void visit(ExpressionSqrt* expr) final {}
    void visit(ExpressionStrcasecmp* expr) final {}
    void visit(ExpressionSubstrBytes* expr) final {}
    void visit(ExpressionSubstrCP* expr) final {}
    void visit(ExpressionStrLenBytes* expr) final {}
    void visit(ExpressionBinarySize* expr) final {}
    void visit(ExpressionStrLenCP* expr) final {}
    void visit(ExpressionSubtract* expr) final {}
    void visit(ExpressionSwitch* expr) final {
        _context->evalStack.emplaceFrame(EvalStage{});
    }
    void visit(ExpressionTestApiVersion* expr) final {}
    void visit(ExpressionToLower* expr) final {}
    void visit(ExpressionToUpper* expr) final {}
    void visit(ExpressionTrim* expr) final {}
    void visit(ExpressionTrunc* expr) final {}
    void visit(ExpressionType* expr) final {}
    void visit(ExpressionZip* expr) final {}
    void visit(ExpressionConvert* expr) final {}
    void visit(ExpressionRegexFind* expr) final {}
    void visit(ExpressionRegexFindAll* expr) final {}
    void visit(ExpressionRegexMatch* expr) final {}
    void visit(ExpressionCosine* expr) final {}
    void visit(ExpressionSine* expr) final {}
    void visit(ExpressionTangent* expr) final {}
    void visit(ExpressionArcCosine* expr) final {}
    void visit(ExpressionArcSine* expr) final {}
    void visit(ExpressionArcTangent* expr) final {}
    void visit(ExpressionArcTangent2* expr) final {}
    void visit(ExpressionHyperbolicArcTangent* expr) final {}
    void visit(ExpressionHyperbolicArcCosine* expr) final {}
    void visit(ExpressionHyperbolicArcSine* expr) final {}
    void visit(ExpressionHyperbolicTangent* expr) final {}
    void visit(ExpressionHyperbolicCosine* expr) final {}
    void visit(ExpressionHyperbolicSine* expr) final {}
    void visit(ExpressionDegreesToRadians* expr) final {}
    void visit(ExpressionRadiansToDegrees* expr) final {}
    void visit(ExpressionDayOfMonth* expr) final {}
    void visit(ExpressionDayOfWeek* expr) final {}
    void visit(ExpressionDayOfYear* expr) final {}
    void visit(ExpressionHour* expr) final {}
    void visit(ExpressionMillisecond* expr) final {}
    void visit(ExpressionMinute* expr) final {}
    void visit(ExpressionMonth* expr) final {}
    void visit(ExpressionSecond* expr) final {}
    void visit(ExpressionWeek* expr) final {}
    void visit(ExpressionIsoWeekYear* expr) final {}
    void visit(ExpressionIsoDayOfWeek* expr) final {}
    void visit(ExpressionIsoWeek* expr) final {}
    void visit(ExpressionYear* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(ExpressionTests::Testable* expr) final {}
    void visit(ExpressionInternalJsEmit* expr) final {}
    void visit(ExpressionInternalFindSlice* expr) final {}
    void visit(ExpressionInternalFindPositional* expr) final {}
    void visit(ExpressionInternalFindElemMatch* expr) final {}
    void visit(ExpressionFunction* expr) final {}
    void visit(ExpressionRandom* expr) final {}
    void visit(ExpressionToHashedIndexKey* expr) final {}
    void visit(ExpressionDateAdd* expr) final {}
    void visit(ExpressionDateSubtract* expr) final {}
    void visit(ExpressionGetField* expr) final {}
    void visit(ExpressionSetField* expr) final {}

private:
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        // The infix visitor should only visit expressions with more than one child.
        invariant(expr->getChildren().size() >= 2);
        invariant(logicOp == sbe::EPrimBinary::logicOr || logicOp == sbe::EPrimBinary::logicAnd);
        _context->evalStack.emplaceFrame(EvalStage{});
    }

    ExpressionVisitorContext* _context;
};


struct DoubleBound {
    DoubleBound(double b, bool isInclusive) : bound(b), inclusive(isInclusive) {}

    static DoubleBound minInfinity() {
        return DoubleBound(-std::numeric_limits<double>::infinity(), false);
    }
    static DoubleBound plusInfinity() {
        return DoubleBound(std::numeric_limits<double>::infinity(), false);
    }
    std::string printLowerBound() const {
        return str::stream() << (inclusive ? "[" : "(") << bound;
    }
    std::string printUpperBound() const {
        return str::stream() << bound << (inclusive ? "]" : ")");
    }
    double bound;
    bool inclusive;
};

class ExpressionPostVisitor final : public ExpressionVisitor {
public:
    ExpressionPostVisitor(ExpressionVisitorContext* context) : _context{context} {}

    enum class SetOperation {
        Difference,
        Intersection,
        Union,
    };

    void visit(ExpressionConstant* expr) final {
        auto [tag, val] = makeValue(expr->getValue());
        _context->pushExpr(sbe::makeE<sbe::EConstant>(tag, val));
    }

    void visit(ExpressionAbs* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto absExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903700},
                                                 "$abs only supports numeric types")},
            CaseValuePair{generateLongLongMinCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903701},
                                                 "can't take $abs of long long min")},
            makeFunction("abs", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(absExpr)));
    }

    void visit(ExpressionAdd* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->state.frameId();

        auto generateNotNumberOrDate = [frameId](const sbe::value::SlotId slotId) {
            sbe::EVariable var{frameId, slotId};
            return makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                makeNot(makeFunction("isNumber", var.clone())),
                                makeNot(makeFunction("isDate", var.clone())));
        };

        if (arity == 2) {
            auto rhs = _context->popExpr();
            auto lhs = _context->popExpr();
            auto binds = sbe::makeEs(std::move(lhs), std::move(rhs));
            sbe::EVariable lhsVar{frameId, 0};
            sbe::EVariable rhsVar{frameId, 1};

            auto addExpr = makeLocalBind(
                _context->state.frameIdGenerator,
                [&](sbe::EVariable lhsIsDate, sbe::EVariable rhsIsDate) {
                    return buildMultiBranchConditional(
                        CaseValuePair{makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                   generateNullOrMissing(frameId, 0),
                                                   generateNullOrMissing(frameId, 1)),
                                      sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
                        CaseValuePair{
                            makeBinaryOp(sbe::EPrimBinary::logicOr,
                                         generateNotNumberOrDate(0),
                                         generateNotNumberOrDate(1)),
                            sbe::makeE<sbe::EFail>(
                                ErrorCodes::Error{4974201},
                                "only numbers and dates are allowed in an $add expression")},
                        CaseValuePair{
                            makeBinaryOp(
                                sbe::EPrimBinary::logicAnd, lhsIsDate.clone(), rhsIsDate.clone()),
                            sbe::makeE<sbe::EFail>(ErrorCodes::Error{4974202},
                                                   "only one date allowed in an $add expression")},
                        // An EPrimBinary::add expression, which compiles directly into an "add"
                        // instruction, efficiently handles the general case for for $add with
                        // exactly two operands, but when one of the operands is a date, we need to
                        // use the "doubleDoubleSum" function to perform the required conversions.
                        CaseValuePair{
                            makeBinaryOp(
                                sbe::EPrimBinary::logicOr, lhsIsDate.clone(), rhsIsDate.clone()),
                            makeFunction("doubleDoubleSum", lhsVar.clone(), rhsVar.clone())},
                        makeBinaryOp(sbe::EPrimBinary::add, lhsVar.clone(), rhsVar.clone()));
                },
                makeFunction("isDate", lhsVar.clone()),
                makeFunction("isDate", rhsVar.clone()));

            _context->pushExpr(
                sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(addExpr)));
        } else {
            std::vector<std::unique_ptr<sbe::EExpression>> binds;
            std::vector<std::unique_ptr<sbe::EExpression>> argVars;
            std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNull;
            std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNotNumberOrDate;
            binds.reserve(arity);
            argVars.reserve(arity);
            checkExprsNull.reserve(arity);
            checkExprsNotNumberOrDate.reserve(arity);
            for (size_t idx = 0; idx < arity; ++idx) {
                binds.push_back(_context->popExpr());
                argVars.push_back(sbe::makeE<sbe::EVariable>(frameId, idx));

                checkExprsNull.push_back(generateNullOrMissing(frameId, idx));
                checkExprsNotNumberOrDate.push_back(generateNotNumberOrDate(idx));
            }

            // At this point 'binds' vector contains arguments of $add expression in the reversed
            // order. We need to reverse it back to perform summation in the right order below.
            // Summation in different order can lead to different result because of accumulated
            // precision errors from floating point types.
            std::reverse(std::begin(binds), std::end(binds));

            using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
            auto checkNullAllArguments = std::accumulate(
                std::move_iterator<iter_t>(checkExprsNull.begin() + 1),
                std::move_iterator<iter_t>(checkExprsNull.end()),
                std::move(checkExprsNull.front()),
                [](auto&& acc, auto&& ex) {
                    return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
                });
            auto checkNotNumberOrDateAllArguments = std::accumulate(
                std::move_iterator<iter_t>(checkExprsNotNumberOrDate.begin() + 1),
                std::move_iterator<iter_t>(checkExprsNotNumberOrDate.end()),
                std::move(checkExprsNotNumberOrDate.front()),
                [](auto&& acc, auto&& ex) {
                    return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
                });
            auto addExpr = sbe::makeE<sbe::EIf>(
                std::move(checkNullAllArguments),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                sbe::makeE<sbe::EIf>(
                    std::move(checkNotNumberOrDateAllArguments),
                    sbe::makeE<sbe::EFail>(
                        ErrorCodes::Error{4974203},
                        "only numbers and dates are allowed in an $add expression"),
                    sbe::makeE<sbe::EFunction>("doubleDoubleSum", std::move(argVars))));
            _context->pushExpr(
                sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(addExpr)));
        }
    }

    void visit(ExpressionAllElementsTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArrayElemAt* expr) final {
        _context->ensureArity(2);

        auto index = _context->popExpr();
        auto array = _context->popExpr();

        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(std::move(array), std::move(index));
        sbe::EVariable arrayRef{frameId, 0};
        sbe::EVariable indexRef{frameId, 1};

        auto int32Index = [&]() {
            auto convertedIndex = sbe::makeE<sbe::ENumericConvert>(
                indexRef.clone(), sbe::value::TypeTags::NumberInt32);
            auto frameId = _context->state.frameId();
            auto binds = sbe::makeEs(std::move(convertedIndex));
            sbe::EVariable convertedIndexRef{frameId, 0};

            auto inExpression = sbe::makeE<sbe::EIf>(
                makeFunction("exists", convertedIndexRef.clone()),
                convertedIndexRef.clone(),
                sbe::makeE<sbe::EFail>(
                    ErrorCodes::Error{5126703},
                    "$arrayElemAt second argument cannot be represented as a 32-bit integer"));

            return sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(inExpression));
        }();

        auto anyOfArgumentsIsNullish = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                    generateNullOrMissing(arrayRef),
                                                    generateNullOrMissing(indexRef));
        auto firstArgumentIsNotArray = makeNot(makeFunction("isArray", arrayRef.clone()));
        auto secondArgumentIsNotNumeric = generateNonNumericCheck(indexRef);
        auto arrayElemAtExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(anyOfArgumentsIsNullish),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(firstArgumentIsNotArray),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126701},
                                                 "$arrayElemAt first argument must be an array")},
            CaseValuePair{std::move(secondArgumentIsNotNumeric),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126702},
                                                 "$arrayElemAt second argument must be a number")},
            makeFunction("getElement", arrayRef.clone(), std::move(int32Index)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(arrayElemAtExpr)));
    }
    void visit(ExpressionFirst* expr) final {
        buildArrayAccessByConstantIndex(_context, expr->getOpName(), 0);
    }
    void visit(ExpressionLast* expr) final {
        buildArrayAccessByConstantIndex(_context, expr->getOpName(), -1);
    }
    void visit(ExpressionObjectToArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArrayToObject* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionBsonSize* expr) final {
        // Build an expression which evaluates the size of a BSON document and validates the input
        // argument.
        // 1. If the argument is null or empty, return null.
        // 2. Else, if the argument is a BSON document, return its size.
        // 3. Else, raise an error.

        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto bsonSizeExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonObjectCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5043001},
                                                 "$bsonSize requires a document input")},
            makeFunction("bsonSize", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(bsonSizeExpr)));
    }
    void visit(ExpressionCeil* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto ceilExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903702},
                                                 "$ceil only supports numeric types")},
            makeFunction("ceil", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(ceilExpr)));
    }
    void visit(ExpressionCoerceToBool* expr) final {
        // Since $coerceToBool is internal-only and there are not yet any input expressions that
        // generate an ExpressionCoerceToBool expression, we will leave it as unreachable for now.
        MONGO_UNREACHABLE;
    }
    void visit(ExpressionCompare* expr) final {
        _context->ensureArity(2);
        std::vector<std::unique_ptr<sbe::EExpression>> operands(2);
        for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
            *it = _context->popExpr();
        }

        auto frameId = _context->state.frameId();
        sbe::EVariable lhsRef(frameId, 0);
        sbe::EVariable rhsRef(frameId, 1);

        auto comparisonOperator = [expr]() {
            switch (expr->getOp()) {
                case ExpressionCompare::CmpOp::EQ:
                    return sbe::EPrimBinary::eq;
                case ExpressionCompare::CmpOp::NE:
                    return sbe::EPrimBinary::neq;
                case ExpressionCompare::CmpOp::GT:
                    return sbe::EPrimBinary::greater;
                case ExpressionCompare::CmpOp::GTE:
                    return sbe::EPrimBinary::greaterEq;
                case ExpressionCompare::CmpOp::LT:
                    return sbe::EPrimBinary::less;
                case ExpressionCompare::CmpOp::LTE:
                    return sbe::EPrimBinary::lessEq;
                case ExpressionCompare::CmpOp::CMP:
                    return sbe::EPrimBinary::cmp3w;
            }
            MONGO_UNREACHABLE;
        }();

        // We use the "cmp3w" primitive for every comparison, because it "type brackets" its
        // comparisons (for example, a number will always compare as less than a string). The other
        // comparison primitives are designed for comparing values of the same type.
        auto cmp3w = makeBinaryOp(
            sbe::EPrimBinary::cmp3w, lhsRef.clone(), rhsRef.clone(), _context->state.env);
        auto cmp = (comparisonOperator == sbe::EPrimBinary::cmp3w)
            ? std::move(cmp3w)
            : makeBinaryOp(comparisonOperator,
                           std::move(cmp3w),
                           sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                      sbe::value::bitcastFrom<int32_t>(0)));

        // If either operand evaluates to "Nothing", then the entire operation expressed by 'cmp'
        // will also evaluate to "Nothing". MQL comparisons, however, treat "Nothing" as if it is a
        // value that is less than everything other than MinKey. (Notably, two expressions that
        // evaluate to "Nothing" are considered equal to each other.)
        // We also need to explicitly check for 'bsonUndefined' type because it is considered equal
        // to "Nothing" according to MQL semantics.
        auto generateExists = [&](const sbe::EVariable& var) {
            return makeBinaryOp(
                sbe::EPrimBinary::logicAnd,
                makeFunction("exists", var.clone()),
                sbe::makeE<sbe::ETypeMatch>(var.clone(), ~getBSONTypeMask(BSONType::Undefined)));
        };

        auto nothingFallbackCmp =
            makeBinaryOp(comparisonOperator, generateExists(lhsRef), generateExists(rhsRef));

        auto cmpWithFallback =
            makeFunction("fillEmpty", std::move(cmp), std::move(nothingFallbackCmp));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(operands), std::move(cmpWithFallback)));
    }

    void visit(ExpressionConcat* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->state.frameId();

        std::vector<std::unique_ptr<sbe::EExpression>> binds;
        std::vector<std::unique_ptr<sbe::EExpression>> checkNullArg;
        std::vector<std::unique_ptr<sbe::EExpression>> checkStringArg;
        std::vector<std::unique_ptr<sbe::EExpression>> argVars;
        sbe::value::SlotId slot{0};
        for (size_t idx = 0; idx < arity; ++idx, ++slot) {
            sbe::EVariable var(frameId, slot);
            binds.push_back(_context->popExpr());
            checkNullArg.push_back(generateNullOrMissing(frameId, slot));
            checkStringArg.push_back(makeFunction("isString", var.clone()));
            argVars.push_back(var.clone());
        }
        std::reverse(std::begin(binds), std::end(binds));

        using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;

        auto checkNullAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkNullArg.begin() + 1),
            std::move_iterator<iter_t>(checkNullArg.end()),
            std::move(checkNullArg.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });

        auto checkStringAllArguments = std::accumulate(
            std::move_iterator<iter_t>(checkStringArg.begin() + 1),
            std::move_iterator<iter_t>(checkStringArg.end()),
            std::move(checkStringArg.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicAnd, std::move(acc), std::move(ex));
            });

        auto concatExpr = sbe::makeE<sbe::EIf>(
            std::move(checkNullAnyArgument),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(std::move(checkStringAllArguments),
                                 sbe::makeE<sbe::EFunction>("concat", std::move(argVars)),
                                 sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073001},
                                                        "$concat supports only strings")));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(concatExpr)));
    }

    void visit(ExpressionConcatArrays* expr) final {
        // Pop eval frames pushed by pre and in visitors off the stack.
        std::vector<EvalExprStagePair> branches;
        auto numChildren = expr->getChildren().size();
        branches.reserve(numChildren);
        for (size_t idx = 0; idx < numChildren; ++idx) {
            auto [branchExpr, branchEvalStage] = _context->popFrame();
            branches.emplace_back(std::move(branchExpr), std::move(branchEvalStage));
        }
        std::reverse(branches.begin(), branches.end());

        auto getUnionOutputSlot = [](EvalExpr& unionEvalExpr) {
            auto slot = *(unionEvalExpr.getSlot());
            invariant(slot);
            return slot;
        };

        auto makeNullLimitCoscanTree = [&]() {
            auto outputSlot = _context->state.slotId();
            auto nullEvalStage =
                makeProject({makeLimitCoScanTree(_context->planNodeId), sbe::makeSV()},
                            _context->planNodeId,
                            outputSlot,
                            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0));
            return std::make_pair(outputSlot, std::move(nullEvalStage));
        };

        // Build a union stage to consolidate array input branches into a stream.
        auto [unionEvalExpr, unionEvalStage] = generateUnion(
            std::move(branches), {}, _context->planNodeId, _context->state.slotIdGenerator);
        auto unionSlot = getUnionOutputSlot(unionEvalExpr);
        sbe::EVariable unionVar{unionSlot};

        // Filter stage to EFail if an element is not an array, null, or missing, and EOF if an
        // element is null or missing: not(isNullOrMissing) && (isArray || EFail).
        auto filterExpr = makeBinaryOp(
            sbe::EPrimBinary::logicAnd,
            makeNot(generateNullOrMissing(unionVar)),
            makeBinaryOp(sbe::EPrimBinary::logicOr,
                         makeFunction("isArray", unionVar.clone()),
                         sbe::makeE<sbe::EFail>(ErrorCodes::Error{5153400},
                                                "$concatArrays only supports arrays")));
        auto filter = makeFilter<false, true>(
            std::move(unionEvalStage), std::move(filterExpr), _context->planNodeId);

        // Create a union stage to replace any values filtered out by the previous stage with null.
        // For example, [a, b, null, c, d] would become [a, b, null].
        std::vector<EvalExprStagePair> unionWithNullBranches;
        unionWithNullBranches.emplace_back(sbe::makeE<sbe::EVariable>(unionSlot),
                                           std::move(filter));
        unionWithNullBranches.emplace_back(makeNullLimitCoscanTree());
        auto [unionWithNullExpr, unionWithNullStage] =
            generateUnion(std::move(unionWithNullBranches),
                          {},
                          _context->planNodeId,
                          _context->state.slotIdGenerator);
        auto unionWithNullSlot = getUnionOutputSlot(unionWithNullExpr);

        // Create a limit stage to EOF once numChildren results have been obtained.
        auto limitNumChildren =
            makeLimitSkip(std::move(unionWithNullStage), _context->planNodeId, numChildren);

        // Create a group stage to aggregate elements into a single array.
        auto collatorSlot = _context->state.env->getSlotIfExists("collator"_sd);
        auto addToArrayExpr =
            makeFunction("addToArray", sbe::makeE<sbe::EVariable>(unionWithNullSlot));
        auto groupSlot = _context->state.slotId();
        auto groupStage = makeHashAgg(std::move(limitNumChildren),
                                      sbe::makeSV(),
                                      sbe::makeEM(groupSlot, std::move(addToArrayExpr)),
                                      collatorSlot,
                                      _context->planNodeId);

        // Build subtree to handle nulls. If an input is null, return null. Otherwise, unwind the
        // input twice, and concatenate it into an array using addToArray. This is necessary to
        // implement the MQL behavior where one null or missing input results in a null output.

        // Create two unwind stages to unwind the array that was built from inputs
        // and unwind each input array into its constituent elements. We need a limit 1/coscan stage
        // here to call getNext() on, but we use the output slot of groupStage to obtain the array
        // of inputs.
        auto unwindEvalStage = makeUnwind(
            makeUnwind({makeLimitCoScanStage(_context->planNodeId).stage, sbe::makeSV(groupSlot)},
                       _context->state.slotIdGenerator,
                       _context->planNodeId),
            _context->state.slotIdGenerator,
            _context->planNodeId);
        auto unwindSlot = unwindEvalStage.outSlots.front();

        // Create a group stage to append all streamed elements into one array. This is the final
        // output when the input consists entirely of arrays.
        auto finalAddToArrayExpr =
            makeFunction("addToArray", sbe::makeE<sbe::EVariable>(unwindSlot));
        auto finalGroupSlot = _context->state.slotId();
        auto finalGroupStage =
            makeHashAgg(std::move(unwindEvalStage),
                        sbe::makeSV(),
                        sbe::makeEM(finalGroupSlot, std::move(finalAddToArrayExpr)),
                        collatorSlot,
                        _context->planNodeId);

        // Create a branch stage to select between the branch that produces one null if any elements
        // in the original input were null or missing, or otherwise select the branch that unwinds
        // and concatenates elements into the output array.
        auto [nullSlot, nullStage] = makeNullLimitCoscanTree();
        auto nullIsMemberExpr =
            makeIsMember(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                         sbe::makeE<sbe::EVariable>(groupSlot));
        auto branchSlot = _context->state.slotId();
        auto branchNullEvalStage = makeBranch(std::move(nullStage),
                                              std::move(finalGroupStage),
                                              std::move(nullIsMemberExpr),
                                              sbe::makeSV(nullSlot),
                                              sbe::makeSV(finalGroupSlot),
                                              sbe::makeSV(branchSlot),
                                              _context->planNodeId);

        // Create nlj to connect outer group with inner branch that handles null input.
        auto nljStage = makeLoopJoin(std::move(groupStage),
                                     std::move(branchNullEvalStage),
                                     _context->planNodeId,
                                     _context->getLexicalEnvironment());

        // Top level nlj to inject input slots.
        auto finalNljStage = makeLoopJoin(_context->extractCurrentEvalStage(),
                                          std::move(nljStage),
                                          _context->planNodeId,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(sbe::makeE<sbe::EVariable>(branchSlot), std::move(finalNljStage));
    }
    void visit(ExpressionCond* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(ExpressionDateDiff* expr) final {
        using namespace std::literals;
        auto frameId = _context->state.frameId();
        std::vector<std::unique_ptr<sbe::EExpression>> arguments;
        std::vector<std::unique_ptr<sbe::EExpression>> bindings;
        sbe::EVariable startDateRef(frameId, 0);
        sbe::EVariable endDateRef(frameId, 1);
        sbe::EVariable unitRef(frameId, 2);
        sbe::EVariable timezoneRef(frameId, 3);
        sbe::EVariable startOfWeekRef(frameId, 4);

        // An auxiliary boolean variable to hold a value of a common subexpression 'unit'=="week"
        // (string).
        sbe::EVariable unitIsWeekRef(frameId, 5);

        auto children = expr->getChildren();
        invariant(children.size() == 5);
        _context->ensureArity(3 + (expr->isTimezoneSpecified() ? 1 : 0) +
                              (expr->isStartOfWeekSpecified() ? 1 : 0));

        // Get child expressions.
        auto startOfWeekExpression = expr->isStartOfWeekSpecified() ? _context->popExpr() : nullptr;
        auto timezoneExpression =
            expr->isTimezoneSpecified() ? _context->popExpr() : makeConstant("UTC"_sd);
        auto unitExpression = _context->popExpr();
        auto endDateExpression = _context->popExpr();
        auto startDateExpression = _context->popExpr();

        auto timezoneDBSlot = _context->state.env->getSlot("timeZoneDB"_sd);

        //  Set parameters for an invocation of built-in "dateDiff" function.
        arguments.push_back(sbe::makeE<sbe::EVariable>(timezoneDBSlot));
        arguments.push_back(startDateRef.clone());
        arguments.push_back(endDateRef.clone());
        arguments.push_back(unitRef.clone());
        arguments.push_back(timezoneRef.clone());
        if (expr->isStartOfWeekSpecified()) {
            // Parameter "startOfWeek" - if the time unit is the week, then pass value of parameter
            // "startOfWeek" of "$dateDiff" expression, otherwise pass a valid default value, since
            // "dateDiff" built-in function does not accept non-string type values for this
            // parameter.
            arguments.push_back(sbe::makeE<sbe::EIf>(
                unitIsWeekRef.clone(), startOfWeekRef.clone(), makeConstant("sun"_sd)));
        }

        // Set bindings for the frame.
        bindings.push_back(std::move(startDateExpression));
        bindings.push_back(std::move(endDateExpression));
        bindings.push_back(std::move(unitExpression));
        bindings.push_back(std::move(timezoneExpression));
        if (expr->isStartOfWeekSpecified()) {
            bindings.push_back(std::move(startOfWeekExpression));
            bindings.push_back(generateIsEqualToStringCheck(unitRef, "week"_sd));
        }

        // Create an expression to invoke built-in "dateDiff" function.
        auto dateDiffFunctionCall = sbe::makeE<sbe::EFunction>("dateDiff"_sd, std::move(arguments));

        // Create expressions to check that each argument to "dateDiff" function exists, is not
        // null, and is of the correct type.
        std::vector<CaseValuePair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateReturnNullIfNullOrMissing(startDateRef));
        inputValidationCases.push_back(generateReturnNullIfNullOrMissing(endDateRef));
        inputValidationCases.push_back(generateReturnNullIfNullOrMissing(unitRef));
        inputValidationCases.push_back(generateReturnNullIfNullOrMissing(timezoneRef));
        if (expr->isStartOfWeekSpecified()) {
            inputValidationCases.emplace_back(
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                             unitIsWeekRef.clone(),
                                             generateNullOrMissing(startOfWeekRef)),
                makeConstant(sbe::value::TypeTags::Null, 0));
        }

        // "timezone" parameter validation.
        inputValidationCases.emplace_back(
            generateNonStringCheck(timezoneRef),
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166504},
                                   "$dateDiff parameter 'timezone' must be a string"));
        inputValidationCases.emplace_back(
            makeNot(makeFunction(
                "isTimezone", sbe::makeE<sbe::EVariable>(timezoneDBSlot), timezoneRef.clone())),
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166505},
                                   "$dateDiff parameter 'timezone' must be a valid timezone"));

        // "startDate" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            startDateRef, ErrorCodes::Error{5166500}, "$dateDiff"_sd, "startDate"_sd));

        // "endDate" parameter validation.
        inputValidationCases.emplace_back(generateFailIfNotCoercibleToDate(
            endDateRef, ErrorCodes::Error{5166501}, "$dateDiff"_sd, "endDate"_sd));

        // "unit" parameter validation.
        inputValidationCases.emplace_back(
            generateNonStringCheck(unitRef),
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166502},
                                   "$dateDiff parameter 'unit' must be a string"));
        inputValidationCases.emplace_back(
            makeNot(makeFunction("isTimeUnit", unitRef.clone())),
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166503},
                                   "$dateDiff parameter 'unit' must be a valid time unit"));

        // "startOfWeek" parameter validation.
        if (expr->isStartOfWeekSpecified()) {
            // If 'timeUnit' value is equal to "week" then validate "startOfWeek" parameter.
            inputValidationCases.emplace_back(
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                             unitIsWeekRef.clone(),
                                             generateNonStringCheck(startOfWeekRef)),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5338801},
                                       "$dateDiff parameter 'startOfWeek' must be a string"));
            inputValidationCases.emplace_back(
                sbe::makeE<sbe::EPrimBinary>(
                    sbe::EPrimBinary::logicAnd,
                    unitIsWeekRef.clone(),
                    makeNot(makeFunction("isDayOfWeek", startOfWeekRef.clone()))),
                sbe::makeE<sbe::EFail>(
                    ErrorCodes::Error{5338802},
                    "$dateDiff parameter 'startOfWeek' must be a valid day of the week"));
        }

        auto dateDiffExpression = buildMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases), std::move(dateDiffFunctionCall));
        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(bindings), std::move(dateDiffExpression)));
    }
    void visit(ExpressionDateFromString* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(ExpressionDateFromParts* expr) final {
        // This expression can carry null children depending on the set of fields provided,
        // to compute a date from parts so we only need to pop if a child exists.
        auto children = expr->getChildren();
        invariant(children.size() == 11);

        auto eTimezone = children[10] ? _context->popExpr() : nullptr;
        auto eIsoDayOfWeek = children[9] ? _context->popExpr() : nullptr;
        auto eIsoWeek = children[8] ? _context->popExpr() : nullptr;
        auto eIsoWeekYear = children[7] ? _context->popExpr() : nullptr;
        auto eMillisecond = children[6] ? _context->popExpr() : nullptr;
        auto eSecond = children[5] ? _context->popExpr() : nullptr;
        auto eMinute = children[4] ? _context->popExpr() : nullptr;
        auto eHour = children[3] ? _context->popExpr() : nullptr;
        auto eDay = children[2] ? _context->popExpr() : nullptr;
        auto eMonth = children[1] ? _context->popExpr() : nullptr;
        auto eYear = children[0] ? _context->popExpr() : nullptr;

        // Save a flag to determine if we are in the case of an iso
        // week year. Note that the agg expression parser ensures that one of date or
        // isoWeekYear inputs are provided so we don't need to enforce that at this depth.
        auto isIsoWeekYear = eIsoWeekYear ? true : false;

        auto frameId = _context->state.frameId();
        sbe::EVariable yearRef(frameId, 0);
        sbe::EVariable monthRef(frameId, 1);
        sbe::EVariable dayRef(frameId, 2);
        sbe::EVariable hourRef(frameId, 3);
        sbe::EVariable minRef(frameId, 4);
        sbe::EVariable secRef(frameId, 5);
        sbe::EVariable millisecRef(frameId, 6);
        sbe::EVariable timeZoneRef(frameId, 7);

        // Build a chain of nested bounds checks for each date part that is provided in the
        // expression. We elide the checks in the case that default values are used. These bound
        // checks are then used by folding over pairs of ite tests and else branches to implement
        // short-circuiting in the case that checks fail. To emulate the control flow of MQL for
        // this expression we interleave type conversion checks with time component bound checks.
        const auto minInt16 = std::numeric_limits<int16_t>::lowest();
        const auto maxInt16 = std::numeric_limits<int16_t>::max();

        // Constructs an expression that does a bound check of var over a closed interval [lower,
        // upper].
        auto boundedCheck =
            [](sbe::EExpression& var, int16_t lower, int16_t upper, const std::string& varName) {
                str::stream errMsg;
                if (varName == "year" || varName == "isoWeekYear") {
                    errMsg << "'" << varName << "'"
                           << " must evaluate to an integer in the range " << lower << " to "
                           << upper;
                } else {
                    errMsg << "'" << varName << "'"
                           << " must evaluate to a value in the range [" << lower << ", " << upper
                           << "]";
                }
                return std::make_pair(
                    makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                 makeBinaryOp(sbe::EPrimBinary::greaterEq,
                                              var.clone(),
                                              sbe::makeE<sbe::EConstant>(
                                                  sbe::value::TypeTags::NumberInt32,
                                                  sbe::value::bitcastFrom<int32_t>(lower))),
                                 makeBinaryOp(sbe::EPrimBinary::lessEq,
                                              var.clone(),
                                              sbe::makeE<sbe::EConstant>(
                                                  sbe::value::TypeTags::NumberInt32,
                                                  sbe::value::bitcastFrom<int32_t>(upper)))),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848972}, errMsg));
            };

        // Here we want to validate each field that is provided as input to the agg expression. To
        // do this we implement the following checks:
        //
        // 1) Check if the value in a given slot null or missing. If so bind null to l1.0, and
        // continue to the next binding. Otherwise, do check 2 below.
        //
        // 2) Check if the value in a given slot is an integral int64. This test is done by
        // computing a lossless conversion of the value in s1 to an int64. The exposed
        // conversion function by the vm returns a value if there is no loss of precision,
        // otherwise it returns Nothing. In both the valid or Nothing case, we can store the result
        // of the conversion in l2.0 of the inner let binding and test for existence. If the
        // existence check fails we know the conversion is lossy and we can fail the query.
        // Otherwise, the inner let evaluates to the converted value which is then bound to the
        // outer let.
        //
        // Each invocation of fieldConversionBinding will produce a nested let of the form.
        //
        // let [l1.0 = s1] in
        //   if (isNull(l1.0) || !exists(l1.0), null,
        //     let [l2.0 = convert(l1.0, int)] in
        //       if (exists(l2.0), l2.0, fail("... must evaluate to an integer")]), ...]
        //  in ...
        auto fieldConversionBinding = [](std::unique_ptr<sbe::EExpression> expr,
                                         sbe::value::FrameIdGenerator* frameIdGenerator,
                                         const std::string& varName) {
            auto outerFrameId = frameIdGenerator->generate();
            auto innerFrameId = frameIdGenerator->generate();
            sbe::EVariable outerSlotRef(outerFrameId, 0);
            sbe::EVariable convertedFieldRef(innerFrameId, 0);
            return sbe::makeE<sbe::ELocalBind>(
                outerFrameId,
                sbe::makeEs(expr->clone()),
                sbe::makeE<sbe::EIf>(
                    makeBinaryOp(sbe::EPrimBinary::logicOr,
                                 makeNot(makeFunction("exists", outerSlotRef.clone())),
                                 makeFunction("isNull", outerSlotRef.clone())),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                    sbe::makeE<sbe::ELocalBind>(
                        innerFrameId,
                        sbe::makeEs(sbe::makeE<sbe::ENumericConvert>(
                            outerSlotRef.clone(), sbe::value::TypeTags::NumberInt64)),
                        sbe::makeE<sbe::EIf>(
                            makeFunction("exists", convertedFieldRef.clone()),
                            convertedFieldRef.clone(),
                            sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848979},
                                                   str::stream()
                                                       << "'" << varName << "'"
                                                       << " must evaluate to an integer")))));
        };

        // Build two vectors on the fly to elide bound and conversion for defaulted values.
        std::vector<std::pair<std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::EExpression>>>
            boundChecks;  // checks for lower and upper bounds of date fields.

        // Operands is for the outer let bindings.
        std::vector<std::unique_ptr<sbe::EExpression>> operands;
        if (isIsoWeekYear) {
            if (!eIsoWeekYear) {
                eIsoWeekYear = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                          sbe::value::bitcastFrom<int32_t>(1970));
                operands.push_back(std::move(eIsoWeekYear));
            } else {
                boundChecks.push_back(boundedCheck(yearRef, 1, 9999, "isoWeekYear"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoWeekYear), _context->state.frameIdGenerator, "isoWeekYear"));
            }
            if (!eIsoWeek) {
                eIsoWeek = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                      sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eIsoWeek));
            } else {
                boundChecks.push_back(boundedCheck(monthRef, minInt16, maxInt16, "isoWeek"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoWeek), _context->state.frameIdGenerator, "isoWeek"));
            }
            if (!eIsoDayOfWeek) {
                eIsoDayOfWeek = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                           sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eIsoDayOfWeek));
            } else {
                boundChecks.push_back(boundedCheck(dayRef, minInt16, maxInt16, "isoDayOfWeek"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoDayOfWeek), _context->state.frameIdGenerator, "isoDayOfWeek"));
            }
        } else {
            // The regular year/month/day case.
            if (!eYear) {
                eYear = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                   sbe::value::bitcastFrom<int32_t>(1970));
                operands.push_back(std::move(eYear));
            } else {
                boundChecks.push_back(boundedCheck(yearRef, 1, 9999, "year"));
                operands.push_back(fieldConversionBinding(
                    std::move(eYear), _context->state.frameIdGenerator, "year"));
            }
            if (!eMonth) {
                eMonth = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                    sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eMonth));
            } else {
                boundChecks.push_back(boundedCheck(monthRef, minInt16, maxInt16, "month"));
                operands.push_back(fieldConversionBinding(
                    std::move(eMonth), _context->state.frameIdGenerator, "month"));
            }
            if (!eDay) {
                eDay = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                  sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eDay));
            } else {
                boundChecks.push_back(boundedCheck(dayRef, minInt16, maxInt16, "day"));
                operands.push_back(fieldConversionBinding(
                    std::move(eDay), _context->state.frameIdGenerator, "day"));
            }
        }
        if (!eHour) {
            eHour = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                               sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eHour));
        } else {
            boundChecks.push_back(boundedCheck(hourRef, minInt16, maxInt16, "hour"));
            operands.push_back(
                fieldConversionBinding(std::move(eHour), _context->state.frameIdGenerator, "hour"));
        }
        if (!eMinute) {
            eMinute = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                 sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eMinute));
        } else {
            boundChecks.push_back(boundedCheck(minRef, minInt16, maxInt16, "minute"));
            operands.push_back(fieldConversionBinding(
                std::move(eMinute), _context->state.frameIdGenerator, "minute"));
        }
        if (!eSecond) {
            eSecond = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                 sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eSecond));
        } else {
            // MQL doesn't place bound restrictions on the second field, because seconds carry over
            // to minutes and can be large ints such as 71,841,012 or even unix epochs.
            operands.push_back(fieldConversionBinding(
                std::move(eSecond), _context->state.frameIdGenerator, "second"));
        }
        if (!eMillisecond) {
            eMillisecond = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                      sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eMillisecond));
        } else {
            // MQL doesn't enforce bound restrictions on millisecond fields because milliseconds
            // carry over to seconds.
            operands.push_back(fieldConversionBinding(
                std::move(eMillisecond), _context->state.frameIdGenerator, "millisecond"));
        }
        if (!eTimezone) {
            eTimezone = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::StringSmall, 0);
            operands.push_back(std::move(eTimezone));
        } else {
            // Validate that eTimezone is a string.
            auto tzFrameId = _context->state.frameId();
            sbe::EVariable timezoneRef(tzFrameId, 0);
            operands.push_back(sbe::makeE<sbe::ELocalBind>(
                tzFrameId,
                sbe::makeEs(std::move(eTimezone)),
                sbe::makeE<sbe::EIf>(
                    makeFunction("isString", timeZoneRef.clone()),
                    timezoneRef.clone(),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848980},
                                           str::stream()
                                               << "'timezone' must evaluate to a string"))));
        }

        // Make a disjunction of null checks for each date part by over this vector. These checks
        // are necessary after the initial conversion computation because we need have the outer let
        // binding evaluate to null if any field is null.
        auto nullExprs =
            makeVector<std::unique_ptr<sbe::EExpression>>(generateNullOrMissing(frameId, 7),
                                                          generateNullOrMissing(frameId, 6),
                                                          generateNullOrMissing(frameId, 5),
                                                          generateNullOrMissing(frameId, 4),
                                                          generateNullOrMissing(frameId, 3),
                                                          generateNullOrMissing(frameId, 2),
                                                          generateNullOrMissing(frameId, 1),
                                                          generateNullOrMissing(frameId, 0));

        using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
        auto checkPartsForNull = std::accumulate(
            std::move_iterator<iter_t>(nullExprs.begin() + 1),
            std::move_iterator<iter_t>(nullExprs.end()),
            std::move(nullExprs.front()),
            [](auto&& acc, auto&& b) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(b));
            });

        // Invocation of the datePartsWeekYear and dateParts functions depend on a TimeZoneDatabase
        // for datetime computation. This global object is registered as an unowned value in the
        // runtime environment so we pass the corresponding slot to the datePartsWeekYear and
        // dateParts functions as a variable.
        auto timeZoneDBSlot = _context->state.env->getSlot("timeZoneDB"_sd);
        auto computeDate = makeFunction(isIsoWeekYear ? "datePartsWeekYear" : "dateParts",
                                        sbe::makeE<sbe::EVariable>(timeZoneDBSlot),
                                        yearRef.clone(),
                                        monthRef.clone(),
                                        dayRef.clone(),
                                        hourRef.clone(),
                                        minRef.clone(),
                                        secRef.clone(),
                                        millisecRef.clone(),
                                        timeZoneRef.clone());

        using iterPair_t = std::vector<std::pair<std::unique_ptr<sbe::EExpression>,
                                                 std::unique_ptr<sbe::EExpression>>>::iterator;
        auto computeBoundChecks =
            std::accumulate(std::move_iterator<iterPair_t>(boundChecks.begin()),
                            std::move_iterator<iterPair_t>(boundChecks.end()),
                            std::move(computeDate),
                            [](auto&& acc, auto&& b) {
                                return sbe::makeE<sbe::EIf>(
                                    std::move(b.first), std::move(acc), std::move(b.second));
                            });

        // This final ite expression allows short-circuting of the null field case. If the nullish,
        // checks pass, then we check the bounds of each field and invoke the builtins if all checks
        // pass.
        auto computeDateOrNull =
            sbe::makeE<sbe::EIf>(std::move(checkPartsForNull),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                                 std::move(computeBoundChecks));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(computeDateOrNull)));
    }

    void visit(ExpressionDateToParts* expr) final {
        auto frameId = _context->state.frameId();
        auto children = expr->getChildren();
        std::unique_ptr<sbe::EExpression> date, timezone, isoflag;
        std::unique_ptr<sbe::EExpression> totalExprDateToParts;
        std::vector<std::unique_ptr<sbe::EExpression>> args;
        std::vector<std::unique_ptr<sbe::EExpression>> isoargs;
        std::vector<std::unique_ptr<sbe::EExpression>> operands;
        sbe::EVariable dateRef(frameId, 0);
        sbe::EVariable timezoneRef(frameId, 1);
        sbe::EVariable isoflagRef(frameId, 2);

        // Initialize arguments with values from stack or default values.
        if (children[2]) {
            isoflag = _context->popExpr();
        } else {
            isoflag = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, false);
        }
        if (children[1]) {
            timezone = _context->popExpr();
        } else {
            auto [utcTag, utcVal] = sbe::value::makeNewString("UTC");
            timezone = sbe::makeE<sbe::EConstant>(utcTag, utcVal);
        }
        if (children[0]) {
            date = _context->popExpr();
        } else {
            _context->pushExpr(sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997700},
                                                      "$dateToParts must include a date"));
            return;
        }

        // Add timezoneDB to arguments.
        args.push_back(sbe::makeE<sbe::EVariable>(_context->state.env->getSlot("timeZoneDB"_sd)));
        isoargs.push_back(
            sbe::makeE<sbe::EVariable>(_context->state.env->getSlot("timeZoneDB"_sd)));

        // Add date to arguments.
        operands.push_back(std::move(date));
        args.push_back(dateRef.clone());
        isoargs.push_back(dateRef.clone());

        // Add timezone to arguments.
        operands.push_back(std::move(timezone));
        args.push_back(timezoneRef.clone());
        isoargs.push_back(timezoneRef.clone());

        // Add iso8601 to arguments.
        uint32_t isoTypeMask = getBSONTypeMask(sbe::value::TypeTags::Boolean);
        operands.push_back(std::move(isoflag));
        args.push_back(isoflagRef.clone());
        isoargs.push_back(isoflagRef.clone());

        // Determine whether to call dateToParts or isoDateToParts.
        auto checkIsoflagValue = buildMultiBranchConditional(
            CaseValuePair{
                makeBinaryOp(sbe::EPrimBinary::eq,
                             isoflagRef.clone(),
                             sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, false)),
                sbe::makeE<sbe::EFunction>("dateToParts", std::move(args))},
            sbe::makeE<sbe::EFunction>("isoDateToParts", std::move(isoargs)));

        // Check that each argument exists, is not null, and is the correct type.
        auto totalDateToPartsFunc = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(frameId, 1),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{makeNot(makeFunction("isString", timezoneRef.clone())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997701},
                                                 "$dateToParts timezone must be a string")},
            CaseValuePair{makeNot(makeFunction("isTimezone",
                                               sbe::makeE<sbe::EVariable>(
                                                   _context->state.env->getSlot("timeZoneDB"_sd)),
                                               timezoneRef.clone())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997704},
                                                 "$dateToParts timezone must be a valid timezone")},
            CaseValuePair{generateNullOrMissing(frameId, 2),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{makeNot(sbe::makeE<sbe::ETypeMatch>(isoflagRef.clone(), isoTypeMask)),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997702},
                                                 "$dateToParts iso8601 must be a boolean")},
            CaseValuePair{generateNullOrMissing(frameId, 0),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{
                makeNot(sbe::makeE<sbe::ETypeMatch>(dateRef.clone(), dateTypeMask())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997703},
                                       "$dateToParts date must have the format of a date")},
            std::move(checkIsoflagValue));
        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(totalDateToPartsFunc)));
    }
    void visit(ExpressionDateToString* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(ExpressionDateTrunc*) final {
        unsupportedExpression("$dateTrunc");
    }
    void visit(ExpressionDivide* expr) final {
        _context->ensureArity(2);

        auto rhs = _context->popExpr();
        auto lhs = _context->popExpr();

        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(std::move(lhs), std::move(rhs));
        sbe::EVariable lhsRef{frameId, 0};
        sbe::EVariable rhsRef{frameId, 1};

        auto checkIsNumber = makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                          makeFunction("isNumber", lhsRef.clone()),
                                          makeFunction("isNumber", rhsRef.clone()));

        auto checkIsNullOrMissing = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                 generateNullOrMissing(lhsRef),
                                                 generateNullOrMissing(rhsRef));

        auto divideExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkIsNullOrMissing),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(checkIsNumber),
                          makeBinaryOp(sbe::EPrimBinary::div, lhsRef.clone(), rhsRef.clone())},
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073101},
                                   "$divide only supports numeric types"));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(divideExpr)));
    }
    void visit(ExpressionExp* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto expExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903703},
                                                 "$exp only supports numeric types")},
            makeFunction("exp", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(expExpr)));
    }
    void visit(ExpressionFieldPath* expr) final {
        sbe::value::SlotId slotId;

        if (!Variables::isUserDefinedVariable(expr->getVariableId())) {
            if (expr->getVariableId() == Variables::kRootId) {
                slotId = _context->rootSlot;
            } else if (expr->getVariableId() == Variables::kRemoveId) {
                // For the field paths that begin with "$$REMOVE", we always produce Nothing,
                // so no traversal is necessary.
                _context->pushExpr(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0));
                return;
            } else {
                auto it = Variables::kIdToBuiltinVarName.find(expr->getVariableId());
                tassert(5611300,
                        "Encountered unexpected system variable ID",
                        it != Variables::kIdToBuiltinVarName.end());

                auto variableSlot = _context->state.env->getSlotIfExists(it->second);
                uassert(5611301,
                        str::stream()
                            << "Builtin variable '$$" << it->second << "' is not available",
                        variableSlot.has_value());

                slotId = *variableSlot;
            }
        } else {
            auto it = _context->environment.find(expr->getVariableId());
            if (it != _context->environment.end()) {
                slotId = it->second;
            } else {
                slotId = _context->state.getGlobalVariableSlot(expr->getVariableId());
            }
        }

        if (expr->getFieldPath().getPathLength() == 1) {
            // A solo variable reference (e.g.: "$$ROOT" or "$$myvar") that doesn't need any
            // traversal.
            _context->pushExpr(sbe::makeE<sbe::EVariable>(slotId));
            return;
        }

        // Dereference a dotted path, which may contain arrays requiring implicit traversal.
        const bool expectsDocumentInputOnly = slotId == _context->rootSlot;
        auto [outputSlot, stage] = generateTraverse(_context->extractCurrentEvalStage(),
                                                    slotId,
                                                    expectsDocumentInputOnly,
                                                    expr->getFieldPathWithoutCurrentPrefix(),
                                                    _context->planNodeId,
                                                    _context->state.slotIdGenerator);

        _context->pushExpr(sbe::makeE<sbe::EVariable>(outputSlot), std::move(stage));
    }
    void visit(ExpressionFilter* expr) final {
        // Extract filter predicate expression and sub-tree.
        auto [filterPredicate, filterStage] = _context->popFrame();

        auto input = _context->popExpr();

        // Filter predicate of $filter expression expects current array element to be stored in the
        // specific variable. We already allocated slot for it in the "in" visitor, now we just need
        // to retrieve it from the environment.
        // This slot will be used in the traverse stage twice - to store the input array and to
        // store current element in this array.
        auto currentElementVariable = expr->getVariableId();
        invariant(_context->environment.count(currentElementVariable));
        auto inputArraySlot = _context->environment.at(currentElementVariable);

        // We no longer need this mapping because filter predicate which expects it was already
        // compiled.
        _context->environment.erase(currentElementVariable);

        // Construct 'from' branch of traverse stage. SBE tree stored in 'fromBranch' variable looks
        // like this:
        //
        // project inputIsNotNullishSlot = !(isNull(inputArraySlot) || !exists(inputArraySlot))
        // project inputArraySlot = (
        //   let inputRef = input
        //   in
        //       if isArray(inputRef) || isNull(inputRef) || !exists(inputRef)
        //         inputRef
        //       else
        //         fail()
        // )
        // <current sub-tree stage>
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(std::move(input));
        sbe::EVariable inputRef(frameId, 0);

        auto inputIsArrayOrNullish = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                  generateNullOrMissing(inputRef),
                                                  makeFunction("isArray", inputRef.clone()));
        auto checkInputArrayType =
            sbe::makeE<sbe::EIf>(std::move(inputIsArrayOrNullish),
                                 inputRef.clone(),
                                 sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073201},
                                                        "input to $filter must be an array"));
        auto inputArray =
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(checkInputArrayType));

        sbe::EVariable inputArrayVariable{inputArraySlot};
        auto projectInputArray = makeProject(_context->extractCurrentEvalStage(),
                                             _context->planNodeId,
                                             inputArraySlot,
                                             std::move(inputArray));

        auto inputIsNotNullish = makeNot(generateNullOrMissing(inputArrayVariable));
        auto inputIsNotNullishSlot = _context->state.slotId();
        auto fromBranch = makeProject(std::move(projectInputArray),
                                      _context->planNodeId,
                                      inputIsNotNullishSlot,
                                      std::move(inputIsNotNullish));

        // Construct 'in' branch of traverse stage. SBE tree stored in 'inBranch' variable looks
        // like this:
        //
        // cfilter Variable{inputIsNotNullishSlot}
        // filter filterPredicate
        // filterStage
        //
        // Filter predicate can return non-boolean values. To fix this, we generate expression to
        // coerce it to bool type.
        frameId = _context->state.frameId();
        auto boolFilterPredicate =
            sbe::makeE<sbe::ELocalBind>(frameId,
                                        sbe::makeEs(std::move(filterPredicate)),
                                        generateCoerceToBoolExpression(sbe::EVariable{frameId, 0}));
        auto filterWithPredicate = makeFilter<false>(
            std::move(filterStage), std::move(boolFilterPredicate), _context->planNodeId);

        // If input array is null or missing, we do not evaluate filter predicate and return EOF.
        auto innerBranch = makeFilter<true>(std::move(filterWithPredicate),
                                            sbe::makeE<sbe::EVariable>(inputIsNotNullishSlot),
                                            _context->planNodeId);

        // Construct traverse stage with the following slots:
        // * inputArraySlot - slot containing input array of $filter expression
        // * filteredArraySlot - slot containing the array with items on which filter predicate has
        //   evaluated to true
        // * inputArraySlot - slot where 'in' branch of traverse stage stores current array
        //   element if it satisfies the filter predicate
        auto filteredArraySlot = _context->state.slotId();
        auto traverseStage = makeTraverse(std::move(fromBranch),
                                          std::move(innerBranch),
                                          inputArraySlot /* inField */,
                                          filteredArraySlot /* outField */,
                                          inputArraySlot /* outFieldInner */,
                                          nullptr /* foldExpr */,
                                          nullptr /* finalExpr */,
                                          _context->planNodeId,
                                          1 /* nestedArraysDepth */,
                                          _context->getLexicalEnvironment());

        // If input array is null or missing, 'in' stage of traverse will return EOF. In this case
        // traverse sets output slot (filteredArraySlot) to Nothing. We replace it with Null to
        // match $filter expression behaviour.
        auto result = makeFunction("fillEmpty",
                                   sbe::makeE<sbe::EVariable>(filteredArraySlot),
                                   sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0));

        _context->pushExpr(std::move(result), std::move(traverseStage));
    }
    void visit(ExpressionFloor* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto floorExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903704},
                                                 "$floor only supports numeric types")},
            makeFunction("floor", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(floorExpr)));
    }
    void visit(ExpressionIfNull* expr) final {
        auto numChildren = expr->getChildren().size();
        invariant(numChildren >= 2);

        std::vector<EvalExprStagePair> branches;
        branches.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            auto [expr, stage] = _context->popFrame();
            branches.emplace_back(std::move(expr), std::move(stage));
        }
        std::reverse(branches.begin(), branches.end());

        // Prepare to create limit-1/union with N branches (where N is the number of operands). Each
        // branch will be evaluated from left to right until one of the branches produces a value.
        auto branchFn = [](EvalExpr evalExpr,
                           EvalStage stage,
                           PlanNodeId planNodeId,
                           sbe::value::SlotIdGenerator* slotIdGenerator) {
            auto slot = slotIdGenerator->generate();
            stage = makeProject(std::move(stage), planNodeId, slot, evalExpr.extractExpr());

            // Create a FilterStage for each branch (except the last one). If a branch's filter
            // condition is true, it will "short-circuit" the evaluation process. For ifNull,
            // short-circuiting should happen if the current variable is not null or missing.
            auto filterExpr = makeNot(generateNullOrMissing(slot));
            auto filterStage =
                makeFilter<false>(std::move(stage), std::move(filterExpr), planNodeId);

            // Set the current expression as the output to be returned if short-circuiting occurs.
            return std::make_pair(slot, std::move(filterStage));
        };

        auto [resultExpr, opStage] = generateSingleResultUnion(
            std::move(branches), branchFn, _context->planNodeId, _context->state.slotIdGenerator);

        auto loopJoinStage = makeLoopJoin(_context->extractCurrentEvalStage(),
                                          std::move(opStage),
                                          _context->planNodeId,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(resultExpr.extractExpr(), std::move(loopJoinStage));
    }
    void visit(ExpressionIn* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIndexOfArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(ExpressionIndexOfBytes* expr) final {
        visitIndexOfFunction(expr, _context, "indexOfBytes");
    }

    void visit(ExpressionIndexOfCP* expr) final {
        visitIndexOfFunction(expr, _context, "indexOfCP");
    }
    void visit(ExpressionIsNumber* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto exprIsNum =
            sbe::makeE<sbe::EIf>(makeFunction("exists", inputRef.clone()),
                                 makeFunction("isNumber", inputRef.clone()),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                            sbe::value::bitcastFrom<bool>(false)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprIsNum)));
    }
    void visit(ExpressionLet* expr) final {
        // The evaluated result of the $let is the evaluated result of its "in" field, which is
        // already on top of the stack. The "infix" visitor has already popped the variable
        // initializers off the expression stack.
        _context->ensureArity(1);

        // We should have bound all the variables from this $let expression.
        invariant(!_context->varsFrameStack.empty());
        auto& currentFrame = _context->varsFrameStack.top();
        invariant(currentFrame.variablesToBind.empty());

        // Pop the lexical frame for this $let and remove all its bindings, which are now out of
        // scope.
        auto it = _context->environment.begin();
        while (it != _context->environment.end()) {
            if (currentFrame.slotsForLetVariables.count(it->second)) {
                it = _context->environment.erase(it);
            } else {
                ++it;
            }
        }
        _context->varsFrameStack.pop();

        // Note that there is no need to remove SlotId bindings from the the _context's environment.
        // The AST parser already enforces scope rules.
    }
    void visit(ExpressionLn* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto lnExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903705},
                                                 "$ln only supports numeric types")},
            // Note: In MQL, $ln on a NumberDecimal NaN historically evaluates to a NumberDouble
            // NaN.
            CaseValuePair{generateNaNCheck(inputRef),
                          sbe::makeE<sbe::ENumericConvert>(inputRef.clone(),
                                                           sbe::value::TypeTags::NumberDouble)},
            CaseValuePair{generateNonPositiveCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903706},
                                                 "$ln's argument must be a positive number")},
            makeFunction("ln", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(lnExpr)));
    }
    void visit(ExpressionLog* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionLog10* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto log10Expr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903707},
                                                 "$log10 only supports numeric types")},
            // Note: In MQL, $log10 on a NumberDecimal NaN historically evaluates to a NumberDouble
            // NaN.
            CaseValuePair{generateNaNCheck(inputRef),
                          sbe::makeE<sbe::ENumericConvert>(inputRef.clone(),
                                                           sbe::value::TypeTags::NumberDouble)},
            CaseValuePair{generateNonPositiveCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903708},
                                                 "$log10's argument must be a positive number")},
            makeFunction("log10", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(log10Expr)));
    }
    void visit(ExpressionMap* expr) final {
        unsupportedExpression("$map");
    }
    void visit(ExpressionMeta* expr) final {
        unsupportedExpression("$meta");
    }
    void visit(ExpressionMod* expr) final {
        auto frameId = _context->state.frameId();
        auto rhs = _context->popExpr();
        auto lhs = _context->popExpr();
        auto binds = sbe::makeEs(std::move(lhs), std::move(rhs));
        sbe::EVariable lhsVar{frameId, 0};
        sbe::EVariable rhsVar{frameId, 1};

        // If the rhs is a small integral double, convert it to int32 to match $mod MQL semantics.
        auto numericConvert32 =
            sbe::makeE<sbe::ENumericConvert>(rhsVar.clone(), sbe::value::TypeTags::NumberInt32);
        auto rhsExpr = buildMultiBranchConditional(
            CaseValuePair{
                makeBinaryOp(
                    sbe::EPrimBinary::logicAnd,
                    sbe::makeE<sbe::ETypeMatch>(
                        rhsVar.clone(), getBSONTypeMask(sbe::value::TypeTags::NumberDouble)),
                    makeNot(sbe::makeE<sbe::ETypeMatch>(
                        lhsVar.clone(), getBSONTypeMask(sbe::value::TypeTags::NumberDouble)))),
                makeFunction("fillEmpty", std::move(numericConvert32), rhsVar.clone())},
            rhsVar.clone());

        auto modExpr = buildMultiBranchConditional(
            CaseValuePair{makeBinaryOp(sbe::EPrimBinary::logicOr,
                                       generateNullOrMissing(lhsVar),
                                       generateNullOrMissing(rhsVar)),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{makeBinaryOp(sbe::EPrimBinary::logicOr,
                                       generateNonNumericCheck(lhsVar),
                                       generateNonNumericCheck(rhsVar)),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154000},
                                                 "$mod only supports numeric types")},
            makeFunction("mod", lhsVar.clone(), std::move(rhsExpr)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(modExpr)));
    }
    void visit(ExpressionMultiply* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->state.frameId();

        std::vector<std::unique_ptr<sbe::EExpression>> binds;
        std::vector<std::unique_ptr<sbe::EExpression>> variables;
        std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNull;
        std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNumber;
        binds.reserve(arity);
        variables.reserve(arity);
        checkExprsNull.reserve(arity);
        checkExprsNumber.reserve(arity);
        sbe::value::SlotId slot{0};
        for (size_t idx = 0; idx < arity; ++idx, ++slot) {
            binds.push_back(_context->popExpr());
            sbe::EVariable currentVariable{frameId, slot};
            variables.push_back(currentVariable.clone());

            checkExprsNull.push_back(generateNullOrMissing(currentVariable));
            checkExprsNumber.push_back(makeFunction("isNumber", currentVariable.clone()));
        }

        // At this point 'binds' vector contains arguments of $multiply expression in the reversed
        // order. We need to reverse it back to perform multiplication in the right order below.
        // Multiplication in different order can lead to different result because of accumulated
        // precision errors from floating point types.
        std::reverse(std::begin(binds), std::end(binds));

        using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
        auto checkNullAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkExprsNull.begin() + 1),
            std::move_iterator<iter_t>(checkExprsNull.end()),
            std::move(checkExprsNull.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });

        auto checkNumberAllArguments = std::accumulate(
            std::move_iterator<iter_t>(checkExprsNumber.begin() + 1),
            std::move_iterator<iter_t>(checkExprsNumber.end()),
            std::move(checkExprsNumber.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicAnd, std::move(acc), std::move(ex));
            });

        auto multiplication = std::accumulate(
            std::move_iterator<iter_t>(variables.begin() + 1),
            std::move_iterator<iter_t>(variables.end()),
            std::move(variables.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::mul, std::move(acc), std::move(ex));
            });

        auto multiplyExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkNullAnyArgument),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(checkNumberAllArguments), std::move(multiplication)},
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073102},
                                   "only numbers are allowed in an $multiply expression"));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(multiplyExpr)));
    }
    void visit(ExpressionNot* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());

        auto notExpr = makeNot(generateCoerceToBoolExpression({frameId, 0}));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(notExpr)));
    }
    void visit(ExpressionObject* expr) final {
        unsupportedExpression("$object");
    }
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {
        unsupportedExpression("$pow");
    }
    void visit(ExpressionRange* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionReduce* expr) final {
        unsupportedExpression("$reduce");
    }
    void visit(ExpressionReplaceOne* expr) final {
        auto frameId = _context->state.frameId();

        auto replacement = _context->popExpr();
        auto find = _context->popExpr();
        auto input = _context->popExpr();

        sbe::EVariable inputRef(frameId, 0);
        sbe::EVariable findRef(frameId, 1);
        sbe::EVariable replacementRef(frameId, 2);
        sbe::EVariable inputNullOrMissingRef(frameId, 3);
        sbe::EVariable findNullOrMissingRef(frameId, 4);
        sbe::EVariable replacementNullOrMissingRef(frameId, 5);

        auto binds = sbe::makeEs(std::move(input),
                                 std::move(find),
                                 std::move(replacement),
                                 generateNullOrMissing(inputRef),
                                 generateNullOrMissing(findRef),
                                 generateNullOrMissing(replacementRef));

        auto generateValidateParameter = [](const sbe::EVariable& paramRef,
                                            const sbe::EVariable& paramMissingRef,
                                            const std::string& paramName) {
            return makeBinaryOp(sbe::EPrimBinary::logicOr,
                                makeBinaryOp(sbe::EPrimBinary::logicOr,
                                             paramMissingRef.clone(),
                                             makeFunction("isString", paramRef.clone())),
                                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154400},
                                                       str::stream()
                                                           << "$replaceOne requires that '"
                                                           << paramName << "' be a string"));
        };

        auto inputIsStringOrFail =
            generateValidateParameter(inputRef, inputNullOrMissingRef, "input");
        auto findIsStringOrFail = generateValidateParameter(findRef, findNullOrMissingRef, "find");
        auto replacementIsStringOrFail =
            generateValidateParameter(replacementRef, replacementNullOrMissingRef, "replacement");

        auto checkNullExpr = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                          makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                       inputNullOrMissingRef.clone(),
                                                       findNullOrMissingRef.clone()),
                                          replacementNullOrMissingRef.clone());

        // Order here is important because we want to preserve the precedence of failures in MQL.
        auto isNullExpr = makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                       makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                                    makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                                                 std::move(inputIsStringOrFail),
                                                                 std::move(findIsStringOrFail)),
                                                    std::move(replacementIsStringOrFail)),
                                       std::move(checkNullExpr));

        // Check if find string is empty, and if so return the the concatenation of the replacement
        // string and the input string, otherwise replace the first occurrence of the find string.
        auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");
        auto isEmptyFindStr = makeBinaryOp(sbe::EPrimBinary::eq,
                                           findRef.clone(),
                                           sbe::makeE<sbe::EConstant>(emptyStrTag, emptyStrVal),
                                           _context->state.env);

        auto replaceOrReturnInputExpr = sbe::makeE<sbe::EIf>(
            std::move(isEmptyFindStr),
            makeFunction("concat", replacementRef.clone(), inputRef.clone()),
            makeFunction("replaceOne", inputRef.clone(), findRef.clone(), replacementRef.clone()));

        auto replaceOneExpr =
            sbe::makeE<sbe::EIf>(std::move(isNullExpr),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                                 std::move(replaceOrReturnInputExpr));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(replaceOneExpr)));
    }
    void visit(ExpressionReplaceAll* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetDifference* expr) final {
        invariant(expr->getChildren().size() == 2);

        generateSetExpression(expr, SetOperation::Difference);
    }
    void visit(ExpressionSetEquals* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetIntersection* expr) final {
        generateSetExpression(expr, SetOperation::Intersection);
    }

    void visit(ExpressionSetIsSubset* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetUnion* expr) final {
        generateSetExpression(expr, SetOperation::Union);
    }

    void visit(ExpressionSize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionReverseArray* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef{frameId, 0};

        auto argumentIsNotArray = makeNot(makeFunction("isArray", inputRef.clone()));
        auto exprRevArr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          makeConstant(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(argumentIsNotArray),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5154901},
                                                 "$reverseArray argument must be an array")},
            makeFunction("reverseArray", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprRevArr)));
    }
    void visit(ExpressionSlice* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIsArray* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto exprIsArr = makeFillEmptyFalse(makeFunction("isArray", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprIsArr)));
    }
    void visit(ExpressionRound* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSplit* expr) final {
        auto frameId = _context->state.frameId();
        std::vector<std::unique_ptr<sbe::EExpression>> args;
        std::vector<std::unique_ptr<sbe::EExpression>> binds;
        sbe::EVariable stringExpressionRef(frameId, 0);
        sbe::EVariable delimiterRef(frameId, 1);

        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        auto delimiter = _context->popExpr();
        auto stringExpression = _context->popExpr();

        // Add stringExpression to arguments.
        binds.push_back(std::move(stringExpression));
        args.push_back(stringExpressionRef.clone());

        // Add delimiter to arguments.
        binds.push_back(std::move(delimiter));
        args.push_back(delimiterRef.clone());

        auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");
        auto [arrayWithEmptyStringTag, arrayWithEmptyStringVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard arrayWithEmptyStringGuard{arrayWithEmptyStringTag,
                                                         arrayWithEmptyStringVal};
        auto arrayWithEmptyStringView = sbe::value::getArrayView(arrayWithEmptyStringVal);
        arrayWithEmptyStringView->push_back(emptyStrTag, emptyStrVal);
        arrayWithEmptyStringGuard.reset();

        auto generateIsEmptyString = [this, emptyStrTag = emptyStrTag, emptyStrVal = emptyStrVal](
                                         const sbe::EVariable& var) {
            return makeBinaryOp(sbe::EPrimBinary::eq,
                                var.clone(),
                                sbe::makeE<sbe::EConstant>(emptyStrTag, emptyStrVal),
                                _context->state.env);
        };

        auto checkIsNullOrMissing = makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                 generateNullOrMissing(stringExpressionRef),
                                                 generateNullOrMissing(delimiterRef));

        // In order to maintain MQL semantics, first check both the string expression
        // (first agument), and delimiter string (second argument) for null, undefined, or
        // missing, and if either is nullish make the entire expression return null. Only
        // then make further validity checks against the input. Fail if the delimiter is an empty
        // string. Return [""] if the string expression is an empty string.
        auto totalSplitFunc = buildMultiBranchConditional(
            CaseValuePair{std::move(checkIsNullOrMissing),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(stringExpressionRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5155402},
                              str::stream() << "$split string expression must be a string")},
            CaseValuePair{
                generateNonStringCheck(delimiterRef),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5155400},
                                       str::stream() << "$split delimiter must be a string")},
            CaseValuePair{generateIsEmptyString(delimiterRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5155401},
                              str::stream() << "$split delimiter must not be an empty string")},
            sbe::makeE<sbe::EIf>(
                generateIsEmptyString(stringExpressionRef),
                sbe::makeE<sbe::EConstant>(arrayWithEmptyStringTag, arrayWithEmptyStringVal),
                sbe::makeE<sbe::EFunction>("split", std::move(args))));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(totalSplitFunc)));
    }
    void visit(ExpressionSqrt* expr) final {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto lnExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903709},
                                                 "$sqrt only supports numeric types")},
            CaseValuePair{
                generateNegativeCheck(inputRef),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903710},
                                       "$sqrt's argument must be greater than or equal to 0")},
            makeFunction("sqrt", inputRef.clone()));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(lnExpr)));
    }
    void visit(ExpressionStrcasecmp* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubstrBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubstrCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionStrLenBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionBinarySize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionStrLenCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubtract* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSwitch* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(ExpressionTestApiVersion* expr) final {
        _context->pushExpr(
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int32_t>(1)));
    }
    void visit(ExpressionToLower* expr) final {
        generateStringCaseConversionExpression(_context, "toLower");
    }
    void visit(ExpressionToUpper* expr) final {
        generateStringCaseConversionExpression(_context, "toUpper");
    }
    void visit(ExpressionTrim* expr) final {
        unsupportedExpression("$trim");
    }
    void visit(ExpressionTrunc* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionType* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionZip* expr) final {
        unsupportedExpression("$zip");
    }
    void visit(ExpressionConvert* expr) final {
        unsupportedExpression("$convert");
    }
    void visit(ExpressionRegexFind* expr) final {
        generateRegexExpression(expr, "regexFind");
    }
    void visit(ExpressionRegexFindAll* expr) final {
        generateRegexExpression(expr, "regexFindAll");
    }
    void visit(ExpressionRegexMatch* expr) final {
        generateRegexExpression(expr, "regexMatch");
    }
    void visit(ExpressionCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "cos", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(ExpressionSine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "sin", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(ExpressionTangent* expr) final {
        generateTrigonometricExpressionWithBounds(
            "tan", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(ExpressionArcCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "acos", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(ExpressionArcSine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "asin", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(ExpressionArcTangent* expr) final {
        generateTrigonometricExpression("atan");
    }
    void visit(ExpressionArcTangent2* expr) final {
        generateTrigonometricExpressionBinary("atan2");
    }
    void visit(ExpressionHyperbolicArcTangent* expr) final {
        generateTrigonometricExpressionWithBounds(
            "atanh", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(ExpressionHyperbolicArcCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "acosh", DoubleBound(1.0, true), DoubleBound::plusInfinity());
    }
    void visit(ExpressionHyperbolicArcSine* expr) final {
        generateTrigonometricExpression("asinh");
    }
    void visit(ExpressionHyperbolicCosine* expr) final {
        generateTrigonometricExpression("cosh");
    }
    void visit(ExpressionHyperbolicSine* expr) final {
        generateTrigonometricExpression("sinh");
    }
    void visit(ExpressionHyperbolicTangent* expr) final {
        generateTrigonometricExpression("tanh");
    }
    void visit(ExpressionDegreesToRadians* expr) final {
        generateTrigonometricExpression("degreesToRadians");
    }
    void visit(ExpressionRadiansToDegrees* expr) final {
        generateTrigonometricExpression("radiansToDegrees");
    }
    void visit(ExpressionDayOfMonth* expr) final {
        generateDayOfExpression("dayOfMonth", expr);
    }
    void visit(ExpressionDayOfWeek* expr) final {
        generateDayOfExpression("dayOfWeek", expr);
    }
    void visit(ExpressionDayOfYear* expr) final {
        generateDayOfExpression("dayOfYear", expr);
    }
    void visit(ExpressionHour* expr) final {
        unsupportedExpression("$hour");
    }
    void visit(ExpressionMillisecond* expr) final {
        unsupportedExpression("$millisecond");
    }
    void visit(ExpressionMinute* expr) final {
        unsupportedExpression("$minute");
    }
    void visit(ExpressionMonth* expr) final {
        unsupportedExpression("$month");
    }
    void visit(ExpressionSecond* expr) final {
        unsupportedExpression("$second");
    }
    void visit(ExpressionWeek* expr) final {
        unsupportedExpression("$week");
    }
    void visit(ExpressionIsoWeekYear* expr) final {
        unsupportedExpression("$isoWeekYear");
    }
    void visit(ExpressionIsoDayOfWeek* expr) final {
        unsupportedExpression("$isoDayOfWeek");
    }
    void visit(ExpressionIsoWeek* expr) final {
        unsupportedExpression("$isoWeek");
    }
    void visit(ExpressionYear* expr) final {
        unsupportedExpression("$year");
    }
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionTests::Testable* expr) final {
        unsupportedExpression("$test");
    }
    void visit(ExpressionInternalJsEmit* expr) final {
        unsupportedExpression("$internalJsEmit");
    }
    void visit(ExpressionInternalFindSlice* expr) final {
        unsupportedExpression("$internalFindSlice");
    }
    void visit(ExpressionInternalFindPositional* expr) final {
        unsupportedExpression("$internalFindPositional");
    }
    void visit(ExpressionInternalFindElemMatch* expr) final {
        unsupportedExpression("$internalFindElemMatch");
    }
    void visit(ExpressionFunction* expr) final {
        unsupportedExpression("$function");
    }

    void visit(ExpressionRandom* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(ExpressionToHashedIndexKey* expr) final {
        unsupportedExpression("$toHashedIndexKey");
    }

    void visit(ExpressionDateAdd* expr) final {
        generateDateArithmeticsExpression(expr, "dateAdd");
    }

    void visit(ExpressionDateSubtract* expr) final {
        generateDateArithmeticsExpression(expr, "dateSubtract");
    }

    void visit(ExpressionGetField* expr) final {
        unsupportedExpression("$getField");
    }

    void visit(ExpressionSetField* expr) final {
        unsupportedExpression("$setField");
    }

private:
    /**
     * Shared logic for $and, $or. Converts each child into an EExpression that evaluates to Boolean
     * true or false, based on MQL rules for $and and $or branches, and then chains the branches
     * together using binary and/or EExpressions so that the result has MQL's short-circuit
     * semantics.
     */
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        invariant(logicOp == sbe::EPrimBinary::logicAnd || logicOp == sbe::EPrimBinary::logicOr);

        size_t numChildren = expr->getChildren().size();
        if (numChildren == 0) {
            // Empty $and and $or always evaluate to their logical operator's identity value: true
            // and false, respectively.
            auto logicIdentityVal = (logicOp == sbe::EPrimBinary::logicAnd);
            _context->pushExpr(sbe::makeE<sbe::EConstant>(
                sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(logicIdentityVal)));
            return;
        } else if (numChildren == 1) {
            // No need for short circuiting logic in a singleton $and/$or. Just execute the branch
            // and return its result as a bool.
            auto frameId = _context->state.frameId();
            _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(_context->popExpr()),
                generateCoerceToBoolExpression(sbe::EVariable{frameId, 0})));

            return;
        }

        std::vector<EvalExprStagePair> branches;
        for (size_t i = 0; i < numChildren; ++i) {
            auto [expr, stage] = _context->popFrame();

            auto frameId = _context->state.frameId();
            auto coercedExpr = sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(std::move(expr)),
                generateCoerceToBoolExpression(sbe::EVariable{frameId, 0}));

            branches.emplace_back(std::move(coercedExpr), std::move(stage));
        }
        std::reverse(branches.begin(), branches.end());

        auto [resultExpr, opStage] =
            generateShortCircuitingLogicalOp(logicOp,
                                             std::move(branches),
                                             _context->planNodeId,
                                             _context->state.slotIdGenerator,
                                             BooleanStateHelper{});

        auto loopJoinStage = makeLoopJoin(_context->extractCurrentEvalStage(),
                                          std::move(opStage),
                                          _context->planNodeId,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(resultExpr.extractExpr(), std::move(loopJoinStage));
    }

    /**
     * Handle $switch and $cond, which have different syntax but are structurally identical in the
     * AST.
     */
    void visitConditionalExpression(Expression* expr) {
        // The default case is always the last child in the ExpressionSwitch. If it is unspecified
        // in the user's query, it is a nullptr. In ExpressionCond, the last child is the "else"
        // branch, and it is guaranteed not to be nullptr.
        if (expr->getChildren().back() == nullptr) {
            _context->pushExpr(
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4934200},
                                       "$switch could not find a matching branch for an "
                                       "input, and no default was specified."));
        }

        auto numChildren = expr->getChildren().size();
        std::vector<EvalExprStagePair> branches;
        branches.reserve(numChildren);
        for (size_t i = 0; i < numChildren / 2 + 1; ++i) {
            auto [expr, stage] = _context->popFrame();

            if (i == 0) {
                // The first branch is the default value.
                branches.emplace_back(std::move(expr), std::move(stage));
                continue;
            }

            auto thenSlot = _context->state.slotId();
            auto thenStage =
                makeProject(std::move(stage), _context->planNodeId, thenSlot, std::move(expr));

            // Construct a FilterStage tree that will EOF if "case" expression returns false. In
            // this case inner branch of loop join with "then" expression will never be executed.
            std::tie(expr, stage) = _context->popFrame();
            auto frameId = _context->state.frameId();
            auto coercedExpr = sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(std::move(expr)),
                generateCoerceToBoolExpression(sbe::EVariable{frameId, 0}));
            auto conditionStage =
                makeFilter<false>(std::move(stage), std::move(coercedExpr), _context->planNodeId);

            // Create a LoopJoinStage that will evaluate its outer child exactly once. If outer
            // child produces non-EOF result (i.e. condition evaluated to true), inner child is
            // executed. Inner child simply bounds result of "then" expression to a slot.
            auto loopJoinStage = makeLoopJoin(std::move(conditionStage),
                                              std::move(thenStage),
                                              _context->planNodeId,
                                              _context->getLexicalEnvironment());

            branches.emplace_back(thenSlot, std::move(loopJoinStage));
        }

        std::reverse(branches.begin(), branches.end());

        auto [resultExpr, resultStage] = generateSingleResultUnion(
            std::move(branches), {}, _context->planNodeId, _context->state.slotIdGenerator);

        auto loopJoinStage = makeLoopJoin(_context->extractCurrentEvalStage(),
                                          std::move(resultStage),
                                          _context->planNodeId,
                                          _context->getLexicalEnvironment());

        _context->pushExpr(resultExpr.extractExpr(), std::move(loopJoinStage));
    }

    void generateDayOfExpression(StringData exprName, Expression* expr) {
        auto frameId = _context->state.frameId();
        std::vector<std::unique_ptr<sbe::EExpression>> args;
        std::vector<std::unique_ptr<sbe::EExpression>> binds;
        sbe::EVariable dateRef(frameId, 0);
        sbe::EVariable timezoneRef(frameId, 1);

        auto children = expr->getChildren();
        invariant(children.size() == 2);
        _context->ensureArity(children[1] ? 2 : 1);

        auto timezone = [&]() {
            if (children[1]) {
                return _context->popExpr();
            }
            auto [utcTag, utcVal] = sbe::value::makeNewString("UTC");
            return sbe::makeE<sbe::EConstant>(utcTag, utcVal);
        }();
        auto date = _context->popExpr();

        auto timeZoneDBSlot = _context->state.env->getSlot("timeZoneDB"_sd);
        args.push_back(sbe::makeE<sbe::EVariable>(timeZoneDBSlot));

        // Add date to arguments.
        binds.push_back(std::move(date));
        args.push_back(dateRef.clone());

        // Add timezone to arguments.
        binds.push_back(std::move(timezone));
        args.push_back(timezoneRef.clone());

        // Check that each argument exists, is not null, and is the correct type.
        auto totalDayOfFunc = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(timezoneRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(timezoneRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4998200},
                                                 str::stream() << "$" << exprName.toString()
                                                               << " timezone must be a string")},
            CaseValuePair{
                makeNot(makeFunction(
                    "isTimezone", sbe::makeE<sbe::EVariable>(timeZoneDBSlot), timezoneRef.clone())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4998201},
                                       str::stream() << "$" << exprName.toString()
                                                     << " timezone must be a valid timezone")},
            CaseValuePair{generateNullOrMissing(dateRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{makeNot(sbe::makeE<sbe::ETypeMatch>(dateRef.clone(), dateTypeMask())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4998202},
                                                 str::stream()
                                                     << "$" << exprName.toString()
                                                     << " date must have a format of a date")},
            sbe::makeE<sbe::EFunction>(exprName.toString(), std::move(args)));
        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(totalDayOfFunc)));
    }

    /**
     * Creates a CaseValuePair such that an exception is thrown if a value of the parameter denoted
     * by variable 'dateRef' is of a type that is not coercible to a date.
     *
     * dateRef - a variable corresponding to the parameter.
     * errorCode - error code of the type mismatch error.
     * expressionName - a name of an expression the parameter belongs to.
     * parameterName - a name of the parameter corresponding to variable 'dateRef'.
     */
    static CaseValuePair generateFailIfNotCoercibleToDate(const sbe::EVariable& dateRef,
                                                          ErrorCodes::Error errorCode,
                                                          StringData expressionName,
                                                          StringData parameterName) {
        return {makeNot(sbe::makeE<sbe::ETypeMatch>(dateRef.clone(), dateTypeMask())),
                sbe::makeE<sbe::EFail>(errorCode,
                                       str::stream()
                                           << expressionName << " parameter '" << parameterName
                                           << "' must be coercible to date")};
    }

    /**
     * Creates a CaseValuePair such that Null value is returned if a value of variable denoted by
     * 'variable' is null or missing.
     */
    static CaseValuePair generateReturnNullIfNullOrMissing(const sbe::EVariable& variable) {
        return {generateNullOrMissing(variable), makeConstant(sbe::value::TypeTags::Null, 0)};
    }

    /**
     * Creates a boolean expression to check if 'variable' is equal to string 'string'.
     */
    static std::unique_ptr<sbe::EExpression> generateIsEqualToStringCheck(
        const sbe::EVariable& variable, StringData string) {
        return sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                            makeFunction("isString", variable.clone()),
                                            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::eq,
                                                                         variable.clone(),
                                                                         makeConstant(string)));
    }

    /**
     * Shared expression building logic for trignometric expressions to make sure the operand
     * is numeric and is not null.
     */
    void generateTrigonometricExpression(StringData exprName) {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto genericTrignomentricExpr = sbe::makeE<sbe::EIf>(
            generateNullOrMissing(frameId, 0),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(makeFunction("isNumber", inputRef.clone()),
                                 makeFunction(exprName.toString(), inputRef.clone()),
                                 sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995501},
                                                        str::stream()
                                                            << "$" << exprName.toString()
                                                            << " supports only numeric types")));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(binds), std::move(genericTrignomentricExpr)));
    }

    /**
     * Shared expression building logic for binary trigonometric expressions to make sure the
     * operands are numeric and are not null.
     */
    void generateTrigonometricExpressionBinary(StringData exprName) {
        _context->ensureArity(2);

        auto genericTrignomentricExpr = makeLocalBind(
            _context->state.frameIdGenerator,
            [&](sbe::EVariable lhs, sbe::EVariable rhs) {
                return buildMultiBranchConditional(
                    CaseValuePair{makeBinaryOp(sbe::EPrimBinary::logicOr,
                                               generateNullOrMissing(lhs),
                                               generateNullOrMissing(rhs)),
                                  makeConstant(sbe::value::TypeTags::Null, 0)},
                    CaseValuePair{
                        makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                     makeFunction("isNumber", lhs.clone()),
                                     makeFunction("isNumber", rhs.clone())),
                        makeFunction(exprName.toString(), lhs.clone(), rhs.clone()),
                    },
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{5688500},
                                           str::stream() << "$" << exprName
                                                         << " supports only numeric types"));
            },
            _context->popExpr(),
            _context->popExpr());
        _context->pushExpr(std::move(genericTrignomentricExpr));
    }

    /**
     * Shared expression building logic for trignometric expressions with bounds for the valid
     * values of the argument.
     */
    void generateTrigonometricExpressionWithBounds(StringData exprName,
                                                   const DoubleBound& lowerBound,
                                                   const DoubleBound& upperBound) {
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        sbe::EPrimBinary::Op lowerCmp =
            lowerBound.inclusive ? sbe::EPrimBinary::greaterEq : sbe::EPrimBinary::greater;
        sbe::EPrimBinary::Op upperCmp =
            upperBound.inclusive ? sbe::EPrimBinary::lessEq : sbe::EPrimBinary::less;
        auto checkBounds = makeBinaryOp(
            sbe::EPrimBinary::logicAnd,
            makeBinaryOp(
                lowerCmp,
                inputRef.clone(),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberDouble,
                                           sbe::value::bitcastFrom<double>(lowerBound.bound))),
            makeBinaryOp(
                upperCmp,
                inputRef.clone(),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberDouble,
                                           sbe::value::bitcastFrom<double>(upperBound.bound))));

        auto genericTrignomentricExpr = sbe::makeE<sbe::EIf>(
            generateNullOrMissing(frameId, 0),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(
                makeNot(makeFunction("isNumber", inputRef.clone())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995502},
                                       str::stream() << "$" << exprName.toString()
                                                     << " supports only numeric types"),
                sbe::makeE<sbe::EIf>(
                    std::move(checkBounds),
                    makeFunction(exprName.toString(), inputRef.clone()),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995503},
                                           str::stream() << "Cannot apply $" << exprName.toString()
                                                         << ", value must be in "
                                                         << lowerBound.printLowerBound() << ", "
                                                         << upperBound.printUpperBound()))));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(binds), std::move(genericTrignomentricExpr)));
    }

    /*
     * Generates an EExpression that returns an index for $indexOfBytes or $indexOfCP.
     */
    void visitIndexOfFunction(Expression* expr,
                              ExpressionVisitorContext* _context,
                              const std::string& indexOfFunction) {
        auto frameId = _context->state.frameId();
        auto children = expr->getChildren();
        auto operandSize = children.size() <= 3 ? 3 : 4;
        std::vector<std::unique_ptr<sbe::EExpression>> operands(operandSize);
        std::vector<std::unique_ptr<sbe::EExpression>> bindings;
        sbe::EVariable strRef(frameId, 0);
        sbe::EVariable substrRef(frameId, 1);
        boost::optional<sbe::EVariable> startIndexRef;
        boost::optional<sbe::EVariable> endIndexRef;

        // Get arguments from stack.
        switch (children.size()) {
            case 2: {
                operands[2] = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                         sbe::value::bitcastFrom<int64_t>(0));
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                break;
            }
            case 3: {
                operands[2] = _context->popExpr();
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                break;
            }
            case 4: {
                operands[3] = _context->popExpr();
                operands[2] = _context->popExpr();
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                endIndexRef.emplace(frameId, 3);
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }

        // Add string and substring operands.
        bindings.push_back(strRef.clone());
        bindings.push_back(substrRef.clone());

        // Add start index operand.
        if (startIndexRef) {
            auto numericConvert64 = sbe::makeE<sbe::ENumericConvert>(
                startIndexRef->clone(), sbe::value::TypeTags::NumberInt64);
            auto checkValidStartIndex = buildMultiBranchConditional(
                CaseValuePair{generateNullishOrNotRepresentableInt32Check(*startIndexRef),
                              sbe::makeE<sbe::EFail>(
                                  ErrorCodes::Error{5075303},
                                  str::stream() << "$" << indexOfFunction
                                                << " start index must resolve to a number")},
                CaseValuePair{generateNegativeCheck(*startIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075304},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " start index must be positive")},
                std::move(numericConvert64));
            bindings.push_back(std::move(checkValidStartIndex));
        }
        // Add end index operand.
        if (endIndexRef) {
            auto numericConvert64 = sbe::makeE<sbe::ENumericConvert>(
                endIndexRef->clone(), sbe::value::TypeTags::NumberInt64);
            auto checkValidEndIndex = buildMultiBranchConditional(
                CaseValuePair{generateNullishOrNotRepresentableInt32Check(*endIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075305},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " end index must resolve to a number")},
                CaseValuePair{generateNegativeCheck(*endIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075306},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " end index must be positive")},
                std::move(numericConvert64));
            bindings.push_back(std::move(checkValidEndIndex));
        }

        // Check if string or substring are null or missing before calling indexOfFunction.
        auto checkStringNullOrMissing = generateNullOrMissing(frameId, 0);
        auto checkSubstringNullOrMissing = generateNullOrMissing(frameId, 1);
        auto exprIndexOfFunction = sbe::makeE<sbe::EFunction>(indexOfFunction, std::move(bindings));

        auto totalExprIndexOfFunction = buildMultiBranchConditional(
            CaseValuePair{std::move(checkStringNullOrMissing),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(strRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5075300},
                              str::stream() << "$" << indexOfFunction
                                            << " string must resolve to a string or null")},
            CaseValuePair{std::move(checkSubstringNullOrMissing),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075301},
                                                 str::stream()
                                                     << "$" << indexOfFunction
                                                     << " substring must resolve to a string")},
            CaseValuePair{generateNonStringCheck(substrRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075302},
                                                 str::stream()
                                                     << "$" << indexOfFunction
                                                     << " substring must resolve to a string")},
            std::move(exprIndexOfFunction));
        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(totalExprIndexOfFunction)));
    }

    /**
     * Generic logic for building set expressions: setUnion, setIntersection, etc.
     */
    void generateSetExpression(Expression* expr, SetOperation setOp) {
        using namespace std::literals;

        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->state.frameId();

        auto generateNotArray = [frameId](const sbe::value::SlotId slotId) {
            sbe::EVariable var{frameId, slotId};
            return makeNot(makeFunction("isArray", var.clone()));
        };

        std::vector<std::unique_ptr<sbe::EExpression>> binds;
        std::vector<std::unique_ptr<sbe::EExpression>> argVars;
        std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNull;
        std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNotArray;
        binds.reserve(arity);
        argVars.reserve(arity);
        checkExprsNull.reserve(arity);
        checkExprsNotArray.reserve(arity);

        auto collatorSlot = _context->state.env->getSlotIfExists("collator"_sd);

        auto [operatorName, setFunctionName] = [setOp, collatorSlot]() {
            switch (setOp) {
                case SetOperation::Difference:
                    return std::make_pair("setDifference"_sd,
                                          collatorSlot ? "collSetDifference"_sd
                                                       : "setDifference"_sd);
                case SetOperation::Intersection:
                    return std::make_pair("setIntersection"_sd,
                                          collatorSlot ? "collSetIntersection"_sd
                                                       : "setIntersection"_sd);
                case SetOperation::Union:
                    return std::make_pair("setUnion"_sd,
                                          collatorSlot ? "collSetUnion"_sd : "setUnion"_sd);
                default:
                    MONGO_UNREACHABLE;
            }
        }();

        if (collatorSlot) {
            argVars.push_back(sbe::makeE<sbe::EVariable>(*collatorSlot));
        }

        for (size_t idx = 0; idx < arity; ++idx) {
            binds.push_back(_context->popExpr());
            argVars.push_back(sbe::makeE<sbe::EVariable>(frameId, idx));

            checkExprsNull.push_back(generateNullOrMissing(frameId, idx));
            checkExprsNotArray.push_back(generateNotArray(idx));
        }
        // Reverse the binds array to preserve the original order of the arguments, since some set
        // operations, such as $setDifference, are not commutative.
        std::reverse(std::begin(binds), std::end(binds));

        using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
        auto checkNullAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkExprsNull.begin() + 1),
            std::move_iterator<iter_t>(checkExprsNull.end()),
            std::move(checkExprsNull.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });
        auto checkNotArrayAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkExprsNotArray.begin() + 1),
            std::move_iterator<iter_t>(checkExprsNotArray.end()),
            std::move(checkExprsNotArray.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });
        auto setExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkNullAnyArgument),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(checkNotArrayAnyArgument),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126900},
                                                 str::stream()
                                                     << "All operands of $" << operatorName
                                                     << " must be arrays.")},
            sbe::makeE<sbe::EFunction>(setFunctionName, std::move(argVars)));
        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(setExpr)));
    }

    /**
     * Shared expression building logic for regex expressions.
     */
    void generateRegexExpression(ExpressionRegex* expr, StringData exprName) {
        size_t arity = expr->hasOptions() ? 3 : 2;
        _context->ensureArity(arity);

        std::unique_ptr<sbe::EExpression> options =
            expr->hasOptions() ? _context->popExpr() : nullptr;
        auto pattern = _context->popExpr();
        auto input = _context->popExpr();

        // Create top level local bind.
        auto frameId = _context->state.frameId();
        auto binds = sbe::makeEs(std::move(input));
        sbe::EVariable inputVar{frameId, 0};

        auto makeError = [exprName](int errorCode, StringData message) {
            return sbe::makeE<sbe::EFail>(ErrorCodes::Error{errorCode},
                                          str::stream() << "$" << exprName.toString() << ": "
                                                        << message.toString());
        };

        auto makeRegexFunctionCall = [&](std::unique_ptr<sbe::EExpression> compiledRegex) {
            return makeLocalBind(
                _context->state.frameIdGenerator,
                [&](sbe::EVariable regexResult) {
                    return sbe::makeE<sbe::EIf>(
                        makeFunction("exists", regexResult.clone()),
                        regexResult.clone(),
                        makeError(5073403,
                                  "error occurred while executing the regular expression"));
                },
                makeFunction(exprName.toString(), std::move(compiledRegex), inputVar.clone()));
        };

        auto regexFunctionResult = [&]() {
            if (auto patternAndOptions = expr->getConstantPatternAndOptions(); patternAndOptions) {
                auto [pattern, options] = *patternAndOptions;
                if (!pattern) {
                    // Pattern is null, just generate null result.
                    return generateRegexNullResponse(exprName);
                }

                // Create the compiled Regex from constant pattern and options.
                auto [regexTag, regexVal] = sbe::value::makeNewPcreRegex(*pattern, options);
                auto compiledRegex = sbe::makeE<sbe::EConstant>(regexTag, regexVal);
                return makeRegexFunctionCall(std::move(compiledRegex));
            }

            // Include pattern and options in the outer local bind.
            sbe::EVariable patternVar{frameId, 1};
            binds.push_back(std::move(pattern));

            boost::optional<sbe::EVariable> optionsVar;
            if (options) {
                binds.push_back(std::move(options));
                optionsVar.emplace(frameId, 2);
            }

            // 'patternArgument' contains the following expression:
            //
            // if isString(pattern) {
            //     if hasNullBytes(pattern) {
            //         fail('pattern cannot have null bytes in it')
            //     } else {
            //         pattern
            //     }
            // } else if isBsonRegex(pattern) {
            //     getRegexPattern(pattern)
            // } else {
            //     fail('pattern must be either string or BSON RegEx')
            // }
            auto patternNullBytesCheck = sbe::makeE<sbe::EIf>(
                makeFunction("hasNullBytes", patternVar.clone()),
                makeError(5126602, "regex pattern must not have embedded null bytes"),
                patternVar.clone());
            auto patternArgument = buildMultiBranchConditional(
                CaseValuePair{makeFunction("isString", patternVar.clone()),
                              std::move(patternNullBytesCheck)},
                CaseValuePair{sbe::makeE<sbe::ETypeMatch>(patternVar.clone(),
                                                          getBSONTypeMask(BSONType::RegEx)),
                              makeFunction("getRegexPattern", patternVar.clone())},
                makeError(5126601, "regex pattern must have either string or BSON RegEx type"));

            if (!optionsVar) {
                // If no options are passed to the expression, try to extract them from the pattern.
                auto optionsArgument =
                    sbe::makeE<sbe::EIf>(sbe::makeE<sbe::ETypeMatch>(
                                             patternVar.clone(), getBSONTypeMask(BSONType::RegEx)),
                                         makeFunction("getRegexFlags", patternVar.clone()),
                                         makeConstant(""));
                auto compiledRegex = makeFunction(
                    "regexCompile", std::move(patternArgument), std::move(optionsArgument));
                return sbe::makeE<sbe::EIf>(makeFunction("isNull", patternVar.clone()),
                                            generateRegexNullResponse(exprName),
                                            makeRegexFunctionCall(std::move(compiledRegex)));
            }

            auto optionsArgument = [&]() {
                // The code below generates the following expression:
                //
                // let stringOptions =
                //     if isString(options) {
                //         if hasNullBytes(options) {
                //             fail('options cannot have null bytes in it')
                //         } else {
                //             options
                //         }
                //     } else if isNull(options) {
                //         ''
                //     } else {
                //         fail('options must be either string or null')
                //     }
                // in
                //     if isBsonRegex(pattern) {
                //         let bsonOptions = getRegexFlags(pattern)
                //         in
                //             if stringOptions == "" {
                //                 bsonOptions
                //             } else if bsonOptions == "" {
                //                 stringOptions
                //             } else {
                //                 fail('multiple options specified')
                //             }
                //     } else {
                //         stringOptions
                //     }
                auto optionsNullBytesCheck = sbe::makeE<sbe::EIf>(
                    makeFunction("hasNullBytes", optionsVar->clone()),
                    makeError(5126604, "regex flags must not have embedded null bytes"),
                    optionsVar->clone());
                auto stringOptions = buildMultiBranchConditional(
                    CaseValuePair{makeFunction("isString", optionsVar->clone()),
                                  std::move(optionsNullBytesCheck)},
                    CaseValuePair{makeFunction("isNull", optionsVar->clone()), makeConstant("")},
                    makeError(5126603, "regex flags must have either string or null type"));

                auto generateIsEmptyString = [](const sbe::EVariable& var) {
                    return makeBinaryOp(sbe::EPrimBinary::eq, var.clone(), makeConstant(""));
                };

                return makeLocalBind(
                    _context->state.frameIdGenerator,
                    [&](sbe::EVariable stringOptions) {
                        auto checkBsonRegexOptions = makeLocalBind(
                            _context->state.frameIdGenerator,
                            [&](sbe::EVariable bsonOptions) {
                                return buildMultiBranchConditional(
                                    CaseValuePair{generateIsEmptyString(stringOptions),
                                                  bsonOptions.clone()},
                                    CaseValuePair{generateIsEmptyString(bsonOptions),
                                                  stringOptions.clone()},
                                    makeError(5126605,
                                              "regex options cannot be specified in both BSON "
                                              "RegEx and 'options' field"));
                            },
                            makeFunction("getRegexFlags", patternVar.clone()));

                        return sbe::makeE<sbe::EIf>(
                            sbe::makeE<sbe::ETypeMatch>(patternVar.clone(),
                                                        getBSONTypeMask(BSONType::RegEx)),
                            std::move(checkBsonRegexOptions),
                            stringOptions.clone());
                    },
                    std::move(stringOptions));
            }();

            // If there are options passed to the expression, we construct local bind with options
            // argument because it needs to be validated even when pattern is null.
            return makeLocalBind(
                _context->state.frameIdGenerator,
                [&](sbe::EVariable options) {
                    auto compiledRegex =
                        makeFunction("regexCompile", std::move(patternArgument), options.clone());
                    return sbe::makeE<sbe::EIf>(makeFunction("isNull", patternVar.clone()),
                                                generateRegexNullResponse(exprName),
                                                makeRegexFunctionCall(std::move(compiledRegex)));
                },
                std::move(optionsArgument));
        }();

        auto resultExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputVar), generateRegexNullResponse(exprName)},
            CaseValuePair{generateNonStringCheck(inputVar),
                          makeError(5073401, "input must be of type string")},
            std::move(regexFunctionResult));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(resultExpr)));
    }

    /**
     * Generic logic for building $dateAdd and $dateSubtract expressions.
     */
    void generateDateArithmeticsExpression(ExpressionDateArithmetics* expr,
                                           const std::string& dateExprName) {
        auto children = expr->getChildren();
        auto arity = children.size();
        invariant(arity == 4);
        _context->ensureArity(children[3] ? 4 : 3);

        auto timezoneExpr = [&]() {
            if (children[3]) {
                return _context->popExpr();
            }
            return makeConstant("UTC");
        }();
        auto amountExpr = _context->popExpr();
        auto unitExpr = _context->popExpr();
        auto startDateExpr = _context->popExpr();

        std::vector<std::unique_ptr<sbe::EExpression>> binds;
        binds.push_back(std::move(startDateExpr));
        binds.push_back(std::move(unitExpr));
        binds.push_back(std::move(amountExpr));
        binds.push_back(std::move(timezoneExpr));

        auto frameId = _context->state.frameId();
        sbe::EVariable startDateRef{frameId, 0};
        sbe::EVariable unitRef{frameId, 1};
        sbe::EVariable origAmountRef{frameId, 2};
        sbe::EVariable tzRef{frameId, 3};
        sbe::EVariable amountRef{frameId, 4};

        auto convertedAmountInt64 = [&]() {
            if (dateExprName == "dateAdd") {
                return sbe::makeE<sbe::ENumericConvert>(origAmountRef.clone(),
                                                        sbe::value::TypeTags::NumberInt64);
            } else if (dateExprName == "dateSubtract") {
                return sbe::makeE<sbe::ENumericConvert>(
                    sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::negate, origAmountRef.clone()),
                    sbe::value::TypeTags::NumberInt64);
            } else {
                MONGO_UNREACHABLE;
            }
        }();
        binds.push_back(std::move(convertedAmountInt64));

        std::vector<std::unique_ptr<sbe::EExpression>> args;
        auto timeZoneDBSlot = _context->state.env->getSlot("timeZoneDB"_sd);
        args.push_back(sbe::makeE<sbe::EVariable>(timeZoneDBSlot));
        args.push_back(startDateRef.clone());
        args.push_back(unitRef.clone());
        args.push_back(amountRef.clone());
        args.push_back(tzRef.clone());

        std::vector<std::unique_ptr<sbe::EExpression>> checkNullArg;
        sbe::value::SlotId slot{0};
        for (size_t idx = 0; idx < arity; ++idx, ++slot) {
            checkNullArg.push_back(generateNullOrMissing(frameId, slot));
        }

        using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
        auto checkNullAnyArgument = std::accumulate(
            std::move_iterator<iter_t>(checkNullArg.begin() + 1),
            std::move_iterator<iter_t>(checkNullArg.end()),
            std::move(checkNullArg.front()),
            [](auto&& acc, auto&& ex) {
                return makeBinaryOp(sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
            });

        auto dateAddExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkNullAnyArgument),
                          makeConstant(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(tzRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5166601},
                              str::stream() << "$" << dateExprName
                                            << " expects timezone argument of type string")},
            CaseValuePair{makeNot(makeFunction("isTimezone",
                                               sbe::makeE<sbe::EVariable>(timeZoneDBSlot),
                                               tzRef.clone())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166602},
                                                 str::stream() << "$" << dateExprName
                                                               << " expects a valid timezone")},
            CaseValuePair{
                makeNot(sbe::makeE<sbe::ETypeMatch>(startDateRef.clone(), dateTypeMask())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166603},
                                       str::stream()
                                           << "$" << dateExprName
                                           << " must have startDate argument convertable to date")},
            CaseValuePair{generateNonStringCheck(unitRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166604},
                                                 str::stream()
                                                     << "$" << dateExprName
                                                     << " expects unit argument of type string")},
            CaseValuePair{makeNot(makeFunction("isTimeUnit", unitRef.clone())),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5166605},
                                                 str::stream() << "$" << dateExprName
                                                               << " expects a valid time unit")},
            CaseValuePair{makeNot(makeFunction("exists", amountRef.clone())),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5166606},
                              str::stream() << "$" << dateExprName
                                            << " expects amount argument to be an integer number")},
            sbe::makeE<sbe::EFunction>("dateAdd", std::move(args)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(dateAddExpr)));
    }

    void unsupportedExpression(const char* op) const {
        // We're guaranteed to not fire this assertion by implementing a mechanism in the upper
        // layer which directs the query to the classic engine when an unsupported expression
        // appears.
        tasserted(5182300, str::stream() << "Unsupported expression in SBE stage builder: " << op);
    }

    ExpressionVisitorContext* _context;
};  // namespace

class ExpressionWalker final {
public:
    ExpressionWalker(ExpressionVisitor* preVisitor,
                     ExpressionVisitor* inVisitor,
                     ExpressionVisitor* postVisitor)
        : _preVisitor{preVisitor}, _inVisitor{inVisitor}, _postVisitor{postVisitor} {}

    void preVisit(Expression* expr) {
        expr->acceptVisitor(_preVisitor);
    }

    void inVisit(long long count, Expression* expr) {
        expr->acceptVisitor(_inVisitor);
    }

    void postVisit(Expression* expr) {
        expr->acceptVisitor(_postVisitor);
    }

private:
    ExpressionVisitor* _preVisitor;
    ExpressionVisitor* _inVisitor;
    ExpressionVisitor* _postVisitor;
};
}  // namespace

std::unique_ptr<sbe::EExpression> generateCoerceToBoolExpression(sbe::EVariable branchRef) {
    auto makeNotNullOrUndefinedCheck = [&branchRef]() {
        return makeNot(sbe::makeE<sbe::ETypeMatch>(branchRef.clone(),
                                                   getBSONTypeMask(BSONType::jstNULL) |
                                                       getBSONTypeMask(BSONType::Undefined)));
    };

    auto makeNeqFalseCheck = [&branchRef]() {
        return makeBinaryOp(
            sbe::EPrimBinary::neq,
            makeBinaryOp(sbe::EPrimBinary::cmp3w,
                         branchRef.clone(),
                         sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                    sbe::value::bitcastFrom<bool>(false))),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                       sbe::value::bitcastFrom<int64_t>(0)));
    };

    auto makeNeqZeroCheck = [&branchRef]() {
        return makeBinaryOp(
            sbe::EPrimBinary::neq,
            makeBinaryOp(sbe::EPrimBinary::cmp3w,
                         branchRef.clone(),
                         sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                    sbe::value::bitcastFrom<int64_t>(0))),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                       sbe::value::bitcastFrom<int64_t>(0)));
    };

    return makeBinaryOp(
        sbe::EPrimBinary::logicAnd,
        makeFunction("exists", branchRef.clone()),
        makeBinaryOp(
            sbe::EPrimBinary::logicAnd,
            makeNotNullOrUndefinedCheck(),
            makeBinaryOp(sbe::EPrimBinary::logicAnd, makeNeqFalseCheck(), makeNeqZeroCheck())));
}

std::tuple<sbe::value::SlotId, std::unique_ptr<sbe::EExpression>, EvalStage> generateExpression(
    StageBuilderState& state,
    Expression* expr,
    EvalStage stage,
    sbe::value::SlotId rootSlot,
    PlanNodeId planNodeId) {
    ExpressionVisitorContext context(state, std::move(stage), rootSlot, planNodeId);

    ExpressionPreVisitor preVisitor{&context};
    ExpressionInVisitor inVisitor{&context};
    ExpressionPostVisitor postVisitor{&context};
    ExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    expression_walker::walk(&walker, expr);

    auto [slotId, resultExpr, resultStage] = context.done();
    return {slotId, std::move(resultExpr), std::move(resultStage)};
}
}  // namespace mongo::stage_builder
