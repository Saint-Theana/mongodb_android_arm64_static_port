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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_server_op_observer.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/s/chunk_split_state_driver.h"
#include "mongo/db/s/chunk_splitter.h"
#include "mongo/db/s/collection_critical_section_document_gen.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/recoverable_critical_section_service.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_collection.h"
#include "mongo/db/s/type_shard_database.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

const auto documentIdDecoration = OperationContext::declareDecoration<BSONObj>();

bool isStandaloneOrPrimary(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    return !isReplSet ||
        (repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
         repl::MemberState::RS_PRIMARY);
}

/**
 * Used to notify the catalog cache loader of a new collection version and invalidate the in-memory
 * routing table cache once the oplog updates are committed and become visible.
 */
class CollectionVersionLogOpHandler final : public RecoveryUnit::Change {
public:
    CollectionVersionLogOpHandler(OperationContext* opCtx, const NamespaceString& nss)
        : _opCtx(opCtx), _nss(nss) {}

    void commit(boost::optional<Timestamp>) override {
        invariant(_opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_IX));

        CatalogCacheLoader::get(_opCtx).notifyOfCollectionVersionUpdate(_nss);

        // Force subsequent uses of the namespace to refresh the filtering metadata so they can
        // synchronize with any work happening on the primary (e.g., migration critical section).
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        CollectionShardingRuntime::get(_opCtx, _nss)->clearFilteringMetadata(_opCtx);
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    const NamespaceString _nss;
};

/**
 * Used to perform shard identity initialization once it is certain that the document is committed.
 */
class ShardIdentityLogOpHandler final : public RecoveryUnit::Change {
public:
    ShardIdentityLogOpHandler(OperationContext* opCtx, ShardIdentityType shardIdentity)
        : _opCtx(opCtx), _shardIdentity(std::move(shardIdentity)) {}

    void commit(boost::optional<Timestamp>) override {
        try {
            ShardingInitializationMongoD::get(_opCtx)->initializeFromShardIdentity(_opCtx,
                                                                                   _shardIdentity);
        } catch (const AssertionException& ex) {
            fassertFailedWithStatus(40071, ex.toStatus());
        }
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    const ShardIdentityType _shardIdentity;
};

/**
 * Used to submit a range deletion task once it is certain that the update/insert to
 * config.rangeDeletions is committed.
 */
class SubmitRangeDeletionHandler final : public RecoveryUnit::Change {
public:
    SubmitRangeDeletionHandler(OperationContext* opCtx, RangeDeletionTask task)
        : _opCtx(opCtx), _task(std::move(task)) {}

    void commit(boost::optional<Timestamp>) override {
        migrationutil::submitRangeDeletionTask(_opCtx, _task).getAsync([](auto) {});
    }

    void rollback() override {}

private:
    OperationContext* _opCtx;
    RangeDeletionTask _task;
};


/**
 * Invalidates the in-memory routing table cache when a collection is dropped, so the next caller
 * with routing information will provoke a routing table refresh and see the drop.
 *
 * The query parameter must contain an _id field that identifies which collections entry is being
 * updated.
 *
 * This only runs on secondaries.
 * The global exclusive lock is expected to be held by the caller.
 */
void onConfigDeleteInvalidateCachedCollectionMetadataAndNotify(OperationContext* opCtx,
                                                               const BSONObj& query) {
    // Notification of routing table changes are only needed on secondaries
    if (isStandaloneOrPrimary(opCtx)) {
        return;
    }

    // Extract which collection entry is being deleted from the _id field.
    std::string deletedCollection;
    fassert(40479,
            bsonExtractStringField(query, ShardCollectionType::kNssFieldName, &deletedCollection));
    const NamespaceString deletedNss(deletedCollection);

    // Need the WUOW to retain the lock for CollectionVersionLogOpHandler::commit().
    AutoGetCollection autoColl(opCtx, deletedNss, MODE_IX);

    opCtx->recoveryUnit()->registerChange(
        std::make_unique<CollectionVersionLogOpHandler>(opCtx, deletedNss));
}

/**
 * If the collection is sharded, finds the chunk that contains the specified document and increments
 * the size tracked for that chunk by the specified amount of data written, in bytes. Returns the
 * number of total bytes on that chunk after the data is written.
 */
void incrementChunkOnInsertOrUpdate(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const ChunkManager& chunkManager,
                                    const BSONObj& document,
                                    long dataWritten,
                                    bool fromMigrate) {
    const auto& shardKeyPattern = chunkManager.getShardKeyPattern();
    BSONObj shardKey = shardKeyPattern.extractShardKeyFromDocThrows(document);

    // Use the shard key to locate the chunk into which the document was updated, and increment the
    // number of bytes tracked for the chunk.
    //
    // Note that we can assume the simple collation, because shard keys do not support non-simple
    // collations.
    auto chunk = chunkManager.findIntersectingChunkWithSimpleCollation(shardKey);
    auto chunkWritesTracker = chunk.getWritesTracker();
    chunkWritesTracker->addBytesWritten(dataWritten);
    // Don't trigger chunk splits from inserts happening due to migration since
    // we don't necessarily own that chunk yet
    if (!fromMigrate) {
        const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();

        if (balancerConfig->getShouldAutoSplit() &&
            chunkWritesTracker->shouldSplit(balancerConfig->getMaxChunkSizeBytes())) {
            auto chunkSplitStateDriver =
                ChunkSplitStateDriver::tryInitiateSplit(chunkWritesTracker);
            if (chunkSplitStateDriver) {
                ChunkSplitter::get(opCtx).trySplitting(std::move(chunkSplitStateDriver),
                                                       nss,
                                                       chunk.getMin(),
                                                       chunk.getMax(),
                                                       dataWritten);
            }
        }
    }
}

/**
 * Aborts any ongoing migration for the given namespace. Should only be called when observing
 * index operations.
 */
void abortOngoingMigrationIfNeeded(OperationContext* opCtx, const NamespaceString nss) {
    auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);
    auto msm = MigrationSourceManager::get(csr, csrLock);
    if (msm) {
        msm->abortDueToConflictingIndexOperation(opCtx);
    }
}

}  // namespace

