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

#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_lookup.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/fail_point.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

namespace {

/**
 * Constructs a query of the following shape:
 *  {$or: [
 *    {'fieldName': {$eq: 'values[0]'}},
 *    {'fieldName': {$eq: 'values[1]'}},
 *    ...
 *  ]}
 */
BSONObj buildEqualityOrQuery(const std::string& fieldName, const BSONArray& values) {
    BSONObjBuilder orBuilder;
    {
        BSONArrayBuilder orPredicatesBuilder(orBuilder.subarrayStart("$or"));
        for (auto&& value : values) {
            orPredicatesBuilder.append(BSON(fieldName << BSON("$eq" << value)));
        }
    }
    return orBuilder.obj();
}

void lookupPipeValidator(const Pipeline& pipeline) {
    const auto& sources = pipeline.getSources();
    std::for_each(sources.begin(), sources.end(), [](auto& src) {
        uassert(51047,
                str::stream() << src->getSourceName()
                              << " is not allowed within a $lookup's sub-pipeline",
                src->constraints().isAllowedInLookupPipeline());
    });
}

bool foreignShardedLookupAllowed() {
    return getTestCommandsEnabled() && internalQueryAllowShardedLookup.load();
}

// Parses $lookup 'from' field. The 'from' field must be a string or one of the following
// exceptions:
// {from: {db: "config", coll: "cache.chunks.*"}, ...} or
// {from: {db: "local", coll: "oplog.rs"}, ...} or
// {from: {db: "local", coll: "tenantMigration.oplogView"}, ...} .
NamespaceString parseLookupFromAndResolveNamespace(const BSONElement& elem, StringData defaultDb) {
    // The object syntax only works for 'cache.chunks.*', 'local.oplog.rs', and
    // 'local.tenantMigration.oplogViewwhich' which are not user namespaces so object type is
    // omitted from the error message below.
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$lookup 'from' field must be a string, but found "
                          << typeName(elem.type()),
            elem.type() == BSONType::String || elem.type() == BSONType::Object);

    if (elem.type() == BSONType::String) {
        return NamespaceString(defaultDb, elem.valueStringData());
    }

    // Valdate the db and coll names.
    auto spec = NamespaceSpec::parse({elem.fieldNameStringData()}, elem.embeddedObject());
    auto nss = NamespaceString(spec.getDb().value_or(""), spec.getColl().value_or(""));
    uassert(
        ErrorCodes::FailedToParse,
        str::stream() << "$lookup with syntax {from: {db:<>, coll:<>},..} is not supported for db: "
                      << nss.db() << " and coll: " << nss.coll(),
        nss.isConfigDotCacheDotChunks() || nss == NamespaceString::kRsOplogNamespace ||
            nss == NamespaceString::kTenantMigrationOplogView);
    return nss;
}

/**
 * Checks if a sort stage's pattern is suitable to push the stage before $lookup. The sort stage
 * must not share the same prefix with any field created or modified by the lookup stage.
 */
