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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/drop_collection.h"

#include "mongo/db/audit.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangDropCollectionBeforeLockAcquisition);
MONGO_FAIL_POINT_DEFINE(hangDuringDropCollection);

Status _checkNssAndReplState(OperationContext* opCtx, const CollectionPtr& coll) {
    if (!coll) {
        return Status(ErrorCodes::NamespaceNotFound, "ns not found");
    }

    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, coll->ns())) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while dropping collection " << coll->ns());
    }

    return Status::OK();
}

Status _dropView(OperationContext* opCtx,
                 Database* db,
                 const NamespaceString& collectionName,
                 DropReply* reply) {
    if (!db) {
        Status status = Status(ErrorCodes::NamespaceNotFound, "ns not found");
        audit::logDropView(opCtx->getClient(), collectionName, "", {}, status.code());
        return status;
    }
    auto view =
        ViewCatalog::get(db)->lookupWithoutValidatingDurableViews(opCtx, collectionName.ns());
    if (!view) {
        Status status = Status(ErrorCodes::NamespaceNotFound, "ns not found");
        audit::logDropView(opCtx->getClient(), collectionName, "", {}, status.code());
        return status;
    }

    // Validates the view or throws an "invalid view" error.
    ViewCatalog::get(db)->lookup(opCtx, collectionName.ns());

    Lock::CollectionLock collLock(opCtx, collectionName, MODE_IX);
    // Operations all lock system.views in the end to prevent deadlock.
    Lock::CollectionLock systemViewsLock(opCtx, db->getSystemViewsName(), MODE_X);

    if (MONGO_unlikely(hangDuringDropCollection.shouldFail())) {
        LOGV2(20330,
              "hangDuringDropCollection fail point enabled. Blocking until fail point is "
              "disabled.");
        hangDuringDropCollection.pauseWhileSet();
    }

    AutoStatsTracker statsTracker(
        opCtx,
        collectionName,
        Top::LockType::NotLocked,
        AutoStatsTracker::LogMode::kUpdateCurOp,
        CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(collectionName.db()));

    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionName)) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while dropping collection " << collectionName);
    }

    WriteUnitOfWork wunit(opCtx);

    audit::logDropView(
        opCtx->getClient(), collectionName, view->viewOn().ns(), view->pipeline(), ErrorCodes::OK);

    Status status = db->dropView(opCtx, collectionName);
    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    reply->setNs(collectionName);
    return Status::OK();
}

