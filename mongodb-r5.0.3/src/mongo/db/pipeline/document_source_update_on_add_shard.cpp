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

#include "mongo/db/pipeline/document_source_update_on_add_shard.h"

#include <algorithm>

#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/async_results_merger_params_gen.h"
#include "mongo/s/query/establish_cursors.h"

namespace mongo {
namespace {

// Returns true if the change stream document is an event in 'config.shards'.
bool isShardConfigEvent(const Document& eventDoc) {
    // TODO SERVER-44039: we continue to generate 'kNewShardDetected' events for compatibility
    // with 4.2, even though we no longer rely on them to detect new shards. We swallow the event
    // here. We may wish to remove this mechanism entirely in 4.7+, or retain it for future cases
    // where a change stream is targeted to a subset of shards. See SERVER-44039 for details.
    if (eventDoc[DocumentSourceChangeStream::kOperationTypeField].getStringData() ==
        DocumentSourceChangeStream::kNewShardDetectedOpType) {
        return true;
    }
    auto nsObj = eventDoc[DocumentSourceChangeStream::kNamespaceField];
    return nsObj.getType() == BSONType::Object &&
        nsObj["db"_sd].getStringData() == ShardType::ConfigNS.db() &&
        nsObj["coll"_sd].getStringData() == ShardType::ConfigNS.coll();
}
}  // namespace

boost::intrusive_ptr<DocumentSourceUpdateOnAddShard> DocumentSourceUpdateOnAddShard::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceUpdateOnAddShard(expCtx);
}

DocumentSourceUpdateOnAddShard::DocumentSourceUpdateOnAddShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {}

DocumentSource::GetNextResult DocumentSourceUpdateOnAddShard::doGetNext() {
    // For the first call to the 'doGetNext', the '_mergeCursors' will be null and must be
    // populated. We also resolve the original aggregation command from the expression context.
    if (!_mergeCursors) {
        _mergeCursors = dynamic_cast<DocumentSourceMergeCursors*>(pSource);
        _originalAggregateCommand = pExpCtx->originalAggregateCommand.getOwned();

        tassert(5549100, "Missing $mergeCursors stage", _mergeCursors);
        tassert(
            5549101, "Empty $changeStream command object", !_originalAggregateCommand.isEmpty());
    }

    auto childResult = pSource->getNext();

    // If this is an insertion into the 'config.shards' collection, open a cursor on the new shard.
    while (childResult.isAdvanced() && isShardConfigEvent(childResult.getDocument())) {
        auto opType = childResult.getDocument()[DocumentSourceChangeStream::kOperationTypeField];
        if (opType.getStringData() == DocumentSourceChangeStream::kInsertOpType) {
            addNewShardCursors(childResult.getDocument());
        }
        // For shard removal or update, we do nothing. We also swallow kNewShardDetectedOpType.
        childResult = pSource->getNext();
    }
    return childResult;
}

void DocumentSourceUpdateOnAddShard::addNewShardCursors(const Document& newShardDetectedObj) {
    _mergeCursors->addNewShardCursors(establishShardCursorsOnNewShards(newShardDetectedObj));
}

std::vector<RemoteCursor> DocumentSourceUpdateOnAddShard::establishShardCursorsOnNewShards(
    const Document& newShardDetectedObj) {
    // Reload the shard registry to see the new shard.
    auto* opCtx = pExpCtx->opCtx;
    Grid::get(opCtx)->shardRegistry()->reload(opCtx);

    // Parse the new shard's information from the document inserted into 'config.shards'.
    auto newShardSpec = newShardDetectedObj[DocumentSourceChangeStream::kFullDocumentField];
    auto newShard = uassertStatusOK(ShardType::fromBSON(newShardSpec.getDocument().toBson()));

    // Make sure we are not attempting to open a cursor on a shard that already has one.
    if (_mergeCursors->getShardIds().count(newShard.getName()) != 0) {
        return {};
    }

    auto cmdObj = createUpdatedCommandForNewShard(
        newShardDetectedObj[DocumentSourceChangeStream::kClusterTimeField].getTimestamp());

    const bool allowPartialResults = false;  // partial results are not allowed
    return establishCursors(opCtx,
                            pExpCtx->mongoProcessInterface->taskExecutor,
                            pExpCtx->ns,
                            ReadPreferenceSetting::get(opCtx),
                            {{newShard.getName(), cmdObj}},
                            allowPartialResults);
}

BSONObj DocumentSourceUpdateOnAddShard::createUpdatedCommandForNewShard(Timestamp shardAddedTime) {
    // We must start the new cursor from the moment at which the shard became visible.
    const auto newShardAddedTime = LogicalTime{shardAddedTime};
    auto resumeTokenForNewShard =
        ResumeToken::makeHighWaterMarkToken(newShardAddedTime.addTicks(1).asTimestamp());

    // Create a new shard command object containing the new resume token.
    auto shardCommand = replaceResumeTokenInCommand(resumeTokenForNewShard.toDocument());

    auto* opCtx = pExpCtx->opCtx;
    bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);

    // Create the 'AggregateCommandRequest' object which will help in creating the parsed pipeline.
    auto aggCmdRequest = aggregation_request_helper::parseFromBSON(
        pExpCtx->ns, shardCommand, boost::none, apiStrict);

    // Parse and optimize the pipeline.
    auto pipeline = Pipeline::parse(aggCmdRequest.getPipeline(), pExpCtx);
    pipeline->optimizePipeline();

    // Split the full pipeline to get the shard pipeline.
    auto splitPipelines = sharded_agg_helpers::splitPipeline(std::move(pipeline));

    // Create the new command that will run on the shard.
    return sharded_agg_helpers::createCommandForTargetedShards(pExpCtx,
                                                               Document{shardCommand},
                                                               splitPipelines,
                                                               boost::none, /* exhangeSpec */
                                                               true /* needsMerge */);
}

BSONObj DocumentSourceUpdateOnAddShard::replaceResumeTokenInCommand(Document resumeToken) {
    Document originalCmd(_originalAggregateCommand);
    auto pipeline = originalCmd[AggregateCommandRequest::kPipelineFieldName].getArray();

    // A $changeStream must be the first element of the pipeline in order to be able
    // to replace (or add) a resume token.
    tassert(5549102,
            "Invalid $changeStream command object",
            !pipeline[0][DocumentSourceChangeStream::kStageName].missing());

    MutableDocument changeStreamStage(
        pipeline[0][DocumentSourceChangeStream::kStageName].getDocument());
    changeStreamStage[DocumentSourceChangeStreamSpec::kResumeAfterFieldName] = Value(resumeToken);

    // If the command was initially specified with a startAtOperationTime, we need to remove it to
    // use the new resume token.
    changeStreamStage[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName] = Value();
    pipeline[0] =
        Value(Document{{DocumentSourceChangeStream::kStageName, changeStreamStage.freeze()}});
    MutableDocument newCmd(std::move(originalCmd));
    newCmd[AggregateCommandRequest::kPipelineFieldName] = Value(pipeline);
    return newCmd.freeze().toBson();
}

}  // namespace mongo
