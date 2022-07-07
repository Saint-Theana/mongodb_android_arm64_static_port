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

#include <atomic>
#include <memory>
#include <set>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_metadata_access_stats.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

class BSONObjBuilder;
class MatchExpression;
struct UpdateTicket;
struct InsertDeleteOptions;

/**
 * An IndexAccessMethod is the interface through which all the mutation, lookup, and
 * traversal of index entries is done. The class is designed so that the underlying index
 * data structure is opaque to the caller.
 *
 * IndexAccessMethods for existing indices are obtained through the system catalog.
 *
 * We assume the caller has whatever locks required.  This interface is not thread safe.
 *
 */
class IndexAccessMethod {
    IndexAccessMethod(const IndexAccessMethod&) = delete;
    IndexAccessMethod& operator=(const IndexAccessMethod&) = delete;

public:
    using KeyHandlerFn = std::function<Status(const KeyString::Value&)>;
    using RecordIdHandlerFn = std::function<Status(const RecordId&)>;

    IndexAccessMethod() = default;
    virtual ~IndexAccessMethod() = default;

    //
    // Lookup, traversal, and mutation support
    //

    /**
     * Internally generate the keys {k1, ..., kn} for 'obj'.  For each key k, insert (k ->
     * 'loc') into the index.  'obj' is the object at the location 'loc'.
     * If 'result' is not null, 'numInserted' will be set to the number of keys added to the index
     * for the document and the number of duplicate keys will be appended to 'dupsInserted' if this
     * is a unique index and duplicates are allowed.
     *
     * If there is more than one key for 'obj', either all keys will be inserted or none will.
     *
     * The behavior of the insertion can be specified through 'options'.
     */
    virtual Status insert(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          const BSONObj& obj,
                          const RecordId& loc,
                          const InsertDeleteOptions& options,
                          KeyHandlerFn&& onDuplicateKey,
                          int64_t* numInserted) = 0;

    /**
     * Inserts the specified keys into the index. and determines whether these keys should cause the
     * index to become multikey. If so, this method also handles the task of marking the index as
     * multikey in the catalog, and sets the path-level multikey information if applicable.
     */
    virtual Status insertKeysAndUpdateMultikeyPaths(OperationContext* opCtx,
                                                    const CollectionPtr& coll,
                                                    const KeyStringSet& keys,
                                                    const KeyStringSet& multikeyMetadataKeys,
                                                    const MultikeyPaths& multikeyPaths,
                                                    const RecordId& loc,
                                                    const InsertDeleteOptions& options,
                                                    KeyHandlerFn&& onDuplicateKey,
                                                    int64_t* numInserted) = 0;

    /**
     * Inserts the specified keys into the index. Does not attempt to determine whether the
     * insertion of these keys should cause the index to become multikey. The 'numInserted' output
     * parameter, if non-nullptr, will be reset to the number of keys inserted by this function
     * call, or to zero in the case of either a non-OK return Status or an empty 'keys' argument.
     */
    virtual Status insertKeys(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              const KeyStringSet& keys,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              KeyHandlerFn&& onDuplicateKey,
                              int64_t* numInserted) = 0;

    /**
     * Analogous to insertKeys above, but remove the keys instead of inserting them.
     * 'numDeleted' will be set to the number of keys removed from the index for the provided keys.
     */
    virtual Status removeKeys(OperationContext* opCtx,
                              const KeyStringSet& keys,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numDeleted) = 0;

    /**
     * Gets the keys of the documents 'from' and 'to' and prepares them for the update.
     * Provides a ticket for actually performing the update.
     */
    virtual void prepareUpdate(OperationContext* opCtx,
                               const IndexCatalogEntry* index,
                               const BSONObj& from,
                               const BSONObj& to,
                               const RecordId& loc,
                               const InsertDeleteOptions& options,
                               UpdateTicket* ticket) const = 0;

