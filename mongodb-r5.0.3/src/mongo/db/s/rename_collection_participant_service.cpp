/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/drop_collection_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/recoverable_critical_section_service.h"
#include "mongo/db/s/rename_collection_participant_service.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"

namespace mongo {

namespace {

/*
 * Drop the collection locally and clear stale metadata from cache collections.
 */
void dropCollectionLocally(OperationContext* opCtx, const NamespaceString& nss) {
    bool knownNss = [&]() {
        try {
            DropCollectionCoordinator::dropCollectionLocally(opCtx, nss);
            return true;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            return false;
        }
    }();

    LOGV2_DEBUG(5515100,
                1,
                "Dropped target collection locally on renameCollection participant",
                "namespace"_attr = nss,
                "collectionExisted"_attr = knownNss);
}

/*
 * Rename the collection if exists locally, otherwise simply drop the target collection.
 */
void renameOrDropTarget(OperationContext* opCtx,
                        const NamespaceString& fromNss,
                        const NamespaceString& toNss,
                        const RenameCollectionOptions& options,
                        const UUID& sourceUUID,
                        const boost::optional<UUID>& targetUUID) {
    {
        Lock::DBLock dbLock(opCtx, toNss.db(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, toNss, MODE_IS);
        const auto targetCollPtr =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, toNss);
        if (targetCollPtr) {
            if (targetCollPtr->uuid() == sourceUUID) {
                // Early return if the rename previously succeeded
                return;
            }
            uassert(5807602,
                    str::stream() << "Target collection " << toNss
                                  << " UUID does not match the provided UUID.",
                    !targetUUID || targetCollPtr->uuid() == *targetUUID);
        }
    }

    {
        Lock::DBLock dbLock(opCtx, fromNss.db(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, fromNss, MODE_IS);
        // ensure idempotency by checking sourceUUID
        const auto sourceCollPtr =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, fromNss);
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "Source Collection " << fromNss
                              << " UUID does not match provided uuid.",
                !sourceCollPtr || sourceCollPtr->uuid() == sourceUUID);
    }

    try {
        validateAndRunRenameCollection(opCtx, fromNss, toNss, options);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // It's ok for a participant shard to have no knowledge about a collection
        LOGV2_DEBUG(5515101,
                    1,
                    "Source namespace not found while trying to rename collection on participant",
                    "namespace"_attr = fromNss);
        dropCollectionLocally(opCtx, toNss);
        deleteRangeDeletionTasksForRename(opCtx, fromNss, toNss);
    }
}
}  // namespace

RenameCollectionParticipantService* RenameCollectionParticipantService::getService(
    OperationContext* opCtx) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(kServiceName);
    return checked_cast<RenameCollectionParticipantService*>(std::move(service));
}

std::shared_ptr<RenameCollectionParticipantService::Instance>
RenameCollectionParticipantService::constructInstance(BSONObj initialState) {
    LOGV2_DEBUG(5515102,
                2,
                "Constructing new rename participant",
                "renameParticipantDoc"_attr = initialState);
    return std::make_shared<RenameParticipantInstance>(std::move(initialState));
}

RenameParticipantInstance::~RenameParticipantInstance() {
    invariant(_unblockCRUDPromise.getFuture().isReady());
}

bool RenameParticipantInstance::hasSameOptions(const BSONObj& participantDoc) {
    const auto otherDoc = RenameCollectionParticipantDocument::parse(
        IDLParserErrorContext("RenameCollectionParticipantDocument"), participantDoc);

    const auto& selfReq = _doc.getRenameCollectionRequest().toBSON();
    const auto& otherReq = otherDoc.getRenameCollectionRequest().toBSON();

    return SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq);
}

boost::optional<BSONObj> RenameParticipantInstance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder cmdBob;
    if (const auto& optComment = _doc.getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "RenameParticipantInstance");
    bob.append("op", "command");
    bob.append("ns", fromNss().toString());
    bob.append("to", toNss().toString());
    bob.append("command", cmdBob.obj());
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