Status _abortIndexBuildsAndDrop(OperationContext* opCtx,
                                AutoGetDb&& autoDb,
                                const NamespaceString& startingNss,
                                std::function<Status(Database*, const NamespaceString&)>&& dropFn,
                                DropReply* reply,
                                bool appendNs = true) {
    // We only need to hold an intent lock to send abort signals to the active index builder on this
    // collection.
    boost::optional<AutoGetDb> optionalAutoDb(std::move(autoDb));
    boost::optional<Lock::CollectionLock> collLock;
    collLock.emplace(opCtx, startingNss, MODE_IX);

    // Abandon the snapshot as the index catalog will compare the in-memory state to the disk state,
    // which may have changed when we released the collection lock temporarily.
    opCtx->recoveryUnit()->abandonSnapshot();

    CollectionPtr coll =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, startingNss);
    Status status = _checkNssAndReplState(opCtx, coll);
    if (!status.isOK()) {
        return status;
    }

    if (MONGO_unlikely(hangDuringDropCollection.shouldFail())) {
        LOGV2(518090,
              "hangDuringDropCollection fail point enabled. Blocking until fail point is "
              "disabled.");
        hangDuringDropCollection.pauseWhileSet();
    }

    AutoStatsTracker statsTracker(
        opCtx,
        startingNss,
        Top::LockType::NotLocked,
        AutoStatsTracker::LogMode::kUpdateCurOp,
        CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(startingNss.db()));

    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    const UUID collectionUUID = coll->uuid();
    const NamespaceStringOrUUID dbAndUUID{coll->ns().db().toString(), coll->uuid()};
    const int numIndexes = coll->getIndexCatalog()->numIndexesTotal(opCtx);

    while (true) {
        // Save a copy of the namespace before yielding our locks.
        const NamespaceString collectionNs = coll->ns();

        // Release locks before aborting index builds. The helper will acquire locks on our behalf.
        collLock = boost::none;
        optionalAutoDb = boost::none;

        // Send the abort signal to any active index builds on the collection. This waits until all
        // aborted index builds complete.
        indexBuildsCoord->abortCollectionIndexBuilds(opCtx,
                                                     collectionNs,
                                                     collectionUUID,
                                                     str::stream()
                                                         << "Collection " << collectionNs << "("
                                                         << collectionUUID << ") is being dropped");

        // Take an exclusive lock to finish the collection drop.
        optionalAutoDb.emplace(opCtx, startingNss.db(), MODE_IX);
        collLock.emplace(opCtx, dbAndUUID, MODE_X);

        // Abandon the snapshot as the index catalog will compare the in-memory state to the
        // disk state, which may have changed when we released the collection lock temporarily.
        opCtx->recoveryUnit()->abandonSnapshot();

        coll = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, collectionUUID);
        status = _checkNssAndReplState(opCtx, coll);
        if (!status.isOK()) {
            return status;
        }

        // Check if any new index builds were started while releasing the collection lock
        // temporarily, if so, we need to abort the new index builders.
        const bool abortAgain = indexBuildsCoord->inProgForCollection(collectionUUID);
        if (!abortAgain) {
            break;
        }
    }

    // It's possible for the given collection to be drop pending after obtaining the locks again, if
    // that is the case, then the collection is already registered to be dropped. Return early.
    const NamespaceString resolvedNss = coll->ns();
    if (resolvedNss.isDropPendingNamespace()) {
        return Status::OK();
    }

    // Serialize the drop with refreshes to prevent dropping a collection and creating the same
    // nss as a view while refreshing.
    CollectionShardingState::get(opCtx, resolvedNss)->checkShardVersionOrThrow(opCtx);

    invariant(coll->getIndexCatalog()->numIndexesInProgress(opCtx) == 0);

    status = dropFn(optionalAutoDb->getDb(), resolvedNss);
    if (!status.isOK()) {
        return status;
    }

    reply->setNIndexesWas(numIndexes);
    if (appendNs) {
        reply->setNs(resolvedNss);
    }

    return Status::OK();
}