    /**
     * Perform a validated update.  The keys for the 'from' object will be removed, and the keys
     * for the object 'to' will be added.  Returns OK if the update succeeded, failure if it did
     * not.  If an update does not succeed, the index will be unmodified, and the keys for
     * 'from' will remain.  Assumes that the index has not changed since prepareUpdate was
     * called.  If the index was changed, we may return an error, as our ticket may have been
     * invalidated.
     *
     * 'numInserted' will be set to the number of keys inserted into the index for the document.
     * 'numDeleted' will be set to the number of keys removed from the index for the document.
     */
    virtual Status update(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          const UpdateTicket& ticket,
                          int64_t* numInserted,
                          int64_t* numDeleted) = 0;

    /**
     * Returns an unpositioned cursor over 'this' index.
     */
    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                                   bool isForward) const = 0;
    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(
        OperationContext* opCtx) const = 0;

    // ------ index level operations ------


    /**
     * initializes this index
     * only called once for the lifetime of the index
     * if called multiple times, is an error
     */
    virtual Status initializeAsEmpty(OperationContext* opCtx) = 0;

    /**
     * Walk the entire index, checking the internal structure for consistency.
     * Set numKeys to the number of keys in the index.
     */
    virtual void validate(OperationContext* opCtx,
                          int64_t* numKeys,
                          IndexValidateResults* fullResults) const = 0;

    /**
     * Add custom statistics about this index to BSON object builder, for display.
     *
     * 'scale' is a scaling factor to apply to all byte statistics.
     *
     * Returns true if stats were appended.
     */
    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const = 0;

    /**
     * @return The number of bytes consumed by this index.
     *         Exactly what is counted is not defined based on padding, re-use, etc...
     */
    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const = 0;

    /**
     * The number of unused free bytes consumed by this index on disk.
     */
    virtual long long getFreeStorageBytes(OperationContext* opCtx) const = 0;

    virtual RecordId findSingle(OperationContext* opCtx, const BSONObj& key) const = 0;

    /**
     * Attempt compaction to regain disk space if the indexed record store supports
     * compaction-in-place.
     */
    virtual Status compact(OperationContext* opCtx) = 0;

    /**
     * Sets this index as multikey with the provided paths.
     */
    virtual void setIndexIsMultikey(OperationContext* opCtx,
                                    const CollectionPtr& collection,
                                    KeyStringSet multikeyMetadataKeys,
                                    MultikeyPaths paths) = 0;

    //
    // Bulk operations support
    //

    class BulkBuilder {
    public:
        using Sorter = mongo::Sorter<KeyString::Value, mongo::NullValue>;

        virtual ~BulkBuilder() = default;

        /**
         * Insert into the BulkBuilder as-if inserting into an IndexAccessMethod.
         *
         * 'saveCursorBeforeWrite' and 'restoreCursorAfterWrite' will be used to save and restore
         * the cursor around any constraint violation side table write that may occur, in case a WCE
         * occurs internally that would otherwise unposition the cursor.
         *
         * Note: we pass the cursor down into this insert function so we can limit cursor
         * save/restore to around constraints violation side table writes only. Otherwise, we would
         * have to save/restore around each insert() call just in case there is a side table write.
         */
        virtual Status insert(OperationContext* opCtx,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              const std::function<void()>& saveCursorBeforeWrite,
                              const std::function<void()>& restoreCursorAfterWrite) = 0;

        virtual const MultikeyPaths& getMultikeyPaths() const = 0;

        virtual bool isMultikey() const = 0;

        /**
         * Inserts all multikey metadata keys cached during the BulkBuilder's lifetime into the
         * underlying Sorter, finalizes it, and returns an iterator over the sorted dataset.
         */
        virtual Sorter::Iterator* done() = 0;

        /**
         * Returns number of keys inserted using this BulkBuilder.
         */
        virtual int64_t getKeysInserted() const = 0;

        /**
         * Persists on disk the keys that have been inserted using this BulkBuilder. Returns the
         * state of the underlying Sorter.
         */
        virtual Sorter::PersistedState persistDataForShutdown() = 0;
    };

    /**
     * Starts a bulk operation.
     * You work on the returned BulkBuilder and then call commitBulk.
     * This can return NULL, meaning bulk mode is not available.
     *
     * It is only legal to initiate bulk when the index is new and empty, or when resuming an index
     * build.
     *
     * maxMemoryUsageBytes: amount of memory consumed before the external sorter starts spilling to
     *                      disk
     * stateInfo: the information to use to resume the index build, or boost::none if starting a
     * new index build.
     */
    virtual std::unique_ptr<BulkBuilder> initiateBulk(
        size_t maxMemoryUsageBytes,
        const boost::optional<IndexStateInfo>& stateInfo,
        StringData dbName) = 0;

    /**
     * Call this when you are ready to finish your bulk work.
     * Pass in the BulkBuilder returned from initiateBulk.
     * @param bulk - Something created from initiateBulk
     * @param mayInterrupt - Is this commit interruptible (will cancel)
     * @param dupsAllowed - If false and 'dupRecords' is not null, append with the RecordIds of
     *                      the uninserted duplicates.
     * @param onDuplicateKeyInserted - Will be called for each duplicate key inserted into the
     * index.
     * @param onDuplicateRecord - If not nullptr, will be called for each RecordId of uninserted
     * duplicate keys.
     */
    virtual Status commitBulk(OperationContext* opCtx,
                              BulkBuilder* bulk,
                              bool dupsAllowed,
                              const KeyHandlerFn& onDuplicateKeyInserted,
                              const RecordIdHandlerFn& onDuplicateRecord) = 0;

    /**
     * Specifies whether getKeys should relax the index constraints or not, in order of most
     * permissive to least permissive.
     */
    enum class GetKeysMode {
        // Relax all constraints.
        kRelaxConstraints,
        // Relax all constraints on documents that don't apply to a partial index.
        kRelaxConstraintsUnfiltered,
        // Enforce all constraints.
        kEnforceConstraints
    };

    /**
     * Specifies whether getKeys is being used in the context of creating new keys, deleting
     * or validating existing keys.
     */
    enum class GetKeysContext { kRemovingKeys, kAddingKeys, kValidatingKeys };

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     * Based on 'mode', it will honor or ignore index constraints, e.g. duplicated key, key too
     * long, and geo index parsing errors. The ignoring of constraints is for replication due to
     * idempotency reasons. In those cases, the generated 'keys' will be empty.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information, then this function resizes
     * 'multikeyPaths' to have the same number of elements as the index key pattern and fills each
     * element with the prefixes of the indexed field that would cause this index to be multikey as
     * a result of inserting 'keys'.
     *
     * If the 'multikeyMetadataKeys' pointer is non-null, then the function will populate the
     * BSONObjSet with any multikey metadata keys generated while processing the document. These
     * keys are not associated with the document itself, but instead represent multi-key path
     * information that must be stored in a reserved keyspace within the index.
     *
     * If any key generation errors are encountered and suppressed due to the provided GetKeysMode,
     * 'onSuppressedErrorFn' is called.
     */
    using OnSuppressedErrorFn =
        std::function<void(Status status, const BSONObj& obj, boost::optional<RecordId> loc)>;
    virtual void getKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                         const BSONObj& obj,
                         GetKeysMode mode,
                         GetKeysContext context,
                         KeyStringSet* keys,
                         KeyStringSet* multikeyMetadataKeys,
                         MultikeyPaths* multikeyPaths,
                         boost::optional<RecordId> id,
                         OnSuppressedErrorFn onSuppressedError) const = 0;

    static OnSuppressedErrorFn kNoopOnSuppressedErrorFn;

    /**
     * Given the set of keys, multikeyMetadataKeys and multikeyPaths generated by a particular
     * document, return 'true' if the index should be marked as multikey and 'false' otherwise.
     */
    virtual bool shouldMarkIndexAsMultikey(size_t numberOfKeys,
                                           const KeyStringSet& multikeyMetadataKeys,
                                           const MultikeyPaths& multikeyPaths) const = 0;

    /**
     * Provides direct access to the SortedDataInterface. This should not be used to insert
     * documents into an index, except for testing purposes.
     */
    virtual SortedDataInterface* getSortedDataInterface() const = 0;
};