void RenameParticipantInstance::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(5515104,
                2,
                "Rename participant phase transition",
                "fromNs"_attr = fromNss(),
                "toNs"_attr = toNss(),
                "newPhase"_attr = RenameCollectionParticipantPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = RenameCollectionParticipantPhase_serializer(_doc.getPhase()));

    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<StateDoc> store(NamespaceString::kShardingRenameParticipantsNamespace);

    if (_doc.getPhase() == Phase::kUnset) {
        store.add(opCtx.get(), newDoc, WriteConcerns::kMajorityWriteConcern);
    } else {
        store.update(opCtx.get(),
                     BSON(StateDoc::kFromNssFieldName << fromNss().ns()),
                     newDoc.toBSON(),
                     WriteConcerns::kMajorityWriteConcern);
    }

    _doc = std::move(newDoc);
}

void RenameParticipantInstance::_removeStateDocument(OperationContext* opCtx) {
    LOGV2_DEBUG(5515105,
                2,
                "Removing state document for rename collection participant",
                "fromNs"_attr = fromNss(),
                "toNs"_attr = toNss());

    PersistentTaskStore<StateDoc> store(NamespaceString::kShardingRenameParticipantsNamespace);
    store.remove(opCtx,
                 BSON(StateDoc::kFromNssFieldName << fromNss().ns()),
                 WriteConcerns::kMajorityWriteConcern);

    _doc = {};
}


void RenameParticipantInstance::_invalidateFutures(const Status& errStatus) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_blockCRUDAndRenameCompletionPromise.getFuture().isReady()) {
        _blockCRUDAndRenameCompletionPromise.setError(errStatus);
    }

    if (!_canUnblockCRUDPromise.getFuture().isReady()) {
        _canUnblockCRUDPromise.setError(errStatus);
    }

    if (!_unblockCRUDPromise.getFuture().isReady()) {
        _unblockCRUDPromise.setError(errStatus);
    }
}

