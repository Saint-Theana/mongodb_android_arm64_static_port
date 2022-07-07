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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/bson/oid.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#define ASSERT_ID_EQ(EXPR, ID)                        \
    [](boost::optional<Record> record, RecordId id) { \
        ASSERT(record);                               \
        ASSERT_EQ(record->id, id);                    \
    }((EXPR), (ID));

namespace {

using namespace mongo;

class CollectionTest : public CatalogTestFixture {
protected:
    void makeCapped(NamespaceString nss, long long cappedSize = 8192);
    void makeCollectionForMultikey(NamespaceString nss, StringData indexName);
};

void CollectionTest::makeCapped(NamespaceString nss, long long cappedSize) {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = cappedSize;  // Maximum size of capped collection in bytes.
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));
}

TEST_F(CollectionTest, CappedNotifierKillAndIsDead) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    ASSERT_FALSE(notifier->isDead());
    notifier->kill();
    ASSERT(notifier->isDead());
}

TEST_F(CollectionTest, CappedNotifierTimeouts) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    ASSERT_EQ(notifier->getVersion(), 0u);

    auto before = Date_t::now();
    notifier->waitUntil(0u, before + Milliseconds(25));
    auto after = Date_t::now();
    ASSERT_GTE(after - before, Milliseconds(25));
    ASSERT_EQ(notifier->getVersion(), 0u);
}

TEST_F(CollectionTest, CappedNotifierWaitAfterNotifyIsImmediate) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();

    auto prevVersion = notifier->getVersion();
    notifier->notifyAll();
    auto thisVersion = prevVersion + 1;
    ASSERT_EQ(notifier->getVersion(), thisVersion);

    auto before = Date_t::now();
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
}

TEST_F(CollectionTest, CappedNotifierWaitUntilAsynchronousNotifyAll) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    stdx::thread thread([before, prevVersion, &notifier] {
        notifier->waitUntil(prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->notifyAll();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), thisVersion);
}

TEST_F(CollectionTest, CappedNotifierWaitUntilAsynchronousKill) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();

    auto before = Date_t::now();
    stdx::thread thread([before, prevVersion, &notifier] {
        notifier->waitUntil(prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->kill();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), prevVersion);
}

TEST_F(CollectionTest, HaveCappedWaiters) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    ASSERT_FALSE(col->getCappedCallback()->haveCappedWaiters());
    {
        auto notifier = col->getCappedInsertNotifier();
        ASSERT(col->getCappedCallback()->haveCappedWaiters());
    }
    ASSERT_FALSE(col->getCappedCallback()->haveCappedWaiters());
}

TEST_F(CollectionTest, NotifyCappedWaitersIfNeeded) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    col->getCappedCallback()->notifyCappedWaitersIfNeeded();
    {
        auto notifier = col->getCappedInsertNotifier();
        ASSERT_EQ(notifier->getVersion(), 0u);
        col->getCappedCallback()->notifyCappedWaitersIfNeeded();
        ASSERT_EQ(notifier->getVersion(), 1u);
    }
}

TEST_F(CollectionTest, AsynchronouslyNotifyCappedWaitersIfNeeded) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    notifier->waitUntil(prevVersion, before + Milliseconds(25));
    stdx::thread thread([before, prevVersion, &col] {
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        col->getCappedCallback()->notifyCappedWaitersIfNeeded();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), thisVersion);
}

void CollectionTest::makeCollectionForMultikey(NamespaceString nss, StringData indexName) {
    auto opCtx = operationContext();
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        auto db = autoColl.ensureDbExists();
        WriteUnitOfWork wuow(opCtx);
        ASSERT(db->createCollection(opCtx, nss));
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        auto collWriter = autoColl.getWritableCollection();
        ASSERT_OK(collWriter->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx, collWriter, BSON("v" << 2 << "name" << indexName << "key" << BSON("a" << 1))));
        wuow.commit();
    }
}

