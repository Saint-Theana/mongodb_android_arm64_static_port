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

#include "mongo/db/query/projection_parser.h"
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/pipeline_d.h"

#include "mongo/base/exact_cast.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/multi_iterator.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/sample_from_timeseries_bucket.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/exec/unpack_timeseries_bucket.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/util/time_support.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using write_ops::InsertCommandRequest;

namespace {
/**
 * Returns a 'PlanExecutor' which uses a random cursor to sample documents if successful as
 * determined by the boolean. Returns {} if the storage engine doesn't support random cursors, or if
 * 'sampleSize' is a large enough percentage of the collection.
 */
StatusWith<std::pair<unique_ptr<PlanExecutor, PlanExecutor::Deleter>, bool>>
createRandomCursorExecutor(const CollectionPtr& coll,
                           const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           long long sampleSize,
                           long long numRecords,
                           boost::optional<BucketUnpacker> bucketUnpacker) {
    OperationContext* opCtx = expCtx->opCtx;

    // Verify that we are already under a collection lock. We avoid taking locks ourselves in this
    // function because double-locking forces any PlanExecutor we create to adopt a NO_YIELD policy.
    invariant(opCtx->lockState()->isCollectionLockedForMode(coll->ns(), MODE_IS));

    static const double kMaxSampleRatioForRandCursor = 0.05;
    if (!expCtx->ns.isTimeseriesBucketsCollection()) {
        if (sampleSize > numRecords * kMaxSampleRatioForRandCursor || numRecords <= 100) {
            return std::pair{nullptr, false};
        }
    } else {
        // Suppose that a time-series bucket collection is observed to contain 200 buckets, and the
        // 'gTimeseriesBucketMaxCount' parameter is set to 1000. If all buckets are full, then the
        // maximum possible measurment count would be 200 * 1000 = 200,000. While the
        // 'SampleFromTimeseriesBucket' plan is more efficient when the sample size is small
        // relative to the total number of measurements in the time-series collection, for larger
        // sample sizes the top-k sort based sample is faster. Experiments have approximated that
        // the tipping point is roughly when the requested sample size is greater than 1% of the
        // maximum possible number of measurements in the collection (i.e. numBuckets *
        // maxMeasurementsPerBucket).
        static const double kCoefficient = 0.01;
        if (sampleSize > kCoefficient * numRecords * gTimeseriesBucketMaxCount) {
            return std::pair{nullptr, false};
        }
    }

    // Attempt to get a random cursor from the RecordStore.
    auto rsRandCursor = coll->getRecordStore()->getRandomCursor(opCtx);
    if (!rsRandCursor) {
        // The storage engine has no random cursor support.
        return std::pair{nullptr, false};
    }

    // Build a MultiIteratorStage and pass it the random-sampling RecordCursor.
    auto ws = std::make_unique<WorkingSet>();
    std::unique_ptr<PlanStage> root =
        std::make_unique<MultiIteratorStage>(expCtx.get(), ws.get(), coll);
    static_cast<MultiIteratorStage*>(root.get())->addIterator(std::move(rsRandCursor));

    TrialStage* trialStage = nullptr;

    // Because 'numRecords' includes orphan documents, our initial decision to optimize the $sample
    // cursor may have been mistaken. For sharded collections, build a TRIAL plan that will switch
    // to a collection scan if the ratio of orphaned to owned documents encountered over the first
    // 100 works() is such that we would have chosen not to optimize.
    static const size_t kMaxPresampleSize = 100;
    if (auto css = CollectionShardingState::get(opCtx, coll->ns());
        css->getCollectionDescription(opCtx).isSharded() &&
        !expCtx->ns.isTimeseriesBucketsCollection()) {
        // The ratio of owned to orphaned documents must be at least equal to the ratio between the
        // requested sampleSize and the maximum permitted sampleSize for the original constraints to
        // be satisfied. For instance, if there are 200 documents and the sampleSize is 5, then at
        // least (5 / (200*0.05)) = (5/10) = 50% of those documents must be owned. If less than 5%
        // of the documents in the collection are owned, we default to the backup plan.
        const auto minAdvancedToWorkRatio = std::max(
            sampleSize / (numRecords * kMaxSampleRatioForRandCursor), kMaxSampleRatioForRandCursor);
        // Since the incoming operation is sharded, use the CSS to infer the filtering metadata for
        // the collection. We get the shard ownership filter after checking to see if the collection
        // is sharded to avoid an invariant from being fired in this call.
        auto collectionFilter = css->getOwnershipFilter(
            opCtx, CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup);
        // The trial plan is SHARDING_FILTER-MULTI_ITERATOR.
        auto randomCursorPlan = std::make_unique<ShardFilterStage>(
            expCtx.get(), collectionFilter, ws.get(), std::move(root));
        // The backup plan is SHARDING_FILTER-COLLSCAN.
        std::unique_ptr<PlanStage> collScanPlan = std::make_unique<CollectionScan>(
            expCtx.get(), coll, CollectionScanParams{}, ws.get(), nullptr);
        collScanPlan = std::make_unique<ShardFilterStage>(
            expCtx.get(), collectionFilter, ws.get(), std::move(collScanPlan));
        // Place a TRIAL stage at the root of the plan tree, and pass it the trial and backup plans.
        root = std::make_unique<TrialStage>(expCtx.get(),
                                            ws.get(),
                                            std::move(randomCursorPlan),
                                            std::move(collScanPlan),
                                            kMaxPresampleSize,
                                            minAdvancedToWorkRatio);
        trialStage = static_cast<TrialStage*>(root.get());
    } else if (expCtx->ns.isTimeseriesBucketsCollection()) {
        // We can't take ARHASH optimization path for a direct $sample on the system.buckets
        // collection because data is in compressed form. If we did have a direct $sample on the
        // system.buckets collection, then the 'bucketUnpacker' would not be set up properly. We
        // also should bail out early if a $sample is made against a time series collection that is
        // empty. If we don't the 'minAdvancedToWorkRatio' can be nan/-nan depending on the
        // architecture.
        if (!(bucketUnpacker && numRecords)) {
            return std::pair{nullptr, false};
        }

        // Use a 'TrialStage' to run a trial between 'SampleFromTimeseriesBucket' and
        // 'UnpackTimeseriesBucket' with $sample left in the pipeline in-place. If the buckets are
        // not sufficiently full, or the 'SampleFromTimeseriesBucket' plan draws too many
        // duplicates, then we will fall back to the 'TrialStage' backup plan. This backup plan uses
        // the top-k sort sampling approach.
        //
        // Suppose the 'gTimeseriesBucketMaxCount' is 1000, but each bucket only contains 500
        // documents on average. The observed trial advanced/work ratio approximates the average
        // bucket fullness, noted here as "abf". In this example, abf = 500 / 1000 = 0.5.
        // Experiments have shown that the optimized 'SampleFromTimeseriesBucket' algorithm performs
        // better than backup plan when
        //
        //     sampleSize < 0.02 * abf * numRecords * gTimeseriesBucketMaxCount
        //
        //  This inequality can be rewritten as
        //
        //     abf > sampleSize / (0.02 * numRecords * gTimeseriesBucketMaxCount)
        //
        // Therefore, if the advanced/work ratio exceeds this threshold, we will use the
        // 'SampleFromTimeseriesBucket' plan. Note that as the sample size requested by the user
        // becomes larger with respect to the number of buckets, we require a higher advanced/work
        // ratio in order to justify using 'SampleFromTimeseriesBucket'.
        //
        // Additionally, we require the 'TrialStage' to approximate the abf as at least 0.25. When
        // buckets are mostly empty, the 'SampleFromTimeseriesBucket' will be inefficient due to a
        // lot of sampling "misses".
        static const auto kCoefficient = 0.02;
        static const auto kMinBucketFullness = 0.25;
        const auto minAdvancedToWorkRatio = std::max(
            std::min(sampleSize / (kCoefficient * numRecords * gTimeseriesBucketMaxCount), 1.0),
            kMinBucketFullness);

        auto arhashPlan = std::make_unique<SampleFromTimeseriesBucket>(
            expCtx.get(),
            ws.get(),
            std::move(root),
            *bucketUnpacker,
            // By using a quantity slightly higher than 'kMaxPresampleSize', we ensure that the
            // 'SampleFromTimeseriesBucket' stage won't fail due to too many consecutive sampling
            // attempts during the 'TrialStage's trial period.
            kMaxPresampleSize + 5,
            sampleSize,
            gTimeseriesBucketMaxCount);

        std::unique_ptr<PlanStage> collScanPlan = std::make_unique<CollectionScan>(
            expCtx.get(), coll, CollectionScanParams{}, ws.get(), nullptr);

        auto topkSortPlan = std::make_unique<UnpackTimeseriesBucket>(
            expCtx.get(), ws.get(), std::move(collScanPlan), *bucketUnpacker);

        root = std::make_unique<TrialStage>(expCtx.get(),
                                            ws.get(),
                                            std::move(arhashPlan),
                                            std::move(topkSortPlan),
                                            kMaxPresampleSize,
                                            minAdvancedToWorkRatio);
        trialStage = static_cast<TrialStage*>(root.get());
    }

    auto execStatus = plan_executor_factory::make(expCtx,
                                                  std::move(ws),
                                                  std::move(root),
                                                  &coll,
                                                  opCtx->inMultiDocumentTransaction()
                                                      ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                                                      : PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                  QueryPlannerParams::RETURN_OWNED_DATA);
    if (!execStatus.isOK()) {
        return execStatus.getStatus();
    }

    // For sharded collections, the root of the plan tree is a TrialStage that may have chosen
    // either a random-sampling cursor trial plan or a COLLSCAN backup plan. We can only optimize
    // the $sample aggregation stage if the trial plan was chosen.
    return std::pair{std::move(execStatus.getValue()),
                     !trialStage || !trialStage->pickedBackupPlan()};
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> attemptToGetExecutor(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionPtr& collection,
    const NamespaceString& nss,
    BSONObj queryObj,
    BSONObj projectionObj,
    const QueryMetadataBitSet& metadataRequested,
    BSONObj sortObj,
    SkipThenLimit skipThenLimit,
    boost::optional<std::string> groupIdForDistinctScan,
    const AggregateCommandRequest* aggRequest,
    const size_t plannerOpts,
    const MatchExpressionParser::AllowedFeatureSet& matcherFeatures) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    query_request_helper::setTailableMode(expCtx->tailableMode, findCommand.get());
    findCommand->setFilter(queryObj.getOwned());
    findCommand->setProjection(projectionObj.getOwned());
    findCommand->setSort(sortObj.getOwned());
    if (auto skip = skipThenLimit.getSkip()) {
        findCommand->setSkip(static_cast<std::int64_t>(*skip));
    }
    if (auto limit = skipThenLimit.getLimit()) {
        findCommand->setLimit(static_cast<std::int64_t>(*limit));
    }

    bool isExplain = false;
    if (aggRequest) {
        findCommand->setHint(aggRequest->getHint().value_or(BSONObj()).getOwned());
        isExplain = static_cast<bool>(aggRequest->getExplain());
    }

    // The collation on the ExpressionContext has been resolved to either the user-specified
    // collation or the collection default. This BSON should never be empty even if the resolved
    // collator is simple.
    findCommand->setCollation(expCtx->getCollatorBSON().getOwned());

    const ExtensionsCallbackReal extensionsCallback(expCtx->opCtx, &nss);

    // Reset the 'sbeCompatible' flag before canonicalizing the 'findCommand' to potentially allow
    // SBE to execute the portion of the query that's pushed down, even if the portion of the query
    // that is not pushed down contains expressions not supported by SBE.
    expCtx->sbeCompatible = true;

    auto cq = CanonicalQuery::canonicalize(expCtx->opCtx,
                                           std::move(findCommand),
                                           isExplain,
                                           expCtx,
                                           extensionsCallback,
                                           matcherFeatures,
                                           ProjectionPolicies::aggregateProjectionPolicies());

    if (!cq.isOK()) {
        // Return an error instead of uasserting, since there are cases where the combination of
        // sort and projection will result in a bad query, but when we try with a different
        // combination it will be ok. e.g. a sort by {$meta: 'textScore'}, without any projection
        // will fail, but will succeed when the corresponding '$meta' projection is passed in
        // another attempt.
        return {cq.getStatus()};
    }

    // Mark the metadata that's requested by the pipeline on the CQ.
    cq.getValue()->requestAdditionalMetadata(metadataRequested);

    if (groupIdForDistinctScan) {
        // When the pipeline includes a $group that groups by a single field
        // (groupIdForDistinctScan), we use getExecutorDistinct() to attempt to get an executor that
        // uses a DISTINCT_SCAN to scan exactly one document for each group. When that's not
        // possible, we return nullptr, and the caller is responsible for trying again without
        // passing a 'groupIdForDistinctScan' value.
        ParsedDistinct parsedDistinct(std::move(cq.getValue()), *groupIdForDistinctScan);

        // Note that we request a "strict" distinct plan because:
        // 1) We do not want to have to de-duplicate the results of the plan.
        //
        // 2) We not want a plan that will return separate values for each array element. For
        // example, if we have a document {a: [1,2]} and group by "a" a DISTINCT_SCAN on an "a"
        // index would produce one result for '1' and another for '2', which would be incorrect.
        auto distinctExecutor = getExecutorDistinct(
            &collection, plannerOpts | QueryPlannerParams::STRICT_DISTINCT_ONLY, &parsedDistinct);
        if (!distinctExecutor.isOK()) {
            return distinctExecutor.getStatus().withContext(
                "Unable to use distinct scan to optimize $group stage");
        } else if (!distinctExecutor.getValue()) {
            return {ErrorCodes::NoQueryExecutionPlans,
                    "Unable to use distinct scan to optimize $group stage"};
        } else {
            return distinctExecutor;
        }
    }

    bool permitYield = true;
    return getExecutorFind(
        expCtx->opCtx, &collection, std::move(cq.getValue()), permitYield, plannerOpts);
}

/**
 * Examines the indexes in 'collection' and returns the field name of a geo-indexed field suitable
 * for use in $geoNear. 2d indexes are given priority over 2dsphere indexes.
 *
 * The 'collection' is required to exist. Throws if no usable 2d or 2dsphere index could be found.
 */
StringData extractGeoNearFieldFromIndexes(OperationContext* opCtx,
                                          const CollectionPtr& collection) {
    invariant(collection);

    std::vector<const IndexDescriptor*> idxs;
    collection->getIndexCatalog()->findIndexByType(opCtx, IndexNames::GEO_2D, idxs);
    uassert(ErrorCodes::IndexNotFound,
            str::stream() << "There is more than one 2d index on " << collection->ns().ns()
                          << "; unsure which to use for $geoNear",
            idxs.size() <= 1U);
    if (idxs.size() == 1U) {
        for (auto&& elem : idxs.front()->keyPattern()) {
            if (elem.type() == BSONType::String && elem.valueStringData() == IndexNames::GEO_2D) {
                return elem.fieldNameStringData();
            }
        }
        MONGO_UNREACHABLE;
    }

    // If there are no 2d indexes, look for a 2dsphere index.
    idxs.clear();
    collection->getIndexCatalog()->findIndexByType(opCtx, IndexNames::GEO_2DSPHERE, idxs);
    uassert(ErrorCodes::IndexNotFound,
            "$geoNear requires a 2d or 2dsphere index, but none were found",
            !idxs.empty());
    uassert(ErrorCodes::IndexNotFound,
            str::stream() << "There is more than one 2dsphere index on " << collection->ns().ns()
                          << "; unsure which to use for $geoNear",
            idxs.size() <= 1U);

    invariant(idxs.size() == 1U);
    for (auto&& elem : idxs.front()->keyPattern()) {
        if (elem.type() == BSONType::String && elem.valueStringData() == IndexNames::GEO_2DSPHERE) {
            return elem.fieldNameStringData();
        }
    }
    MONGO_UNREACHABLE;
}

/**
 * This attempts to either extract a $sample stage at the front of the pipeline or a
 * $_internalUnpackBucket stage at the front of the pipeline immediately followed by a $sample
 * stage. In the former case a 'nullptr' is returned for the second element of the pair <$sample,
 * $_internalUnpackBucket>, and if the latter case is encountered both elements of the pair will be
 * a populated. If the pipeline doesn't contain a $_internalUnpackBucket at the front of the
 * pipeline immediately followed by a $sample stage, then the first element in the pair will be a
 * 'nullptr'.
 */
std::pair<DocumentSourceSample*, DocumentSourceInternalUnpackBucket*> extractSampleUnpackBucket(
    const Pipeline::SourceContainer& sources) {
    DocumentSourceSample* sampleStage = nullptr;
    DocumentSourceInternalUnpackBucket* unpackStage = nullptr;

    auto sourcesIt = sources.begin();
    if (sourcesIt != sources.end()) {
        sampleStage = dynamic_cast<DocumentSourceSample*>(sourcesIt->get());
        if (sampleStage) {
            return std::pair{sampleStage, unpackStage};
        }

        unpackStage = dynamic_cast<DocumentSourceInternalUnpackBucket*>(sourcesIt->get());
        ++sourcesIt;

        if (unpackStage && sourcesIt != sources.end()) {
            sampleStage = dynamic_cast<DocumentSourceSample*>(sourcesIt->get());
            return std::pair{sampleStage, unpackStage};
        }
    }

    return std::pair{sampleStage, unpackStage};
}
}  // namespace

