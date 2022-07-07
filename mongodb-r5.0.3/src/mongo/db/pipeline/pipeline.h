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

#pragma once

#include <functional>
#include <list>
#include <vector>

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/async_results_merger_params_gen.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/timer.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class CollatorInterface;
class DocumentSource;
class ExpressionContext;
class OperationContext;
class Pipeline;
class PipelineDeleter;

/**
 * Enabling the disablePipelineOptimization fail point will stop the aggregate command from
 * attempting to optimize the pipeline or the pipeline stages. Neither DocumentSource::optimizeAt()
 * nor DocumentSource::optimize() will be attempted.
 */
extern FailPoint disablePipelineOptimization;

using PipelineValidatorCallback = std::function<void(const Pipeline&)>;

struct MakePipelineOptions {
    bool optimize = true;
    bool attachCursorSource = true;
    bool allowTargetingShards = true;
    PipelineValidatorCallback validator = nullptr;
};

/**
 * A Pipeline object represents a list of DocumentSources and is responsible for optimizing the
 * pipeline.
 */
class Pipeline {
public:
    typedef std::list<boost::intrusive_ptr<DocumentSource>> SourceContainer;

    /**
     * A SplitState specifies whether the pipeline is currently unsplit, split for the shards, or
     * split for merging.
     */
    enum class SplitState { kUnsplit, kSplitForShards, kSplitForMerge };

    /**
     * The list of default supported match expression features.
     */
    static constexpr MatchExpressionParser::AllowedFeatureSet kAllowedMatcherFeatures =
        MatchExpressionParser::AllowedFeatures::kText |
        MatchExpressionParser::AllowedFeatures::kExpr |
        MatchExpressionParser::AllowedFeatures::kJSONSchema |
        MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

    /**
     * The match expression features allowed when running a pipeline with $geoNear.
     */
    static constexpr MatchExpressionParser::AllowedFeatureSet kGeoNearMatcherFeatures =
        MatchExpressionParser::AllowedFeatures::kText |
        MatchExpressionParser::AllowedFeatures::kExpr |
        MatchExpressionParser::AllowedFeatures::kJSONSchema |
        MatchExpressionParser::AllowedFeatures::kEncryptKeywords |
        MatchExpressionParser::AllowedFeatures::kGeoNear;

    /**
     * Parses a Pipeline from a vector of BSONObjs then invokes the optional 'validator' callback
     * with a reference to the newly created Pipeline. If no validator callback is given, this
     * method assumes that we're parsing a top-level pipeline. Throws an exception if it failed to
     * parse or if any exception occurs in the validator. The returned pipeline is not optimized,
     * but the caller may convert it to an optimized pipeline by calling optimizePipeline().
     *
     * It is illegal to create a pipeline using an ExpressionContext which contains a collation that
     * will not be used during execution of the pipeline. Doing so may cause comparisons made during
     * parse-time to return the wrong results.
     */
    static std::unique_ptr<Pipeline, PipelineDeleter> parse(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        PipelineValidatorCallback validator = nullptr);

    /**
     * Creates a Pipeline from an existing SourceContainer.
     *
     * Returns a non-OK status if any stage is in an invalid position. For example, if an $out stage
     * is present but is not the last stage.
     */
    static std::unique_ptr<Pipeline, PipelineDeleter> create(
        SourceContainer sources, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Returns true if the provided aggregation command has an $out or $merge stage.
     */
    static bool aggHasWriteStage(const BSONObj& cmd);

    /**
     * Parses a Pipeline from a vector of BSONObjs representing DocumentSources. The state of the
     * returned pipeline will depend upon the supplied MakePipelineOptions:
     * - The boolean opts.optimize determines whether the pipeline will be optimized.
     * - If opts.attachCursorSource is false, the pipeline will be returned without attempting to
     * add an initial cursor source.
     *
     * This function throws if parsing the pipeline failed.
     */
    static std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts = MakePipelineOptions{});

    std::unique_ptr<Pipeline, PipelineDeleter> clone() const;

    const boost::intrusive_ptr<ExpressionContext>& getContext() const {
        return pCtx;
    }

    /**
     * Sets the OperationContext of 'pCtx' to nullptr and calls 'detachFromOperationContext()' on
     * all underlying DocumentSources.
     */
    void detachFromOperationContext();

