/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/executor/task_executor_cursor.h"

#include "mongo/client/dbclient_base.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace executor {
namespace {

class TaskExecutorCursorFixture : public mongo::unittest::Test {
public:
    void setUp() override {
        std::shared_ptr<NetworkInterface> ni = makeNetworkInterface("TaskExecutorCursorTest");
        auto tp = std::make_unique<NetworkInterfaceThreadPool>(ni.get());

        _executor = std::make_unique<ThreadPoolTaskExecutor>(std::move(tp), std::move(ni));
        _executor->startup();
    };

    void tearDown() override {
        _executor->shutdown();
        _executor.reset();
    };

    TaskExecutor* executor() {
        return _executor.get();
    }

    ServiceContext::UniqueServiceContext _serviceCtx = ServiceContext::make();
    std::unique_ptr<ThreadPoolTaskExecutor> _executor;
};


// Test that we can actually use a TaskExecutorCursor to read multiple batches from a remote host
TEST_F(TaskExecutorCursorFixture, Basic) {
    auto client = _serviceCtx->makeClient("TaskExecutorCursorTest");
    auto opCtx = client->makeOperationContext();

    // Write 100 documents to "test.test" via dbclient
    auto swConn = unittest::getFixtureConnectionString().connect("TaskExecutorCursorTest");
    uassertStatusOK(swConn.getStatus());
    auto dbclient = std::move(swConn.getValue());

    const size_t numDocs = 100;

    std::vector<BSONObj> docs;
    docs.reserve(numDocs);
    for (size_t i = 0; i < numDocs; ++i) {
        docs.emplace_back(BSON("x" << int(i)));
    }
    dbclient->dropCollection("test.test");
    dbclient->insert("test.test", docs);
    ASSERT_EQUALS(dbclient->count(NamespaceString("test.test")), numDocs);

    RemoteCommandRequest rcr(unittest::getFixtureConnectionString().getServers().front(),
                             "test",
                             BSON("find"
                                  << "test"
                                  << "batchSize" << 10),
                             opCtx.get());

    TaskExecutorCursor tec(executor(), rcr, [] {
        TaskExecutorCursor::Options opts;
        opts.batchSize = 10;
        return opts;
    }());

    size_t count = 0;
    while (auto doc = tec.getNext(opCtx.get())) {
        count++;
    }

    ASSERT_EQUALS(count, numDocs);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