std::pair<PipelineD::AttachExecutorCallback, std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
PipelineD::buildInnerQueryExecutorSample(DocumentSourceSample* sampleStage,
                                         DocumentSourceInternalUnpackBucket* unpackBucketStage,
                                         const CollectionPtr& collection,
                                         Pipeline* pipeline) {
    tassert(5422105, "sampleStage cannot be a nullptr", sampleStage);

    auto expCtx = pipeline->getContext();

    Pipeline::SourceContainer& sources = pipeline->_sources;

    const long long sampleSize = sampleStage->getSampleSize();
    const long long numRecords = collection->getRecordStore()->numRecords(expCtx->opCtx);

    boost::optional<BucketUnpacker> bucketUnpacker;
    if (unpackBucketStage) {
        bucketUnpacker = unpackBucketStage->bucketUnpacker();
    }
    auto&& [exec, isStorageOptimizedSample] = uassertStatusOK(createRandomCursorExecutor(
        collection, expCtx, sampleSize, numRecords, std::move(bucketUnpacker)));

    AttachExecutorCallback attachExecutorCallback;
    if (exec) {
        if (!unpackBucketStage) {
            if (isStorageOptimizedSample) {
                // Replace $sample stage with $sampleFromRandomCursor stage.
                pipeline->popFront();
                std::string idString = collection->ns().isOplog() ? "ts" : "_id";
                pipeline->addInitialSource(DocumentSourceSampleFromRandomCursor::create(
                    expCtx, sampleSize, idString, numRecords));
            }
        } else {
            if (isStorageOptimizedSample) {
                // If there are non-nullptrs for 'sampleStage' and 'unpackBucketStage', then
                // 'unpackBucketStage' is at the front of the pipeline immediately followed by a
                // 'sampleStage'. We need to use a TrialStage approach to handle a problem where
                // ARHASH sampling can fail due to small measurement counts. We can push sampling
                // and bucket unpacking down to the PlanStage layer and erase $_internalUnpackBucket
                // and $sample.
                sources.erase(sources.begin());
                sources.erase(sources.begin());
            } else {
                // The TrialStage chose the backup plan and we need to erase just the
                // $_internalUnpackBucket stage and leave $sample where it is.
                sources.erase(sources.begin());
            }
        }

        // The order in which we evaluate these arguments is significant. We'd like to be
        // sure that the DocumentSourceCursor is created _last_, because if we run into a
        // case where a DocumentSourceCursor has been created (yet hasn't been put into a
        // Pipeline) and an exception is thrown, an invariant will trigger in the
        // DocumentSourceCursor. This is a design flaw in DocumentSourceCursor.
        auto deps = pipeline->getDependencies(DepsTracker::kAllMetadata);
        const auto cursorType = deps.hasNoRequirements()
            ? DocumentSourceCursor::CursorType::kEmptyDocuments
            : DocumentSourceCursor::CursorType::kRegular;
        attachExecutorCallback =
            [cursorType](const CollectionPtr& collection,
                         std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                         Pipeline* pipeline) {
                auto cursor = DocumentSourceCursor::create(
                    collection, std::move(exec), pipeline->getContext(), cursorType);
                pipeline->addInitialSource(std::move(cursor));
            };
        return std::pair(std::move(attachExecutorCallback), std::move(exec));
    }
    return std::pair(std::move(attachExecutorCallback), nullptr);
}