/**
 * Factory class that constructs an IndexAccessMethod depending on the type of index.
 */
class IndexAccessMethodFactory {
public:
    IndexAccessMethodFactory() = default;
    virtual ~IndexAccessMethodFactory() = default;

    static IndexAccessMethodFactory* get(ServiceContext* service);
    static IndexAccessMethodFactory* get(OperationContext* opCtx);
    static void set(ServiceContext* service,
                    std::unique_ptr<IndexAccessMethodFactory> collectionFactory);

    virtual std::unique_ptr<IndexAccessMethod> make(
        IndexCatalogEntry* entry, std::unique_ptr<SortedDataInterface> sortedDataInterface) = 0;
};

/**
 * Updates are two steps: verify that it's a valid update, and perform it.
 * prepareUpdate fills out the UpdateStatus and update actually applies it.
 */
struct UpdateTicket {
    bool _isValid{false};

    KeyStringSet oldKeys;
    KeyStringSet newKeys;

    KeyStringSet newMultikeyMetadataKeys;

    KeyStringSet removed;
    KeyStringSet added;

    RecordId loc;
    bool dupsAllowed;

    // Holds the path components that would cause this index to be multikey as a result of inserting
    // 'newKeys'. The 'newMultikeyPaths' vector remains empty if this index doesn't support
    // path-level multikey tracking.
    MultikeyPaths newMultikeyPaths;
};

