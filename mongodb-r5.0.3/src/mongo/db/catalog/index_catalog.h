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

#include <memory>
#include <vector>

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class Client;
class Collection;
class CollectionPtr;

class IndexDescriptor;
struct InsertDeleteOptions;

struct BsonRecord {
    RecordId id;
    Timestamp ts;
    const BSONObj* docPtr;
};

enum class IndexBuildMethod {
    /**
     * Use a collection scan to dump all keys into an external sorter. During this process,
     * concurrent client writes are accepted, and their generated keys are written into an
     * interceptor. On completion, this interceptor is drained and used to verify uniqueness
     * constraints on the index.
     *
     * This is the default for all index builds.
     */
    kHybrid,
    /**
     * Perform a collection scan to dump all keys into the exteral sorter, then into the index.
     * During this process, callers guarantee that no writes will be accepted on this collection.
     */
    kForeground,
};

StringData toString(IndexBuildMethod method);

enum class CreateIndexEntryFlags : int {
    kNone = 0x0,
    /**
     * kInitFromDisk avoids registering a change to undo this operation when set to true. You
     * must set this flag if calling this function outside of a WriteUnitOfWork.
     */
    kInitFromDisk = 0x1,
    /**
     * kIsReady controls whether the index will be directly available for query usage without
     * needing to complete the IndexBuildBlock process.
     */
    kIsReady = 0x2,
    /**
     * kFrozen indicates that the index is not usable and that it is not currently being
     * built. This is used when starting a node in standalone mode and a two-phase index build
     * is incomplete. kIsReady must not also be set.
     */
    kFrozen = 0x4
};

inline bool operator&(CreateIndexEntryFlags lhs, CreateIndexEntryFlags rhs) {
    return (static_cast<int>(lhs) & static_cast<int>(rhs)) != 0;
}

inline CreateIndexEntryFlags operator|(CreateIndexEntryFlags lhs, CreateIndexEntryFlags rhs) {
    return CreateIndexEntryFlags(static_cast<int>(lhs) | static_cast<int>(rhs));
}

/**
 * The IndexCatalog is owned by the Collection and is responsible for the lookup and lifetimes of
 * the indexes in a collection. Every collection has exactly one instance of this class.
 *
 * Callers are expected to have acquired the necessary locks while accessing this interface.
 *
 * To inspect the contents of this IndexCatalog, callers may obtain an iterator from
 * getIndexIterator().
 *
 * Index building functionality is supported by the IndexBuildBlock interface. However, it is
 * recommended to use the higher level MultiIndexBlock interface.
 *
 * Due to the resource-intensive nature of the index building process, this interface also provides
 * information on which indexes are available for queries through the following functions:
 *     int numIndexesTotal();
 *     int numIndexesReady();
 *     int numIndexesInProgress();
 */
class IndexCatalog {
public:
    class IndexIterator {
    public:
        virtual ~IndexIterator() = default;
        bool more();
        const IndexCatalogEntry* next();

    protected:
        /**
         * Advance the underlying iterator and returns the next index entry. Returns nullptr when
         * the iterator is exhausted.
         */
        virtual const IndexCatalogEntry* _advance() = 0;

    private:
        bool _start = true;
        const IndexCatalogEntry* _prev = nullptr;
        const IndexCatalogEntry* _next = nullptr;
    };

    class ReadyIndexesIterator : public IndexIterator {
    public:
        ReadyIndexesIterator(OperationContext* const opCtx,
                             IndexCatalogEntryContainer::const_iterator beginIterator,
                             IndexCatalogEntryContainer::const_iterator endIterator);

    private:
        const IndexCatalogEntry* _advance() override;

        OperationContext* const _opCtx;
        IndexCatalogEntryContainer::const_iterator _iterator;
        IndexCatalogEntryContainer::const_iterator _endIterator;
    };

    class AllIndexesIterator : public IndexIterator {
    public:
        /**
         * `ownedContainer` is a container whose lifetime the begin and end iterators depend
         * on. If the caller will keep control of the container for the entire iterator lifetime,
         * it should pass in a null value.
         */
        AllIndexesIterator(OperationContext* const opCtx,
                           std::unique_ptr<std::vector<IndexCatalogEntry*>> ownedContainer);

    private:
        const IndexCatalogEntry* _advance() override;

