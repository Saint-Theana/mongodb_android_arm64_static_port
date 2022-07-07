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
#include <sstream>
#include <string>
#include <time.h>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/json.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
using std::string;
using std::stringstream;
using std::unique_ptr;

namespace {
TEST(WiredTigerRecordStoreTest, StorageSizeStatisticsDisabled) {
    WiredTigerHarnessHelper harnessHelper("statistics=(none)");
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("a.b"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
    ASSERT_THROWS(rs->storageSize(opCtx.get()), AssertionException);
}

TEST(WiredTigerRecordStoreTest, SizeStorer1) {
    unique_ptr<WiredTigerHarnessHelper> harnessHelper(new WiredTigerHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    string ident = rs->getIdent();
    string uri = checked_cast<WiredTigerRecordStore*>(rs.get())->getURI();

    string indexUri = WiredTigerKVEngine::kTableUriPrefix + "myindex";
    const bool enableWtLogging = false;
    WiredTigerSizeStorer ss(harnessHelper->conn(), indexUri, enableWtLogging);
    checked_cast<WiredTigerRecordStore*>(rs.get())->setSizeStorer(&ss);

    int N = 12;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            for (int i = 0; i < N; i++) {
                StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp());
                ASSERT_OK(res.getStatus());
            }
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(N, rs->numRecords(opCtx.get()));
    }

    rs.reset(nullptr);

    {
        auto& info = *ss.load(uri);
        ASSERT_EQUALS(N, info.numRecords.load());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WiredTigerRecordStore::Params params;
        params.ns = "a.b"_sd;
        params.ident = ident;
        params.engineName = kWiredTigerEngineName;
        params.isCapped = false;
        params.keyFormat = KeyFormat::Long;
        params.overwrite = true;
        params.isEphemeral = false;
        params.cappedCallback = nullptr;
        params.sizeStorer = &ss;
        params.isReadOnly = false;
        params.tracksSizeAdjustments = true;
        params.forceUpdateWithFullDocument = false;

        auto ret = new StandardWiredTigerRecordStore(nullptr, opCtx.get(), params);
        ret->postConstructorInit(opCtx.get());
        rs.reset(ret);
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(N, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WiredTigerRecoveryUnit* ru = checked_cast<WiredTigerRecoveryUnit*>(opCtx->recoveryUnit());

        {
            WriteUnitOfWork uow(opCtx.get());
            WT_SESSION* s = ru->getSession()->getSession();
            invariantWTOK(s->create(s, indexUri.c_str(), ""));
            uow.commit();
        }

        ss.flush(true);
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const bool enableWtLogging = false;
        WiredTigerSizeStorer ss2(harnessHelper->conn(), indexUri, enableWtLogging);
        auto info = ss2.load(uri);
        ASSERT_EQUALS(N, info->numRecords.load());
    }

    rs.reset(nullptr);  // this has to be deleted before ss
}

class SizeStorerUpdateTest : public mongo::unittest::Test {
private:
    virtual void setUp() {
        harnessHelper.reset(new WiredTigerHarnessHelper());
        const bool enableWtLogging = false;
        sizeStorer.reset(
            new WiredTigerSizeStorer(harnessHelper->conn(),
                                     WiredTigerKVEngine::kTableUriPrefix + "sizeStorer",
                                     enableWtLogging));
        rs = harnessHelper->newNonCappedRecordStore();
        WiredTigerRecordStore* wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());
        wtrs->setSizeStorer(sizeStorer.get());
        ident = wtrs->getIdent();
        uri = wtrs->getURI();
    }
    virtual void tearDown() {
        rs.reset(nullptr);
        sizeStorer->flush(false);
        sizeStorer.reset(nullptr);
        harnessHelper.reset(nullptr);
    }

protected:
    long long getNumRecords() const {
        return sizeStorer->load(uri)->numRecords.load();
    }

    long long getDataSize() const {
        return sizeStorer->load(uri)->dataSize.load();
    }

    std::unique_ptr<WiredTigerHarnessHelper> harnessHelper;
    std::unique_ptr<WiredTigerSizeStorer> sizeStorer;
    std::unique_ptr<RecordStore> rs;
    std::string ident;
    std::string uri;
};

// Basic validation - size storer data is updated.
TEST_F(SizeStorerUpdateTest, Basic) {
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    long long val = 5;
    rs->updateStatsAfterRepair(opCtx.get(), val, val);
    ASSERT_EQUALS(getNumRecords(), val);
    ASSERT_EQUALS(getDataSize(), val);
}

}  // namespace
}  // namespace mongo
