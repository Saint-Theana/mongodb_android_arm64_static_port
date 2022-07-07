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

#include "mongo/db/catalog/collection.h"

#include <sstream>

namespace mongo {

//
// CappedInsertNotifier
//

void CappedInsertNotifier::notifyAll() const {
    stdx::lock_guard<Latch> lk(_mutex);
    ++_version;
    _notifier.notify_all();
}

void CappedInsertNotifier::waitUntil(uint64_t prevVersion, Date_t deadline) const {
    stdx::unique_lock<Latch> lk(_mutex);
    while (!_dead && prevVersion == _version) {
        if (stdx::cv_status::timeout == _notifier.wait_until(lk, deadline.toSystemTimePoint())) {
            return;
        }
    }
}

void CappedInsertNotifier::kill() {
    stdx::lock_guard<Latch> lk(_mutex);
    _dead = true;
    _notifier.notify_all();
}

bool CappedInsertNotifier::isDead() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _dead;
}

CollectionPtr CollectionPtr::null;

CollectionPtr::CollectionPtr() : _collection(nullptr), _opCtx(nullptr) {}
CollectionPtr::CollectionPtr(OperationContext* opCtx,
                             const Collection* collection,
                             RestoreFn restoreFn)
    : _collection(collection), _opCtx(opCtx), _restoreFn(std::move(restoreFn)) {}
CollectionPtr::CollectionPtr(const Collection* collection, NoYieldTag)
    : CollectionPtr(nullptr, collection, nullptr) {}
CollectionPtr::CollectionPtr(Collection* collection) : CollectionPtr(collection, NoYieldTag{}) {}
CollectionPtr::CollectionPtr(CollectionPtr&&) = default;
CollectionPtr::~CollectionPtr() {}
CollectionPtr& CollectionPtr::operator=(CollectionPtr&&) = default;

bool CollectionPtr::_canYield() const {
    // We only set the opCtx when we use a constructor that allows yielding.
    return _opCtx;
}

void CollectionPtr::yield() const {
    // Yield if we are yieldable and have a valid collection
    if (_canYield() && _collection) {
        _yieldedUUID = _collection->uuid();
        _collection = nullptr;
    }
}
void CollectionPtr::restore() const {
    // Restore from yield if we are yieldable and if uuid was set in a previous yield.
    if (_canYield() && _yieldedUUID) {
        // We may only do yield restore when we were holding locks that was yielded so we need to
        // refresh from the catalog to make sure we have a valid collection pointer.
        _collection = _restoreFn(_opCtx, *_yieldedUUID);
        _yieldedUUID.reset();
    }
}

const BSONObj& CollectionPtr::getShardKeyPattern() const {
    dassert(_shardKeyPattern);
    return _shardKeyPattern.get();
}

// ----

namespace {
const auto getFactory = ServiceContext::declareDecoration<std::unique_ptr<Collection::Factory>>();
}

Collection::Factory* Collection::Factory::get(ServiceContext* service) {
    return getFactory(service).get();
}

Collection::Factory* Collection::Factory::get(OperationContext* opCtx) {
    return getFactory(opCtx->getServiceContext()).get();
};

void Collection::Factory::set(ServiceContext* service,
                              std::unique_ptr<Collection::Factory> newFactory) {
    auto& factory = getFactory(service);
    factory = std::move(newFactory);
}
}  // namespace mongo
