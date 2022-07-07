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

#include <iostream>
#include <string>

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

/**
 * Fixture for testing correctness of multikey paths.
 *
 * Has helper functions for creating indexes and asserting that the multikey paths after performing
 * write operations are as expected.
 */
class MultikeyPathsTest : public unittest::Test {
public:
    MultikeyPathsTest() : _nss("unittests.multikey_paths") {}

    void setUp() final {
        AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_IX);
        auto db = autoColl.ensureDbExists();

        WriteUnitOfWork wuow(_opCtx.get());
        ASSERT(db->createCollection(_opCtx.get(), _nss));
        wuow.commit();
    }

    void tearDown() final {
        AutoGetCollection autoColl(_opCtx.get(), _nss, MODE_X);
        if (!autoColl) {
            return;
        }

        auto db = autoColl.getDb();

        WriteUnitOfWork wuow(_opCtx.get());
        ASSERT_OK(db->dropCollection(_opCtx.get(), _nss));
        wuow.commit();
    }

    Status createIndex(const CollectionPtr& collection, BSONObj indexSpec) {
        return dbtests::createIndexFromSpec(_opCtx.get(), collection->ns().ns(), indexSpec);
    }

    void assertMultikeyPaths(const CollectionPtr& collection,
                             BSONObj keyPattern,
                             const MultikeyPaths& expectedMultikeyPaths) {
        const IndexCatalog* indexCatalog = collection->getIndexCatalog();
        std::vector<const IndexDescriptor*> indexes;
        indexCatalog->findIndexesByKeyPattern(_opCtx.get(), keyPattern, false, &indexes);
        ASSERT_EQ(indexes.size(), 1U);
        auto desc = indexes[0];
        const IndexCatalogEntry* ice = indexCatalog->getEntry(desc);

        auto actualMultikeyPaths = ice->getMultikeyPaths(_opCtx.get());
        ASSERT_FALSE(actualMultikeyPaths.empty());
        const bool match = (expectedMultikeyPaths == actualMultikeyPaths);
        if (!match) {
            FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                               << ", Actual: " << dumpMultikeyPaths(actualMultikeyPaths));
        }
        ASSERT_TRUE(match);
    }

protected:
    const ServiceContext::UniqueOperationContext _opCtx = cc().makeOperationContext();
    const NamespaceString _nss;

private:
    std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
        std::stringstream ss;

        ss << "[ ";
        for (const auto& multikeyComponents : multikeyPaths) {
            ss << "[ ";
            for (const auto& multikeyComponent : multikeyComponents) {
                ss << multikeyComponent << " ";
            }
            ss << "] ";
        }
        ss << "]";

        return ss.str();
    }
};

TEST_F(MultikeyPathsTest, PathsUpdatedOnIndexCreation) {
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_X);
    invariant(collection);

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug));
        wuow.commit();
    }

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection.getCollection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnIndexCreationWithMultipleDocuments) {
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_X);
    invariant(collection);

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug));
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5)),
            nullOpDebug));
        wuow.commit();
    }

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection.getCollection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    assertMultikeyPaths(collection.getCollection(), keyPattern, {{0U}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentInsert) {
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_X);
    invariant(collection);

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection.getCollection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5)),
            nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {{0U}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedOnDocumentUpdate) {
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_X);
    invariant(collection);

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection.getCollection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(), InsertStatement(BSON("_id" << 0 << "a" << 5)), nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(
        collection.getCollection(), keyPattern, {MultikeyComponents{}, MultikeyComponents{}});

    {
        auto cursor = collection->getCursor(_opCtx.get());
        auto record = cursor->next();
        invariant(record);

        auto oldDoc = collection->docFor(_opCtx.get(), record->id);
        {
            WriteUnitOfWork wuow(_opCtx.get());
            const bool indexesAffected = true;
            OpDebug* opDebug = nullptr;
            CollectionUpdateArgs args;
            collection->updateDocument(
                _opCtx.get(),
                record->id,
                oldDoc,
                BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3)),
                indexesAffected,
                opDebug,
                &args);
            wuow.commit();
        }
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsNotUpdatedOnDocumentDelete) {
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_X);
    invariant(collection);

    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    createIndex(collection.getCollection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPattern << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(BSON("_id" << 0 << "a" << 5 << "b" << BSON_ARRAY(1 << 2 << 3))),
            nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});

    {
        auto cursor = collection->getCursor(_opCtx.get());
        auto record = cursor->next();
        invariant(record);

        {
            WriteUnitOfWork wuow(_opCtx.get());
            OpDebug* const nullOpDebug = nullptr;
            collection->deleteDocument(_opCtx.get(), kUninitializedStmtId, record->id, nullOpDebug);
            wuow.commit();
        }
    }

    assertMultikeyPaths(collection.getCollection(), keyPattern, {MultikeyComponents{}, {0U}});
}

TEST_F(MultikeyPathsTest, PathsUpdatedForMultipleIndexesOnDocumentInsert) {
    AutoGetCollection collection(_opCtx.get(), _nss, MODE_X);
    invariant(collection);

    BSONObj keyPatternAB = BSON("a" << 1 << "b" << 1);
    createIndex(collection.getCollection(),
                BSON("name"
                     << "a_1_b_1"
                     << "key" << keyPatternAB << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();

    BSONObj keyPatternAC = BSON("a" << 1 << "c" << 1);
    createIndex(collection.getCollection(),
                BSON("name"
                     << "a_1_c_1"
                     << "key" << keyPatternAC << "v" << static_cast<int>(kIndexVersion)))
        .transitional_ignore();
    {
        WriteUnitOfWork wuow(_opCtx.get());
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(),
            InsertStatement(
                BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5 << "c" << 8)),
            nullOpDebug));
        wuow.commit();
    }

    assertMultikeyPaths(collection.getCollection(), keyPatternAB, {{0U}, MultikeyComponents{}});
    assertMultikeyPaths(collection.getCollection(), keyPatternAC, {{0U}, MultikeyComponents{}});
}

}  // namespace
}  // namespace mongo