SemiFuture<void> RenameParticipantInstance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kBlockCRUDAndSnapshotRangeDeletions,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                // Acquire source/target critical sections
                const auto reason =
                    BSON("command"
                         << "rename"
                         << "from" << fromNss().toString() << "to" << toNss().toString());
                auto service = RecoverableCriticalSectionService::get(opCtx);
                service->acquireRecoverableCriticalSectionBlockWrites(
                    opCtx, fromNss(), reason, ShardingCatalogClient::kLocalWriteConcern);
                service->promoteRecoverableCriticalSectionToBlockAlsoReads(
                    opCtx, fromNss(), reason, ShardingCatalogClient::kLocalWriteConcern);
                service->acquireRecoverableCriticalSectionBlockWrites(
                    opCtx, toNss(), reason, ShardingCatalogClient::kLocalWriteConcern);
                service->promoteRecoverableCriticalSectionToBlockAlsoReads(
                    opCtx, toNss(), reason, ShardingCatalogClient::kLocalWriteConcern);

                snapshotRangeDeletionsForRename(opCtx, fromNss(), toNss());
            }))
        .then(_executePhase(
            Phase::kRenameLocalAndRestoreRangeDeletions,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                _doc.getForwardableOpMetadata().setOn(opCtx);

                const RenameCollectionOptions options{_doc.getDropTarget(), _doc.getStayTemp()};
                renameOrDropTarget(
                    opCtx, fromNss(), toNss(), options, _doc.getSourceUUID(), _doc.getTargetUUID());

                restoreRangeDeletionTasksForRename(opCtx, toNss());
            }))
        .then(
            _executePhase(Phase::kDeleteFromRangeDeletions,
                          [this, anchor = shared_from_this()] {
                              auto opCtxHolder = cc().makeOperationContext();
                              auto* opCtx = opCtxHolder.get();
                              deleteRangeDeletionTasksForRename(opCtx, fromNss(), toNss());

                              {
                                  stdx::lock_guard<Latch> lg(_mutex);
                                  if (!_blockCRUDAndRenameCompletionPromise.getFuture().isReady()) {
                                      _blockCRUDAndRenameCompletionPromise.setFrom(Status::OK());
                                  }
                              }

                              LOGV2(5515106,
                                    "Collection locally renamed, waiting for CRUD to be unblocked",
                                    "fromNs"_attr = fromNss(),
                                    "toNs"_attr = toNss());
                          }))
        .then([this, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kUnblockCRUD) {
                return _canUnblockCRUDPromise.getFuture();
            }

            return SemiFuture<void>::makeReady().share();
        })
        .then(_executePhase(
            Phase::kUnblockCRUD,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                // Clear the CollectionShardingRuntime entry
                auto clearFilteringMetadata = [&](const NamespaceString& nss) {
                    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IX);
                    Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
                    auto* csr = CollectionShardingRuntime::get(opCtx, nss);
                    csr->clearFilteringMetadata(opCtx);
                };
                clearFilteringMetadata(fromNss());
                clearFilteringMetadata(toNss());

                // Force the refresh of the catalog cache for both source and destination
                // collections to purge outdated information.
                //
                // (SERVER-58465) Note that we have to wait for the asynchronous tasks submitted to
                // the background thread of the ShardServerCatalogCacheLoader because those tasks
                // might conflict with the next refresh if the loader relies on UUID-based
                // config.cache.chunks.* collections.
                const auto catalog = Grid::get(opCtx)->catalogCache();
                uassertStatusOK(catalog->getCollectionRoutingInfoWithRefresh(opCtx, fromNss()));
                CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, fromNss());

                uassertStatusOK(catalog->getCollectionRoutingInfoWithRefresh(opCtx, toNss()));
                CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, toNss());

                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);

                // Release source/target critical sections
                const auto reason =
                    BSON("command"
                         << "rename"
                         << "from" << fromNss().toString() << "to" << toNss().toString());
                auto service = RecoverableCriticalSectionService::get(opCtx);
                service->releaseRecoverableCriticalSection(
                    opCtx, fromNss(), reason, ShardingCatalogClient::kLocalWriteConcern);
                service->releaseRecoverableCriticalSection(
                    opCtx, toNss(), reason, ShardingCatalogClient::kMajorityWriteConcern);

                LOGV2(5515107, "CRUD unblocked", "fromNs"_attr = fromNss(), "toNs"_attr = toNss());
            }))
        .onCompletion([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isOK()) {
                if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                    !status.isA<ErrorCategory::ShutdownError>()) {
                    LOGV2_ERROR(5515109,
                                "Error executing rename collection participant",
                                "fromNs"_attr = fromNss(),
                                "toNs"_attr = toNss(),
                                "error"_attr = redact(status));
                }

                _invalidateFutures(status);
                return;
            }

            try {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                _removeStateDocument(opCtx);
            } catch (DBException& ex) {
                LOGV2_WARNING(5515108,
                              "Failed to remove rename participant state document",
                              "error"_attr = redact(ex));
                ex.addContext("Failed to remove rename participant state document"_sd);
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_unblockCRUDPromise.getFuture().isReady()) {
                    _unblockCRUDPromise.setError(ex.toStatus());
                }
                throw;
            }

            stdx::lock_guard<Latch> lg(_mutex);
            if (!_unblockCRUDPromise.getFuture().isReady()) {
                _unblockCRUDPromise.emplaceValue();
            }
        })
        .semi();
}

void RenameParticipantInstance::interrupt(Status status) noexcept {
    LOGV2_DEBUG(5515110,
                2,
                "Interrupt while running rename collection on participant",
                "fromNs"_attr = fromNss(),
                "toNs"_attr = toNss(),
                "error"_attr = redact(status));

    invariant(status.isA<ErrorCategory::NotPrimaryError>() ||
              status.isA<ErrorCategory::ShutdownError>());

    _invalidateFutures(status);
}

}  // namespace mongo