bool checkModifiedPathsSortReorder(const SortPattern& sortPattern,
                                   const DocumentSource::GetModPathsReturn& modPaths) {
    for (const auto& sortKey : sortPattern) {
        if (!sortKey.fieldPath.has_value()) {
            return false;
        }
        if (sortKey.fieldPath->getPathLength() < 1) {
            return false;
        }
        auto sortField = sortKey.fieldPath->getFieldName(0);
        auto it = std::find_if(
            modPaths.paths.begin(), modPaths.paths.end(), [&sortField](const auto& modPath) {
                // Finds if the shorter path is a prefix field of or the same as the longer one.
                return sortField == modPath || expression::isPathPrefixOf(sortField, modPath) ||
                    expression::isPathPrefixOf(modPath, sortField);
            });
        if (it != modPaths.paths.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace

DocumentSourceLookUp::DocumentSourceLookUp(
    NamespaceString fromNs,
    std::string as,
    boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx),
      _fromNs(std::move(fromNs)),
      _as(std::move(as)),
      _variables(expCtx->variables),
      _variablesParseState(expCtx->variablesParseState.copyWith(_variables.useIdGenerator())) {
    const auto& resolvedNamespace = expCtx->getResolvedNamespace(_fromNs);
    _resolvedNs = resolvedNamespace.ns;
    _resolvedPipeline = resolvedNamespace.pipeline;
    _fromExpCtx = expCtx->copyForSubPipeline(resolvedNamespace.ns);
    if (fromCollator) {
        _fromExpCtx->setCollator(std::move(fromCollator.get()));
        _hasExplicitCollation = true;
    }
}

DocumentSourceLookUp::DocumentSourceLookUp(
    NamespaceString fromNs,
    std::string as,
    std::string localField,
    std::string foreignField,
    boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceLookUp(fromNs, as, std::move(fromCollator), expCtx) {
    _localField = std::move(localField);
    _foreignField = std::move(foreignField);

    // We append an additional BSONObj to '_resolvedPipeline' as a placeholder for the $match stage
    // we'll eventually construct from the input document.
    _resolvedPipeline.reserve(_resolvedPipeline.size() + 1);
    _resolvedPipeline.push_back(BSON("$match" << BSONObj()));
    _fieldMatchPipelineIdx = _resolvedPipeline.size() - 1;

    initializeResolvedIntrospectionPipeline();
}

DocumentSourceLookUp::DocumentSourceLookUp(
    NamespaceString fromNs,
    std::string as,
    std::vector<BSONObj> pipeline,
    BSONObj letVariables,
    boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
    boost::optional<std::pair<std::string, std::string>> localForeignFields,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceLookUp(fromNs, as, std::move(fromCollator), expCtx) {
    // '_resolvedPipeline' will first be initialized by the constructor delegated to within this
    // constructor's initializer list. It will be populated with view pipeline prefix if 'fromNs'
    // represents a view. We will then append stages to ensure any view prefix is not overwritten.

    if (localForeignFields != boost::none) {
        std::tie(_localField, _foreignField) = *localForeignFields;

        // Append a BSONObj to '_resolvedPipeline' as a placeholder for the stage corresponding to
        // the local/foreignField $match.
        _resolvedPipeline.reserve(_resolvedPipeline.size() + 1);
        _resolvedPipeline.push_back(BSON("$match" << BSONObj()));
        _fieldMatchPipelineIdx = _resolvedPipeline.size() - 1;
    } else {
        // When local/foreignFields are included, we cannot enable the cache because the $match
        // is a correlated prefix that will not be detected. Here, local/foreignFields are absent,
        // so we enable the cache.
        _cache.emplace(internalDocumentSourceLookupCacheSizeBytes.load());
    }

    // Add the user pipeline to '_resolvedPipeline' after any potential view prefix and $match
    _resolvedPipeline.insert(_resolvedPipeline.end(), pipeline.begin(), pipeline.end());
    _userPipeline = std::move(pipeline);

    for (auto&& varElem : letVariables) {
        const auto varName = varElem.fieldNameStringData();
        variableValidation::validateNameForUserWrite(varName);

        _letVariables.emplace_back(
            varName.toString(),
            Expression::parseOperand(expCtx.get(), varElem, expCtx->variablesParseState),
            _variablesParseState.defineVariable(varName));
    }

    initializeResolvedIntrospectionPipeline();
}

std::unique_ptr<DocumentSourceLookUp::LiteParsed> DocumentSourceLookUp::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $lookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    auto specObj = spec.Obj();
    auto fromElement = specObj["from"];
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "missing 'from' option to $lookup stage specification: " << specObj,
            fromElement);
    auto fromNss = parseLookupFromAndResolveNamespace(fromElement, nss.db());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $lookup namespace: " << fromNss.ns(),
            fromNss.isValid());

    // Recursively lite parse the nested pipeline, if one exists.
    auto pipelineElem = specObj["pipeline"];
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    if (pipelineElem) {
        auto pipeline = parsePipelineFromBSON(pipelineElem);
        liteParsedPipeline = LiteParsedPipeline(fromNss, pipeline);
    }

    bool hasInternalCollation = static_cast<bool>(specObj["_internalCollation"]);

    return std::make_unique<DocumentSourceLookUp::LiteParsed>(
        spec.fieldName(), std::move(fromNss), std::move(liteParsedPipeline), hasInternalCollation);
}

PrivilegeVector DocumentSourceLookUp::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {
    PrivilegeVector requiredPrivileges;
    invariant(_pipelines.size() <= 1);
    invariant(_foreignNss);

    // If no pipeline is specified or the local/foreignField syntax was used, then assume that we're
    // reading directly from the collection.
    if (_pipelines.empty() || !_pipelines[0].startsWithInitialSource()) {
        Privilege::addPrivilegeToPrivilegeVector(
            &requiredPrivileges,
            Privilege(ResourcePattern::forExactNamespace(*_foreignNss), ActionType::find));
    }

    // Add the sub-pipeline privileges, if one was specified.
    if (!_pipelines.empty()) {
        const LiteParsedPipeline& pipeline = _pipelines[0];
        Privilege::addPrivilegesToPrivilegeVector(
            &requiredPrivileges,
            std::move(pipeline.requiredPrivileges(isMongos, bypassDocumentValidation)));
    }

    return requiredPrivileges;
}

REGISTER_DOCUMENT_SOURCE(lookup,
                         DocumentSourceLookUp::LiteParsed::parse,
                         DocumentSourceLookUp::createFromBson,
                         AllowedWithApiStrict::kSometimes);

const char* DocumentSourceLookUp::getSourceName() const {
    return kStageName.rawData();
}

StageConstraints DocumentSourceLookUp::constraints(Pipeline::SplitState) const {
    HostTypeRequirement hostRequirement;
    if (_fromNs.isConfigDotCacheDotChunks()) {
        // $lookup from config.cache.chunks* namespaces is permitted to run on each individual
        // shard, rather than just the primary, since each shard should have an identical copy of
        // the namespace.
        hostRequirement = HostTypeRequirement::kAnyShard;
    } else {
        // When $lookup on sharded foreign collections is allowed, the foreign collection is
        // sharded, and the stage is executing on mongos, the stage can run on mongos or any shard.
        hostRequirement = (foreignShardedLookupAllowed() && pExpCtx->inMongos &&
                           pExpCtx->mongoProcessInterface->isSharded(pExpCtx->opCtx, _fromNs))
            ? HostTypeRequirement::kNone
            : HostTypeRequirement::kPrimaryShard;
    }

    // By default, $lookup is allowed in a transaction and does not use disk.
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 hostRequirement,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kAllowed,
                                 TransactionRequirement::kAllowed,
                                 LookupRequirement::kAllowed,
                                 UnionRequirement::kAllowed);

    // However, if $lookup is specified with a pipeline, it inherits the strictest disk use, facet,
    // transaction, and lookup requirements from the children in its pipeline.
    if (hasPipeline()) {
        constraints = StageConstraints::getStrictestConstraints(
            _resolvedIntrospectionPipeline->getSources(), constraints);
    }

    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSkippingOrLimitingStage = !_unwindSrc;

    return constraints;
}

DocumentSource::GetNextResult DocumentSourceLookUp::doGetNext() {
    if (_unwindSrc) {
        return unwindResult();
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced()) {
        return nextInput;
    }

    auto inputDoc = nextInput.releaseDocument();

    // If we have not absorbed a $unwind, we cannot absorb a $match. If we have absorbed a $unwind,
    // '_unwindSrc' would be non-null, and we would not have made it here.
    invariant(!_matchSrc);

    if (hasLocalFieldForeignFieldJoin()) {
        auto matchStage =
            makeMatchStageFromInput(inputDoc, *_localField, _foreignField->fullPath(), BSONObj());
        // We've already allocated space for the trailing $match stage in '_resolvedPipeline'.
        _resolvedPipeline[*_fieldMatchPipelineIdx] = matchStage;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        pipeline = buildPipeline(inputDoc);
    } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>& ex) {
        // If lookup on a sharded collection is disallowed and the foreign collection is sharded,
        // throw a custom exception.
        if (auto staleInfo = ex.extraInfo<StaleConfigInfo>()) {
            uassert(51069,
                    "Cannot run $lookup with sharded foreign collection",
                    foreignShardedLookupAllowed() || !staleInfo->getVersionWanted() ||
                        staleInfo->getVersionWanted() == ChunkVersion::UNSHARDED());
        }
        throw;
    }

    std::vector<Value> results;
    long long objsize = 0;
    const auto maxBytes = internalLookupStageIntermediateDocumentMaxSizeBytes.load();

    while (auto result = pipeline->getNext()) {
        long long safeSum = 0;
        bool hasOverflowed = overflow::add(objsize, result->getApproximateSize(), &safeSum);
        uassert(4568,
                str::stream() << "Total size of documents in " << _fromNs.coll()
                              << " matching pipeline's $lookup stage exceeds " << maxBytes
                              << " bytes",

                !hasOverflowed && objsize <= maxBytes);
        objsize = safeSum;
        results.emplace_back(std::move(*result));
    }

    recordPlanSummaryStats(*pipeline);
    MutableDocument output(std::move(inputDoc));
    output.setNestedField(_as, Value(std::move(results)));
    return output.freeze();
}