ShardServerOpObserver::ShardServerOpObserver() = default;

ShardServerOpObserver::~ShardServerOpObserver() = default;

void ShardServerOpObserver::onInserts(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      OptionalCollectionUUID uuid,
                                      std::vector<InsertStatement>::const_iterator begin,
                                      std::vector<InsertStatement>::const_iterator end,
                                      bool fromMigrate) {
    const auto metadata = CollectionShardingRuntime::get(opCtx, nss)->getCurrentMetadataIfKnown();

    for (auto it = begin; it != end; ++it) {
        const auto& insertedDoc = it->doc;

        if (nss == NamespaceString::kServerConfigurationNamespace) {
            if (auto idElem = insertedDoc["_id"]) {
                if (idElem.str() == ShardIdentityType::IdName) {
                    auto shardIdentityDoc =
                        uassertStatusOK(ShardIdentityType::fromShardIdentityDocument(insertedDoc));
                    uassertStatusOK(shardIdentityDoc.validate());
                    opCtx->recoveryUnit()->registerChange(
                        std::make_unique<ShardIdentityLogOpHandler>(opCtx,
                                                                    std::move(shardIdentityDoc)));
                }
            }
        }

        if (nss == NamespaceString::kRangeDeletionNamespace) {
            if (!isStandaloneOrPrimary(opCtx)) {
                return;
            }

            auto deletionTask = RangeDeletionTask::parse(
                IDLParserErrorContext("ShardServerOpObserver"), insertedDoc);

            if (!deletionTask.getPending()) {
                opCtx->recoveryUnit()->registerChange(
                    std::make_unique<SubmitRangeDeletionHandler>(opCtx, deletionTask));
            }
        }

        if (nss == NamespaceString::kCollectionCriticalSectionsNamespace) {
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            if (!replCoord->isReplEnabled() ||
                (!replCoord->getMemberState().recovering() &&
                 !replCoord->getMemberState().rollback())) {
                const auto collCSDoc = CollectionCriticalSectionDocument::parse(
                    IDLParserErrorContext("ShardServerOpObserver"), insertedDoc);
                opCtx->recoveryUnit()->onCommit([opCtx,
                                                 insertedNss = collCSDoc.getNss(),
                                                 reason = collCSDoc.getReason().getOwned()](
                                                    boost::optional<Timestamp>) {
                    boost::optional<AutoGetCollection> lockCollectionIfNotPrimary;
                    if (!isStandaloneOrPrimary(opCtx))
                        lockCollectionIfNotPrimary.emplace(opCtx, insertedNss, MODE_IX);

                    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                    auto* const csr = CollectionShardingRuntime::get(opCtx, insertedNss);
                    auto csrLock = CollectionShardingRuntime ::CSRLock::lockExclusive(opCtx, csr);
                    csr->enterCriticalSectionCatchUpPhase(csrLock, reason);
                });
            }
        }

        if (metadata && metadata->isSharded()) {
            incrementChunkOnInsertOrUpdate(opCtx,
                                           nss,
                                           *metadata->getChunkManager(),
                                           insertedDoc,
                                           insertedDoc.objsize(),
                                           fromMigrate);
        }
    }
}

void ShardServerOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    const auto& updateDoc = args.updateArgs.update;
    // Most of these handlers do not need to run when the update is a full document replacement.
    const bool isReplacementUpdate = (update_oplog_entry::extractUpdateType(updateDoc) ==
                                      update_oplog_entry::UpdateType::kReplacement);
    if (args.nss == NamespaceString::kShardConfigCollectionsNamespace && !isReplacementUpdate) {
        // Notification of routing table changes are only needed on secondaries
        if (isStandaloneOrPrimary(opCtx)) {
            return;
        }

        // This logic runs on updates to the shard's persisted cache of the config server's
        // config.collections collection.
        //
        // If an update occurs to the 'lastRefreshedCollectionVersion' field it notifies the catalog
        // cache loader of a new collection version and clears the routing table so the next caller
        // with routing information will provoke a routing table refresh.
        //
        // When 'lastRefreshedCollectionVersion' is in 'update', it means that a chunk metadata
        // refresh has finished being applied to the collection's locally persisted metadata store.
        //
        // If an update occurs to the 'enterCriticalSectionSignal' field, simply clear the routing
        // table immediately. This will provoke the next secondary caller to refresh through the
        // primary, blocking behind the critical section.

        // Extract which user collection was updated
        const auto updatedNss([&] {
            std::string coll;
            fassert(40477,
                    bsonExtractStringField(
                        args.updateArgs.criteria, ShardCollectionType::kNssFieldName, &coll));
            return NamespaceString(coll);
        }());

        auto enterCriticalSectionFieldNewVal = update_oplog_entry::extractNewValueForField(
            updateDoc, ShardCollectionType::kEnterCriticalSectionCounterFieldName);
        auto refreshingFieldNewVal = update_oplog_entry::extractNewValueForField(
            updateDoc, ShardCollectionType::kRefreshingFieldName);

        // Need the WUOW to retain the lock for CollectionVersionLogOpHandler::commit().
        AutoGetCollection autoColl(opCtx, updatedNss, MODE_IX);
        if (refreshingFieldNewVal.isBoolean() && !refreshingFieldNewVal.boolean()) {
            opCtx->recoveryUnit()->registerChange(
                std::make_unique<CollectionVersionLogOpHandler>(opCtx, updatedNss));
        }

        if (enterCriticalSectionFieldNewVal.ok()) {
            // Force subsequent uses of the namespace to refresh the filtering metadata so they
            // can synchronize with any work happening on the primary (e.g., migration critical
            // section).
            CollectionShardingRuntime::get(opCtx, updatedNss)->clearFilteringMetadata(opCtx);
        }
    }

    if (args.nss == NamespaceString::kShardConfigDatabasesNamespace && !isReplacementUpdate) {
        // Notification of routing table changes are only needed on secondaries
        if (isStandaloneOrPrimary(opCtx)) {
            return;
        }

        // This logic runs on updates to the shard's persisted cache of the config server's
        // config.databases collection.
        //
        // If an update occurs to the 'enterCriticalSectionSignal' field, clear the routing
        // table immediately. This will provoke the next secondary caller to refresh through the
        // primary, blocking behind the critical section.

        // Extract which database was updated
        std::string db;
        fassert(
            40478,
            bsonExtractStringField(args.updateArgs.criteria, ShardDatabaseType::name.name(), &db));

        auto enterCriticalSectionCounterFieldNewVal = update_oplog_entry::extractNewValueForField(
            updateDoc, ShardDatabaseType::enterCriticalSectionCounter.name());

        if (enterCriticalSectionCounterFieldNewVal.ok()) {
            AutoGetDb autoDb(opCtx, db, MODE_X);
            auto dss = DatabaseShardingState::get(opCtx, db);
            dss->clearDatabaseInfo(opCtx);
        }
    }

    if (args.nss == NamespaceString::kRangeDeletionNamespace && !isReplacementUpdate) {
        if (!isStandaloneOrPrimary(opCtx))
            return;

        const auto pendingFieldRemovedStatus =
            update_oplog_entry::isFieldRemovedByUpdate(args.updateArgs.update, "pending");

        if (pendingFieldRemovedStatus == update_oplog_entry::FieldRemovedStatus::kFieldRemoved) {
            auto deletionTask = RangeDeletionTask::parse(
                IDLParserErrorContext("ShardServerOpObserver"), args.updateArgs.updatedDoc);

            if (deletionTask.getDonorShardId() != ShardingState::get(opCtx)->shardId()) {
                // Range deletion tasks for moved away chunks are scheduled through the
                // MigrationCoordinator, so only schedule a task for received chunks.
                opCtx->recoveryUnit()->registerChange(
                    std::make_unique<SubmitRangeDeletionHandler>(opCtx, deletionTask));
            }
        }
    }

    if (args.nss == NamespaceString::kCollectionCriticalSectionsNamespace) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (!replCoord->isReplEnabled() ||
            (!replCoord->getMemberState().recovering() &&
             !replCoord->getMemberState().rollback())) {
            const auto collCSDoc = CollectionCriticalSectionDocument::parse(
                IDLParserErrorContext("ShardServerOpObserver"), args.updateArgs.updatedDoc);

            opCtx->recoveryUnit()->onCommit(
                [opCtx, updatedNss = collCSDoc.getNss(), reason = collCSDoc.getReason().getOwned()](
                    boost::optional<Timestamp>) {
                    boost::optional<AutoGetCollection> lockCollectionIfNotPrimary;
                    if (!isStandaloneOrPrimary(opCtx))
                        lockCollectionIfNotPrimary.emplace(opCtx, updatedNss, MODE_IX);

                    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                    auto* const csr = CollectionShardingRuntime::get(opCtx, updatedNss);
                    auto csrLock = CollectionShardingRuntime ::CSRLock::lockExclusive(opCtx, csr);
                    csr->enterCriticalSectionCommitPhase(csrLock, reason);
                });
        }
    }

    auto* const csr = CollectionShardingRuntime::get(opCtx, args.nss);
    const auto metadata = csr->getCurrentMetadataIfKnown();
    if (metadata && metadata->isSharded()) {
        incrementChunkOnInsertOrUpdate(opCtx,
                                       args.nss,
                                       *metadata->getChunkManager(),
                                       args.updateArgs.updatedDoc,
                                       args.updateArgs.updatedDoc.objsize(),
                                       args.updateArgs.source == OperationSource::kFromMigrate);
    }
}