/**
 * Flags we can set for inserts and deletes (and updates, which are kind of both).
 */
struct InsertDeleteOptions {
    // If there's an error, log() it.
    bool logIfError = false;

    // Are duplicate keys allowed in the index?
    bool dupsAllowed = false;

    // Only an index builder is allowed to insert into the index while it is building, so only the
    // index builder should set this to 'true'.
    bool fromIndexBuilder = false;

    // Should we relax the index constraints?
    IndexAccessMethod::GetKeysMode getKeysMode =
        IndexAccessMethod::GetKeysMode::kEnforceConstraints;
};

/**
 * Provides implementations for many functions in the IndexAccessMethod interface that will be
 * shared across concrete implementations.
 *
 * IndexCatalogEntry owns an instance of IndexAccessMethod; an IndexCatalogEntry is also required
 * for the initialization and core functionality of this abstract class. To avoid any circular
 * dependencies, it is important that IndexAccessMethod remain an interface.
 */
class AbstractIndexAccessMethod : public IndexAccessMethod {
    AbstractIndexAccessMethod(const AbstractIndexAccessMethod&) = delete;
    AbstractIndexAccessMethod& operator=(const AbstractIndexAccessMethod&) = delete;

public:
    /**
     * Splits the sets 'left' and 'right' into two sets, the first containing the elements that
     * only appeared in 'left', and the second containing only elements that appeared in 'right'.
     *
     * Note this considers objects which are not identical as distinct objects. For example,
     * setDifference({BSON("a" << 0.0)}, {BSON("a" << 0LL)}) would result in the pair
     * ( {BSON("a" << 0.0)}, {BSON("a" << 0LL)} ).
     */
    static std::pair<KeyStringSet, KeyStringSet> setDifference(const KeyStringSet& left,
                                                               const KeyStringSet& right);

    AbstractIndexAccessMethod(const IndexCatalogEntry* btreeState,
                              std::unique_ptr<SortedDataInterface> btree);

    Status insert(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options,
                  KeyHandlerFn&& onDuplicateKey,
                  int64_t* numInserted) final;

    Status insertKeys(OperationContext* opCtx,
                      const CollectionPtr& coll,
                      const KeyStringSet& keys,
                      const RecordId& loc,
                      const InsertDeleteOptions& options,
                      KeyHandlerFn&& onDuplicateKey,
                      int64_t* numInserted) final;

    Status insertKeysAndUpdateMultikeyPaths(OperationContext* opCtx,
                                            const CollectionPtr& coll,
                                            const KeyStringSet& keys,
                                            const KeyStringSet& multikeyMetadataKeys,
                                            const MultikeyPaths& multikeyPaths,
                                            const RecordId& loc,
                                            const InsertDeleteOptions& options,
                                            KeyHandlerFn&& onDuplicateKey,
                                            int64_t* numInserted) final;

