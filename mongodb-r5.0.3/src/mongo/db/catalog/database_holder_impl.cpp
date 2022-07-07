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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder_impl.h"

#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/database_impl.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

StringData _todb(StringData ns) {
    std::size_t i = ns.find('.');
    if (i == std::string::npos) {
        uassert(13074, "db name can't be empty", ns.size());
        return ns;
    }

    uassert(13075, "db name can't be empty", i > 0);

    const StringData d = ns.substr(0, i);
    uassert(13280,
            "invalid db name: " + ns.toString(),
            NamespaceString::validDBName(d, NamespaceString::DollarInDbNameBehavior::Allow));

    return d;
}

}  // namespace

Database* DatabaseHolderImpl::getDb(OperationContext* opCtx, StringData ns) const {
    const StringData db = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(db, MODE_IS) ||
              (db.compare("local") == 0 && opCtx->lockState()->isLocked()));

    stdx::lock_guard<SimpleMutex> lk(_m);
    DBs::const_iterator it = _dbs.find(db);
    if (it != _dbs.end()) {
        return it->second;
    }

    return nullptr;
}

std::shared_ptr<const ViewCatalog> DatabaseHolderImpl::getViewCatalog(OperationContext* opCtx,
                                                                      StringData dbName) const {
    stdx::lock_guard<SimpleMutex> lk(_m);
    DBs::const_iterator it = _dbs.find(dbName);
    if (it != _dbs.end()) {
        const Database* db = it->second;
        if (db) {
            return ViewCatalog::get(db);
        }
    }

    return nullptr;
}

std::set<std::string> DatabaseHolderImpl::_getNamesWithConflictingCasing_inlock(StringData name) {
    std::set<std::string> duplicates;

    for (const auto& nameAndPointer : _dbs) {
        // A name that's equal with case-insensitive match must be identical, or it's a duplicate.
        if (name.equalCaseInsensitive(nameAndPointer.first) && name != nameAndPointer.first)
            duplicates.insert(nameAndPointer.first);
    }
    return duplicates;
}

std::set<std::string> DatabaseHolderImpl::getNamesWithConflictingCasing(StringData name) {
    stdx::lock_guard<SimpleMutex> lk(_m);
    return _getNamesWithConflictingCasing_inlock(name);
}

std::vector<std::string> DatabaseHolderImpl::getNames() {
    stdx::lock_guard<SimpleMutex> lk(_m);
    std::vector<std::string> names;
    for (const auto& nameAndPointer : _dbs) {
        names.push_back(nameAndPointer.first);
    }
    return names;
}

Database* DatabaseHolderImpl::openDb(OperationContext* opCtx, StringData ns, bool* justCreated) {
    const StringData dbname = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(dbname, MODE_IX));

    if (justCreated)
        *justCreated = false;  // Until proven otherwise.

    stdx::unique_lock<SimpleMutex> lk(_m);

    // The following will insert a nullptr for dbname, which will treated the same as a non-
    // existant database by the get method, yet still counts in getNamesWithConflictingCasing.
    if (auto db = _dbs[dbname])
        return db;

    // We've inserted a nullptr entry for dbname: make sure to remove it on unsuccessful exit.
    auto removeDbGuard = makeGuard([this, &lk, dbname] {
        if (!lk.owns_lock())
            lk.lock();
        _dbs.erase(dbname);
    });

    // Check casing in lock to avoid transient duplicates.
    auto duplicates = _getNamesWithConflictingCasing_inlock(dbname);
    uassert(ErrorCodes::DatabaseDifferCase,
            str::stream() << "db already exists with different case already have: ["
                          << *duplicates.cbegin() << "] trying to create [" << dbname.toString()
                          << "]",
            duplicates.empty());

    // Do the catalog lookup and database creation outside of the scoped lock, because these may
    // block.
    lk.unlock();

    if (CollectionCatalog::get(opCtx)->getAllCollectionUUIDsFromDb(dbname).empty()) {
        audit::logCreateDatabase(opCtx->getClient(), dbname);
        if (justCreated)
            *justCreated = true;
    }

    auto newDb = std::make_unique<DatabaseImpl>(dbname);
    newDb->init(opCtx);

    // Finally replace our nullptr entry with the new Database pointer.
    removeDbGuard.dismiss();
    lk.lock();
    auto it = _dbs.find(dbname);
    // Dropping a database requires a MODE_X lock, so the entry in the `_dbs` map cannot disappear.
    invariant(it != _dbs.end());
    if (it->second) {
        // Creating databases only requires a DB lock in MODE_IX. Thus databases can concurrently
        // created. If this thread "lost the race", return the database object that was persisted in
        // the `_dbs` map.
        return it->second;
    }

    it->second = newDb.release();
    invariant(_getNamesWithConflictingCasing_inlock(dbname.toString()).empty());

    return it->second;
}