void ShardServerOpObserver::aboutToDelete(OperationContext* opCtx,
                                          NamespaceString const& nss,
                                          BSONObj const& doc) {

    if (nss == NamespaceString::kCollectionCriticalSectionsNamespace) {
        documentIdDecoration(opCtx) = doc;
    } else {
        // Extract the _id field from the document. If it does not have an _id, use the
        // document itself as the _id.
        documentIdDecoration(opCtx) = doc["_id"] ? doc["_id"].wrap() : doc;
    }
}

void ShardServerOpObserver::onDelete(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     OptionalCollectionUUID uuid,
                                     StmtId stmtId,
                                     const OplogDeleteEntryArgs& args) {
    auto& documentId = documentIdDecoration(opCtx);
    invariant(!documentId.isEmpty());

    if (nss == NamespaceString::kShardConfigCollectionsNamespace) {
        onConfigDeleteInvalidateCachedCollectionMetadataAndNotify(opCtx, documentId);
    }

    if (nss == NamespaceString::kShardConfigDatabasesNamespace) {
        if (isStandaloneOrPrimary(opCtx)) {
            return;
        }

        // Extract which database entry is being deleted from the _id field.
        std::string deletedDatabase;
        fassert(
            50772,
            bsonExtractStringField(documentId, ShardDatabaseType::name.name(), &deletedDatabase));

        AutoGetDb autoDb(opCtx, deletedDatabase, MODE_X);
        auto dss = DatabaseShardingState::get(opCtx, deletedDatabase);
        dss->clearDatabaseInfo(opCtx);
    }

    if (nss == NamespaceString::kServerConfigurationNamespace) {
        if (auto idElem = documentId.firstElement()) {
            auto idStr = idElem.str();
            if (idStr == ShardIdentityType::IdName) {
                if (!repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
                    uasserted(40070,
                              "cannot delete shardIdentity document while in --shardsvr mode");
                } else {
                    LOGV2_WARNING(23779,
                                  "Shard identity document rolled back.  Will shut down after "
                                  "finishing rollback.");
                    ShardIdentityRollbackNotifier::get(opCtx)->recordThatRollbackHappened();
                }
            }
        }
    }

    if (nss == NamespaceString::kCollectionCriticalSectionsNamespace) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (!replCoord->isReplEnabled() ||
            (!replCoord->getMemberState().recovering() &&
             !replCoord->getMemberState().rollback())) {
            const auto& deletedDoc = documentId;
            const auto collCSDoc = CollectionCriticalSectionDocument::parse(
                IDLParserErrorContext("ShardServerOpObserver"), deletedDoc);

            opCtx->recoveryUnit()->onCommit(
                [opCtx, deletedNss = collCSDoc.getNss(), reason = collCSDoc.getReason().getOwned()](
                    boost::optional<Timestamp>) {
                    boost::optional<AutoGetCollection> lockCollectionIfNotPrimary;
                    if (!isStandaloneOrPrimary(opCtx))
                        lockCollectionIfNotPrimary.emplace(opCtx, deletedNss, MODE_IX);

                    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                    auto* const csr = CollectionShardingRuntime::get(opCtx, deletedNss);
                    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
                    csr->exitCriticalSection(csrLock, reason);
                });
        }
    }
}