        OperationContext* const _opCtx;
        std::vector<IndexCatalogEntry*>::const_iterator _iterator;
        std::vector<IndexCatalogEntry*>::const_iterator _endIterator;
        std::unique_ptr<std::vector<IndexCatalogEntry*>> _ownedContainer;
    };

    IndexCatalog() = default;
    virtual ~IndexCatalog() = default;

    IndexCatalog(const IndexCatalog&) = default;
    IndexCatalog& operator=(const IndexCatalog&) = default;

    IndexCatalog(IndexCatalog&&) = delete;
    IndexCatalog& operator=(IndexCatalog&&) = delete;

    virtual std::unique_ptr<IndexCatalog> clone() const = 0;

    // Must be called before used.
    virtual Status init(OperationContext* const opCtx, Collection* collection) = 0;

    // ---- accessors -----

    virtual bool haveAnyIndexes() const = 0;

    virtual bool haveAnyIndexesInProgress() const = 0;

    virtual int numIndexesTotal(OperationContext* const opCtx) const = 0;

    virtual int numIndexesReady(OperationContext* const opCtx) const = 0;

    virtual int numIndexesInProgress(OperationContext* const opCtx) const = 0;

    virtual bool haveIdIndex(OperationContext* const opCtx) const = 0;

    /**
     * Returns the spec for the id index to create by default for this collection.
     */
    virtual BSONObj getDefaultIdIndexSpec(const CollectionPtr& collection) const = 0;

    virtual const IndexDescriptor* findIdIndex(OperationContext* const opCtx) const = 0;

    /**
     * Find index by name.  The index name uniquely identifies an index.
     *
     * @return null if cannot find
     */
    virtual const IndexDescriptor* findIndexByName(
        OperationContext* const opCtx,
        const StringData name,
        const bool includeUnfinishedIndexes = false) const = 0;

    /**
     * Find index by matching key pattern and options. The key pattern, collation spec, and partial
     * filter expression together uniquely identify an index.
     *
     * @return null if cannot find index, otherwise the index with a matching signature.
     */
    virtual const IndexDescriptor* findIndexByKeyPatternAndOptions(
        OperationContext* const opCtx,
        const BSONObj& key,
        const BSONObj& indexSpec,
        const bool includeUnfinishedIndexes = false) const = 0;

    /**
     * Find indexes with a matching key pattern, putting them into the vector 'matches'.  The key
     * pattern alone does not uniquely identify an index.
     *
     * Consider using 'findIndexByName' if expecting to match one index.
     */
    virtual void findIndexesByKeyPattern(
        OperationContext* const opCtx,
        const BSONObj& key,
        const bool includeUnfinishedIndexes,
        std::vector<const IndexDescriptor*>* const matches) const = 0;

    /**
     * Returns an index suitable for shard key range scans.
     *
     * This index:
     * - must be prefixed by 'shardKey', and
     * - must not be a partial index.
     * - must have the simple collation.
     *
     * If the parameter 'requireSingleKey' is true, then this index additionally must not be
     * multi-key.
     *
     * If no such index exists, returns NULL.
     */
    virtual const IndexDescriptor* findShardKeyPrefixedIndex(OperationContext* const opCtx,
                                                             const BSONObj& shardKey,
                                                             const bool requireSingleKey) const = 0;

    virtual void findIndexByType(OperationContext* const opCtx,
                                 const std::string& type,
                                 std::vector<const IndexDescriptor*>& matches,
                                 const bool includeUnfinishedIndexes = false) const = 0;

    /**
     * Reload the index definition for 'oldDesc' from the CollectionCatalogEntry.  'oldDesc'
     * must be a ready index that is already registered with the index catalog.  Returns an
     * unowned pointer to the descriptor for the new index definition.
     *
     * Use this method to notify the IndexCatalog that the spec for this index has changed.
     *
     * It is invalid to dereference 'oldDesc' after calling this method.
     *
     * The caller must hold the collection X lock and ensure no index builds are in progress
     * on the collection.
     */
    virtual const IndexDescriptor* refreshEntry(OperationContext* const opCtx,
                                                Collection* collection,
                                                const IndexDescriptor* const oldDesc) = 0;

