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

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/views/view.h"

namespace mongo {

/**
 * RAII-style class, which acquires a lock on the specified database in the requested mode and
 * obtains a reference to the database. Used as a shortcut for calls to
 * DatabaseHolder::get(opCtx)->get().
 *
 * Use this when you want to do a database-level operation, like read a list of all collections, or
 * drop a collection.
 *
 * It is guaranteed that the lock will be released when this object goes out of scope, therefore
 * the database reference returned by this class should not be retained.
 */
class AutoGetDb {
    AutoGetDb(const AutoGetDb&) = delete;
    AutoGetDb& operator=(const AutoGetDb&) = delete;

public:
    AutoGetDb(OperationContext* opCtx,
              StringData dbName,
              LockMode mode,
              Date_t deadline = Date_t::max());

    AutoGetDb(AutoGetDb&&) = default;

    /**
     * Returns the database, or nullptr if it didn't exist.
     */
    Database* getDb() const {
        return _db;
    }

    /**
     * Returns the database, creating it if it does not exist.
     */
    Database* ensureDbExists();

private:
    OperationContext* _opCtx;
    std::string _dbName;

    Lock::DBLock _dbLock;
    Database* _db;
};

enum class AutoGetCollectionViewMode { kViewsPermitted, kViewsForbidden };

/**
 * RAII-style class, which acquires global, database, and collection locks according to the chart
 * below.
 *
 * | modeColl | Global Lock Result | DB Lock Result | Collection Lock Result |
 * |----------+--------------------+----------------+------------------------|
 * | MODE_IX  | MODE_IX            | MODE_IX        | MODE_IX                |
 * | MODE_X   | MODE_IX            | MODE_IX        | MODE_X                 |
 * | MODE_IS  | MODE_IS            | MODE_IS        | MODE_IS                |
 * | MODE_S   | MODE_IS            | MODE_IS        | MODE_S                 |
 *
 * NOTE: Throws NamespaceNotFound if the collection UUID cannot be resolved to a name.
 *
 * Any acquired locks may be released when this object goes out of scope, therefore the database
 * and the collection references returned by this class should not be retained.
 */
class AutoGetCollection {
    AutoGetCollection(const AutoGetCollection&) = delete;
    AutoGetCollection& operator=(const AutoGetCollection&) = delete;

public:
    AutoGetCollection(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        LockMode modeColl,
        AutoGetCollectionViewMode viewMode = AutoGetCollectionViewMode::kViewsForbidden,
        Date_t deadline = Date_t::max());

    explicit operator bool() const {
        return static_cast<bool>(getCollection());
    }

    /**
     * AutoGetCollection can be used as a pointer with the -> operator.
     */
    const Collection* operator->() const {
        return getCollection().get();
    }

    const CollectionPtr& operator*() const {
        return getCollection();
    }

    /**
     * Returns the database, or nullptr if it didn't exist.
     */
    Database* getDb() const {
        return _autoDb.getDb();
    }

    /**
     * Returns the database, creating it if it does not exist.
     */
    Database* ensureDbExists() {
        return _autoDb.ensureDbExists();
    }

    /**
     * Returns nullptr if the collection didn't exist.
     *
     * Deprecated in favor of the new ->(), *() and bool() accessors above!
     */
    const CollectionPtr& getCollection() const {
        return _coll;
    }

    /**
     * Returns nullptr if the view didn't exist.
     */
    const ViewDefinition* getView() const {
        return _view.get();
    }

    /**
     * Returns the resolved namespace of the collection or view.
     */
    const NamespaceString& getNss() const {
        return _resolvedNss;
    }

    /**
     * Returns a writable Collection copy that will be returned by current and future calls to this
     * function as well as getCollection(). Any previous Collection pointers that were returned may
     * be invalidated.
     *
     * CollectionCatalog::LifetimeMode::kManagedInWriteUnitOfWork will register an onCommit()
     * handler to reset the pointers and an onRollback() handler that will reset getCollection() to
     * the original Collection pointer.
     */
    Collection* getWritableCollection(
        CollectionCatalog::LifetimeMode mode =
            CollectionCatalog::LifetimeMode::kManagedInWriteUnitOfWork);

    OperationContext* getOperationContext() const {
        return _opCtx;
    }

protected:
    OperationContext* _opCtx = nullptr;
    AutoGetDb _autoDb;
    boost::optional<Lock::CollectionLock> _collLock;
    CollectionPtr _coll = nullptr;
    std::shared_ptr<const ViewDefinition> _view;

    // If the object was instantiated with a UUID, contains the resolved namespace, otherwise it is
    // the same as the input namespace string
    NamespaceString _resolvedNss;