void ShardServerOpObserver::onCreateCollection(OperationContext* opCtx,
                                               const CollectionPtr& coll,
                                               const NamespaceString& collectionName,
                                               const CollectionOptions& options,
                                               const BSONObj& idIndex,
                                               const OplogSlot& createOpTime) {
    // Only the shard primay nodes control the collection creation and secondaries just follow
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // Collections which are always UNSHARDED have a fixed CSS, which never changes, so we don't
    // need to do anything
    if (collectionName.isNamespaceAlwaysUnsharded()) {
        return;
    }

    // Temp collections are always UNSHARDED
    if (options.temp) {
        CollectionShardingRuntime::get(opCtx, collectionName)
            ->setFilteringMetadata(opCtx, CollectionMetadata());
        return;
    }

    const auto& oss = OperationShardingState::get(opCtx);
    uassert(CannotImplicitlyCreateCollectionInfo(collectionName),
            "Implicit collection creation on a sharded cluster must go through the "
            "CreateCollectionCoordinator",
            oss._allowCollectionCreation);

    // If the check above passes, this means the caller is responsible to eventially set the shard
    // version for the collection
    //
    // TODO (SERVER-55284): Delete the lines below once all usages of
    // ScopedAllowImplicitCollectionCreate_UNSAFE have been removed
    auto* const csr = CollectionShardingRuntime::get(opCtx, collectionName);
    if (!csr->getCurrentMetadataIfKnown()) {
        csr->setFilteringMetadata(opCtx, CollectionMetadata());
    }
}

repl::OpTime ShardServerOpObserver::onDropCollection(OperationContext* opCtx,
                                                     const NamespaceString& collectionName,
                                                     OptionalCollectionUUID uuid,
                                                     std::uint64_t numRecords,
                                                     const CollectionDropType dropType) {
    if (collectionName == NamespaceString::kServerConfigurationNamespace) {
        // Dropping system collections is not allowed for end users
        invariant(!opCtx->writesAreReplicated());
        invariant(repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback());

        // Can't confirm whether there was a ShardIdentity document or not yet, so assume there was
        // one and shut down the process to clear the in-memory sharding state
        LOGV2_WARNING(23780,
                      "admin.system.version collection rolled back. Will shut down after finishing "
                      "rollback");

        ShardIdentityRollbackNotifier::get(opCtx)->recordThatRollbackHappened();
    }

    return {};
}

void ShardServerOpObserver::onStartIndexBuild(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              CollectionUUID collUUID,
                                              const UUID& indexBuildUUID,
                                              const std::vector<BSONObj>& indexes,
                                              bool fromMigrate) {
    abortOngoingMigrationIfNeeded(opCtx, nss);
};

void ShardServerOpObserver::onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    abortOngoingMigrationIfNeeded(opCtx, nss);
}

void ShardServerOpObserver::onDropIndex(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        OptionalCollectionUUID uuid,
                                        const std::string& indexName,
                                        const BSONObj& indexInfo) {
    abortOngoingMigrationIfNeeded(opCtx, nss);
};

void ShardServerOpObserver::onCollMod(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& uuid,
                                      const BSONObj& collModCmd,
                                      const CollectionOptions& oldCollOptions,
                                      boost::optional<IndexCollModInfo> indexInfo) {
    abortOngoingMigrationIfNeeded(opCtx, nss);
};

void ShardServerOpObserver::onReplicationRollback(OperationContext* opCtx,
                                                  const RollbackObserverInfo& rbInfo) {
    if (rbInfo.rollbackNamespaces.find(NamespaceString::kCollectionCriticalSectionsNamespace) !=
        rbInfo.rollbackNamespaces.end()) {
        RecoverableCriticalSectionService::get(opCtx)->recoverRecoverableCriticalSections(opCtx);
    }
}


}  // namespace mongo
