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

#include <vector>

#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/scoped_collection_metadata.h"

namespace mongo {

/**
 * Each collection on a MongoD instance is assigned an instance of CollectionShardingState for the
 * duration of its lifetime, which represents the shard's knowledge of that collection's shard
 * version and the set of chunks that it owns, as well as functions for tracking this state.
 *
 * This is the only interface that non-sharding consumers should be interfacing with.
 *
 * On shard servers, the implementation used is CollectionShardingRuntime.
 *
 * On embedded or non-shard servers, the implementation used is CollectionShardingStateStandalone,
 * which is a mostly empty implementation.
 *
 * The CollectionShardingStateFactory class below is used to instantiate the correct subclass of
 * CollectionShardingState at runtime.
 *
 * Synchronization rule: In order to obtain an instance of this object, the caller must have some
 * lock on the respective collection. Different functions require different lock levels though, so
 * be sure to check the function-level comments for details.
 */
class CollectionShardingState {
public:
    CollectionShardingState() = default;
    virtual ~CollectionShardingState() = default;

    CollectionShardingState(const CollectionShardingState&) = delete;
    CollectionShardingState& operator=(const CollectionShardingState&) = delete;

    /**
     * Obtains the sharding state for the specified collection. If it does not exist, it will be
     * created and will remain in memory until the collection is dropped.
     *
     * Must be called with some lock held on the specific collection being looked up and the
     * returned pointer must not be stored.
     */
    static CollectionShardingState* get(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Returns the names of the collections that have a CollectionShardingState.
     */
    static std::vector<NamespaceString> getCollectionNames(OperationContext* opCtx);

    /**
     * Obtain a pointer to the CollectionShardingState that remains safe to access without holding
     * a collection lock. Should be called instead of the regular get() if no collection lock is
     * held. The returned CollectionShardingState instance should not be modified!
     */
    static std::shared_ptr<CollectionShardingState> getSharedForLockFreeReads(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Reports all collections which have filtering information associated.
     */
    static void appendInfoForShardingStateCommand(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Attaches info for server status.
     */
    static void appendInfoForServerStatus(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * If the shard currently doesn't know whether the collection is sharded or not, it will throw
     * StaleShardVersion.
     *
     * If the request doesn't have a shard version all collections will be treated as UNSHARDED.
     *
     * The returned object *is not safe* to access after the collection lock has been dropped.
     */
    virtual ScopedCollectionDescription getCollectionDescription(OperationContext* opCtx) = 0;

    /**
     * This method must be called with an OperationShardingState, which specifies an expected shard
     * version for the collection and it will invariant otherwise.
     *
     * If the shard currently doesn't know whether the collection is sharded or not, or if the
     * expected shard version doesn't match with the one in the OperationShardingState, it will
     * throw StaleShardVersion.
     *
     * If the operation context contains an 'atClusterTime', the returned filtering object will be
     * tied to a specific point in time. Otherwise, it will reference the latest cluster time
     * available.
     *
     * If 'kDisallowOrphanCleanup' is passed as 'OrphanCleanupPolicy', the range deleter won't
     * delete any orphan chunk associated with this ScopedCollectionFilter until the object is
     * destroyed. The intended users of this mode are read operations, which need to yield the
     * collection lock, but still perform filtering.
     *
     * If the request doesn't have a shard version all collections will be treated as UNSHARDED.
     *
     * Use 'getCollectionDescription' for other cases, like obtaining information about
     * sharding-related properties of the collection are necessary that won't change under
     * collection IX/IS lock (e.g., isSharded or the shard key).
     *
     * The returned object *is safe* to access even after the collection lock has been dropped.
     */
    enum class OrphanCleanupPolicy { kDisallowOrphanCleanup, kAllowOrphanCleanup };
    virtual ScopedCollectionFilter getOwnershipFilter(OperationContext* opCtx,
                                                      OrphanCleanupPolicy orphanCleanupPolicy) = 0;

    /**
     * Checks whether the shard version in the operation context is compatible with the shard
     * version of the collection and if not, throws StaleConfigException populated with the received
     * and wanted versions.
     *
     * If the request is not versioned all collections will be treated as UNSHARDED.
     */
    virtual void checkShardVersionOrThrow(OperationContext* opCtx) = 0;

    /**
     * Appends information about the shard version of the collection.
     */
    virtual void appendShardVersion(BSONObjBuilder* builder) = 0;

    /**
     * Returns the number of ranges scheduled for deletion on the collection.
     */
    virtual size_t numberOfRangesScheduledForDeletion() const = 0;

protected:
    /**
     * It is the caller's responsibility to ensure that the collection locks for this namespace are
     * held when this is called. The returned pointer should never be stored.
     */
    static CollectionShardingState* get_UNSAFE(ServiceContext* svcCtx, const NamespaceString& nss);
};

/**
 * Singleton factory to instantiate CollectionShardingState objects specific to the type of instance
 * which is running.
 */
class CollectionShardingStateFactory {
    CollectionShardingStateFactory(const CollectionShardingStateFactory&) = delete;
    CollectionShardingStateFactory& operator=(const CollectionShardingStateFactory&) = delete;

public:
    static void set(ServiceContext* service,
                    std::unique_ptr<CollectionShardingStateFactory> factory);
    static void clear(ServiceContext* service);

    virtual ~CollectionShardingStateFactory() = default;

    /**
     * Must be called prior to destruction to wait for any ongoing work to complete.
     */
    virtual void join() = 0;

    /**
     * Called by the CollectionShardingState::get method once per newly cached namespace. It is
     * invoked under a mutex and must not acquire any locks or do blocking work.
     *
     * Implementations must be thread-safe when called from multiple threads.
     */
    virtual std::unique_ptr<CollectionShardingState> make(const NamespaceString& nss) = 0;

protected:
    CollectionShardingStateFactory(ServiceContext* serviceContext)
        : _serviceContext(serviceContext) {}

    // The service context which owns this factory
    ServiceContext* const _serviceContext;
};

}  // namespace mongo