std::unique_ptr<Pipeline, PipelineDeleter> DocumentSourceLookUp::buildPipeline(
    const Document& inputDoc) {
    // Copy all 'let' variables into the foreign pipeline's expression context.
    _variables.copyToExpCtx(_variablesParseState, _fromExpCtx.get());

    // Resolve the 'let' variables to values per the given input document.
    resolveLetVariables(inputDoc, &_fromExpCtx->variables);

    if (!foreignShardedLookupAllowed()) {
        // Enforce that the foreign collection must be unsharded for lookup.
        _fromExpCtx->mongoProcessInterface->setExpectedShardVersion(
            _fromExpCtx->opCtx, _fromExpCtx->ns, ChunkVersion::UNSHARDED());
    }

    // If we don't have a cache, build and return the pipeline immediately.
    if (!_cache || _cache->isAbandoned()) {
        MakePipelineOptions pipelineOpts;
        pipelineOpts.optimize = true;
        pipelineOpts.attachCursorSource = true;
        pipelineOpts.validator = lookupPipeValidator;
        // By default, $lookup doesnt support sharded 'from' collections.
        pipelineOpts.allowTargetingShards = internalQueryAllowShardedLookup.load();
        return Pipeline::makePipeline(_resolvedPipeline, _fromExpCtx, pipelineOpts);
    }

    // Construct the basic pipeline without a cache stage. Avoid optimizing here since we need to
    // add the cache first, as detailed below.
    MakePipelineOptions pipelineOpts;
    pipelineOpts.optimize = false;
    pipelineOpts.attachCursorSource = false;
    pipelineOpts.validator = lookupPipeValidator;
    auto pipeline = Pipeline::makePipeline(_resolvedPipeline, _fromExpCtx, pipelineOpts);

    // Add the cache stage at the end and optimize. During the optimization process, the cache will
    // either move itself to the correct position in the pipeline, or will abandon itself if no
    // suitable cache position exists. Do it only if pipeline optimization is enabled, otherwise
    // Pipeline::optimizePipeline() will exit early and correct placement of the cache will not
    // occur.
    if (auto fp = globalFailPointRegistry().find("disablePipelineOptimization");
        fp && fp->shouldFail()) {
        _cache->abandon();
    } else {
        pipeline->addFinalSource(
            DocumentSourceSequentialDocumentCache::create(_fromExpCtx, _cache.get_ptr()));
    }

    pipeline->optimizePipeline();

    if (!_cache->isServing()) {
        // The cache has either been abandoned or has not yet been built. Attach a cursor.
        pipeline = pExpCtx->mongoProcessInterface->attachCursorSourceToPipeline(
            pipeline.release(), internalQueryAllowShardedLookup.load() /* allowTargetingShards*/);
    }

    // If the cache has been abandoned, release it.
    if (_cache->isAbandoned()) {
        _cache.reset();
    }

    invariant(pipeline);
    return pipeline;
}

