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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding_util.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/resharding/document_source_resharding_iterate_transaction.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {
using namespace fmt::literals;

namespace {

UUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));

    auto uuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    invariant(uuid);

    return *uuid;
}

// Ensure that this shard owns the document. This must be called after verifying that we
// are in a resharding operation so that we are guaranteed that migrations are suspended.
bool documentBelongsToMe(OperationContext* opCtx,
                         CollectionShardingState* css,
                         const ScopedCollectionDescription& collDesc,
                         const BSONObj& doc) {
    auto currentKeyPattern = ShardKeyPattern(collDesc.getKeyPattern());
    auto ownershipFilter = css->getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);

    return ownershipFilter.keyBelongsToMe(currentKeyPattern.extractShardKeyFromDoc(doc));
}
}  // namespace

BSONObj serializeAndTruncateReshardingErrorIfNeeded(Status originalError) {
    BSONObjBuilder originalBob;
    originalError.serializeErrorToBSON(&originalBob);
    auto originalObj = originalBob.obj();

    if (originalObj.objsize() <= kReshardErrorMaxBytes ||
        originalError.code() == ErrorCodes::ReshardCollectionTruncatedError) {
        // The provided originalError either meets the size constraints or has already been
        // truncated (and is just slightly larger than 2000 bytes to avoid complicating the
        // truncation math).
        return originalObj;
    }

    // ReshardCollectionAborted has special internal handling. It should always have a short, fixed
    // error message so it never exceeds the size limit and requires truncation and error code
    // substitution.
    invariant(originalError.code() != ErrorCodes::ReshardCollectionAborted);

    auto originalErrorStr = originalError.toString();
    auto truncatedErrorStr =
        str::UTF8SafeTruncation(StringData(originalErrorStr), kReshardErrorMaxBytes);
    Status truncatedError{ErrorCodes::ReshardCollectionTruncatedError, truncatedErrorStr};
    BSONObjBuilder truncatedBob;
    truncatedError.serializeErrorToBSON(&truncatedBob);
    return truncatedBob.obj();
}

DonorShardEntry makeDonorShard(ShardId shardId,
                               DonorStateEnum donorState,
                               boost::optional<Timestamp> minFetchTimestamp,
                               boost::optional<Status> abortReason) {
    DonorShardContext donorCtx;
    donorCtx.setState(donorState);
    emplaceMinFetchTimestampIfExists(donorCtx, minFetchTimestamp);
    emplaceTruncatedAbortReasonIfExists(donorCtx, abortReason);

    return DonorShardEntry{std::move(shardId), std::move(donorCtx)};
}

RecipientShardEntry makeRecipientShard(ShardId shardId,
                                       RecipientStateEnum recipientState,
                                       boost::optional<Status> abortReason) {
    RecipientShardContext recipientCtx;
    recipientCtx.setState(recipientState);
    emplaceTruncatedAbortReasonIfExists(recipientCtx, abortReason);

    return RecipientShardEntry{std::move(shardId), std::move(recipientCtx)};
}

UUID getCollectionUUIDFromChunkManger(const NamespaceString& originalNss, const ChunkManager& cm) {
    auto collectionUUID = cm.getUUID();
    uassert(ErrorCodes::InvalidUUID,
            "Cannot reshard collection {} due to missing UUID"_format(originalNss.ns()),
            collectionUUID);

    return collectionUUID.get();
}

NamespaceString constructTemporaryReshardingNss(StringData db, const UUID& sourceUuid) {
    return NamespaceString(db,
                           fmt::format("{}{}",
                                       NamespaceString::kTemporaryReshardingCollectionPrefix,
                                       sourceUuid.toString()));
}

std::set<ShardId> getRecipientShards(OperationContext* opCtx,
                                     const NamespaceString& sourceNss,
                                     const UUID& reshardingUUID) {
    const auto& tempNss = constructTemporaryReshardingNss(sourceNss.db(), reshardingUUID);
    auto* catalogCache = Grid::get(opCtx)->catalogCache();
    auto cm = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, tempNss));

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Expected collection " << tempNss << " to be sharded",
            cm.isSharded());

    std::set<ShardId> recipients;
    cm.getAllShardIds(&recipients);
    return recipients;
}

