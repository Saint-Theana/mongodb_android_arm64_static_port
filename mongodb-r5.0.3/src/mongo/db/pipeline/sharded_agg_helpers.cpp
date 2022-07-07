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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "sharded_agg_helpers.h"

#include "mongo/db/curop.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/document_source_update_on_add_shard.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/visit_helper.h"

namespace mongo::sharded_agg_helpers {

MONGO_FAIL_POINT_DEFINE(shardedAggregateHangBeforeEstablishingShardCursors);

namespace {

/**
 * Given a document representing an aggregation command such as
 * {aggregate: "myCollection", pipeline: [], ...},
 *
 * produces the corresponding explain command:
 * {explain: {aggregate: "myCollection", pipline: [], ...}, $queryOptions: {...}, verbosity: ...}
 */
Document wrapAggAsExplain(Document aggregateCommand, ExplainOptions::Verbosity verbosity) {
    MutableDocument explainCommandBuilder;
    explainCommandBuilder["explain"] = Value(aggregateCommand);
    // Downstream host targeting code expects queryOptions at the top level of the command object.
    explainCommandBuilder[query_request_helper::kUnwrappedReadPrefField] =
        Value(aggregateCommand[query_request_helper::kUnwrappedReadPrefField]);

    // readConcern needs to be promoted to the top-level of the request.
    explainCommandBuilder[repl::ReadConcernArgs::kReadConcernFieldName] =
        Value(aggregateCommand[repl::ReadConcernArgs::kReadConcernFieldName]);

    // Add explain command options.
    for (auto&& explainOption : ExplainOptions::toBSON(verbosity)) {
        explainCommandBuilder[explainOption.fieldNameStringData()] = Value(explainOption);
    }

    return explainCommandBuilder.freeze();
}

/**
 * Open a $changeStream cursor on the 'config.shards' collection to watch for new shards.
 */
RemoteCursor openChangeStreamNewShardMonitor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             Timestamp startMonitoringAtTime) {
    const auto& configShard = Grid::get(expCtx->opCtx)->shardRegistry()->getConfigShard();
    // Pipeline: {$changeStream: {startAtOperationTime: [now], allowToRunOnConfigDB: true}}
    AggregateCommandRequest aggReq(
        ShardType::ConfigNS,
        {BSON(DocumentSourceChangeStream::kStageName
              << BSON(DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName
                      << startMonitoringAtTime
                      << DocumentSourceChangeStreamSpec::kAllowToRunOnConfigDBFieldName << true))});
    aggReq.setFromMongos(true);
    aggReq.setNeedsMerge(true);
    SimpleCursorOptions cursor;
    cursor.setBatchSize(0);
    aggReq.setCursor(cursor);
    auto cmdObjWithRWC =
        applyReadWriteConcern(expCtx->opCtx,
                              true,             /* appendRC */
                              !expCtx->explain, /* appendWC */
                              aggregation_request_helper::serializeToCommandObj(aggReq));
    auto configCursor = establishCursors(expCtx->opCtx,
                                         expCtx->mongoProcessInterface->taskExecutor,
                                         aggReq.getNamespace(),
                                         ReadPreferenceSetting{ReadPreference::SecondaryPreferred},
                                         {{configShard->getId(), cmdObjWithRWC}},
                                         false);
    invariant(configCursor.size() == 1);
    return std::move(*configCursor.begin());
}

BSONObj genericTransformForShards(MutableDocument&& cmdForShards,
                                  const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  boost::optional<ExplainOptions::Verbosity> explainVerbosity,
                                  BSONObj collationObj) {
    // Serialize the variables.
    // Check whether we are in a mixed-version cluster and so must use the old serialization format.
    // This is only tricky in the case we are sending an aggregate from shard to shard. For this
    // scenario we can rely on feature compatibility version to detect when this is safe. Feature
    // compatibility version is not generally accurate on mongos since it was intended to guard
    // changes to data format and mongos has no persisted state. However, mongos is upgraded last
    // after all the shards, so any recipient will understand the 'let' parameter.
    // TODO SERVER-52900 This code can be remove when we branch for the next LTS release.
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
        !serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
            ServerGlobalParams::FeatureCompatibility::Version::kVersion49)) {
        // A mixed version cluster. Use the old format to be sure it is understood.
        auto [legacyRuntimeConstants, unusedSerializedVariables] =
            expCtx->variablesParseState.transitionalCompatibilitySerialize(expCtx->variables);

        cmdForShards[AggregateCommandRequest::kLegacyRuntimeConstantsFieldName] =
            Value(legacyRuntimeConstants.toBSON());
    } else {
        // Either this is a "modern" cluster or we are a mongos and can assume the shards are
        // "modern" and will understand the 'let' parameter.
        cmdForShards[AggregateCommandRequest::kLetFieldName] =
            Value(expCtx->variablesParseState.serialize(expCtx->variables));
    }

    cmdForShards[AggregateCommandRequest::kFromMongosFieldName] = Value(expCtx->inMongos);

    if (!collationObj.isEmpty()) {
        cmdForShards[AggregateCommandRequest::kCollationFieldName] = Value(collationObj);
    }

    // If this is a request for an aggregation explain, then we must wrap the aggregate inside an
    // explain command.
    if (explainVerbosity) {
        cmdForShards.reset(wrapAggAsExplain(cmdForShards.freeze(), *explainVerbosity));
    }

    if (expCtx->opCtx->getTxnNumber()) {
        invariant(cmdForShards.peek()[OperationSessionInfo::kTxnNumberFieldName].missing(),
                  str::stream() << "Command for shards unexpectedly had the "
                                << OperationSessionInfo::kTxnNumberFieldName
                                << " field set: " << cmdForShards.peek().toString());
        cmdForShards[OperationSessionInfo::kTxnNumberFieldName] =
            Value(static_cast<long long>(*expCtx->opCtx->getTxnNumber()));
    }

    return cmdForShards.freeze().toBson();
}