DocumentSource::GetModPathsReturn DocumentSourceLookUp::getModifiedPaths() const {
    std::set<std::string> modifiedPaths{_as.fullPath()};
    if (_unwindSrc) {
        auto pathsModifiedByUnwind = _unwindSrc->getModifiedPaths();
        invariant(pathsModifiedByUnwind.type == GetModPathsReturn::Type::kFiniteSet);
        modifiedPaths.insert(pathsModifiedByUnwind.paths.begin(),
                             pathsModifiedByUnwind.paths.end());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
}

Pipeline::SourceContainer::iterator DocumentSourceLookUp::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // If the following stage is $sort, consider pushing it ahead of $lookup.
    if (auto sortPtr = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get())) {
        // TODO (SERVER-55417): Conditionally reorder $sort and $lookup depending on whether the
        // query planner allows for an index-provided sort.
        if (!_unwindSrc &&
            checkModifiedPathsSortReorder(sortPtr->getSortKeyPattern(), getModifiedPaths())) {
            // We have a sort not on as field following this stage. Reorder sort and current doc.
            std::swap(*itr, *std::next(itr));

            return itr == container->begin() ? itr : std::prev(itr);
        }
    }

    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());

    // If we are not already handling an $unwind stage internally, we can combine with the
    // following $unwind stage.
    if (nextUnwind && !_unwindSrc && nextUnwind->getUnwindPath() == _as.fullPath()) {
        _unwindSrc = std::move(nextUnwind);
        container->erase(std::next(itr));
        return itr;
    }

    // Attempt to internalize any predicates of a $match upon the "_as" field.
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (!nextMatch) {
        return std::next(itr);
    }

    if (!_unwindSrc || _unwindSrc->indexPath() || _unwindSrc->preserveNullAndEmptyArrays()) {
        // We must be unwinding our result to internalize a $match. For example, consider the
        // following pipeline:
        //
        // Input: {_id: 0}
        // Foreign Collection: {a: 0, b: 0}, {a: 0, b: 5}
        // Pipeline:
        //   {$lookup: {localField: "_id", foreignField: "a", as: "foo"}}
        //   {$match: {'foo.b': {$gt: 0}}}
        // Output: {_id: 0, foo: [{a: 0, b: 0}, {a: 0, b: 5}]}
        //
        // If we executed {b: {$gt: 0}} as part of our $lookup, our output would instead be:
        // {_id: 0, foo: [{a: 0, b: 5}]}
        //
        // However, if we are already unwinding 'foo', then we can move the $match inside, since it
        // will have the same effect as filtering the unwound results, that is, the output will be:
        // {_id: 0, foo: {a: 0, b: 5}}
        //
        // Note that we cannot absorb a $match if the absorbed $unwind has
        // "preserveNullAndEmptyArrays" set to true, for the following reason: A document that had
        // an empty output array from $lookup would be preserved by the $unwind, but could be
        // removed by the $match. However, if we absorb the $match into the $lookup, our joined
        // query inside the $lookup will output an empty array, which $unwind will then preserve.
        // Thus, depending on the optimization, the user would see a different output.
        //
        // In addition, we must avoid internalizing a $match if an absorbed $unwind has an
        // "includeArrayIndex" option, since the $match will alter the indices of the returned
        // values.
        return std::next(itr);
    }

    // We cannot internalize a $match if a collation has been set on the $lookup stage and it
    // differs from that of the parent pipeline.
    if (_fromExpCtx->getCollator() &&
        !CollatorInterface::collatorsMatch(pExpCtx->getCollator(), _fromExpCtx->getCollator())) {
        return std::next(itr);
    }

    auto outputPath = _as.fullPath();

    // Since $match splitting is handled in a generic way, we expect to have already swapped
    // portions of the $match that do not depend on the 'as' path or on an internalized $unwind's
    // index path before ourselves. But due to the early return above, we know there is no
    // internalized $unwind with an index path.
    //
    // Therefore, 'nextMatch' should only depend on the 'as' path. We now try to absorb the match on
    // the 'as' path in order to push down these predicates into the foreign collection.
    bool isMatchOnlyOnAs = true;
    auto computeWhetherMatchOnAs = [&isMatchOnlyOnAs, &outputPath](MatchExpression* expression,
                                                                   std::string path) -> void {
        // If 'expression' is the child of a $elemMatch, we cannot internalize the $match. For
        // example, {b: {$elemMatch: {$gt: 1, $lt: 4}}}, where "b" is our "_as" field. This is
        // because there's no way to modify the expression to be a match just on 'b'--we cannot
        // change the path to an empty string, or remove the node entirely.
        if (expression->matchType() == MatchExpression::ELEM_MATCH_VALUE ||
            expression->matchType() == MatchExpression::ELEM_MATCH_OBJECT) {
            isMatchOnlyOnAs = false;
        }
        if (expression->numChildren() == 0) {
            // 'expression' is a leaf node; examine the path. It is important that 'outputPath'
            // not equal 'path', because we cannot change the expression {b: {$eq: 3}}, where
            // 'path' is 'b', to be a match on a subfield, since no subfield exists.
            isMatchOnlyOnAs = isMatchOnlyOnAs && expression::isPathPrefixOf(outputPath, path);
        }
    };

    expression::mapOver(nextMatch->getMatchExpression(), computeWhetherMatchOnAs);

    if (!isMatchOnlyOnAs) {
        // "nextMatch" does not contain any predicates that can be absorbed into this stage.
        return std::next(itr);
    }

    // We can internalize the $match.
    if (!_matchSrc) {
        _matchSrc = nextMatch;
    } else {
        // We have already absorbed a $match. We need to join it with 'dependent'.
        _matchSrc->joinMatchWith(nextMatch);
    }

    // Remove the original $match.
    container->erase(std::next(itr));

    // We have internalized a $match, but have not yet computed the descended $match that should
    // be applied to our queries.
    _additionalFilter = DocumentSourceMatch::descendMatchOnPath(
                            _matchSrc->getMatchExpression(), _as.fullPath(), pExpCtx)
                            ->getQuery()
                            .getOwned();

    // Add '_additionalFilter' to '_resolvedPipeline' if there is a pipeline. If there is no
    // pipeline, '_additionalFilter' can safely be added to the local/foreignField $match stage
    // during 'doGetNext()'.
    if (hasPipeline()) {
        auto matchObj = BSON("$match" << *_additionalFilter);
        _resolvedPipeline.push_back(matchObj);
    }

    // There may be further optimization between this $lookup and the new neighbor, so we return an
    // iterator pointing to ourself.
    return itr;
}