void DatabaseHolderImpl::dropDb(OperationContext* opCtx, Database* db) {
    invariant(db);

    // Store the name so we have if for after the db object is deleted
    auto name = db->name();

    LOGV2_DEBUG(20310, 1, "dropDatabase {name}", "name"_attr = name);

    invariant(opCtx->lockState()->isDbLockedForMode(name, MODE_X));

    auto catalog = CollectionCatalog::get(opCtx);
    for (auto collIt = catalog->begin(opCtx, name); collIt != catalog->end(opCtx); ++collIt) {
        auto coll = *collIt;
        if (!coll) {
            break;
        }

        // It is the caller's responsibility to ensure that no index builds are active in the
        // database.
        invariant(!coll->getIndexCatalog()->haveAnyIndexesInProgress(),
                  str::stream() << "An index is building on collection '" << coll->ns() << "'.");
    }

    audit::logDropDatabase(opCtx->getClient(), name);

    auto const serviceContext = opCtx->getServiceContext();

    for (auto collIt = catalog->begin(opCtx, name); collIt != catalog->end(opCtx); ++collIt) {
        auto coll = *collIt;
        if (!coll) {
            break;
        }

        // The in-memory ViewCatalog gets cleared when opObserver::onDropCollection() is called for
        // the system.views collection. Since it is a replicated collection, this call occurs in
        // dropCollectionEvenIfSystem(). For standalones, `system.views` and the ViewCatalog are
        // dropped/cleared here.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (!replCoord->isReplEnabled() && coll->ns().isSystemDotViews()) {
            opCtx->getServiceContext()->getOpObserver()->onDropCollection(
                opCtx,
                coll->ns(),
                coll->uuid(),
                coll->numRecords(opCtx),
                OpObserver::CollectionDropType::kOnePhase);
        }

        Top::get(serviceContext).collectionDropped(coll->ns());
    }

    // Clean up the in-memory database state.
    CollectionCatalog::write(
        opCtx, [&](CollectionCatalog& catalog) { catalog.clearDatabaseProfileSettings(name); });
    close(opCtx, name);

    auto const storageEngine = serviceContext->getStorageEngine();
    writeConflictRetry(opCtx, "dropDatabase", name, [&] {
        storageEngine->dropDatabase(opCtx, name).transitional_ignore();
    });
}

void DatabaseHolderImpl::close(OperationContext* opCtx, StringData ns) {
    const StringData dbName = _todb(ns);
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));

    stdx::lock_guard<SimpleMutex> lk(_m);

    DBs::const_iterator it = _dbs.find(dbName);
    if (it == _dbs.end()) {
        return;
    }

    auto db = it->second;
    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.onCloseDatabase(opCtx, dbName.toString());
    });

    delete db;
    db = nullptr;

    _dbs.erase(it);

    auto* const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->closeDatabase(opCtx, dbName.toString()).transitional_ignore();
}

void DatabaseHolderImpl::closeAll(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());

    stdx::lock_guard<SimpleMutex> lk(_m);

    std::set<std::string> dbs;
    for (DBs::const_iterator i = _dbs.begin(); i != _dbs.end(); ++i) {
        // It is the caller's responsibility to ensure that no index builds are active in the
        // database.
        IndexBuildsCoordinator::get(opCtx)->assertNoBgOpInProgForDb(i->first);
        dbs.insert(i->first);
    }

    auto* const storageEngine = opCtx->getServiceContext()->getStorageEngine();

    for (const auto& name : dbs) {
        LOGV2_DEBUG(20311, 2, "DatabaseHolder::closeAll name:{name}", "name"_attr = name);

        Database* db = _dbs[name];
        CollectionCatalog::write(
            opCtx, [&](CollectionCatalog& catalog) { catalog.onCloseDatabase(opCtx, name); });
        delete db;

        _dbs.erase(name);

        storageEngine->closeDatabase(opCtx, name).transitional_ignore();
    }
}

}  // namespace mongo