std::pair<PipelineD::AttachExecutorCallback, std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
PipelineD::buildInnerQueryExecutor(const CollectionPtr& collection,
                                   const NamespaceString& nss,
                                   const AggregateCommandRequest* aggRequest,
                                   Pipeline* pipeline) {
    auto expCtx = pipeline->getContext();

    // We will be modifying the source vector as we go.
    Pipeline::SourceContainer& sources = pipeline->_sources;

    if (!sources.empty() && !sources.front()->constraints().requiresInputDocSource) {
        return {};
    }

    if (!sources.empty()) {
        // Try to inspect if the DocumentSourceSample or a DocumentSourceInternalUnpackBucket stage
        // can be optimized for sampling backed by a storage engine supplied random cursor.
        auto&& [sampleStage, unpackBucketStage] = extractSampleUnpackBucket(sources);

        // Optimize an initial $sample stage if possible.
        if (collection && sampleStage) {
            auto [attachExecutorCallback, exec] =
                buildInnerQueryExecutorSample(sampleStage, unpackBucketStage, collection, pipeline);
            if (exec) {
                return std::make_pair(std::move(attachExecutorCallback), std::move(exec));
            }
        }
    }

    // If the first stage is $geoNear, prepare a special DocumentSourceGeoNearCursor stage;
    // otherwise, create a generic DocumentSourceCursor.
    const auto geoNearStage =
        sources.empty() ? nullptr : dynamic_cast<DocumentSourceGeoNear*>(sources.front().get());
    if (geoNearStage) {
        return buildInnerQueryExecutorGeoNear(collection, nss, aggRequest, pipeline);
    } else {
        return buildInnerQueryExecutorGeneric(collection, nss, aggRequest, pipeline);
    }
}