bool DocumentSourceLookUp::usedDisk() {
    if (_pipeline)
        _stats.planSummaryStats.usedDisk =
            _stats.planSummaryStats.usedDisk || _pipeline->usedDisk();

    return _stats.planSummaryStats.usedDisk;
}

void DocumentSourceLookUp::doDispose() {
    if (_pipeline) {
        recordPlanSummaryStats(*_pipeline);
        _pipeline->dispose(pExpCtx->opCtx);
        _pipeline.reset();
    }
}

BSONObj DocumentSourceLookUp::makeMatchStageFromInput(const Document& input,
                                                      const FieldPath& localFieldPath,
                                                      const std::string& foreignFieldName,
                                                      const BSONObj& additionalFilter) {
    // Add the 'localFieldPath' of 'input' into 'localFieldList'. If 'localFieldPath' references a
    // field with an array in its path, we may need to join on multiple values, so we add each
    // element to 'localFieldList'.
    BSONArrayBuilder arrBuilder;
    bool containsRegex = false;
    document_path_support::visitAllValuesAtPath(input, localFieldPath, [&](const Value& nextValue) {
        arrBuilder << nextValue;
        if (!containsRegex && nextValue.getType() == BSONType::RegEx) {
            containsRegex = true;
        }
    });

    if (arrBuilder.arrSize() == 0) {
        // Missing values are treated as null.
        arrBuilder << BSONNULL;
    }

    const auto localFieldListSize = arrBuilder.arrSize();
    const auto localFieldList = arrBuilder.arr();

    // We construct a query of one of the following forms, depending on the contents of
    // 'localFieldList'.
    //
    //   {$and: [{<foreignFieldName>: {$eq: <localFieldList[0]>}}, <additionalFilter>]}
    //     if 'localFieldList' contains a single element.
    //
    //   {$and: [{<foreignFieldName>: {$in: [<value>, <value>, ...]}}, <additionalFilter>]}
    //     if 'localFieldList' contains more than one element but doesn't contain any that are
    //     regular expressions.
    //
    //   {$and: [{$or: [{<foreignFieldName>: {$eq: <value>}},
    //                  {<foreignFieldName>: {$eq: <value>}}, ...]},
    //           <additionalFilter>]}
    //     if 'localFieldList' contains more than one element and it contains at least one element
    //     that is a regular expression.

    // We wrap the query in a $match so that it can be parsed into a DocumentSourceMatch when
    // constructing a pipeline to execute.
    BSONObjBuilder match;
    BSONObjBuilder query(match.subobjStart("$match"));

    BSONArrayBuilder andObj(query.subarrayStart("$and"));
    BSONObjBuilder joiningObj(andObj.subobjStart());

    if (localFieldListSize > 1) {
        // A $lookup on an array value corresponds to finding documents in the foreign collection
        // that have a value of any of the elements in the array value, rather than finding
        // documents that have a value equal to the entire array value. These semantics are
        // automatically provided to us by using the $in query operator.
        if (containsRegex) {
            // A regular expression inside the $in query operator will perform pattern matching on
            // any string values. Since we want regular expressions to only match other RegEx types,
            // we write the query as a $or of equality comparisons instead.
            BSONObj orQuery = buildEqualityOrQuery(foreignFieldName, localFieldList);
            joiningObj.appendElements(orQuery);
        } else {
            // { <foreignFieldName> : { "$in" : <localFieldList> } }
            BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
            subObj << "$in" << localFieldList;
            subObj.doneFast();
        }
    } else {
        // { <foreignFieldName> : { "$eq" : <localFieldList[0]> } }
        BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
        subObj << "$eq" << localFieldList[0];
        subObj.doneFast();
    }

    joiningObj.doneFast();

    BSONObjBuilder additionalFilterObj(andObj.subobjStart());
    additionalFilterObj.appendElements(additionalFilter);
    additionalFilterObj.doneFast();

    andObj.doneFast();

    query.doneFast();
    return match.obj();
}