std::vector<RemoteCursor> establishShardCursors(OperationContext* opCtx,
                                                std::shared_ptr<executor::TaskExecutor> executor,
                                                const NamespaceString& nss,
                                                bool mustRunOnAll,
                                                boost::optional<ChunkManager>& cm,
                                                const std::set<ShardId>& shardIds,
                                                const BSONObj& cmdObj,
                                                const ReadPreferenceSetting& readPref) {
    LOGV2_DEBUG(20904,
                1,
                "Dispatching command {cmdObj} to establish cursors on shards",
                "cmdObj"_attr = redact(cmdObj));

    std::vector<std::pair<ShardId, BSONObj>> requests;

    // If we don't need to run on all shards, then we should always have a valid routing table.
    invariant(cm || mustRunOnAll);

    if (mustRunOnAll) {
        // The pipeline contains a stage which must be run on all shards. Skip versioning and
        // enqueue the raw command objects.
        for (const auto& shardId : shardIds) {
            requests.emplace_back(shardId, cmdObj);
        }
    } else if (cm->isSharded()) {
        // The collection is sharded. Use the routing table to decide which shards to target
        // based on the query and collation, and build versioned requests for them.
        for (const auto& shardId : shardIds) {
            auto versionedCmdObj = appendShardVersion(cmdObj, cm->getVersion(shardId));
            requests.emplace_back(shardId, std::move(versionedCmdObj));
        }
    } else {
        // The collection is unsharded. Target only the primary shard for the database.
        // Don't append shard version info when contacting the config servers.
        const auto cmdObjWithShardVersion = cm->dbPrimary() != ShardId::kConfigServerId
            ? appendShardVersion(cmdObj, ChunkVersion::UNSHARDED())
            : cmdObj;
        requests.emplace_back(cm->dbPrimary(),
                              appendDbVersionIfPresent(cmdObjWithShardVersion, cm->dbVersion()));
    }

    if (MONGO_unlikely(shardedAggregateHangBeforeEstablishingShardCursors.shouldFail())) {
        LOGV2(20905,
              "shardedAggregateHangBeforeEstablishingShardCursors fail point enabled.  Blocking "
              "until fail point is disabled.");
        while (MONGO_unlikely(shardedAggregateHangBeforeEstablishingShardCursors.shouldFail())) {
            sleepsecs(1);
        }
    }

    return establishCursors(opCtx,
                            std::move(executor),
                            nss,
                            readPref,
                            requests,
                            false /* do not allow partial results */,
                            getDesiredRetryPolicy(opCtx));
}

std::set<ShardId> getTargetedShards(boost::intrusive_ptr<ExpressionContext> expCtx,
                                    bool mustRunOnAllShards,
                                    const boost::optional<ChunkManager>& cm,
                                    const BSONObj shardQuery,
                                    const BSONObj collation) {
    if (mustRunOnAllShards) {
        // The pipeline begins with a stage which must be run on all shards.
        auto shardIds = Grid::get(expCtx->opCtx)->shardRegistry()->getAllShardIds(expCtx->opCtx);
        return {std::make_move_iterator(shardIds.begin()), std::make_move_iterator(shardIds.end())};
    }

    invariant(cm);
    return getTargetedShardsForQuery(expCtx, *cm, shardQuery, collation);
}

/**
 * Moves everything before a splittable stage to the shards. If there are no splittable stages,
 * moves everything to the shards.
 *
 * It is not safe to call this optimization multiple times.
 *
 * Returns the sort specification if the input streams are sorted, and false otherwise.
 */
boost::optional<BSONObj> findSplitPoint(Pipeline::SourceContainer* shardPipe, Pipeline* mergePipe) {
    while (!mergePipe->getSources().empty()) {
        boost::intrusive_ptr<DocumentSource> current = mergePipe->popFront();

        // Check if this source is splittable.
        auto distributedPlanLogic = current->distributedPlanLogic();
        if (!distributedPlanLogic) {
            // Move the source from the merger _sources to the shard _sources.
            shardPipe->push_back(current);
            continue;
        }

        // A source may not simultaneously be present on both sides of the split.
        invariant(distributedPlanLogic->shardsStage != distributedPlanLogic->mergingStage);

        if (distributedPlanLogic->shardsStage)
            shardPipe->push_back(std::move(distributedPlanLogic->shardsStage));

        if (distributedPlanLogic->mergingStage)
            mergePipe->addInitialSource(std::move(distributedPlanLogic->mergingStage));

        return distributedPlanLogic->inputSortPattern;
    }
    return boost::none;
}

/**
 * If the final stage on shards is to unwind an array, move that stage to the merger. This cuts down
 * on network traffic and allows us to take advantage of reduced copying in unwind.
 */
void moveFinalUnwindFromShardsToMerger(Pipeline* shardPipe, Pipeline* mergePipe) {
    while (!shardPipe->getSources().empty() &&
           dynamic_cast<DocumentSourceUnwind*>(shardPipe->getSources().back().get())) {
        mergePipe->addInitialSource(shardPipe->popBack());
    }
}

/**
 * When the last stage of shard pipeline is $sort, move stages that can run on shards and don't
 * rename or modify the fields in $sort from merge pipeline. The function starts from the beginning
 * of the merge pipeline and finds the first consecutive eligible stages.
 */
void moveEligibleStreamingStagesBeforeSortOnShards(Pipeline* shardPipe,
                                                   Pipeline* mergePipe,
                                                   const BSONObj& sortPattern) {
    tassert(5363800,
            "Expected non-empty shardPipe consisting of at least a $sort stage",
            !shardPipe->getSources().empty());
    if (!dynamic_cast<DocumentSourceSort*>(shardPipe->getSources().back().get())) {
        // Expected last stage on the shards to be a $sort.
        return;
    }
    auto sortPaths = sortPattern.getFieldNames<std::set<std::string>>();
    auto firstMergeStage = mergePipe->getSources().cbegin();
    std::function<bool(DocumentSource*)> distributedPlanLogicCallback = [](DocumentSource* stage) {
        return !static_cast<bool>(stage->distributedPlanLogic());
    };
    auto [lastUnmodified, renameMap] = semantic_analysis::findLongestViablePrefixPreservingPaths(
        firstMergeStage, mergePipe->getSources().cend(), sortPaths, distributedPlanLogicCallback);
    for (const auto& sortPath : sortPaths) {
        auto pair = renameMap.find(sortPath);
        if (pair == renameMap.end() || pair->first != pair->second) {
            return;
        }
    }
    shardPipe->getSources().insert(shardPipe->getSources().end(), firstMergeStage, lastUnmodified);
    mergePipe->getSources().erase(firstMergeStage, lastUnmodified);
}

/**
 * Returns true if the final stage of the pipeline limits the number of documents it could output
 * (such as a $limit stage).
 *
 * This function is not meant to exhaustively catch every single case where a pipeline might have
 * some kind of limit. It's only here so that propagateDocLimitsToShards() can avoid adding an
 * obviously unnecessary $limit to a shard's pipeline.
 */