void PipelineD::attachInnerQueryExecutorToPipeline(
    const CollectionPtr& collection,
    PipelineD::AttachExecutorCallback attachExecutorCallback,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    Pipeline* pipeline) {
    // If the pipeline doesn't need a $cursor stage, there will be no callback function and
    // PlanExecutor provided in the 'attachExecutorCallback' object, so we don't need to do
    // anything.
    if (attachExecutorCallback && exec) {
        attachExecutorCallback(collection, std::move(exec), pipeline);
    }
}

void PipelineD::buildAndAttachInnerQueryExecutorToPipeline(
    const CollectionPtr& collection,
    const NamespaceString& nss,
    const AggregateCommandRequest* aggRequest,
    Pipeline* pipeline) {

    auto callback = PipelineD::buildInnerQueryExecutor(collection, nss, aggRequest, pipeline);
    PipelineD::attachInnerQueryExecutorToPipeline(
        collection, callback.first, std::move(callback.second), pipeline);
}

namespace {

/**
 * Look for $sort, $group at the beginning of the pipeline, potentially returning either or both.
 * Returns nullptr for any of the stages that are not found. Note that we are not looking for the
 * opposite pattern ($group, $sort). In that case, this function will return only the $group stage.
 *
 * This function will not return the $group in the case that there is an initial $sort with
 * intermediate stages that separate it from the $group (e.g.: $sort, $limit, $group). That includes
 * the case of a $sort with a non-null value for getLimitSrc(), indicating that there was previously
 * a $limit stage that was optimized away.
 */
std::pair<boost::intrusive_ptr<DocumentSourceSort>, boost::intrusive_ptr<DocumentSourceGroup>>
getSortAndGroupStagesFromPipeline(const Pipeline::SourceContainer& sources) {
    boost::intrusive_ptr<DocumentSourceSort> sortStage = nullptr;
    boost::intrusive_ptr<DocumentSourceGroup> groupStage = nullptr;

    auto sourcesIt = sources.begin();
    if (sourcesIt != sources.end()) {
        sortStage = dynamic_cast<DocumentSourceSort*>(sourcesIt->get());
        if (sortStage) {
            if (!sortStage->hasLimit()) {
                ++sourcesIt;
            } else {
                // This $sort stage was previously followed by a $limit stage.
                sourcesIt = sources.end();
            }
        }
    }

    if (sourcesIt != sources.end()) {
        groupStage = dynamic_cast<DocumentSourceGroup*>(sourcesIt->get());
    }

    return std::make_pair(sortStage, groupStage);
}

boost::optional<long long> extractSkipForPushdown(Pipeline* pipeline) {
    // If the disablePipelineOptimization failpoint is enabled, then do not attempt the skip
    // pushdown optimization.
    if (MONGO_unlikely(disablePipelineOptimization.shouldFail())) {
        return boost::none;
    }
    auto&& sources = pipeline->getSources();

    auto skip = extractSkipForPushdown(sources.begin(), &sources);
    if (skip) {
        // Removing stages may have produced the opportunity for additional optimizations.
        pipeline->optimizePipeline();
    }
    return skip;
}

SkipThenLimit extractSkipAndLimitForPushdown(Pipeline* pipeline) {
    // If the disablePipelineOptimization failpoint is enabled, then do not attempt the limit and
    // skip pushdown optimization.
    if (MONGO_unlikely(disablePipelineOptimization.shouldFail())) {
        return {boost::none, boost::none};
    }
    auto&& sources = pipeline->getSources();

    // It is important to call 'extractLimitForPushdown' before 'extractSkipForPushdown'. Otherwise
    // there could be a situation when $limit stages in pipeline would prevent
    // 'extractSkipForPushdown' from extracting all $skip stages.
    auto limit = extractLimitForPushdown(sources.begin(), &sources);
    auto skip = extractSkipForPushdown(sources.begin(), &sources);
    auto skipThenLimit = LimitThenSkip(limit, skip).flip();
    if (skipThenLimit.getSkip() || skipThenLimit.getLimit()) {
        // Removing stages may have produced the opportunity for additional optimizations.
        pipeline->optimizePipeline();
    }
    return skipThenLimit;
}

/**
 * Given a dependency set and a pipeline, builds a projection BSON object to push down into the
 * PlanStage layer. The rules to push down the projection are as follows:
 *    1. If there is an inclusion projection at the front of the pipeline, it will be pushed down
 *       as is.
 *    2. If there is no inclusion projection at the front of the pipeline, but there is a finite
 *       dependency set, a projection representing this dependency set will be pushed down.
 *    3. Otherwise, an empty projection is returned and no projection push down will happen.
 *
 * If 'allowExpressions' is true, the returned projection may include expressions (which can only
 * happen in case 1). If 'allowExpressions' is false and the projection we find has expressions,
 * then we fall through to case 2 and attempt to push down a pure-inclusion projection based on its
 * dependencies.
 */
auto buildProjectionForPushdown(const DepsTracker& deps,
                                Pipeline* pipeline,
                                bool allowExpressions) {
    auto&& sources = pipeline->getSources();

    // Short-circuit if the pipeline is empty: there is no projection and nothing to push down.
    if (sources.empty()) {
        return BSONObj();
    }

    if (const auto projStage =
            exact_pointer_cast<DocumentSourceSingleDocumentTransformation*>(sources.front().get());
        projStage) {
        if (projStage->getType() == TransformerInterface::TransformerType::kInclusionProjection) {
            auto projObj =
                projStage->getTransformer().serializeTransformation(boost::none).toBson();
            auto projAst = projection_ast::parse(projStage->getContext(),
                                                 projObj,
                                                 ProjectionPolicies::aggregateProjectionPolicies());
            if (!projAst.hasExpressions() || allowExpressions) {
                // If there is an inclusion projection at the front of the pipeline, we have case 1.
                sources.pop_front();
                return projObj;
            }
        }
    }

    // Depending of whether there is a finite dependency set, either return a projection
    // representing this dependency set, or an empty BSON, meaning no projection push down will
    // happen. This covers cases 2 and 3.
    if (deps.getNeedsAnyMetadata())
        return BSONObj();
    return deps.toProjectionWithoutMetadata();
}
}  // namespace