DocumentSource::GetNextResult DocumentSourceLookUp::unwindResult() {
    const boost::optional<FieldPath> indexPath(_unwindSrc->indexPath());

    // Loop until we get a document that has at least one match.
    // Note we may return early from this loop if our source stage is exhausted or if the unwind
    // source was asked to return empty arrays and we get a document without a match.
    while (!_pipeline || !_nextValue) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        _input = nextInput.releaseDocument();

        if (hasLocalFieldForeignFieldJoin()) {
            // At this point, if there is a pipeline, '_additionalFilter' was added to the end of
            // '_resolvedPipeline' in doOptimizeAt(). If there is no pipeline, we must add it to the
            // $match stage created here.
            BSONObj filter = hasPipeline() ? BSONObj() : _additionalFilter.value_or(BSONObj());
            auto matchStage =
                makeMatchStageFromInput(*_input, *_localField, _foreignField->fullPath(), filter);
            // We've already allocated space for the trailing $match stage in '_resolvedPipeline'.
            _resolvedPipeline[*_fieldMatchPipelineIdx] = matchStage;
        }

        if (_pipeline) {
            recordPlanSummaryStats(*_pipeline);
            _pipeline->dispose(pExpCtx->opCtx);
        }

        _pipeline = buildPipeline(*_input);

        // The $lookup stage takes responsibility for disposing of its Pipeline, since it will
        // potentially be used by multiple OperationContexts, and the $lookup stage is part of an
        // outer Pipeline that will propagate dispose() calls before being destroyed.
        _pipeline.get_deleter().dismissDisposal();

        _cursorIndex = 0;
        _nextValue = _pipeline->getNext();

        if (_unwindSrc->preserveNullAndEmptyArrays() && !_nextValue) {
            // There were no results for this cursor, but the $unwind was asked to preserve empty
            // arrays, so we should return a document without the array.
            MutableDocument output(std::move(*_input));
            // Note this will correctly create objects in the prefix of '_as', to act as if we had
            // created an empty array and then removed it.
            output.setNestedField(_as, Value());
            if (indexPath) {
                output.setNestedField(*indexPath, Value(BSONNULL));
            }
            return output.freeze();
        }
    }

    invariant(bool(_input) && bool(_nextValue));
    auto currentValue = *_nextValue;
    _nextValue = _pipeline->getNext();

    // Move input document into output if this is the last or only result, otherwise perform a copy.
    MutableDocument output(_nextValue ? *_input : std::move(*_input));
    output.setNestedField(_as, Value(currentValue));

    if (indexPath) {
        output.setNestedField(*indexPath, Value(_cursorIndex));
    }

    ++_cursorIndex;
    return output.freeze();
}

void DocumentSourceLookUp::resolveLetVariables(const Document& localDoc, Variables* variables) {
    invariant(variables);

    for (auto& letVar : _letVariables) {
        auto value = letVar.expression->evaluate(localDoc, &pExpCtx->variables);
        variables->setConstantValue(letVar.id, value);
    }
}

void DocumentSourceLookUp::initializeResolvedIntrospectionPipeline() {
    _variables.copyToExpCtx(_variablesParseState, _fromExpCtx.get());
    _resolvedIntrospectionPipeline =
        Pipeline::parse(_resolvedPipeline, _fromExpCtx, lookupPipeValidator);
}

void DocumentSourceLookUp::recordPlanSummaryStats(const Pipeline& pipeline) {
    for (auto&& source : pipeline.getSources()) {
        if (auto specificStats = source->getSpecificStats()) {
            specificStats->accumulate(_stats.planSummaryStats);
        }
    }
}

void DocumentSourceLookUp::appendSpecificExecStats(MutableDocument& doc) const {
    const PlanSummaryStats& stats = _stats.planSummaryStats;
    doc["totalDocsExamined"] = Value(static_cast<long long>(stats.totalDocsExamined));
    doc["totalKeysExamined"] = Value(static_cast<long long>(stats.totalKeysExamined));
    doc["collectionScans"] = Value(stats.collectionScans);
    std::vector<Value> indexesUsedVec;
    std::transform(stats.indexesUsed.begin(),
                   stats.indexesUsed.end(),
                   std::back_inserter(indexesUsedVec),
                   [](std::string idx) -> Value { return Value(idx); });
    doc["indexesUsed"] = Value{std::move(indexesUsedVec)};
}