    /**
     * Sets the OperationContext of 'pCtx' to 'opCtx', and reattaches all underlying DocumentSources
     * to 'opCtx'.
     */
    void reattachToOperationContext(OperationContext* opCtx);

    /**
     * Releases any resources held by this pipeline such as PlanExecutors or in-memory structures.
     * Must be called before deleting a Pipeline.
     *
     * There are multiple cleanup scenarios:
     *  - This Pipeline will only ever use one OperationContext. In this case the PipelineDeleter
     *    will automatically call dispose() before deleting the Pipeline, and the owner need not
     *    call dispose().
     *  - This Pipeline may use multiple OperationContexts over its lifetime. In this case it
     *    is the owner's responsibility to call dispose() with a valid OperationContext before
     *    deleting the Pipeline.
     */
    void dispose(OperationContext* opCtx);

    bool isDisposed() const {
        return _disposed;
    }

    /**
     * Checks to see if disk is ever used within the pipeline.
     */
    bool usedDisk();

    /**
     * Communicates to the pipeline which part of a split pipeline it is when the pipeline has been
     * split in two.
     */
    void setSplitState(SplitState state) {
        _splitState = state;
    }

    /**
     * If the pipeline starts with a stage which is or includes a query predicate (e.g. a $match),
     * returns a BSON object representing that query. Otherwise, returns an empty BSON object.
     */
    BSONObj getInitialQuery() const;

    /**
     * Returns 'true' if the pipeline must merge on the primary shard.
     */
    bool needsPrimaryShardMerger() const;

    /**
     * Returns 'true' if the pipeline must merge on mongoS.
     */
    bool needsMongosMerger() const;

    /**
     * Returns 'true' if any stage in the pipeline must run on a shard.
     */
    bool needsShard() const;

    /**
     * Returns true if the pipeline can run on mongoS, but is not obliged to; that is, it can run
     * either on mongoS or on a shard.
     */
    bool canRunOnMongos() const;

    /**
     * Returns true if this pipeline must only run on mongoS. Can be called on unsplit or merge
     * pipelines, but not on the shards part of a split pipeline.
     */
    bool requiredToRunOnMongos() const;

    /**
     * Modifies the pipeline, optimizing it by combining and swapping stages.
     */
    void optimizePipeline();

    /**
     * Modifies the container, optimizing it by combining and swapping stages.
     */
    static void optimizeContainer(SourceContainer* container);

    /**
     * Returns any other collections involved in the pipeline in addition to the collection the
     * aggregation is run on. All namespaces returned are the names of collections, after views have
     * been resolved.
     */
    stdx::unordered_set<NamespaceString> getInvolvedCollections() const;

    /**
     * Serializes the pipeline into a form that can be parsed into an equivalent pipeline.
     */
    std::vector<Value> serialize() const;
    std::vector<BSONObj> serializeToBson() const;

    // The initial source is special since it varies between mongos and mongod.
    void addInitialSource(boost::intrusive_ptr<DocumentSource> source);

    void addFinalSource(boost::intrusive_ptr<DocumentSource> source);

    /**
     * Returns the next result from the pipeline, or boost::none if there are no more results.
     */
    boost::optional<Document> getNext();

    /**
     * Write the pipeline's operators to a std::vector<Value>, providing the level of detail
     * specified by 'verbosity'.
     */
    std::vector<Value> writeExplainOps(ExplainOptions::Verbosity verbosity) const;

    /**
     * Returns the dependencies needed by this pipeline. 'unavailableMetadata' should reflect what
     * metadata is not present on documents that are input to the front of the pipeline. If
     * 'unavailableMetadata' is specified, this method will throw if any of the dependencies
     * reference unavailable metadata.
     */
    DepsTracker getDependencies(boost::optional<QueryMetadataBitSet> unavailableMetadata) const;

    /**
     * Returns the dependencies needed by the SourceContainer. 'unavailableMetadata' should reflect
     * what metadata is not present on documents that are input to the front of the pipeline. If
     * 'unavailableMetadata' is specified, this method will throw if any of the dependencies
     * reference unavailable metadata.
     */
    static DepsTracker getDependenciesForContainer(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const SourceContainer& container,
        boost::optional<QueryMetadataBitSet> unavailableMetadata);

    const SourceContainer& getSources() const {
        return _sources;
    }