std::pair<PipelineD::AttachExecutorCallback, std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
PipelineD::buildInnerQueryExecutorGeneric(const CollectionPtr& collection,
                                          const NamespaceString& nss,
                                          const AggregateCommandRequest* aggRequest,
                                          Pipeline* pipeline) {
    // Make a last effort to optimize pipeline stages before potentially detaching them to be pushed
    // down into the query executor.
    pipeline->optimizePipeline();

    Pipeline::SourceContainer& sources = pipeline->_sources;
    auto expCtx = pipeline->getContext();

    // Look for an initial match. This works whether we got an initial query or not. If not, it
    // results in a "{}" query, which will be what we want in that case.
    const BSONObj queryObj = pipeline->getInitialQuery();
    if (!queryObj.isEmpty()) {
        auto matchStage = dynamic_cast<DocumentSourceMatch*>(sources.front().get());
        if (matchStage) {
            // If a $match query is pulled into the cursor, the $match is redundant, and can be
            // removed from the pipeline.
            sources.pop_front();
        } else {
            // A $geoNear stage, the only other stage that can produce an initial query, is also
            // a valid initial stage. However, we should be in prepareGeoNearCursorSource() instead.
            MONGO_UNREACHABLE;
        }
    }

    auto&& [sortStage, groupStage] = getSortAndGroupStagesFromPipeline(pipeline->_sources);
    std::unique_ptr<GroupFromFirstDocumentTransformation> rewrittenGroupStage;
    if (groupStage) {
        rewrittenGroupStage = groupStage->rewriteGroupAsTransformOnFirstDocument();
    }

    // If there is a $limit or $skip stage (or multiple of them) that could be pushed down into the
    // PlanStage layer, obtain the value of the limit and skip and remove the $limit and $skip
    // stages from the pipeline.
    //
    // This analysis is done here rather than in 'optimizePipeline()' because swapping $limit before
    // stages such as $project is not always useful, and can sometimes defeat other optimizations.
    // In particular, in a sharded scenario a pipeline such as [$project, $limit] is preferable to
    // [$limit, $project]. The former permits the execution of the projection operation to be
    // parallelized across all targeted shards, whereas the latter would bring all of the data to a
    // merging shard first, and then apply the projection serially. See SERVER-24981 for a more
    // detailed discussion.
    //
    // This only handles the case in which the the $limit or $skip can logically be swapped to the
    // front of the pipeline. We can also push down a $limit which comes after a $sort into the
    // PlanStage layer, but that is handled elsewhere.
    const auto skipThenLimit = extractSkipAndLimitForPushdown(pipeline);

    auto unavailableMetadata = DocumentSourceMatch::isTextQuery(queryObj)
        ? DepsTracker::kDefaultUnavailableMetadata & ~DepsTracker::kOnlyTextScore
        : DepsTracker::kDefaultUnavailableMetadata;

    // Create the PlanExecutor.
    bool shouldProduceEmptyDocs = false;
    auto exec = uassertStatusOK(prepareExecutor(expCtx,
                                                collection,
                                                nss,
                                                pipeline,
                                                sortStage,
                                                std::move(rewrittenGroupStage),
                                                unavailableMetadata,
                                                queryObj,
                                                skipThenLimit,
                                                aggRequest,
                                                Pipeline::kAllowedMatcherFeatures,
                                                &shouldProduceEmptyDocs));

    const auto cursorType = shouldProduceEmptyDocs
        ? DocumentSourceCursor::CursorType::kEmptyDocuments
        : DocumentSourceCursor::CursorType::kRegular;

    // If this is a change stream pipeline or a resharding resume token has been requested, make
    // sure that we tell DSCursor to track the oplog time.
    const bool trackOplogTS =
        (pipeline->peekFront() && pipeline->peekFront()->constraints().isChangeStreamStage()) ||
        (aggRequest && aggRequest->getRequestReshardingResumeToken());

    auto attachExecutorCallback =
        [cursorType, trackOplogTS](const CollectionPtr& collection,
                                   std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                                   Pipeline* pipeline) {
            auto cursor = DocumentSourceCursor::create(
                collection, std::move(exec), pipeline->getContext(), cursorType, trackOplogTS);
            pipeline->addInitialSource(std::move(cursor));
        };
    return std::make_pair(std::move(attachExecutorCallback), std::move(exec));
}