boost::optional<long long> getPipelineLimit(Pipeline* pipeline) {
    for (auto source_it = pipeline->getSources().rbegin();
         source_it != pipeline->getSources().rend();
         ++source_it) {
        const auto source = source_it->get();

        auto limitStage = dynamic_cast<DocumentSourceLimit*>(source);
        if (limitStage) {
            return limitStage->getLimit();
        }

        auto sortStage = dynamic_cast<DocumentSourceSort*>(source);
        if (sortStage) {
            return sortStage->getLimit();
        }

        auto cursorStage = dynamic_cast<DocumentSourceSort*>(source);
        if (cursorStage) {
            return cursorStage->getLimit();
        }

        // If this stage is one that can swap with a $limit stage, then we can look at the previous
        // stage to see if it includes a limit. Otherwise, we give up trying to find a limit on this
        // stage's output.
        if (!source->constraints().canSwapWithSkippingOrLimitingStage) {
            break;
        }
    }

    return boost::none;
}

/**
 * If the merging pipeline includes a $limit stage that creates an upper bound on how many input
 * documents it needs to compute the aggregation, we can use that as an upper bound on how many
 * documents each of the shards needs to produce. Propagating that upper bound to the shards (using
 * a $limit in the shard pipeline) can reduce the number of documents the shards need to process and
 * transfer over the network (see SERVER-36881).
 *
 * If there are $skip stages before the $limit, the skipped documents also contribute to the upper
 * bound.
 */
void propagateDocLimitToShards(Pipeline* shardPipe, Pipeline* mergePipe) {
    long long numDocumentsNeeded = 0;

    for (auto&& source : mergePipe->getSources()) {
        auto skipStage = dynamic_cast<DocumentSourceSkip*>(source.get());
        if (skipStage) {
            numDocumentsNeeded += skipStage->getSkip();
            continue;
        }

        auto limitStage = dynamic_cast<DocumentSourceLimit*>(source.get());
        if (limitStage) {
            numDocumentsNeeded += limitStage->getLimit();

            auto existingShardLimit = getPipelineLimit(shardPipe);
            if (existingShardLimit && *existingShardLimit <= numDocumentsNeeded) {
                // The sharding pipeline already has a limit that is no greater than the limit we
                // were going to add, so no changes are necessary.
                return;
            }

            auto shardLimit =
                DocumentSourceLimit::create(mergePipe->getContext(), numDocumentsNeeded);
            shardPipe->addFinalSource(shardLimit);

            // We have successfully applied a limit to the number of documents we need from each
            // shard.
            return;
        }

        // If there are any stages in the merge pipeline before the $skip and $limit stages, then we
        // cannot use the $limit to determine an upper bound, unless those stages could be swapped
        // with the $limit.
        if (!source->constraints().canSwapWithSkippingOrLimitingStage) {
            return;
        }
    }

    // We did not find any limit in the merge pipeline that would allow us to set an upper bound on
    // the number of documents we need from each shard.
    return;
}

/**
 * Adds a stage to the end of 'shardPipe' explicitly requesting all fields that 'mergePipe' needs.
 * This is only done if it heuristically determines that it is needed. This optimization can reduce
 * the amount of network traffic and can also enable the shards to convert less source BSON into
 * Documents.
 */
void limitFieldsSentFromShardsToMerger(Pipeline* shardPipe, Pipeline* mergePipe) {
    DepsTracker mergeDeps(mergePipe->getDependencies(DepsTracker::kNoMetadata));
    if (mergeDeps.needWholeDocument)
        return;  // the merge needs all fields, so nothing we can do.

    // Empty project is "special" so if no fields are needed, we just ask for _id instead.
    if (mergeDeps.fields.empty())
        mergeDeps.fields.insert("_id");

    // HEURISTIC: only apply optimization if none of the shard stages have an exhaustive list of
    // field dependencies. While this may not be 100% ideal in all cases, it is simple and
    // avoids the worst cases by ensuring that:
    // 1) Optimization IS applied when the shards wouldn't have known their exhaustive list of
    //    dependencies. This situation can happen when a $sort is before the first $project or
    //    $group. Without the optimization, the shards would have to reify and transmit full
    //    objects even though only a subset of fields are needed.
    // 2) Optimization IS NOT applied immediately following a $project or $group since it would
    //    add an unnecessary project (and therefore a deep-copy).
    for (auto&& source : shardPipe->getSources()) {
        DepsTracker dt(DepsTracker::kNoMetadata);
        if (source->getDependencies(&dt) & DepsTracker::State::EXHAUSTIVE_FIELDS)
            return;
    }
    // if we get here, add the project.
    boost::intrusive_ptr<DocumentSource> project = DocumentSourceProject::createFromBson(
        BSON("$project" << mergeDeps.toProjectionWithoutMetadata()).firstElement(),
        shardPipe->getContext());
    shardPipe->pushBack(project);
}

bool stageCanRunInParallel(const boost::intrusive_ptr<DocumentSource>& stage,
                           const std::set<std::string>& nameOfShardKeyFieldsUponEntryToStage) {
    if (stage->distributedPlanLogic()) {
        return stage->canRunInParallelBeforeWriteStage(nameOfShardKeyFieldsUponEntryToStage);
    } else {
        // This stage is fine to execute in parallel on each stream. For example, a $match can be
        // applied to each stream in parallel.
        return true;
    }
}

std::string mapToString(const StringMap<std::string>& map) {
    StringBuilder sb;
    sb << "{";
    for (auto&& entry : map) {
        if (sb.len() != 1) {
            sb << ", ";
        }
        sb << entry.first << ": " << entry.second;
    }
    sb << "}";
    return sb.str();
}

BSONObj buildNewKeyPattern(const ShardKeyPattern& shardKey, StringMap<std::string> renames) {
    BSONObjBuilder newPattern;
    for (auto&& elem : shardKey.getKeyPattern().toBSON()) {
        auto it = renames.find(elem.fieldNameStringData());
        invariant(it != renames.end(),
                  str::stream() << "Could not find new name of shard key field \""
                                << elem.fieldName() << "\": rename map was "
                                << mapToString(renames));
        newPattern.appendAs(elem, it->second);
    }
    return newPattern.obj();
}

StringMap<std::string> computeShardKeyRenameMap(const Pipeline* mergePipeline,
                                                std::set<std::string>&& pathsOfShardKey) {
    auto traversalStart = mergePipeline->getSources().crbegin();
    auto traversalEnd = mergePipeline->getSources().crend();
    const auto leadingGroup =
        dynamic_cast<DocumentSourceGroup*>(mergePipeline->getSources().front().get());
    if (leadingGroup && leadingGroup->doingMerge()) {
        // A leading $group stage will not report to preserve any fields, since it blows away the
        // _id and replaces it with something new. It possibly renames some fields, but when
        // computing the new shard key we are interested in the name of the shard key *in the middle
        // of the $group*. The $exchange will be inserted between the shard-local groups and the
        // global groups. Thus we want to exclude this stage from our rename tracking.
        traversalEnd = std::prev(traversalEnd);
    }
    auto renameMap = semantic_analysis::renamedPaths(traversalStart, traversalEnd, pathsOfShardKey);
    invariant(renameMap,
              str::stream()
                  << "Analyzed pipeline was thought to preserve the shard key fields, but did not: "
                  << Value(mergePipeline->serialize()).toString());
    return *renameMap;
}