    /**
     * Returns a pointer to the index catalog entry associated with 'desc'. Throws if there is no
     * such index. Never returns nullptr.
     */
    virtual const IndexCatalogEntry* getEntry(const IndexDescriptor* const desc) const = 0;

    /**
     * Returns a pointer to the index catalog entry associated with 'desc', where the caller assumes
     * shared ownership of the entry. Returns null if the entry does not exist.
     */
    virtual std::shared_ptr<const IndexCatalogEntry> getEntryShared(
        const IndexDescriptor*) const = 0;

    /**
     * Returns a vector of shared pointers to all index entries. Excludes unfinished indexes.
     */
    virtual std::vector<std::shared_ptr<const IndexCatalogEntry>> getAllReadyEntriesShared()
        const = 0;

    /**
     * Returns an iterator for the index descriptors in this IndexCatalog.
     */
    virtual std::unique_ptr<IndexIterator> getIndexIterator(
        OperationContext* const opCtx, const bool includeUnfinishedIndexes) const = 0;

    // ---- index set modifiers ------

    /*
     * Creates an index entry with the provided descriptor on the catalog's collection.
     *
     */

    virtual IndexCatalogEntry* createIndexEntry(OperationContext* opCtx,
                                                Collection* collection,
                                                std::unique_ptr<IndexDescriptor> descriptor,
                                                CreateIndexEntryFlags flags) = 0;

    /**
     * Call this only on an empty collection from inside a WriteUnitOfWork. Index creation on an
     * empty collection can be rolled back as part of a larger WUOW. Returns the full specification
     * of the created index, as it is stored in this index catalog.
     */
    virtual StatusWith<BSONObj> createIndexOnEmptyCollection(OperationContext* const opCtx,
                                                             Collection* collection,
                                                             const BSONObj spec) = 0;

    /**
     * Checks the spec 'original' to make sure nothing is incorrectly set and cleans up any legacy
     * fields. Lastly, checks whether the spec conflicts with ready and in-progress indexes.
     *
     * Returns an error Status or the cleaned up version of the non-conflicting spec. Returns
     * IndexAlreadyExists if the index already exists; IndexBuildAlreadyInProgress if the index is
     * already being built.
     */
    virtual StatusWith<BSONObj> prepareSpecForCreate(
        OperationContext* const opCtx,
        const CollectionPtr& collection,
        const BSONObj& original,
        const boost::optional<ResumeIndexInfo>& resumeInfo) const = 0;

    /**
     * Returns a copy of 'indexSpecsToBuild' that does not contain index specifications that already
     * exist or are already being built. If this is not done, an index build using
     * 'indexSpecsToBuild' may fail with an IndexAlreadyExists or IndexBuildAlreadyInProgress error.
     * If {buildIndexes:false} is set in the replica set config, also filters non-_id index specs
     * out of the results.
     *
     * Additionally verifies the specs are valid. Throws on any spec validation errors or conflicts
     * other than IndexAlreadyExists, which indicates that the index spec already exists is what
     * this function filters out.
     *
     * 'removeIndexBuildsToo' controls whether in-progress index builds are also filtered out. If
     * they are not, then IndexBuildAlreadyInProgress errors can be thrown.
     */
    virtual std::vector<BSONObj> removeExistingIndexes(
        OperationContext* const opCtx,
        const CollectionPtr& collection,
        const std::vector<BSONObj>& indexSpecsToBuild,
        const bool removeIndexBuildsToo) const = 0;

    /**
     * Filters out ready and in-progress indexes that already exist and returns the remaining
     * indexes. Additionally filters out non-_id indexes if the replica set member config has
     * {buildIndexes:false} set.
     *
     * Does no correctness verification of the provided specs, nor modifications for legacy reasons.
     *
     * This should only be used when we are confident in the specs, such as when specs are received
     * via replica set cloning or chunk migrations.
     */
    virtual std::vector<BSONObj> removeExistingIndexesNoChecks(
        OperationContext* const opCtx,
        const CollectionPtr& collection,
        const std::vector<BSONObj>& indexSpecsToBuild) const = 0;