std::pair<PipelineD::AttachExecutorCallback, std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
PipelineD::buildInnerQueryExecutorGeoNear(const CollectionPtr& collection,
                                          const NamespaceString& nss,
                                          const AggregateCommandRequest* aggRequest,
                                          Pipeline* pipeline) {
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "$geoNear requires a geo index to run, but " << nss.ns()
                          << " does not exist",
            collection);

    Pipeline::SourceContainer& sources = pipeline->_sources;
    auto expCtx = pipeline->getContext();
    const auto geoNearStage = dynamic_cast<DocumentSourceGeoNear*>(sources.front().get());
    invariant(geoNearStage);

    // If the user specified a "key" field, use that field to satisfy the "near" query. Otherwise,
    // look for a geo-indexed field in 'collection' that can.
    auto nearFieldName =
        (geoNearStage->getKeyField() ? geoNearStage->getKeyField()->fullPath()
                                     : extractGeoNearFieldFromIndexes(expCtx->opCtx, collection))
            .toString();

    // Create a PlanExecutor whose query is the "near" predicate on 'nearFieldName' combined with
    // the optional "query" argument in the $geoNear stage.
    BSONObj fullQuery = geoNearStage->asNearQuery(nearFieldName);

    bool shouldProduceEmptyDocs = false;
    auto exec = uassertStatusOK(
        prepareExecutor(expCtx,
                        collection,
                        nss,
                        pipeline,
                        nullptr, /* sortStage */
                        nullptr, /* rewrittenGroupStage */
                        DepsTracker::kDefaultUnavailableMetadata & ~DepsTracker::kAllGeoNearData,
                        std::move(fullQuery),
                        SkipThenLimit{boost::none, boost::none},
                        aggRequest,
                        Pipeline::kGeoNearMatcherFeatures,
                        &shouldProduceEmptyDocs));

    auto attachExecutorCallback = [distanceField = geoNearStage->getDistanceField(),
                                   locationField = geoNearStage->getLocationField(),
                                   distanceMultiplier =
                                       geoNearStage->getDistanceMultiplier().value_or(1.0)](
                                      const CollectionPtr& collection,
                                      std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                                      Pipeline* pipeline) {
        auto cursor = DocumentSourceGeoNearCursor::create(collection,
                                                          std::move(exec),
                                                          pipeline->getContext(),
                                                          distanceField,
                                                          locationField,
                                                          distanceMultiplier);
        pipeline->addInitialSource(std::move(cursor));
    };
    // Remove the initial $geoNear; it will be replaced by $geoNearCursor.
    sources.pop_front();
    return std::make_pair(std::move(attachExecutorCallback), std::move(exec));
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PipelineD::prepareExecutor(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionPtr& collection,
    const NamespaceString& nss,
    Pipeline* pipeline,
    const boost::intrusive_ptr<DocumentSourceSort>& sortStage,
    std::unique_ptr<GroupFromFirstDocumentTransformation> rewrittenGroupStage,
    QueryMetadataBitSet unavailableMetadata,
    const BSONObj& queryObj,
    SkipThenLimit skipThenLimit,
    const AggregateCommandRequest* aggRequest,
    const MatchExpressionParser::AllowedFeatureSet& matcherFeatures,
    bool* hasNoRequirements) {
    invariant(hasNoRequirements);

    // Any data returned from the inner executor must be owned.
    size_t plannerOpts = QueryPlannerParams::DEFAULT;

    if (pipeline->peekFront() && pipeline->peekFront()->constraints().isChangeStreamStage()) {
        invariant(expCtx->tailableMode == TailableModeEnum::kTailableAndAwaitData);
        plannerOpts |= (QueryPlannerParams::TRACK_LATEST_OPLOG_TS |
                        QueryPlannerParams::ASSERT_MIN_TS_HAS_NOT_FALLEN_OFF_OPLOG);
    }

    // The $_requestReshardingResumeToken parameter is only valid for an oplog scan.
    if (aggRequest && aggRequest->getRequestReshardingResumeToken()) {
        plannerOpts |= (QueryPlannerParams::TRACK_LATEST_OPLOG_TS |
                        QueryPlannerParams::ASSERT_MIN_TS_HAS_NOT_FALLEN_OFF_OPLOG);
    }

    // If there is a sort stage eligible for pushdown, serialize its SortPattern to a BSONObj. The
    // BSONObj format is currently necessary to request that the sort is computed by the query layer
    // inside the inner PlanExecutor. We also remove the $sort stage from the Pipeline, since it
    // will be handled instead by PlanStage execution.
    BSONObj sortObj;
    if (sortStage) {
        sortObj = sortStage->getSortKeyPattern()
                      .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization)
                      .toBson();

        pipeline->popFrontWithName(DocumentSourceSort::kStageName);

        // Now that we've pushed down the sort, see if there is a $limit and $skip to push down
        // also. We should not already have a limit or skip here, otherwise it would be incorrect
        // for the caller to pass us a sort stage to push down, since the order matters.
        invariant(!skipThenLimit.getLimit());
        invariant(!skipThenLimit.getSkip());

        // Since all $limit stages were already pushdowned to the sort stage, we are only looking
        // for $skip stages.
        auto skip = extractSkipForPushdown(pipeline);

        // Since the limit from $sort is going before the extracted $skip stages, we construct
        // 'LimitThenSkip' object and then convert it 'SkipThenLimit'.
        skipThenLimit = LimitThenSkip(sortStage->getLimit(), skip).flip();
    }

    // Perform dependency analysis. In order to minimize the dependency set, we only analyze the
    // stages that remain in the pipeline after pushdown. In particular, any dependencies for a
    // $match or $sort pushed down into the query layer will not be reflected here.
    auto deps = pipeline->getDependencies(unavailableMetadata);
    *hasNoRequirements = deps.hasNoRequirements();

    BSONObj projObj;
    if (*hasNoRequirements) {
        // This query might be eligible for count optimizations, since the remaining stages in the
        // pipeline don't actually need to read any data produced by the query execution layer.
        plannerOpts |= QueryPlannerParams::IS_COUNT;
    } else {
        // Build a BSONObj representing a projection eligible for pushdown. If there is an inclusion
        // projection at the front of the pipeline, it will be removed and handled by the PlanStage
        // layer. If a projection cannot be pushed down, an empty BSONObj will be returned.

        // In most cases .find() behaves as if it evaluates in a predictable order:
        //     predicate, sort, skip, limit, projection.
        // But there is at least one case where it runs the projection before the sort/skip/limit:
        // when the predicate has a rooted $or.  (In that case we plan each branch of the $or
        // separately, using Subplan, and include the projection on each branch.)

        // To work around this behavior, don't allow pushing down expressions if we are also going
        // to push down a sort, skip or limit. We don't want the expressions to be evaluated on any
        // documents that the sort/skip/limit would have filtered out. (The sort stage can be a
        // top-k sort, which both sorts and limits.)
        bool allowExpressions = !sortStage && !skipThenLimit.getSkip() && !skipThenLimit.getLimit();
        projObj = buildProjectionForPushdown(deps, pipeline, allowExpressions);
        plannerOpts |= QueryPlannerParams::RETURN_OWNED_DATA;
    }

    if (rewrittenGroupStage) {
        // See if the query system can handle the $group and $sort stage using a DISTINCT_SCAN
        // (SERVER-9507).
        auto swExecutorGrouped = attemptToGetExecutor(expCtx,
                                                      collection,
                                                      nss,
                                                      queryObj,
                                                      projObj,
                                                      deps.metadataDeps(),
                                                      sortObj,
                                                      SkipThenLimit{boost::none, boost::none},
                                                      rewrittenGroupStage->groupId(),
                                                      aggRequest,
                                                      plannerOpts,
                                                      matcherFeatures);

        if (swExecutorGrouped.isOK()) {
            // Any $limit stage before the $group stage should make the pipeline ineligible for this
            // optimization.
            invariant(!sortStage || !sortStage->hasLimit());

            // We remove the $sort and $group stages that begin the pipeline, because the executor
            // will handle the sort, and the groupTransform (added below) will handle the $group
            // stage.
            pipeline->popFrontWithName(DocumentSourceSort::kStageName);
            pipeline->popFrontWithName(DocumentSourceGroup::kStageName);

            boost::intrusive_ptr<DocumentSource> groupTransform(
                new DocumentSourceSingleDocumentTransformation(
                    expCtx,
                    std::move(rewrittenGroupStage),
                    "$groupByDistinctScan",
                    false /* independentOfAnyCollection */));
            pipeline->addInitialSource(groupTransform);

            return swExecutorGrouped;
        } else if (swExecutorGrouped != ErrorCodes::NoQueryExecutionPlans) {
            return swExecutorGrouped.getStatus().withContext(
                "Failed to determine whether query system can provide a "
                "DISTINCT_SCAN grouping");
        }
    }

    return attemptToGetExecutor(expCtx,
                                collection,
                                nss,
                                queryObj,
                                projObj,
                                deps.metadataDeps(),
                                sortObj,
                                skipThenLimit,
                                boost::none, /* groupIdForDistinctScan */
                                aggRequest,
                                plannerOpts,
                                matcherFeatures);
}

Timestamp PipelineD::getLatestOplogTimestamp(const Pipeline* pipeline) {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pipeline->_sources.front().get())) {
        return docSourceCursor->getLatestOplogTimestamp();
    }
    return Timestamp();
}

BSONObj PipelineD::getPostBatchResumeToken(const Pipeline* pipeline) {
    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(pipeline->_sources.front().get())) {
        return docSourceCursor->getPostBatchResumeToken();
    }
    return BSONObj{};
}
}  // namespace mongo