    // Populated if getWritableCollection() is called.
    Collection* _writableColl = nullptr;
};

/**
 * RAII-style class that acquires the global MODE_IS lock. This class should only be used for reads.
 *
 * NOTE: Throws NamespaceNotFound if the collection UUID cannot be resolved to a nss.
 *
 * The collection references returned by this class will no longer be safe to retain after this
 * object goes out of scope. This object ensures the continued existence of a Collection reference,
 * if the collection exists when this object is instantiated.
 *
 * NOTE: this class is not safe to instantiate outside of AutoGetCollectionForReadLockFree. For
 * example, it does not perform database or collection level shard version checks; nor does it
 * establish a consistent storage snapshot with which to read.
 */
class AutoGetCollectionLockFree {
    AutoGetCollectionLockFree(const AutoGetCollectionLockFree&) = delete;
    AutoGetCollectionLockFree& operator=(const AutoGetCollectionLockFree&) = delete;

public:
    /**
     * Function used to customize restore after yield behavior
     */
    using RestoreFromYieldFn =
        std::function<void(std::shared_ptr<const Collection>&, OperationContext*, CollectionUUID)>;

    /**
     * Used by AutoGetCollectionForReadLockFree where it provides implementation for restore after
     * yield.
     */
    AutoGetCollectionLockFree(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        RestoreFromYieldFn restoreFromYield,
        AutoGetCollectionViewMode viewMode = AutoGetCollectionViewMode::kViewsForbidden,
        Date_t deadline = Date_t::max());

    explicit operator bool() const {
        // Use the CollectionPtr because it is updated if it yields whereas _collection is not until
        // restore.
        return static_cast<bool>(_collectionPtr);
    }

    /**
     * AutoGetCollectionLockFree can be used as a Collection pointer with the -> operator.
     */
    const Collection* operator->() const {
        return getCollection().get();
    }

    const CollectionPtr& operator*() const {
        return getCollection();
    }

    /**
     * Returns nullptr if the collection didn't exist.
     *
     * Deprecated in favor of the new ->(), *() and bool() accessors above!
     */
    const CollectionPtr& getCollection() const {
        return _collectionPtr;
    }

    /**
     * Returns nullptr if the view didn't exist.
     */
    const ViewDefinition* getView() const {
        return _view.get();
    }

    /**
     * Returns the resolved namespace of the collection or view.
     */
    const NamespaceString& getNss() const {
        return _resolvedNss;
    }

private:
    // Indicate that we are lock-free on code paths that can run either lock-free or locked for
    // different kinds of operations. Note: this class member is currently declared first so that it
    // destructs last, as a safety measure, but not because it is currently depended upon behavior.
    LockFreeReadsBlock _lockFreeReadsBlock;

    Lock::GlobalLock _globalLock;

    // If the object was instantiated with a UUID, contains the resolved namespace, otherwise it is
    // the same as the input namespace string
    NamespaceString _resolvedNss;

    // The Collection shared_ptr will keep the Collection instance alive even if it is removed from
    // the CollectionCatalog while this lock-free operation runs.
    std::shared_ptr<const Collection> _collection;

    // The CollectionPtr is the access point to the Collection instance for callers.
    CollectionPtr _collectionPtr;

    std::shared_ptr<const ViewDefinition> _view;
};

/**
 * This is a nested lock helper. If a higher level operation is running a lock-free read, then this
 * helper will follow suite and instantiate a AutoGetCollectionLockFree. Otherwise, it will
 * instantiate a regular AutoGetCollection helper.
 */
class AutoGetCollectionMaybeLockFree {
    AutoGetCollectionMaybeLockFree(const AutoGetCollectionMaybeLockFree&) = delete;
    AutoGetCollectionMaybeLockFree& operator=(const AutoGetCollectionMaybeLockFree&) = delete;

public:
    /**
     * Decides whether to instantiate a lock-free or locked helper based on whether a lock-free
     * operation is set on the opCtx.
     */
    AutoGetCollectionMaybeLockFree(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        LockMode modeColl,
        AutoGetCollectionViewMode viewMode = AutoGetCollectionViewMode::kViewsForbidden,
        Date_t deadline = Date_t::max());

private:
    boost::optional<AutoGetCollection> _autoGet;
    boost::optional<AutoGetCollectionLockFree> _autoGetLockFree;
};

/**
 * RAII-style class to handle the lifetime of writable Collections.
 * It does not take any locks, concurrency needs to be handled separately using explicit locks or
 * AutoGetCollection. This class can serve as an adaptor to unify different methods of acquiring a
 * writable collection.
 *
 * It is safe to re-use an instance for multiple WriteUnitOfWorks or to destroy it before the active
 * WriteUnitOfWork finishes.
 */
class CollectionWriter final {
public:
    // Gets the collection from the catalog for the provided uuid
    CollectionWriter(OperationContext* opCtx,
                     const CollectionUUID& uuid,
                     CollectionCatalog::LifetimeMode mode =
                         CollectionCatalog::LifetimeMode::kManagedInWriteUnitOfWork);
    // Gets the collection from the catalog for the provided namespace string
    CollectionWriter(OperationContext* opCtx,
                     const NamespaceString& nss,
                     CollectionCatalog::LifetimeMode mode =
                         CollectionCatalog::LifetimeMode::kManagedInWriteUnitOfWork);
    // Acts as an adaptor for AutoGetCollection
    CollectionWriter(AutoGetCollection& autoCollection,
                     CollectionCatalog::LifetimeMode mode =
                         CollectionCatalog::LifetimeMode::kManagedInWriteUnitOfWork);
    // Acts as an adaptor for a writable Collection that has been retrieved elsewhere
    CollectionWriter(Collection* writableCollection);