TEST_F(CollectionTest, SetIndexIsMultikey) {
    NamespaceString nss("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    MultikeyPaths paths = {{0}};
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_FALSE(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
}

TEST_F(CollectionTest, SetIndexIsMultikeyRemovesUncommittedChangesOnRollback) {
    NamespaceString nss("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    MultikeyPaths paths = {{0}};

    {
        FailPointEnableBlock failPoint("EFTAlwaysThrowWCEOnWrite");
        WriteUnitOfWork wuow(opCtx);
        ASSERT_THROWS(coll->setIndexIsMultikey(opCtx, indexName, paths), WriteConflictException);
    }

    // After rolling back the above WUOW, we should succeed in retrying setIndexIsMultikey().
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
}

TEST_F(CollectionTest, ForceSetIndexIsMultikey) {
    NamespaceString nss("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    MultikeyPaths paths = {{0}};
    {
        WriteUnitOfWork wuow(opCtx);
        auto desc = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
        coll->forceSetIndexIsMultikey(opCtx, desc, true, paths);
        wuow.commit();
    }
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_FALSE(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
}

TEST_F(CollectionTest, ForceSetIndexIsMultikeyRemovesUncommittedChangesOnRollback) {
    NamespaceString nss("test.t");
    auto indexName = "myindex"_sd;
    makeCollectionForMultikey(nss, indexName);

    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const auto& coll = autoColl.getCollection();
    ASSERT(coll);
    MultikeyPaths paths = {{0}};

    {
        FailPointEnableBlock failPoint("EFTAlwaysThrowWCEOnWrite");
        WriteUnitOfWork wuow(opCtx);
        auto desc = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
        ASSERT_THROWS(coll->forceSetIndexIsMultikey(opCtx, desc, true, paths),
                      WriteConflictException);
    }

    // After rolling back the above WUOW, we should succeed in retrying setIndexIsMultikey().
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT(coll->setIndexIsMultikey(opCtx, indexName, paths));
        wuow.commit();
    }
}

TEST_F(CatalogTestFixture, CollectionPtrNoYieldTag) {
    CollectionMock mock(NamespaceString("test.t"));

    CollectionPtr coll(&mock, CollectionPtr::NoYieldTag{});
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &mock);

    // Yield should be a no-op
    coll.yield();
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &mock);

    // Restore should also be a no-op
    coll.restore();
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &mock);

    coll.reset();
    ASSERT_FALSE(coll);
}

TEST_F(CatalogTestFixture, CollectionPtrYieldable) {
    CollectionMock beforeYield(NamespaceString("test.t"));
    CollectionMock afterYield(NamespaceString("test.t"));

    int numRestoreCalls = 0;

    CollectionPtr coll(operationContext(),
                       &beforeYield,
                       [&afterYield, &numRestoreCalls](OperationContext*, CollectionUUID) {
                           ++numRestoreCalls;
                           return &afterYield;
                       });

    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &beforeYield);

    // Calling yield should invalidate
    coll.yield();
    ASSERT_FALSE(coll);
    ASSERT_EQ(numRestoreCalls, 0);

    // Calling yield when already yielded is a no-op
    coll.yield();
    ASSERT_FALSE(coll);
    ASSERT_EQ(numRestoreCalls, 0);

    // Restore should replace Collection pointer
    coll.restore();
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &afterYield);
    ASSERT_NE(coll.get(), &beforeYield);
    ASSERT_EQ(numRestoreCalls, 1);

    // Calling restore when we are valid is a no-op
    coll.restore();
    ASSERT_TRUE(coll);
    ASSERT_EQ(coll.get(), &afterYield);
    ASSERT_NE(coll.get(), &beforeYield);
    ASSERT_EQ(numRestoreCalls, 1);

    coll.reset();
    ASSERT_FALSE(coll);
}

TEST_F(CatalogTestFixture, IsNotCapped) {
    NamespaceString nss("test.t");
    CollectionOptions options;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollectionForRead acfr(operationContext(), nss);
    const CollectionPtr& coll = acfr.getCollection();
    ASSERT(!coll->isCapped());
}

TEST_F(CatalogTestFixture, CappedDeleteRecord) {
    // Insert a document into a capped collection that has a maximum document size of 1.
    NamespaceString nss("test.t");
    CollectionOptions options;
    options.capped = true;
    options.cappedMaxDocs = 1;
    // Large enough to use 'cappedMaxDocs' as the primary indicator for capped deletes.
    options.cappedSize = 512 * 1024 * 1024;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT_EQUALS(0, coll->numRecords(operationContext()));

    BSONObj firstDoc = BSON("_id" << 1);
    BSONObj secondDoc = BSON("_id" << 2);

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(coll->insertDocument(operationContext(), InsertStatement(firstDoc), nullptr));
        wuow.commit();
    }

    ASSERT_EQUALS(1, coll->numRecords(operationContext()));

    // Inserting the second document will remove the first one.
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(coll->insertDocument(operationContext(), InsertStatement(secondDoc), nullptr));
        wuow.commit();
    }

    ASSERT_EQUALS(1, coll->numRecords(operationContext()));

    auto cursor = coll->getRecordStore()->getCursor(operationContext());
    auto record = cursor->next();
    ASSERT(record);
    ASSERT(record->data.toBson().woCompare(secondDoc) == 0);
    ASSERT(!cursor->next());
}

