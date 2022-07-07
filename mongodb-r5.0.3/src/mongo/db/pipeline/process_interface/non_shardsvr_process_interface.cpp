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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/process_interface/non_shardsvr_process_interface.h"

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/list_indexes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/pipeline/document_source_cursor.h"

namespace mongo {

std::unique_ptr<Pipeline, PipelineDeleter>
NonShardServerProcessInterface::attachCursorSourceToPipeline(Pipeline* ownedPipeline,
                                                             bool allowTargetingShards) {
    return attachCursorSourceToPipelineForLocalRead(ownedPipeline);
}

std::list<BSONObj> NonShardServerProcessInterface::getIndexSpecs(OperationContext* opCtx,
                                                                 const NamespaceString& ns,
                                                                 bool includeBuildUUIDs) {
    return listIndexesEmptyListIfMissing(opCtx, ns, includeBuildUUIDs);
}

std::pair<std::vector<FieldPath>, bool>
NonShardServerProcessInterface::collectDocumentKeyFieldsForHostedCollection(
    OperationContext* opCtx, const NamespaceString& nss, UUID uuid) const {
    return {{"_id"}, false};  // Nothing is sharded.
}

std::vector<FieldPath> NonShardServerProcessInterface::collectDocumentKeyFieldsActingAsRouter(
    OperationContext* opCtx, const NamespaceString& nss) const {
    return {"_id"};  // Nothing is sharded.
}

Status NonShardServerProcessInterface::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const NamespaceString& ns,
                                              std::vector<BSONObj>&& objs,
                                              const WriteConcernOptions& wc,
                                              boost::optional<OID> targetEpoch) {
    auto writeResults = write_ops_exec::performInserts(
        expCtx->opCtx, buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    // Need to check each result in the batch since the writes are unordered.
    for (const auto& result : writeResults.results) {
        if (result.getStatus() != Status::OK()) {
            return result.getStatus();
        }
    }
    return Status::OK();
}

StatusWith<MongoProcessInterface::UpdateResult> NonShardServerProcessInterface::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    BatchedObjects&& batch,
    const WriteConcernOptions& wc,
    UpsertType upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    auto writeResults = write_ops_exec::performUpdates(
        expCtx->opCtx, buildUpdateOp(expCtx, ns, std::move(batch), upsert, multi));

    // Need to check each result in the batch since the writes are unordered.
    UpdateResult updateResult;
    for (const auto& result : writeResults.results) {
        if (result.getStatus() != Status::OK()) {
            return result.getStatus();
        }

        updateResult.nMatched += result.getValue().getN();
        updateResult.nModified += result.getValue().getNModified();
    }
    return updateResult;
}

void NonShardServerProcessInterface::createIndexesOnEmptyCollection(
    OperationContext* opCtx, const NamespaceString& ns, const std::vector<BSONObj>& indexSpecs) {
    AutoGetCollection autoColl(opCtx, ns, MODE_X);
    CollectionWriter collection(autoColl);
    writeConflictRetry(
        opCtx, "CommonMongodProcessInterface::createIndexesOnEmptyCollection", ns.ns(), [&] {
            uassert(ErrorCodes::DatabaseDropPending,
                    str::stream() << "The database is in the process of being dropped " << ns.db(),
                    autoColl.getDb() && !autoColl.getDb()->isDropPending(opCtx));

            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Failed to create indexes for aggregation because collection "
                                     "does not exist: "
                                  << ns << ": " << BSON("indexes" << indexSpecs),
                    collection.get());

            invariant(0U == collection->numRecords(opCtx),
                      str::stream() << "Expected empty collection for index creation: " << ns
                                    << ": numRecords: " << collection->numRecords(opCtx) << ": "
                                    << BSON("indexes" << indexSpecs));

            // Secondary index builds do not filter existing indexes so we have to do this on the
            // primary.
            auto removeIndexBuildsToo = false;
            auto filteredIndexes = collection->getIndexCatalog()->removeExistingIndexes(
                opCtx, collection.get(), indexSpecs, removeIndexBuildsToo);
            if (filteredIndexes.empty()) {
                return;
            }

            WriteUnitOfWork wuow(opCtx);
            IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                opCtx, collection, filteredIndexes, false  // fromMigrate
            );
            wuow.commit();
        });
}
void NonShardServerProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& targetNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    NamespaceString sourceNs = NamespaceString(renameCommandObj["renameCollection"].String());
    RenameCollectionOptions options;
    options.dropTarget = renameCommandObj["dropTarget"].trueValue();
    options.stayTemp = renameCommandObj["stayTemp"].trueValue();
    // skip sharding validation on non sharded servers
    doLocalRenameIfOptionsAndIndexesHaveNotChanged(
        opCtx, sourceNs, targetNs, options, originalIndexes, originalCollectionOptions);
}

void NonShardServerProcessInterface::createCollection(OperationContext* opCtx,
                                                      const std::string& dbName,
                                                      const BSONObj& cmdObj) {
    uassertStatusOK(mongo::createCollection(opCtx, dbName, cmdObj));
}

void NonShardServerProcessInterface::dropCollection(OperationContext* opCtx,
                                                    const NamespaceString& ns) {
    uassertStatusOK(mongo::dropCollectionForApplyOps(
        opCtx, ns, {}, DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
}

BSONObj NonShardServerProcessInterface::preparePipelineAndExplain(
    Pipeline* ownedPipeline, ExplainOptions::Verbosity verbosity) {
    std::vector<Value> pipelineVec;
    auto firstStage = ownedPipeline->peekFront();
    // If the pipeline already has a cursor explain with that one, otherwise attach a new one like
    // we would for a normal execution and explain that.
    if (firstStage && typeid(*firstStage) == typeid(DocumentSourceCursor)) {
        // Managed pipeline goes out of scope at the end of this else block, but we've already
        // extracted the necessary information and won't need it again.
        std::unique_ptr<Pipeline, PipelineDeleter> managedPipeline(
            ownedPipeline, PipelineDeleter(ownedPipeline->getContext()->opCtx));
        pipelineVec = managedPipeline->writeExplainOps(verbosity);
        ownedPipeline = nullptr;
    } else {
        auto pipelineWithCursor = attachCursorSourceToPipelineForLocalRead(ownedPipeline);
        // If we need execution stats, this runs the plan in order to gather the stats.
        if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
            while (pipelineWithCursor->getNext()) {
            }
        }
        pipelineVec = pipelineWithCursor->writeExplainOps(verbosity);
    }
    BSONArrayBuilder bab;
    for (auto&& stage : pipelineVec) {
        bab << stage;
    }

    return BSON("pipeline" << bab.arr());
}

}  // namespace mongo