/**
 * Returns true if any stage in the pipeline would modify any of the fields in 'shardKeyPaths', or
 * if there is any stage in the pipeline requires a unified stream to do its computation like a
 * $limit would.
 *
 * Purposefully takes 'shardKeyPaths' by value so that it can be modified throughout.
 */
bool anyStageModifiesShardKeyOrNeedsMerge(std::set<std::string> shardKeyPaths,
                                          const Pipeline* mergePipeline) {
    const auto& stages = mergePipeline->getSources();
    for (auto it = stages.crbegin(); it != stages.crend(); ++it) {
        const auto& stage = *it;
        auto renames = semantic_analysis::renamedPaths(
            std::move(shardKeyPaths), *stage, semantic_analysis::Direction::kBackward);
        if (!renames) {
            return true;
        }
        shardKeyPaths.clear();
        for (auto&& rename : *renames) {
            shardKeyPaths.insert(rename.second);
        }
        if (!stageCanRunInParallel(stage, shardKeyPaths)) {
            // In order for this stage to work it needs a single input stream which it wouldn't get
            // if we inserted an exchange before it.
            return true;
        }
    }
    return false;
}

boost::optional<ShardedExchangePolicy> walkPipelineBackwardsTrackingShardKey(
    OperationContext* opCtx, const Pipeline* mergePipeline, const ChunkManager& chunkManager) {

    const ShardKeyPattern& shardKey = chunkManager.getShardKeyPattern();
    std::set<std::string> shardKeyPaths;
    for (auto&& path : shardKey.getKeyPatternFields()) {
        shardKeyPaths.emplace(path->dottedField().toString());
    }
    if (anyStageModifiesShardKeyOrNeedsMerge(shardKeyPaths, mergePipeline)) {
        return boost::none;
    }

    // All the fields of the shard key are preserved by the pipeline, but they might be renamed. To
    // set up the $exchange, we need to build a fake shard key pattern which uses the names of the
    // shard key fields as they are at the split point of the pipeline.
    auto renames = computeShardKeyRenameMap(mergePipeline, std::move(shardKeyPaths));
    ShardKeyPattern newShardKey(buildNewKeyPattern(shardKey, renames));

    // Append the boundaries with the new names from the new shard key.
    auto translateBoundary = [&renames](const BSONObj& oldBoundary) {
        BSONObjBuilder bob;
        for (auto&& elem : oldBoundary) {
            bob.appendAs(elem, renames[elem.fieldNameStringData()]);
        }
        return bob.obj();
    };

    // Given the new shard key fields, build the distribution map.
    ExchangeSpec exchangeSpec;
    std::vector<BSONObj> boundaries;
    std::vector<int> consumerIds;
    std::map<ShardId, int> shardToConsumer;
    std::vector<ShardId> consumerShards;
    int numConsumers = 0;

    // The chunk manager enumerates the chunks in the ascending order from MinKey to MaxKey. Every
    // chunk has an associated range [from, to); i.e. inclusive lower bound and exclusive upper
    // bound. The chunk ranges must cover all domain without any holes. For the exchange we coalesce
    // ranges into a single vector of points. E.g. chunks [min,5], [5,10], [10,max] will produce
    // [min,5,10,max] vector. Number of points in the vector is always one greater than number of
    // chunks.
    // We also compute consumer indices for every chunk. From the example above (3 chunks) we may
    // get the vector [0,1,2]; i.e. the first chunk goes to the consumer 0 and so on. Note that
    // the consumer id may be repeated if the consumer hosts more than 1 chunk.
    chunkManager.forEachChunk([&](const auto& chunk) {
        if (boundaries.empty())
            boundaries.emplace_back(translateBoundary(chunk.getMin()));

        boundaries.emplace_back(translateBoundary(chunk.getMax()));
        if (shardToConsumer.find(chunk.getShardId()) == shardToConsumer.end()) {
            shardToConsumer.emplace(chunk.getShardId(), numConsumers++);
            consumerShards.emplace_back(chunk.getShardId());
        }
        consumerIds.emplace_back(shardToConsumer[chunk.getShardId()]);

        return true;
    });

    exchangeSpec.setPolicy(ExchangePolicyEnum::kKeyRange);
    exchangeSpec.setKey(newShardKey.toBSON());
    exchangeSpec.setBoundaries(std::move(boundaries));
    exchangeSpec.setConsumers(shardToConsumer.size());
    exchangeSpec.setConsumerIds(std::move(consumerIds));

    return ShardedExchangePolicy{std::move(exchangeSpec), std::move(consumerShards)};
}

/**
 * Non-correlated pipeline caching is only supported locally. When the
 * DocumentSourceSequentialDocumentCache stage has been moved to the shards pipeline, abandon the
 * associated local cache.
 */
void abandonCacheIfSentToShards(Pipeline* shardsPipeline) {
    for (auto&& stage : shardsPipeline->getSources()) {
        if (StringData(stage->getSourceName()) ==
            DocumentSourceSequentialDocumentCache::kStageName) {
            static_cast<DocumentSourceSequentialDocumentCache*>(stage.get())->abandonCache();
        }
    }
}


}  // namespace