void DocumentSourceLookUp::serializeToArrayWithBothSyntaxes(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {

    // Support alternative $lookup from config.cache.chunks* namespaces.
    auto fromValue = (pExpCtx->ns.db() == _fromNs.db())
        ? Value(_fromNs.coll())
        : Value(Document{{"db", _fromNs.db()}, {"coll", _fromNs.coll()}});

    MutableDocument output(
        Document{{getSourceName(), Document{{"from", fromValue}, {"as", _as.fullPath()}}}});

    if (hasLocalFieldForeignFieldJoin()) {
        output[getSourceName()]["localField"] = Value(_localField->fullPath());
        output[getSourceName()]["foreignField"] = Value(_foreignField->fullPath());
    }

    // Add a pipeline field if only-pipeline syntax was used (to ensure the output is valid $lookup
    // syntax) or if a $match was absorbed.
    auto pipeline = _userPipeline;
    if (_additionalFilter) {
        pipeline.emplace_back(BSON("$match" << *_additionalFilter));
    }
    if (!hasLocalFieldForeignFieldJoin() || pipeline.size() > 0) {
        MutableDocument exprList;
        for (auto letVar : _letVariables) {
            exprList.addField(letVar.name,
                              letVar.expression->serialize(static_cast<bool>(explain)));
        }
        output[getSourceName()]["let"] = Value(exprList.freeze());

        output[getSourceName()]["pipeline"] = Value(pipeline);
    }

    if (_hasExplicitCollation) {
        output[getSourceName()]["_internalCollation"] = Value(_fromExpCtx->getCollatorBSON());
    }

    if (explain) {
        if (_unwindSrc) {
            const boost::optional<FieldPath> indexPath = _unwindSrc->indexPath();
            output[getSourceName()]["unwinding"] =
                Value(DOC("preserveNullAndEmptyArrays"
                          << _unwindSrc->preserveNullAndEmptyArrays() << "includeArrayIndex"
                          << (indexPath ? Value(indexPath->fullPath()) : Value())));
        }

        if (explain.get() >= ExplainOptions::Verbosity::kExecStats) {
            appendSpecificExecStats(output);
        }

        array.push_back(output.freezeToValue());
    } else {
        array.push_back(output.freezeToValue());

        if (_unwindSrc) {
            _unwindSrc->serializeToArray(array);
        }
    }
}

void DocumentSourceLookUp::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    if (serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
            FeatureCompatibilityParams::Version::kVersion49)) {
        return serializeToArrayWithBothSyntaxes(array, explain);
    }

    Document doc;

    // Support alternative $lookup from config.cache.chunks* namespaces.
    auto fromValue = (pExpCtx->ns.db() == _fromNs.db())
        ? Value(_fromNs.coll())
        : Value(Document{{"db", _fromNs.db()}, {"coll", _fromNs.coll()}});

    if (!hasLocalFieldForeignFieldJoin()) {
        MutableDocument exprList;
        for (auto letVar : _letVariables) {
            exprList.addField(letVar.name,
                              letVar.expression->serialize(static_cast<bool>(explain)));
        }

        auto pipeline = _userPipeline;
        // With pipeline syntax, any '_additionalFilter' should be added to the user pipeline. With
        // only field syntax, we add '_additionalFilter' or '_matchSrc' to the output below.
        if (_additionalFilter) {
            pipeline.emplace_back(BSON("$match" << *_additionalFilter));
        }

        doc = Document{{getSourceName(),
                        Document{{"from", fromValue},
                                 {"as", _as.fullPath()},
                                 {"let", exprList.freeze()},
                                 {"pipeline", pipeline}}}};
    } else {
        doc = Document{{getSourceName(),
                        Document{{"from", fromValue},
                                 {"as", _as.fullPath()},
                                 {"localField", _localField->fullPath()},
                                 {"foreignField", _foreignField->fullPath()}}}};
    }

    MutableDocument output(std::move(doc));

    if (_hasExplicitCollation) {
        output[getSourceName()]["_internalCollation"] = Value(_fromExpCtx->getCollatorBSON());
    }

    if (explain) {
        if (_unwindSrc) {
            const boost::optional<FieldPath> indexPath = _unwindSrc->indexPath();
            output[getSourceName()]["unwinding"] =
                Value(DOC("preserveNullAndEmptyArrays"
                          << _unwindSrc->preserveNullAndEmptyArrays() << "includeArrayIndex"
                          << (indexPath ? Value(indexPath->fullPath()) : Value())));
        }

        // Add '_additionalFilter' for explain when $lookup was constructed without pipeline syntax.
        if (hasLocalFieldForeignFieldJoin() && _additionalFilter) {
            // Our output does not have to be parseable, so include a "matching" field with the
            // descended match expression.
            output[getSourceName()]["matching"] = Value(*_additionalFilter);
        }

        array.push_back(Value(output.freeze()));
    } else {
        array.push_back(Value(output.freeze()));

        if (_unwindSrc) {
            _unwindSrc->serializeToArray(array);
        }

        if (hasLocalFieldForeignFieldJoin() && _matchSrc) {
            // '_matchSrc' tracks the originally specified $match, before it is descended (modified
            // so it can be moved into '_resolvedPipeline'). It is set in the first call to
            // getNext(), at which point we are confident that we no longer need to serialize the
            // $lookup again.
            _matchSrc->serializeToArray(array);
        }
    }
}