void checkForHolesAndOverlapsInChunks(std::vector<ReshardedChunk>& chunks,
                                      const KeyPattern& keyPattern) {
    std::sort(chunks.begin(), chunks.end(), [](const ReshardedChunk& a, const ReshardedChunk& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a.getMin() < b.getMin());
    });
    // Check for global minKey and maxKey
    uassert(ErrorCodes::BadValue,
            "Chunk range must start at global min for new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(chunks.front().getMin() ==
                                                        keyPattern.globalMin()));
    uassert(ErrorCodes::BadValue,
            "Chunk range must end at global max for new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(chunks.back().getMax() ==
                                                        keyPattern.globalMax()));

    boost::optional<BSONObj> prevMax = boost::none;
    for (auto chunk : chunks) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Chunk ranges must be contiguous",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.get() == chunk.getMin()));
        }
        prevMax = boost::optional<BSONObj>(chunk.getMax());
    }
}

void validateReshardedChunks(const std::vector<ReshardedChunk>& chunks,
                             OperationContext* opCtx,
                             const KeyPattern& keyPattern) {
    std::vector<ReshardedChunk> validChunks;
    for (const auto& chunk : chunks) {
        uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getRecipientShardId()));
        validChunks.push_back(chunk);
    }
    checkForHolesAndOverlapsInChunks(validChunks, keyPattern);
}

Timestamp getHighestMinFetchTimestamp(const std::vector<DonorShardEntry>& donorShards) {
    invariant(!donorShards.empty());

    auto maxMinFetchTimestamp = Timestamp::min();
    for (auto& donor : donorShards) {
        auto donorFetchTimestamp = donor.getMutableState().getMinFetchTimestamp();
        uassert(4957300,
                "All donors must have a minFetchTimestamp, but donor {} does not."_format(
                    StringData{donor.getId()}),
                donorFetchTimestamp.is_initialized());
        if (maxMinFetchTimestamp < donorFetchTimestamp.value()) {
            maxMinFetchTimestamp = donorFetchTimestamp.value();
        }
    }
    return maxMinFetchTimestamp;
}

void checkForOverlappingZones(std::vector<ReshardingZoneType>& zones) {
    std::sort(
        zones.begin(), zones.end(), [](const ReshardingZoneType& a, const ReshardingZoneType& b) {
            return SimpleBSONObjComparator::kInstance.evaluate(a.getMin() < b.getMin());
        });

    boost::optional<BSONObj> prevMax = boost::none;
    for (auto zone : zones) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Zone ranges must not overlap",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.get() <= zone.getMin()));
        }
        prevMax = boost::optional<BSONObj>(zone.getMax());
    }
}

std::vector<BSONObj> buildTagsDocsFromZones(const NamespaceString& tempNss,
                                            const std::vector<ReshardingZoneType>& zones) {
    std::vector<BSONObj> tags;
    tags.reserve(zones.size());
    for (const auto& zone : zones) {
        ChunkRange range(zone.getMin(), zone.getMax());
        TagsType tag(tempNss, zone.getZone().toString(), range);
        tags.push_back(tag.toBSON());
    }

    return tags;
}