TEST_F(CatalogTestFixture, CappedDeleteMultipleRecords) {
    // Insert multiple records at once, requiring multiple deletes.
    NamespaceString nss("test.t");
    CollectionOptions options;
    options.capped = true;
    options.cappedMaxDocs = 10;
    // Large enough to use 'cappedMaxDocs' as the primary indicator for capped deletes.
    options.cappedSize = 512 * 1024 * 1024;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();

    ASSERT_EQUALS(0, coll->numRecords(operationContext()));

    const int nToInsertFirst = options.cappedMaxDocs / 2;
    const int nToInsertSecond = options.cappedMaxDocs;

    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = 0; i < nToInsertFirst; i++) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(coll->insertDocument(operationContext(), InsertStatement(doc), nullptr));
        }
        wuow.commit();
    }

    ASSERT_EQUALS(nToInsertFirst, coll->numRecords(operationContext()));

    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = nToInsertFirst; i < nToInsertFirst + nToInsertSecond; i++) {
            BSONObj doc = BSON("_id" << i);
            ASSERT_OK(coll->insertDocument(operationContext(), InsertStatement(doc), nullptr));
        }
        wuow.commit();
    }

    ASSERT_EQUALS(options.cappedMaxDocs, coll->numRecords(operationContext()));

    const int firstExpectedId = nToInsertFirst + nToInsertSecond - options.cappedMaxDocs;

    int numSeen = 0;
    auto cursor = coll->getRecordStore()->getCursor(operationContext());
    while (auto record = cursor->next()) {
        const BSONObj expectedDoc = BSON("_id" << firstExpectedId + numSeen);
        ASSERT(record->data.toBson().woCompare(expectedDoc) == 0);
        numSeen++;
    }
}

TEST_F(CatalogTestFixture, CappedVisibilityEmptyInitialState) {
    NamespaceString nss("test.t");
    CollectionOptions options;
    options.capped = true;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    auto doInsert = [&](OperationContext* opCtx) -> RecordId {
        std::string data = "data";
        return uassertStatusOK(rs->insertRecord(opCtx, data.c_str(), data.size(), Timestamp()));
    };

    auto longLivedClient = getServiceContext()->makeClient("longLived");
    auto longLivedOpCtx = longLivedClient->makeOperationContext();
    WriteUnitOfWork longLivedWUOW(longLivedOpCtx.get());

    // Collection is really empty.
    ASSERT(!rs->getCursor(longLivedOpCtx.get(), true)->next());
    ASSERT(!rs->getCursor(longLivedOpCtx.get(), false)->next());

    RecordId lowestHiddenId = doInsert(longLivedOpCtx.get());
    RecordId otherId;

    {
        WriteUnitOfWork wuow(operationContext());

        // Can't see uncommitted write from other operation.
        ASSERT(!rs->getCursor(operationContext())->seekExact(lowestHiddenId));

        ASSERT(!rs->getCursor(operationContext(), true)->next());
        ASSERT(!rs->getCursor(operationContext(), false)->next());

        otherId = doInsert(operationContext());

        // Can read own writes.
        ASSERT_ID_EQ(rs->getCursor(operationContext(), true)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(operationContext(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(operationContext())->seekExact(otherId), otherId);

        wuow.commit();
    }

    // longLivedOpCtx is still on old snapshot so it can't see otherId yet.
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT(!rs->getCursor(longLivedOpCtx.get())->seekExact(otherId));

    // Make all documents visible and let longLivedOp get a new snapshot.
    longLivedWUOW.commit();

    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), otherId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(otherId), otherId);
}