Status _dropCollection(OperationContext* opCtx,
                       Database* db,
                       const NamespaceString& collectionName,
                       const repl::OpTime& dropOpTime,
                       DropCollectionSystemCollectionMode systemCollectionMode,
                       DropReply* reply) {
    Lock::CollectionLock collLock(opCtx, collectionName, MODE_X);
    const CollectionPtr& coll =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, collectionName);
    Status status = _checkNssAndReplState(opCtx, coll);
    if (!status.isOK()) {
        return status;
    }

    if (MONGO_unlikely(hangDuringDropCollection.shouldFail())) {
        LOGV2(20331,
              "hangDuringDropCollection fail point enabled. Blocking until fail point is "
              "disabled.");
        hangDuringDropCollection.pauseWhileSet();
    }

    AutoStatsTracker statsTracker(
        opCtx,
        collectionName,
        Top::LockType::NotLocked,
        AutoStatsTracker::LogMode::kUpdateCurOp,
        CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(collectionName.db()));

    WriteUnitOfWork wunit(opCtx);

    int numIndexes = coll->getIndexCatalog()->numIndexesTotal(opCtx);
    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(coll->uuid());
    status =
        systemCollectionMode == DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops
        ? db->dropCollection(opCtx, collectionName, dropOpTime)
        : db->dropCollectionEvenIfSystem(opCtx, collectionName, dropOpTime);

    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    reply->setNIndexesWas(numIndexes);
    reply->setNs(collectionName);

    return Status::OK();
}

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& collectionName,
                      DropReply* reply,
                      DropCollectionSystemCollectionMode systemCollectionMode) {
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(518070, "CMD: drop", logAttrs(collectionName));
    }

    if (MONGO_unlikely(hangDropCollectionBeforeLockAcquisition.shouldFail())) {
        LOGV2(518080, "Hanging drop collection before lock acquisition while fail point is set");
        hangDropCollectionBeforeLockAcquisition.pauseWhileSet();
    }

    try {
        return writeConflictRetry(opCtx, "drop", collectionName.ns(), [&] {
            AutoGetDb autoDb(opCtx, collectionName.db(), MODE_IX);
            auto db = autoDb.getDb();
            if (!db) {
                return Status(ErrorCodes::NamespaceNotFound, "ns not found");
            }

            if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, collectionName)) {
                return _abortIndexBuildsAndDrop(
                    opCtx,
                    std::move(autoDb),
                    collectionName,
                    [opCtx, systemCollectionMode](Database* db, const NamespaceString& resolvedNs) {
                        WriteUnitOfWork wuow(opCtx);

                        auto status = systemCollectionMode ==
                                DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops
                            ? db->dropCollection(opCtx, resolvedNs)
                            : db->dropCollectionEvenIfSystem(opCtx, resolvedNs);
                        if (!status.isOK()) {
                            return status;
                        }

                        wuow.commit();
                        return Status::OK();
                    },
                    reply);
            }

            auto dropTimeseries = [opCtx, &autoDb, &collectionName, &reply](
                                      const NamespaceString& bucketNs, bool dropView) {
                return _abortIndexBuildsAndDrop(
                    opCtx,
                    std::move(autoDb),
                    bucketNs,
                    [opCtx, dropView, &collectionName, &reply](Database* db,
                                                               const NamespaceString& bucketsNs) {
                        if (dropView) {
                            // Take a MODE_X lock when dropping timeseries view. This is to prevent
                            // a concurrent create collection on the same namespace that will
                            // reserve an OpTime before this drop. We already hold a MODE_X lock on
                            // the bucket collection inside '_abortIndexBuildsAndDrop' above. When
                            // taking both these locks it needs to happen in this order to prevent a
                            // deadlock.
                            Lock::CollectionLock viewLock(opCtx, collectionName, MODE_X);
                            auto status = _dropView(opCtx, db, collectionName, reply);
                            if (!status.isOK()) {
                                return status;
                            }
                        }

                        // Drop the buckets collection in its own writeConflictRetry so that if
                        // it throws a WCE, only the buckets collection drop is retried.
                        writeConflictRetry(opCtx, "drop", bucketsNs.ns(), [opCtx, db, &bucketsNs] {
                            WriteUnitOfWork wuow(opCtx);
                            db->dropCollectionEvenIfSystem(opCtx, bucketsNs).ignore();
                            wuow.commit();
                        });

                        return Status::OK();
                    },
                    reply,
                    false /* appendNs */);
            };

            auto view = ViewCatalog::get(db)->lookupWithoutValidatingDurableViews(
                opCtx, collectionName.ns());
            if (!view) {
                // Timeseries bucket collection may exist even without the view. If that is the case
                // delete it.
                auto bucketsNs = collectionName.makeTimeseriesBucketsNamespace();
                if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, bucketsNs)) {
                    return dropTimeseries(bucketsNs, false);
                }

                Status status = Status(ErrorCodes::NamespaceNotFound, "ns not found");
                audit::logDropView(opCtx->getClient(), collectionName, "", {}, status.code());
                return status;
            }
            if (view->timeseries() &&
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, view->viewOn())) {
                return dropTimeseries(view->viewOn(), true);
            }

            return _dropView(opCtx, db, collectionName, reply);
        });
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The shell requires that NamespaceNotFound error codes return the "ns not found"
        // string.
        return Status(ErrorCodes::NamespaceNotFound, "ns not found");
    }
}

Status dropCollectionForApplyOps(OperationContext* opCtx,
                                 const NamespaceString& collectionName,
                                 const repl::OpTime& dropOpTime,
                                 DropCollectionSystemCollectionMode systemCollectionMode) {
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(20332, "CMD: drop", logAttrs(collectionName));
    }

    if (MONGO_unlikely(hangDropCollectionBeforeLockAcquisition.shouldFail())) {
        LOGV2(20333, "Hanging drop collection before lock acquisition while fail point is set");
        hangDropCollectionBeforeLockAcquisition.pauseWhileSet();
    }
    return writeConflictRetry(opCtx, "drop", collectionName.ns(), [&] {
        AutoGetDb autoDb(opCtx, collectionName.db(), MODE_IX);
        Database* db = autoDb.getDb();
        if (!db) {
            return Status(ErrorCodes::NamespaceNotFound, "ns not found");
        }

        const CollectionPtr& coll =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, collectionName);

        DropReply unusedReply;
        if (!coll) {
            return _dropView(opCtx, db, collectionName, &unusedReply);
        } else {
            return _dropCollection(
                opCtx, db, collectionName, dropOpTime, systemCollectionMode, &unusedReply);
        }
    });
}

}  // namespace mongo