    /**
     * Drops all indexes in the index catalog, optionally dropping the id index depending on the
     * 'includingIdIndex' parameter value. If 'onDropFn' is provided, it will be called before each
     * index is dropped to allow timestamping each individual drop.
     */
    virtual void dropAllIndexes(OperationContext* opCtx,
                                Collection* collection,
                                bool includingIdIndex,
                                std::function<void(const IndexDescriptor*)> onDropFn) = 0;
    virtual void dropAllIndexes(OperationContext* opCtx,
                                Collection* collection,
                                bool includingIdIndex) = 0;

    /**
     * Drops the index given its descriptor.
     *
     * The caller must hold the collection X lock and ensure no index builds are in progress on the
     * collection.
     */
    virtual Status dropIndex(OperationContext* const opCtx,
                             Collection* collection,
                             const IndexDescriptor* const desc) = 0;

    /**
     * Drops an unfinished index given its descriptor.
     *
     * The caller must hold the collection X lock.
     */
    virtual Status dropUnfinishedIndex(OperationContext* const opCtx,
                                       Collection* collection,
                                       const IndexDescriptor* const desc) = 0;

    /**
     * Drops the index given its catalog entry.
     *
     * The caller must hold the collection X lock.
     */
    virtual Status dropIndexEntry(OperationContext* opCtx,
                                  Collection* collection,
                                  IndexCatalogEntry* entry) = 0;

    /**
     * Deletes the index from the durable catalog on disk.
     */
    virtual void deleteIndexFromDisk(OperationContext* opCtx,
                                     Collection* collection,
                                     const std::string& indexName) = 0;

    // ---- modify single index

    /**
     * Sets the index 'desc' to be multikey with the provided 'multikeyPaths'.
     *
     * See IndexCatalogEntry::setMultikey().
     */
    virtual void setMultikeyPaths(OperationContext* const opCtx,
                                  const CollectionPtr& coll,
                                  const IndexDescriptor* const desc,
                                  const KeyStringSet& multikeyMetadataKeys,
                                  const MultikeyPaths& multikeyPaths) const = 0;

    // ----- data modifiers ------

    /**
     * When 'keysInsertedOut' is not null, it will be set to the number of index keys inserted by
     * this operation.
     *
     * This method may throw.
     */
    virtual Status indexRecords(OperationContext* const opCtx,
                                const CollectionPtr& collection,
                                const std::vector<BsonRecord>& bsonRecords,
                                int64_t* const keysInsertedOut) const = 0;

    /**
     * Both 'keysInsertedOut' and 'keysDeletedOut' are required and will be set to the number of
     * index keys inserted and deleted by this operation, respectively.
     *
     * This method may throw.
     */
    virtual Status updateRecord(OperationContext* const opCtx,
                                const CollectionPtr& coll,
                                const BSONObj& oldDoc,
                                const BSONObj& newDoc,
                                const RecordId& recordId,
                                int64_t* const keysInsertedOut,
                                int64_t* const keysDeletedOut) const = 0;

    /**
     * When 'keysDeletedOut' is not null, it will be set to the number of index keys removed by
     * this operation.
     */
    virtual void unindexRecord(OperationContext* const opCtx,
                               const CollectionPtr& collection,
                               const BSONObj& obj,
                               const RecordId& loc,
                               const bool noWarn,
                               int64_t* const keysDeletedOut) const = 0;

    /*
     * Attempt compaction on all ready indexes to regain disk space, if the storage engine's index
     * supports compaction in-place.
     */
    virtual Status compactIndexes(OperationContext* opCtx) const = 0;

    virtual std::string getAccessMethodName(const BSONObj& keyPattern) = 0;

    // public helpers

    /**
     * Returns length of longest index name.
     * This includes unfinished indexes.
     */
    virtual std::string::size_type getLongestIndexNameLength(OperationContext* opCtx) const = 0;

    /**
     * Detects and normalizes _id index key pattern if found.
     */
    virtual BSONObj fixIndexKey(const BSONObj& key) const = 0;

    /**
     * Fills out 'options' in order to indicate whether to allow dups or relax
     * index constraints, as needed by replication.
     */
    virtual void prepareInsertDeleteOptions(OperationContext* opCtx,
                                            const NamespaceString& ns,
                                            const IndexDescriptor* desc,
                                            InsertDeleteOptions* options) const = 0;

    virtual void indexBuildSuccess(OperationContext* opCtx,
                                   Collection* coll,
                                   IndexCatalogEntry* index) = 0;
};
}  // namespace mongo