std::unique_ptr<Pipeline, PipelineDeleter> createOplogFetchingPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ReshardingDonorOplogId& startAfter,
    UUID collUUID,
    const ShardId& recipientShard) {
    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;
    const Value EXISTS = V{Doc{{"$exists", true}}};
    const Value DNE = V{Doc{{"$exists", false}}};

    Pipeline::SourceContainer stages;
    // The node receiving the query verifies continuity of oplog entries (i.e: that the recipient
    // hasn't fallen off the oplog). This stage provides the input timestamp that the donor uses for
    // verification.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"ts", Doc{{"$gte", startAfter.getTs()}}}}.toBson(), expCtx));

    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             // Only capture CRUD operations relevant for the `destinedRecipient`.
             Arr{V{Doc{{"op", Doc{{"$in", Arr{V{"i"_sd}, V{"u"_sd}, V{"d"_sd}, V{"n"_sd}}}}},
                       {"ui", collUUID},
                       {"destinedRecipient", recipientShard.toString()}}},
                 // Capture all commands. One cannot determine if a command is relevant to the
                 // `destinedRecipient` until after oplog chaining via `prevOpTime` is resolved.
                 V{Doc{{"op", "c"_sd},
                       {"o.applyOps", EXISTS},
                       {"o.partialTxn", DNE},
                       {"o.prepare", DNE}}},
                 V{Doc{{"op", "c"_sd}, {"o.commitTransaction", EXISTS}}},
                 V{Doc{{"op", "c"_sd}, {"o.abortTransaction", EXISTS}}},
                 V{Doc{{"op", "c"_sd}, {"ui", collUUID}}}}}}
            .toBson(),
        expCtx));

    // Emits transaction entries chronologically, and adds _id to all events in the stream.
    stages.emplace_back(DocumentSourceReshardingIterateTransaction::create(expCtx));

    // Filter out applyOps entries which do not contain any relevant operations.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             Arr{V{Doc{{"op", Doc{{"$ne", "c"_sd}}}}},
                 V{Doc{{"op", "c"_sd}, {"o.applyOps", DNE}}},
                 V{Doc{{"op", "c"_sd},
                       {"o.applyOps",
                        Doc{{"$elemMatch",
                             Doc{{"destinedRecipient", recipientShard.toString()},
                                 {"ui", collUUID}}}}}}}}}}
            .toBson(),
        expCtx));

    // Now that the chained oplog entries are adjacent with an annotated `ReshardingDonorOplogId`,
    // the pipeline can prune anything earlier than the resume time.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"_id", Doc{{"$gt", startAfter.toBSON()}}}}.toBson(), expCtx));

    // Filter out anything inside of an `applyOps` specifically destined for another shard. This
    // ensures zone restrictions are obeyed. Data will never be sent to a shard that it isn't meant
    // to end up on.
    stages.emplace_back(DocumentSourceAddFields::create(
        Doc{{"o.applyOps",
             Doc{{"$cond",
                  Doc{{"if", Doc{{"$eq", Arr{V{"$op"_sd}, V{"c"_sd}}}}},
                      {"then",
                       Doc{{"$filter",
                            Doc{{"input", "$o.applyOps"_sd},
                                {"cond",
                                 Doc{{"$and",
                                      Arr{V{Doc{{"$eq", Arr{V{"$$this.ui"_sd}, V{collUUID}}}}},
                                          V{Doc{{"$eq",
                                                 Arr{V{"$$this.destinedRecipient"_sd},
                                                     V{recipientShard.toString()}}}}}}}}}}}}},
                      {"else", "$o.applyOps"_sd}}}}}}
            .toBson(),
        expCtx));

    return Pipeline::create(std::move(stages), expCtx);
}

boost::optional<ShardId> getDestinedRecipient(OperationContext* opCtx,
                                              const NamespaceString& sourceNss,
                                              const BSONObj& fullDocument,
                                              CollectionShardingState* css,
                                              const ScopedCollectionDescription& collDesc) {
    if (!ShardingState::get(opCtx)->enabled()) {
        // Don't bother looking up the sharding state for the collection if the server isn't even
        // running with sharding enabled. We know there couldn't possibly be any resharding fields.
        return boost::none;
    }

    auto reshardingKeyPattern = collDesc.getReshardingKeyIfShouldForwardOps();
    if (!reshardingKeyPattern)
        return boost::none;

    if (!documentBelongsToMe(opCtx, css, collDesc, fullDocument))
        return boost::none;

    bool allowLocks = true;
    auto tempNssRoutingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
            opCtx,
            constructTemporaryReshardingNss(sourceNss.db(), getCollectionUuid(opCtx, sourceNss)),
            allowLocks));

    auto shardKey = reshardingKeyPattern->extractShardKeyFromDocThrows(fullDocument);

    return tempNssRoutingInfo.findIntersectingChunkWithSimpleCollation(shardKey).getShardId();
}

bool isFinalOplog(const repl::OplogEntry& oplog) {
    if (oplog.getOpType() != repl::OpTypeEnum::kNoop) {
        return false;
    }

    auto o2Field = oplog.getObject2();
    if (!o2Field) {
        return false;
    }

    return o2Field->getField("type").valueStringDataSafe() == kReshardFinalOpLogType;
}

bool isFinalOplog(const repl::OplogEntry& oplog, UUID reshardingUUID) {
    if (!isFinalOplog(oplog)) {
        return false;
    }

    return uassertStatusOK(UUID::parse(oplog.getObject2()->getField("reshardingUUID"))) ==
        reshardingUUID;
}


NamespaceString getLocalOplogBufferNamespace(UUID existingUUID, ShardId donorShardId) {
    return NamespaceString("config.localReshardingOplogBuffer.{}.{}"_format(
        existingUUID.toString(), donorShardId.toString()));
}

NamespaceString getLocalConflictStashNamespace(UUID existingUUID, ShardId donorShardId) {
    return NamespaceString{NamespaceString::kConfigDb,
                           "localReshardingConflictStash.{}.{}"_format(existingUUID.toString(),
                                                                       donorShardId.toString())};
}
}  // namespace mongo