    Status removeKeys(OperationContext* opCtx,
                      const KeyStringSet& keys,
                      const RecordId& loc,
                      const InsertDeleteOptions& options,
                      int64_t* numDeleted) final;

    void prepareUpdate(OperationContext* opCtx,
                       const IndexCatalogEntry* index,
                       const BSONObj& from,
                       const BSONObj& to,
                       const RecordId& loc,
                       const InsertDeleteOptions& options,
                       UpdateTicket* ticket) const final;

    Status update(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  const UpdateTicket& ticket,
                  int64_t* numInserted,
                  int64_t* numDeleted) final;

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool isForward) const final;
    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx) const final;

    Status initializeAsEmpty(OperationContext* opCtx) final;

    void validate(OperationContext* opCtx,
                  int64_t* numKeys,
                  IndexValidateResults* fullResults) const final;

    bool appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* result,
                           double scale) const final;

    long long getSpaceUsedBytes(OperationContext* opCtx) const final;

    long long getFreeStorageBytes(OperationContext* opCtx) const final;

    RecordId findSingle(OperationContext* opCtx, const BSONObj& key) const final;

    Status compact(OperationContext* opCtx) final;

    void setIndexIsMultikey(OperationContext* opCtx,
                            const CollectionPtr& collection,
                            KeyStringSet multikeyMetadataKeys,
                            MultikeyPaths paths) final;

    std::unique_ptr<BulkBuilder> initiateBulk(size_t maxMemoryUsageBytes,
                                              const boost::optional<IndexStateInfo>& stateInfo,
                                              StringData dbName) final;

    Status commitBulk(OperationContext* opCtx,
                      BulkBuilder* bulk,
                      bool dupsAllowed,
                      const KeyHandlerFn& onDuplicateKeyInserted,
                      const RecordIdHandlerFn& onDuplicateRecord) final;

    void getKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                 const BSONObj& obj,
                 GetKeysMode mode,
                 GetKeysContext context,
                 KeyStringSet* keys,
                 KeyStringSet* multikeyMetadataKeys,
                 MultikeyPaths* multikeyPaths,
                 boost::optional<RecordId> id,
                 OnSuppressedErrorFn onSuppressedError) const final;

    bool shouldMarkIndexAsMultikey(size_t numberOfKeys,
                                   const KeyStringSet& multikeyMetadataKeys,
                                   const MultikeyPaths& multikeyPaths) const override;

    SortedDataInterface* getSortedDataInterface() const override final;

protected:
    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information, then this function resizes
     * 'multikeyPaths' to have the same number of elements as the index key pattern and fills each
     * element with the prefixes of the indexed field that would cause this index to be multikey as
     * a result of inserting 'keys'.
     *
     * If the 'multikeyMetadataKeys' pointer is non-null, then the function will populate the
     * BSONObjSet with any multikey metadata keys generated while processing the document. These
     * keys are not associated with the document itself, but instead represent multi-key path
     * information that must be stored in a reserved keyspace within the index.
     */
    virtual void doGetKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                           const BSONObj& obj,
                           GetKeysContext context,
                           KeyStringSet* keys,
                           KeyStringSet* multikeyMetadataKeys,
                           MultikeyPaths* multikeyPaths,
                           boost::optional<RecordId> id) const = 0;

    const IndexCatalogEntry* const _indexCatalogEntry;  // owned by IndexCatalog
    const IndexDescriptor* const _descriptor;

private:
    class BulkBuilderImpl;

    /**
     * Removes a single key from the index.
     *
     * Used by remove() only.
     */
    void removeOneKey(OperationContext* opCtx,
                      const KeyString::Value& keyString,
                      const RecordId& loc,
                      bool dupsAllowed);
    /**
     * While inserting keys into index (from external sorter), if a duplicate key is detected
     * (when duplicates are not allowed), 'onDuplicateRecord' will be called if passed, otherwise a
     * DuplicateKey error will be returned.
     */
    Status _handleDuplicateKey(OperationContext* opCtx,
                               const KeyString::Value& dataKey,
                               const RecordIdHandlerFn& onDuplicateRecord);

    const std::unique_ptr<SortedDataInterface> _newInterface;
};

}  // namespace mongo