std::unique_ptr<Pipeline, PipelineDeleter> targetShardsAndAddMergeCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    stdx::variant<std::unique_ptr<Pipeline, PipelineDeleter>, AggregateCommandRequest>
        targetRequest,
    boost::optional<BSONObj> shardCursorsSortSpec) {
    auto&& [aggRequest, pipeline] = [&] {
        return stdx::visit(
            visit_helper::Overloaded{
                [&](std::unique_ptr<Pipeline, PipelineDeleter>&& pipeline) {
                    return std::make_pair(
                        AggregateCommandRequest(expCtx->ns, pipeline->serializeToBson()),
                        std::move(pipeline));
                },
                [&](AggregateCommandRequest&& aggRequest) {
                    auto rawPipeline = aggRequest.getPipeline();
                    return std::make_pair(std::move(aggRequest),
                                          Pipeline::parse(std::move(rawPipeline), expCtx));
                }},
            std::move(targetRequest));
    }();

    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    // The default value for 'allowDiskUse' and 'maxTimeMS' in the AggregateCommandRequest may not
    // match what was set on the originating command, so copy it from the ExpressionContext.
    aggRequest.setAllowDiskUse(expCtx->allowDiskUse);

    if (auto maxTimeMS = expCtx->opCtx->getRemainingMaxTimeMillis();
        maxTimeMS < Microseconds::max()) {
        aggRequest.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
    }

    LiteParsedPipeline liteParsedPipeline(aggRequest);
    auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    auto shardDispatchResults =
        dispatchShardPipeline(aggregation_request_helper::serializeToCommandDoc(aggRequest),
                              hasChangeStream,
                              std::move(pipeline));

    std::vector<ShardId> targetedShards;
    targetedShards.reserve(shardDispatchResults.remoteCursors.size());
    for (auto&& remoteCursor : shardDispatchResults.remoteCursors) {
        targetedShards.emplace_back(remoteCursor->getShardId().toString());
    }

    std::unique_ptr<Pipeline, PipelineDeleter> mergePipeline;
    if (shardDispatchResults.splitPipeline) {
        mergePipeline = std::move(shardDispatchResults.splitPipeline->mergePipeline);
        if (shardDispatchResults.splitPipeline->shardCursorsSortSpec) {
            uassert(4929304, "Split pipeline provides its own sort already", !shardCursorsSortSpec);
            shardCursorsSortSpec = shardDispatchResults.splitPipeline->shardCursorsSortSpec;
        }
    } else {
        // We have not split the pipeline, and will execute entirely on the remote shards. Set up an
        // empty local pipeline which we will attach the merge cursors stage to.
        mergePipeline = Pipeline::parse(std::vector<BSONObj>(), expCtx);
    }

    addMergeCursorsSource(mergePipeline.get(),
                          shardDispatchResults.commandForTargetedShards,
                          std::move(shardDispatchResults.remoteCursors),
                          targetedShards,
                          shardCursorsSortSpec,
                          hasChangeStream);

    return mergePipeline;
}

std::unique_ptr<Pipeline, PipelineDeleter> runPipelineDirectlyOnSingleShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AggregateCommandRequest request,
    ShardId shardId) {
    invariant(!request.getExplain());

    auto readPreference = uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(
        request.getUnwrappedReadPref().value_or(BSONObj())));

    auto* opCtx = expCtx->opCtx;
    auto* catalogCache = Grid::get(opCtx)->catalogCache();
    auto cm =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, request.getNamespace()));

    auto versionedCmdObj = [&] {
        if (cm.isSharded()) {
            return appendShardVersion(aggregation_request_helper::serializeToCommandObj(request),
                                      cm.getVersion(shardId));
        } else {
            // The collection is unsharded. Don't append shard version info when contacting the
            // config servers.
            const auto cmdObjWithShardVersion = (shardId != ShardId::kConfigServerId)
                ? appendShardVersion(aggregation_request_helper::serializeToCommandObj(request),
                                     ChunkVersion::UNSHARDED())
                : aggregation_request_helper::serializeToCommandObj(request);
            return appendDbVersionIfPresent(std::move(cmdObjWithShardVersion), cm.dbVersion());
        }
    }();

    auto cursors = establishCursors(opCtx,
                                    expCtx->mongoProcessInterface->taskExecutor,
                                    request.getNamespace(),
                                    std::move(readPreference),
                                    {{shardId, versionedCmdObj}},
                                    false /* allowPartialResults */,
                                    Shard::RetryPolicy::kIdempotent);
    invariant(cursors.size() == 1);

    // Convert remote cursors into a vector of "owned" cursors.
    std::vector<OwnedRemoteCursor> ownedCursors;
    for (auto&& cursor : cursors) {
        auto cursorNss = cursor.getCursorResponse().getNSS();
        ownedCursors.emplace_back(opCtx, std::move(cursor), std::move(cursorNss));
    }

    // We have not split the pipeline, and will execute entirely on the remote shard. Set up an
    // empty local pipeline which we will attach the merge cursors stage to.
    auto mergePipeline = Pipeline::parse(std::vector<BSONObj>{}, expCtx);

    addMergeCursorsSource(mergePipeline.get(),
                          std::move(versionedCmdObj),
                          std::move(ownedCursors),
                          {std::move(shardId)},
                          boost::none /* shardCursorsSortSpec */,
                          false /* hasChangeStream */);

    return mergePipeline;
}

boost::optional<ShardedExchangePolicy> checkIfEligibleForExchange(OperationContext* opCtx,
                                                                  const Pipeline* mergePipeline) {
    if (internalQueryDisableExchange.load()) {
        return boost::none;
    }

    if (mergePipeline->getSources().empty()) {
        return boost::none;
    }

    auto mergeStage = dynamic_cast<DocumentSourceMerge*>(mergePipeline->getSources().back().get());
    if (!mergeStage) {
        // If there's no $merge stage we won't try to do an $exchange. For the $out stage there's no
        // point doing an $exchange because all the writes will go to a single node, so we should
        // just perform the merge on that host.
        return boost::none;
    }

    const auto cm =
        uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, mergeStage->getOutputNs()));
    if (!cm.isSharded()) {
        return boost::none;
    }

    // The collection is sharded and we have a $merge stage! Here we assume the $merge stage has
    // already verified that the shard key pattern is compatible with the unique key being used.
    // Assuming this, we just have to make sure the shard key is preserved (though possibly renamed)
    // all the way to the front of the merge pipeline. If this is the case then for any document
    // entering the merging pipeline we can predict which shard it will need to end up being
    // inserted on. With this ability we can insert an exchange on the shards to partition the
    // documents based on which shard will end up owning them. Then each shard can perform a merge
    // of only those documents which belong to it (optimistically, barring chunk migrations).
    return walkPipelineBackwardsTrackingShardKey(opCtx, mergePipeline, cm);
}

SplitPipeline splitPipeline(std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
    auto& expCtx = pipeline->getContext();
    // Re-brand 'pipeline' as the merging pipeline. We will move stages one by one from the merging
    // half to the shards, as possible.
    auto mergePipeline = std::move(pipeline);

    Pipeline::SourceContainer shardStages;
    boost::optional<BSONObj> inputsSort = findSplitPoint(&shardStages, mergePipeline.get());
    auto shardsPipeline = Pipeline::create(std::move(shardStages), expCtx);

    // The order in which optimizations are applied can have significant impact on the efficiency of
    // the final pipeline. Be Careful!
    if (inputsSort) {
        moveEligibleStreamingStagesBeforeSortOnShards(
            shardsPipeline.get(), mergePipeline.get(), *inputsSort);
    }
    moveFinalUnwindFromShardsToMerger(shardsPipeline.get(), mergePipeline.get());
    propagateDocLimitToShards(shardsPipeline.get(), mergePipeline.get());
    limitFieldsSentFromShardsToMerger(shardsPipeline.get(), mergePipeline.get());

    abandonCacheIfSentToShards(shardsPipeline.get());
    shardsPipeline->setSplitState(Pipeline::SplitState::kSplitForShards);
    mergePipeline->setSplitState(Pipeline::SplitState::kSplitForMerge);

    return {std::move(shardsPipeline), std::move(mergePipeline), std::move(inputsSort)};
}