TEST_F(CatalogTestFixture, CappedVisibilityNonEmptyInitialState) {
    NamespaceString nss("test.t");
    CollectionOptions options;
    options.capped = true;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    auto doInsert = [&](OperationContext* opCtx) -> RecordId {
        std::string data = "data";
        return uassertStatusOK(rs->insertRecord(opCtx, data.c_str(), data.size(), Timestamp()));
    };

    auto longLivedClient = getServiceContext()->makeClient("longLived");
    auto longLivedOpCtx = longLivedClient->makeOperationContext();

    RecordId initialId;
    {
        WriteUnitOfWork wuow(longLivedOpCtx.get());
        initialId = doInsert(longLivedOpCtx.get());
        wuow.commit();
    }

    WriteUnitOfWork longLivedWUOW(longLivedOpCtx.get());

    // Can see initial doc.
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), initialId);

    RecordId lowestHiddenId = doInsert(longLivedOpCtx.get());

    // Collection still looks like it only has a single doc to iteration but not seekExact.
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(initialId), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);

    RecordId otherId;
    {
        WriteUnitOfWork wuow(operationContext());

        // Can only see committed writes from other operation.
        ASSERT_ID_EQ(rs->getCursor(operationContext())->seekExact(initialId), initialId);
        ASSERT(!rs->getCursor(operationContext())->seekExact(lowestHiddenId));

        ASSERT_ID_EQ(rs->getCursor(operationContext(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(operationContext(), false)->next(), initialId);

        otherId = doInsert(operationContext());

        ASSERT_ID_EQ(rs->getCursor(operationContext(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(operationContext(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(operationContext())->seekExact(otherId), otherId);

        wuow.commit();

        ASSERT_ID_EQ(rs->getCursor(operationContext(), true)->next(), initialId);
        ASSERT_ID_EQ(rs->getCursor(operationContext(), false)->next(), otherId);
        ASSERT_ID_EQ(rs->getCursor(operationContext())->seekExact(otherId), otherId);
        ASSERT(!rs->getCursor(operationContext())->seekExact(lowestHiddenId));
    }

    // longLivedOpCtx is still on old snapshot so it can't see otherId yet.
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT(!rs->getCursor(longLivedOpCtx.get())->seekExact(otherId));

    // This makes all documents visible and lets longLivedOpCtx get a new snapshot.
    longLivedWUOW.commit();

    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), true)->next(), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get(), false)->next(), otherId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(initialId), initialId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(lowestHiddenId), lowestHiddenId);
    ASSERT_ID_EQ(rs->getCursor(longLivedOpCtx.get())->seekExact(otherId), otherId);
}

TEST_F(CatalogTestFixture, CappedCursorRollover) {
    NamespaceString nss("test.t");
    CollectionOptions options;
    options.capped = true;
    options.cappedMaxDocs = 5;
    // Large enough to use 'cappedMaxDocs' as the primary indicator for capped deletes.
    options.cappedSize = 512 * 1024 * 1024;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    // First insert 3 documents.
    const int numToInsertFirst = 3;

    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = 0; i < numToInsertFirst; ++i) {
            const BSONObj doc = BSON("_id" << i);
            ASSERT_OK(coll->insertDocument(operationContext(), InsertStatement(doc), nullptr));
        }
        wuow.commit();
    }

    // Setup the cursor that should rollover.
    auto otherClient = getServiceContext()->makeClient("otherClient");
    auto otherOpCtx = otherClient->makeOperationContext();
    auto cursor = rs->getCursor(otherOpCtx.get());
    ASSERT(cursor->next());
    cursor->save();
    otherOpCtx->recoveryUnit()->abandonSnapshot();

    // Insert 10 documents which causes a rollover.
    {
        WriteUnitOfWork wuow(operationContext());
        for (int i = numToInsertFirst; i < numToInsertFirst + 10; ++i) {
            const BSONObj doc = BSON("_id" << i);
            ASSERT_OK(coll->insertDocument(operationContext(), InsertStatement(doc), nullptr));
        }
        wuow.commit();
    }

    // Cursor should now be dead.
    ASSERT_FALSE(cursor->restore());
    ASSERT(!cursor->next());
}

TEST_F(CatalogTestFixture, CappedCursorYieldFirst) {
    NamespaceString nss("test.t");
    CollectionOptions options;
    options.capped = true;
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, options));

    AutoGetCollection autoColl(operationContext(), nss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    RecordStore* rs = coll->getRecordStore();

    RecordId recordId;
    {
        WriteUnitOfWork wuow(operationContext());
        std::string data = "data";
        StatusWith<RecordId> res =
            rs->insertRecord(operationContext(), data.c_str(), data.size(), Timestamp());
        ASSERT_OK(res.getStatus());
        recordId = res.getValue();
        wuow.commit();
    }

    auto cursor = rs->getCursor(operationContext());

    // See that things work if you yield before you first call next().
    cursor->save();
    operationContext()->recoveryUnit()->abandonSnapshot();

    ASSERT_TRUE(cursor->restore());

    auto record = cursor->next();
    ASSERT(record);
    ASSERT_EQ(recordId, record->id);

    ASSERT(!cursor->next());
}

}  // namespace