    ~CollectionWriter();

    // Not allowed to copy or move.
    CollectionWriter(const CollectionWriter&) = delete;
    CollectionWriter(CollectionWriter&&) = delete;
    CollectionWriter& operator=(const CollectionWriter&) = delete;
    CollectionWriter& operator=(CollectionWriter&&) = delete;

    explicit operator bool() const {
        return static_cast<bool>(get());
    }

    const Collection* operator->() const {
        return get().get();
    }

    const Collection& operator*() const {
        return *get().get();
    }

    const CollectionPtr& get() const {
        return *_collection;
    }

    // Returns writable Collection, any previous Collection that has been returned may be
    // invalidated.
    Collection* getWritableCollection();

private:
    // If this class is instantiated with the constructors that take UUID or nss we need somewhere
    // to store the CollectionPtr used. But if it is instantiated with an AutoGetCollection then the
    // lifetime of the object is managed there. To unify the two code paths we have a pointer that
    // points to either the CollectionPtr in an AutoGetCollection or to a stored CollectionPtr in
    // this instance. This can also be used to determine how we were instantiated.
    const CollectionPtr* _collection = nullptr;
    CollectionPtr _storedCollection;
    Collection* _writableCollection = nullptr;
    OperationContext* _opCtx = nullptr;
    CollectionCatalog::LifetimeMode _mode;

    struct SharedImpl;
    std::shared_ptr<SharedImpl> _sharedImpl;
};

/**
 * Writes to system.views need to use a stronger lock to prevent inconsistencies like view cycles.
 */
LockMode fixLockModeForSystemDotViewsChanges(const NamespaceString& nss, LockMode mode);

/**
 * RAII type to set and restore the timestamp read source on the recovery unit.
 *
 * Snapshot is abandoned in constructor and destructor, so it can only be used before
 * the recovery unit becomes active or when the existing snapshot is no longer needed.
 */
class ReadSourceScope {
public:
    ReadSourceScope(OperationContext* opCtx,
                    RecoveryUnit::ReadSource readSource,
                    boost::optional<Timestamp> provided = boost::none);
    ~ReadSourceScope();

private:
    OperationContext* _opCtx;
    RecoveryUnit::ReadSource _originalReadSource;
    Timestamp _originalReadTimestamp;
};

/**
 * RAII-style class to acquire proper locks using special oplog locking rules for oplog accesses.
 *
 * Only the global lock is acquired:
 * | OplogAccessMode | Global Lock |
 * +-----------------+-------------|
 * | kRead           | MODE_IS     |
 * | kWrite          | MODE_IX     |
 *
 * kLogOp is a special mode for replication operation logging and it behaves similar to kWrite. The
 * difference between kWrite and kLogOp is that kLogOp invariants that global IX lock is already
 * held. It is the caller's responsibility to ensure the global lock already held is still valid
 * within the lifetime of this object.
 *
 * Any acquired locks may be released when this object goes out of scope, therefore the oplog
 * collection reference returned by this class should not be retained.
 */
enum class OplogAccessMode { kRead, kWrite, kLogOp };
class AutoGetOplog {
    AutoGetOplog(const AutoGetOplog&) = delete;
    AutoGetOplog& operator=(const AutoGetOplog&) = delete;

public:
    AutoGetOplog(OperationContext* opCtx, OplogAccessMode mode, Date_t deadline = Date_t::max());

    /**
     * Return a pointer to the per-service-context LocalOplogInfo.
     */
    LocalOplogInfo* getOplogInfo() const {
        return _oplogInfo;
    }

    /**
     * Returns a pointer to the oplog collection or nullptr if the oplog collection didn't exist.
     */
    const CollectionPtr& getCollection() const {
        return *_oplog;
    }

private:
    ShouldNotConflictWithSecondaryBatchApplicationBlock
        _shouldNotConflictWithSecondaryBatchApplicationBlock;
    boost::optional<Lock::GlobalLock> _globalLock;
    boost::optional<Lock::DBLock> _dbWriteLock;
    boost::optional<Lock::CollectionLock> _collWriteLock;
    LocalOplogInfo* _oplogInfo;
    const CollectionPtr* _oplog;
};

}  // namespace mongo