BSONObj createPassthroughCommandForShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Document serializedCommand,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity,
    Pipeline* pipeline,
    BSONObj collationObj) {
    // Create the command for the shards.
    MutableDocument targetedCmd(serializedCommand);
    if (pipeline) {
        targetedCmd[AggregateCommandRequest::kPipelineFieldName] = Value(pipeline->serialize());
    }

    auto shardCommand =
        genericTransformForShards(std::move(targetedCmd), expCtx, explainVerbosity, collationObj);

    // Apply filter and RW concern to the final shard command.
    return CommandHelpers::filterCommandRequestForPassthrough(
        applyReadWriteConcern(expCtx->opCtx,
                              true,              /* appendRC */
                              !explainVerbosity, /* appendWC */
                              shardCommand));
}

BSONObj createCommandForTargetedShards(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       Document serializedCommand,
                                       const SplitPipeline& splitPipeline,
                                       const boost::optional<ShardedExchangePolicy> exchangeSpec,
                                       bool needsMerge) {
    // Create the command for the shards.
    MutableDocument targetedCmd(serializedCommand);
    // If we've parsed a pipeline on mongos, always override the pipeline, in case parsing it
    // has defaulted any arguments or otherwise changed the spec. For example, $listSessions may
    // have detected a logged in user and appended that user name to the $listSessions spec to
    // send to the shards.
    targetedCmd[AggregateCommandRequest::kPipelineFieldName] =
        Value(splitPipeline.shardsPipeline->serialize());

    // When running on many shards with the exchange we may not need merging.
    if (needsMerge) {
        targetedCmd[AggregateCommandRequest::kNeedsMergeFieldName] = Value(true);

        // If there aren't any stages like $out in the pipeline being sent to the shards, remove the
        // write concern. The write concern should only be applied when there are writes performed
        // to avoid mistakenly waiting for writes which didn't happen.
        const auto& shardsPipe = splitPipeline.shardsPipeline->getSources();
        if (!std::any_of(shardsPipe.begin(), shardsPipe.end(), [](const auto& stage) {
                return stage->constraints().writesPersistentData();
            })) {
            targetedCmd[WriteConcernOptions::kWriteConcernField] = Value();
        }
    }

    targetedCmd[AggregateCommandRequest::kCursorFieldName] =
        Value(DOC(aggregation_request_helper::kBatchSizeField << 0));

    targetedCmd[AggregateCommandRequest::kExchangeFieldName] =
        exchangeSpec ? Value(exchangeSpec->exchangeSpec.toBSON()) : Value();

    auto shardCommand = genericTransformForShards(
        std::move(targetedCmd), expCtx, expCtx->explain, expCtx->getCollatorBSON());

    // Apply RW concern to the final shard command.
    return applyReadWriteConcern(expCtx->opCtx,
                                 true,             /* appendRC */
                                 !expCtx->explain, /* appendWC */
                                 shardCommand);
}

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and
 * the pipeline that will need to be executed to merge the results from the remotes. If a stale
 * shard version is encountered, refreshes the routing table and tries again.
 */