    SourceContainer& getSources() {
        return _sources;
    }

    /**
     * Removes and returns the first stage of the pipeline. Returns nullptr if the pipeline is
     * empty.
     */
    boost::intrusive_ptr<DocumentSource> popFront();

    /**
     * Returns a pointer to the first stage of the pipeline, or a nullptr if the pipeline is empty.
     */
    DocumentSource* peekFront() const;

    /**
     * Removes and returns the last stage of the pipeline. Returns nullptr if the pipeline is empty.
     */
    boost::intrusive_ptr<DocumentSource> popBack();

    /**
     * Adds the given stage to the end of the pipeline.
     */
    void pushBack(boost::intrusive_ptr<DocumentSource>);

    /**
     * Removes and returns the first stage of the pipeline if its name is 'targetStageName'.
     * Returns nullptr if there is no first stage with that name.
     */
    boost::intrusive_ptr<DocumentSource> popFrontWithName(StringData targetStageName);

    /**
     * Removes and returns the first stage of the pipeline if its name is 'targetStageName' and the
     * given 'predicate' function, if present, returns 'true' when called with a pointer to the
     * stage. Returns nullptr if there is no first stage which meets these criteria.
     */
    boost::intrusive_ptr<DocumentSource> popFrontWithNameAndCriteria(
        StringData targetStageName, std::function<bool(const DocumentSource* const)> predicate);

    /**
     * PipelineD is a "sister" class that has additional functionality for the Pipeline. It exists
     * because of linkage requirements. Pipeline needs to function in mongod and mongos. PipelineD
     * contains extra functionality required in mongod, and which can't appear in mongos because the
     * required symbols are unavailable for linking there. Consider PipelineD to be an extension of
     * this class for mongod only.
     */
    friend class PipelineD;

private:
    friend class PipelineDeleter;

    Pipeline(const boost::intrusive_ptr<ExpressionContext>& pCtx);
    Pipeline(SourceContainer stages, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    ~Pipeline();

    /**
     * Stitch together the source pointers by calling setSource() for each source in '_sources'.
     * This function must be called any time the order of stages within the pipeline changes, e.g.
     * in optimizePipeline().
     */
    void stitch();

    /**
     * Stitch together the source pointers by calling setSource() for each source in 'container'.
     * This function must be called any time the order of stages within the container changes, e.g.
     * in optimizeContainer().
     */
    static void stitch(SourceContainer* container);

    /**
     * Performs common validation for top-level or facet pipelines. Throws if the pipeline is
     * invalid.
     *
     * Includes checking for illegal stage positioning. For example, $out must be at the end, while
     * a $match stage with a text query must be at the start. Note that this method accepts an
     * initial source as the first stage, which is illegal for $facet pipelines.
     */
    void validateCommon() const;

    /**
     * Returns Status::OK if the pipeline can run on mongoS, or an error with a message explaining
     * why it cannot.
     */
    Status _pipelineCanRunOnMongoS() const;

    SourceContainer _sources;

    SplitState _splitState = SplitState::kUnsplit;
    boost::intrusive_ptr<ExpressionContext> pCtx;
    bool _disposed = false;
};

/**
 * This class will ensure a Pipeline is disposed before it is deleted.
 */
class PipelineDeleter {
public:
    /**
     * Constructs an empty deleter. Useful for creating a
     * unique_ptr<Pipeline, PipelineDeleter> without populating it.
     */
    PipelineDeleter() {}

    explicit PipelineDeleter(OperationContext* opCtx) : _opCtx(opCtx) {}

    /**
     * If an owner of a std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> wants to assume
     * responsibility for calling PlanExecutor::dispose(), they can call dismissDisposal(). If
     * dismissed, a PipelineDeleter will not call dispose() when deleting the PlanExecutor.
     */
    void dismissDisposal() {
        _dismissed = true;
    }

    /**
     * Calls dispose() on 'pipeline', unless this PipelineDeleter has been dismissed.
     */
    void operator()(Pipeline* pipeline) {
        // It is illegal to call this method on a default-constructed PipelineDeleter.
        invariant(_opCtx);
        if (!_dismissed) {
            pipeline->dispose(_opCtx);
        }
        delete pipeline;
    }

private:
    OperationContext* _opCtx = nullptr;

    bool _dismissed = false;
};

}  // namespace mongo