DepsTracker::State DocumentSourceLookUp::getDependencies(DepsTracker* deps) const {
    if (hasPipeline() || _letVariables.size() > 0) {
        // We will use the introspection pipeline which we prebuilt during construction.
        invariant(_resolvedIntrospectionPipeline);

        // We are not attempting to enforce that any referenced metadata are in fact unavailable,
        // this is done elsewhere. We only need to know what variable dependencies exist in the
        // subpipeline for the top-level pipeline. So without knowledge of what metadata is in fact
        // unavailable, we "lie" and say that all metadata is available to avoid tripping any
        // assertions.
        DepsTracker subDeps(DepsTracker::kNoMetadata);

        // Get the subpipeline dependencies. Subpipeline stages may reference both 'let' variables
        // declared by this $lookup and variables declared externally.
        for (auto&& source : _resolvedIntrospectionPipeline->getSources()) {
            source->getDependencies(&subDeps);
        }

        // Add the 'let' dependencies to the tracker. Because the caller is only interested in
        // references to external variables, filter out any subpipeline references to 'let'
        // variables declared by this $lookup.
        for (auto&& letVar : _letVariables) {
            letVar.expression->addDependencies(deps);
            subDeps.vars.erase(letVar.id);
        }

        // Add sub-pipeline variable dependencies. Do not add field dependencies, since these refer
        // to the fields from the foreign collection rather than the local collection.
        deps->vars.insert(subDeps.vars.begin(), subDeps.vars.end());
    }

    if (hasLocalFieldForeignFieldJoin()) {
        deps->fields.insert(_localField->fullPath());
    }

    return DepsTracker::State::SEE_NEXT;
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceLookUp::distributedPlanLogic() {
    if (_fromExpCtx->ns.isConfigDotCacheDotChunks()) {
        // When $lookup reads from config.cache.chunks.* namespaces, it should run on each
        // individual shard in parallel. This is a special case, and atypical for standard $lookup
        // since a full copy of config.cache.chunks.* collections exists on all shards.
        return boost::none;
    }

    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{nullptr, this, boost::none};
}

void DocumentSourceLookUp::detachFromOperationContext() {
    if (_pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::detachFromOperationContext() to take care of updating '_fromExpCtx->opCtx'.
        _pipeline->detachFromOperationContext();
        invariant(_fromExpCtx->opCtx == nullptr);
    } else if (_fromExpCtx) {
        _fromExpCtx->opCtx = nullptr;
    }
}

void DocumentSourceLookUp::reattachToOperationContext(OperationContext* opCtx) {
    if (_pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::reattachToOperationContext() to take care of updating '_fromExpCtx->opCtx'.
        _pipeline->reattachToOperationContext(opCtx);
        invariant(_fromExpCtx->opCtx == opCtx);
    } else if (_fromExpCtx) {
        _fromExpCtx->opCtx = opCtx;
    }
}

intrusive_ptr<DocumentSource> DocumentSourceLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            "the $lookup specification must be an Object",
            elem.type() == BSONType::Object);

    NamespaceString fromNs;
    std::string as;

    std::string localField;
    std::string foreignField;

    BSONObj letVariables;
    std::vector<BSONObj> pipeline;
    bool hasPipeline = false;
    bool hasLet = false;
    boost::optional<std::unique_ptr<CollatorInterface>> fromCollator;

    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();

        if (argName == "pipeline"_sd) {
            pipeline = parsePipelineFromBSON(argument);
            hasPipeline = true;
            continue;
        }

        if (argName == "let"_sd) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "$lookup argument '" << argument
                                  << "' must be an object, is type " << argument.type(),
                    argument.type() == BSONType::Object);
            letVariables = argument.Obj();
            hasLet = true;
            continue;
        }

        if (argName == "from"_sd) {
            fromNs = parseLookupFromAndResolveNamespace(argument, pExpCtx->ns.db());
            continue;
        }

        if (argName == "_internalCollation"_sd) {
            const auto& collationSpec = argument.Obj();
            if (!collationSpec.isEmpty()) {
                fromCollator.emplace(uassertStatusOK(
                    CollatorFactoryInterface::get(pExpCtx->opCtx->getServiceContext())
                        ->makeFromBSON(collationSpec)));
            }
            continue;
        }

        uassert(ErrorCodes::FailedToParse,
                str::stream() << "$lookup argument '" << argName << "' must be a string, found "
                              << argument << ": " << argument.type(),
                argument.type() == BSONType::String);

        if (argName == "as"_sd) {
            as = argument.String();
        } else if (argName == "localField"_sd) {
            localField = argument.String();
        } else if (argName == "foreignField"_sd) {
            foreignField = argument.String();
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "unknown argument to $lookup: " << argument.fieldName());
        }
    }

    uassert(
        ErrorCodes::FailedToParse, "must specify 'from' field for a $lookup", !fromNs.ns().empty());
    uassert(ErrorCodes::FailedToParse, "must specify 'as' field for a $lookup", !as.empty());

    if (hasPipeline) {
        uassert(ErrorCodes::FailedToParse,
                "$lookup with 'pipeline' may not specify 'localField' or 'foreignField'",
                (localField.empty() && foreignField.empty()) ||
                    serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
                        FeatureCompatibilityParams::Version::kVersion49));

        if (localField.empty() && foreignField.empty()) {
            // $lookup specified with only pipeline syntax.
            return new DocumentSourceLookUp(std::move(fromNs),
                                            std::move(as),
                                            std::move(pipeline),
                                            std::move(letVariables),
                                            std::move(fromCollator),
                                            boost::none,
                                            pExpCtx);
        } else {
            // $lookup specified with pipeline syntax and local/foreignField syntax.
            uassert(ErrorCodes::FailedToParse,
                    "$lookup requires both or neither of 'localField' and 'foreignField' to be "
                    "specified",
                    !localField.empty() && !foreignField.empty());

            return new DocumentSourceLookUp(
                std::move(fromNs),
                std::move(as),
                std::move(pipeline),
                std::move(letVariables),
                std::move(fromCollator),
                std::pair(std::move(localField), std::move(foreignField)),
                pExpCtx);
        }
    } else {
        // $lookup specified with only local/foreignField syntax.
        uassert(ErrorCodes::FailedToParse,
                "$lookup requires either 'pipeline' or both 'localField' and 'foreignField' to be "
                "specified",
                !localField.empty() && !foreignField.empty());
        uassert(ErrorCodes::FailedToParse,
                "$lookup with a 'let' argument must also specify 'pipeline'",
                !hasLet);

        return new DocumentSourceLookUp(std::move(fromNs),
                                        std::move(as),
                                        std::move(localField),
                                        std::move(foreignField),
                                        std::move(fromCollator),
                                        pExpCtx);
    }
}

void DocumentSourceLookUp::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_resolvedNs);
    for (auto&& stage : _resolvedIntrospectionPipeline->getSources()) {
        stage->addInvolvedCollections(collectionNames);
    }
}

}  // namespace mongo