DispatchShardPipelineResults dispatchShardPipeline(
    Document serializedCommand,
    bool hasChangeStream,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
    auto expCtx = pipeline->getContext();

    // The process is as follows:
    // - First, determine whether we need to target more than one shard. If so, we split the
    // pipeline; if not, we retain the existing pipeline.
    // - Call establishShardCursors to dispatch the aggregation to the targeted shards.
    // - Stale shard version errors are thrown up to the top-level handler, causing a retry on the
    // entire aggregation command.
    auto cursors = std::vector<RemoteCursor>();
    auto shardResults = std::vector<AsyncRequestsSender::Response>();
    auto opCtx = expCtx->opCtx;

    const bool needsPrimaryShardMerge =
        (pipeline->needsPrimaryShardMerger() || internalQueryAlwaysMergeOnPrimaryShard.load());

    const bool needsMongosMerge = pipeline->needsMongosMerger();

    const auto shardQuery = pipeline->getInitialQuery();

    auto executionNsRoutingInfoStatus = getExecutionNsRoutingInfo(opCtx, expCtx->ns);

    // If this is a $changeStream, we swallow NamespaceNotFound exceptions and continue.
    // Otherwise, uassert on all exceptions here.
    if (!(hasChangeStream && executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
        uassertStatusOK(executionNsRoutingInfoStatus);
    }

    auto executionNsRoutingInfo = executionNsRoutingInfoStatus.isOK()
        ? std::move(executionNsRoutingInfoStatus.getValue())
        : boost::optional<ChunkManager>{};

    // Determine whether we can run the entire aggregation on a single shard.
    const auto collationObj = expCtx->getCollatorBSON();
    const bool mustRunOnAll = mustRunOnAllShards(expCtx->ns, hasChangeStream);
    std::set<ShardId> shardIds =
        getTargetedShards(expCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, collationObj);

    // Don't need to split the pipeline if we are only targeting a single shard, unless:
    // - There is a stage that needs to be run on the primary shard and the single target shard
    //   is not the primary.
    // - The pipeline contains one or more stages which must always merge on mongoS.
    const bool needsSplit = (shardIds.size() > 1u || needsMongosMerge ||
                             (needsPrimaryShardMerge && executionNsRoutingInfo &&
                              *(shardIds.begin()) != executionNsRoutingInfo->dbPrimary()));

    boost::optional<ShardedExchangePolicy> exchangeSpec;
    boost::optional<SplitPipeline> splitPipelines;

    if (needsSplit) {
        LOGV2_DEBUG(20906,
                    5,
                    "Splitting pipeline: targeting = {shardIds_size} shards, needsMongosMerge = "
                    "{needsMongosMerge}, needsPrimaryShardMerge = {needsPrimaryShardMerge}",
                    "shardIds_size"_attr = shardIds.size(),
                    "needsMongosMerge"_attr = needsMongosMerge,
                    "needsPrimaryShardMerge"_attr = needsPrimaryShardMerge);
        splitPipelines = splitPipeline(std::move(pipeline));

        exchangeSpec = checkIfEligibleForExchange(opCtx, splitPipelines->mergePipeline.get());
    }

    // Generate the command object for the targeted shards.
    BSONObj targetedCommand =
        (splitPipelines
             ? createCommandForTargetedShards(
                   expCtx, serializedCommand, *splitPipelines, exchangeSpec, true /* needsMerge */)
             : createPassthroughCommandForShard(
                   expCtx, serializedCommand, expCtx->explain, pipeline.get(), collationObj));

    // A $changeStream pipeline must run on all shards, and will also open an extra cursor on the
    // config server in order to monitor for new shards. To guarantee that we do not miss any
    // shards, we must ensure that the list of shards to which we initially dispatch the pipeline is
    // at least as current as the logical time at which the stream begins scanning for new shards.
    // We therefore set 'shardRegistryReloadTime' to the current clusterTime and then hard-reload
    // the shard registry. We don't refresh for other pipelines that must run on all shards (e.g.
    // $currentOp) because, unlike $changeStream, those pipelines may not have been forced to split
    // if there was only one shard in the cluster when the command began execution. If a shard was
    // added since the earlier targeting logic ran, then refreshing here may cause us to illegally
    // target an unsplit pipeline to more than one shard.
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    auto shardRegistryReloadTime = currentTime.clusterTime().asTimestamp();
    if (hasChangeStream) {
        Grid::get(opCtx)->shardRegistry()->reload(opCtx);
        // Rebuild the set of shards as the shard registry might have changed.
        shardIds = getTargetedShards(
            expCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, collationObj);
    }

    // If there were no shards when we began execution, we wouldn't have run this aggregation in the
    // first place. Here, we double-check that the shards have not been removed mid-operation.
    uassert(ErrorCodes::ShardNotFound,
            "Unexpectedly found 0 shards while preparing to dispatch aggregation requests. Were "
            "the shards removed mid-operation?",
            shardIds.size() > 0);

    // Explain does not produce a cursor, so instead we scatter-gather commands to the shards.
    if (expCtx->explain) {
        if (mustRunOnAll) {
            // Some stages (such as $currentOp) need to be broadcast to all shards, and
            // should not participate in the shard version protocol.
            shardResults =
                scatterGatherUnversionedTargetAllShards(opCtx,
                                                        expCtx->ns.db(),
                                                        targetedCommand,
                                                        ReadPreferenceSetting::get(opCtx),
                                                        Shard::RetryPolicy::kIdempotent);
        } else {
            // Aggregations on a real namespace should use the routing table to target
            // shards, and should participate in the shard version protocol.
            invariant(executionNsRoutingInfo);
            shardResults =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           expCtx->ns.db(),
                                                           expCtx->ns,
                                                           *executionNsRoutingInfo,
                                                           targetedCommand,
                                                           ReadPreferenceSetting::get(opCtx),
                                                           Shard::RetryPolicy::kIdempotent,
                                                           shardQuery,
                                                           collationObj);
        }
    } else {
        cursors = establishShardCursors(opCtx,
                                        expCtx->mongoProcessInterface->taskExecutor,
                                        expCtx->ns,
                                        mustRunOnAll,
                                        executionNsRoutingInfo,
                                        shardIds,
                                        targetedCommand,
                                        ReadPreferenceSetting::get(opCtx));
        invariant(cursors.size() % shardIds.size() == 0,
                  str::stream() << "Number of cursors (" << cursors.size()
                                << ") is not a multiple of producers (" << shardIds.size() << ")");

        // For $changeStream, we must open an extra cursor on the 'config.shards' collection, so
        // that we can monitor for the addition of new shards inline with real events.
        if (hasChangeStream && expCtx->ns.db() != ShardType::ConfigNS.db()) {
            cursors.emplace_back(openChangeStreamNewShardMonitor(expCtx, shardRegistryReloadTime));
        }
    }

    // Convert remote cursors into a vector of "owned" cursors.
    std::vector<OwnedRemoteCursor> ownedCursors;
    for (auto&& cursor : cursors) {
        auto cursorNss = cursor.getCursorResponse().getNSS();
        ownedCursors.emplace_back(opCtx, std::move(cursor), std::move(cursorNss));
    }

    // Record the number of shards involved in the aggregation. If we are required to merge on
    // the primary shard, but the primary shard was not in the set of targeted shards, then we
    // must increment the number of involved shards.
    CurOp::get(opCtx)->debug().nShards = shardIds.size() +
        (needsPrimaryShardMerge && executionNsRoutingInfo &&
         !shardIds.count(executionNsRoutingInfo->dbPrimary()));

    return DispatchShardPipelineResults{needsPrimaryShardMerge,
                                        std::move(ownedCursors),
                                        std::move(shardResults),
                                        std::move(splitPipelines),
                                        std::move(pipeline),
                                        targetedCommand,
                                        shardIds.size(),
                                        exchangeSpec};
}

void addMergeCursorsSource(Pipeline* mergePipeline,
                           BSONObj cmdSentToShards,
                           std::vector<OwnedRemoteCursor> ownedCursors,
                           const std::vector<ShardId>& targetedShards,
                           boost::optional<BSONObj> shardCursorsSortSpec,
                           bool hasChangeStream) {
    auto* opCtx = mergePipeline->getContext()->opCtx;
    AsyncResultsMergerParams armParams;
    armParams.setSort(shardCursorsSortSpec);
    armParams.setTailableMode(mergePipeline->getContext()->tailableMode);
    armParams.setNss(mergePipeline->getContext()->ns);

    OperationSessionInfoFromClient sessionInfo;
    boost::optional<LogicalSessionFromClient> lsidFromClient;

    auto lsid = opCtx->getLogicalSessionId();
    if (lsid) {
        lsidFromClient.emplace(lsid->getId());
        lsidFromClient->setUid(lsid->getUid());
    }

    sessionInfo.setSessionId(lsidFromClient);
    sessionInfo.setTxnNumber(opCtx->getTxnNumber());

    if (TransactionRouter::get(opCtx)) {
        sessionInfo.setAutocommit(false);
    }

    armParams.setOperationSessionInfo(sessionInfo);

    // Convert owned cursors into a vector of remote cursors to be transferred to the merge
    // pipeline.
    std::vector<RemoteCursor> remoteCursors;
    for (auto&& cursor : ownedCursors) {
        // Transfer ownership of the remote cursor to the $mergeCursors stage.
        remoteCursors.emplace_back(cursor.releaseCursor());
    }

    armParams.setRemotes(std::move(remoteCursors));

    auto mergeCursorsStage =
        DocumentSourceMergeCursors::create(mergePipeline->getContext(), std::move(armParams));

    mergePipeline->addInitialSource(std::move(mergeCursorsStage));
}

Status appendExplainResults(DispatchShardPipelineResults&& dispatchResults,
                            const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                            BSONObjBuilder* result) {
    if (dispatchResults.splitPipeline) {
        auto* mergePipeline = dispatchResults.splitPipeline->mergePipeline.get();
        const char* mergeType = [&]() {
            if (mergePipeline->canRunOnMongos()) {
                if (mergeCtx->inMongos) {
                    return "mongos";
                }
                return "local";
            } else if (dispatchResults.exchangeSpec) {
                return "exchange";
            } else if (mergePipeline->needsPrimaryShardMerger()) {
                return "primaryShard";
            } else {
                return "anyShard";
            }
        }();

        *result << "mergeType" << mergeType;

        MutableDocument pipelinesDoc;
        // We specify "queryPlanner" verbosity when building the output for "shardsPart" because
        // execution stats are reported by each shard individually.
        pipelinesDoc.addField("shardsPart",
                              Value(dispatchResults.splitPipeline->shardsPipeline->writeExplainOps(
                                  ExplainOptions::Verbosity::kQueryPlanner)));
        if (dispatchResults.exchangeSpec) {
            BSONObjBuilder bob;
            dispatchResults.exchangeSpec->exchangeSpec.serialize(&bob);
            bob.append("consumerShards", dispatchResults.exchangeSpec->consumerShards);
            pipelinesDoc.addField("exchange", Value(bob.obj()));
        }
        // We specify "queryPlanner" verbosity because execution stats are not currently
        // supported when building the output for "mergerPart".
        pipelinesDoc.addField(
            "mergerPart",
            Value(mergePipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner)));

        *result << "splitPipeline" << pipelinesDoc.freeze();
    } else {
        *result << "splitPipeline" << BSONNULL;
    }

    BSONObjBuilder shardExplains(result->subobjStart("shards"));
    for (const auto& shardResult : dispatchResults.remoteExplainOutput) {
        invariant(shardResult.shardHostAndPort);

        uassertStatusOK(shardResult.swResponse.getStatus());
        uassertStatusOK(getStatusFromCommandResult(shardResult.swResponse.getValue().data));

        auto shardId = shardResult.shardId.toString();
        const auto& data = shardResult.swResponse.getValue().data;
        BSONObjBuilder explain(shardExplains.subobjStart(shardId));
        explain << "host" << shardResult.shardHostAndPort->toString();
        if (auto stagesElement = data["stages"]) {
            explain << "stages" << stagesElement;
        } else {
            auto queryPlannerElement = data["queryPlanner"];
            uassert(51157,
                    str::stream() << "Malformed explain response received from shard " << shardId
                                  << ": " << data.toString(),
                    queryPlannerElement);
            explain << "queryPlanner" << queryPlannerElement;
            if (auto executionStatsElement = data["executionStats"]) {
                explain << "executionStats" << executionStatsElement;
            }
        }
    }
    return Status::OK();
}

BSONObj targetShardsForExplain(Pipeline* ownedPipeline) {
    auto expCtx = ownedPipeline->getContext();
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));
    // The pipeline is going to be explained on the shards, and we don't want to send a
    // mergeCursors stage.
    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));
    invariant(expCtx->explain);
    // Generate the command object for the targeted shards.
    auto rawStages = [&pipeline]() {
        auto serialization = pipeline->serialize();
        std::vector<BSONObj> stages;
        stages.reserve(serialization.size());

        for (const auto& stageObj : serialization) {
            invariant(stageObj.getType() == BSONType::Object);
            stages.push_back(stageObj.getDocument().toBson());
        }

        return stages;
    }();

    AggregateCommandRequest aggRequest(expCtx->ns, rawStages);
    LiteParsedPipeline liteParsedPipeline(aggRequest);
    auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    auto shardDispatchResults =
        dispatchShardPipeline(aggregation_request_helper::serializeToCommandDoc(aggRequest),
                              hasChangeStream,
                              std::move(pipeline));
    BSONObjBuilder explainBuilder;
    auto appendStatus =
        appendExplainResults(std::move(shardDispatchResults), expCtx, &explainBuilder);
    uassertStatusOK(appendStatus);
    return BSON("pipeline" << explainBuilder.done());
}

StatusWith<ChunkManager> getExecutionNsRoutingInfo(OperationContext* opCtx,
                                                   const NamespaceString& execNss) {
    // First, verify that there are shards present in the cluster. If not, then we return the
    // stronger 'ShardNotFound' error rather than 'NamespaceNotFound'. We must do this because
    // $changeStream aggregations ignore NamespaceNotFound in order to allow streams to be opened on
    // a collection before its enclosing database is created. However, if there are no shards
    // present, then $changeStream should immediately return an empty cursor just as other
    // aggregations do when the database does not exist.
    const auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    if (shardIds.empty()) {
        return {ErrorCodes::ShardNotFound, "No shards are present in the cluster"};
    }

    // This call to getCollectionRoutingInfoForTxnCmd will return !OK if the database does not exist
    return getCollectionRoutingInfoForTxnCmd(opCtx, execNss);
}

Shard::RetryPolicy getDesiredRetryPolicy(OperationContext* opCtx) {
    // The idempotent retry policy will retry even for writeConcern failures, so only set it if the
    // pipeline does not support writeConcern.
    if (!opCtx->getWriteConcern().usedDefaultConstructedWC) {
        return Shard::RetryPolicy::kNotIdempotent;
    }
    return Shard::RetryPolicy::kIdempotent;
}

bool mustRunOnAllShards(const NamespaceString& nss, bool hasChangeStream) {
    // The following aggregations must be routed to all shards:
    // - Any collectionless aggregation, such as non-localOps $currentOp.
    // - Any aggregation which begins with a $changeStream stage.
    return nss.isCollectionlessAggregateNS() || hasChangeStream;
}

std::unique_ptr<Pipeline, PipelineDeleter> attachCursorToPipeline(Pipeline* ownedPipeline,
                                                                  bool allowTargetingShards) {
    auto expCtx = ownedPipeline->getContext();
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));
    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    if (expCtx->ns.isConfigDotCacheDotChunks()) {
        // We take special care to attach the local cursor stage to 'ownedPipeline' here rather than
        // attaching it to a serialized and re-parsed copy of the pipeline to avoid optimizations
        // such as the $sequentialCache stage from being lost. This is safe because each shard has
        // its own complete copy of any "config.cache.chunks.*" namespace.
        return expCtx->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
            pipeline.release());
    }

    auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
    return shardVersionRetry(
        expCtx->opCtx, catalogCache, expCtx->ns, "targeting pipeline to attach cursors"_sd, [&]() {
            auto pipelineToTarget = pipeline->clone();
            if (!allowTargetingShards || expCtx->ns.db() == "local") {
                // If the db is local, this may be a change stream examining the oplog. We know the
                // oplog (and any other local collections) will not be sharded.
                return expCtx->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
                    pipelineToTarget.release());
            }
            return targetShardsAndAddMergeCursors(expCtx, std::move(pipelineToTarget));
        });
}

}  // namespace mongo::sharded_agg_helpers
