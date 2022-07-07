// Tests that a user can only run a getMore on a cursor that they created.
// @tags: [requires_sharding]
(function() {
"use strict";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

function runTest(conn) {
    let adminDB = conn.getDB("admin");
    let hello = adminDB.runCommand("hello");
    assert.commandWorked(hello);
    const isMongos = (hello.msg === "isdbgrid");

    // Create the admin user.
    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    assert.eq(1, adminDB.auth("admin", "admin"));

    // Set up the test database.
    const testDBName = "auth_getMore";
    let testDB = adminDB.getSiblingDB(testDBName);
    testDB.dropDatabase();
    assert.commandWorked(testDB.foo.insert({_id: 0}));
    assert.commandWorked(testDB.foo.insert({_id: 1}));
    assert.commandWorked(testDB.foo.insert({_id: 2}));

    //
    // Test that a user can only run a getMore on a cursor that they created.
    //

    // Create two users, "Alice" and "Mallory".
    assert.commandWorked(
        testDB.runCommand({createUser: "Alice", pwd: "pwd", roles: ["readWrite"]}));
    assert.commandWorked(
        testDB.runCommand({createUser: "Mallory", pwd: "pwd", roles: ["readWrite"]}));
    adminDB.logout();

    // Test that "Mallory" cannot use a find cursor created by "Alice".
    assert.eq(1, testDB.auth("Alice", "pwd"));
    let res = assert.commandWorked(testDB.runCommand({find: "foo", batchSize: 0}));
    let cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    testDB.logout();
    assert.eq(1, testDB.auth("Mallory", "pwd"));
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                 ErrorCodes.Unauthorized,
                                 "read from another user's find cursor");
    testDB.logout();

    // Test that "Mallory" cannot use a legacy find cursor created by "Alice".
    testDB.getMongo().forceReadMode("legacy");
    assert.eq(1, testDB.auth("Alice", "pwd"));
    let cursor = testDB.foo.find().batchSize(2);
    cursor.next();
    cursor.next();
    testDB.logout();
    assert.eq(1, testDB.auth("Mallory", "pwd"));
    assert.throws(function() {
        cursor.next();
    }, [], "read from another user's legacy find cursor");
    testDB.logout();
    testDB.getMongo().forceReadMode("commands");

    // Test that "Mallory" cannot use an aggregation cursor created by "Alice".
    assert.eq(1, testDB.auth("Alice", "pwd"));
    res = assert.commandWorked(
        testDB.runCommand({aggregate: "foo", pipeline: [], cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    testDB.logout();
    assert.eq(1, testDB.auth("Mallory", "pwd"));
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                 ErrorCodes.Unauthorized,
                                 "read from another user's aggregate cursor");
    testDB.logout();

    // Test that "Mallory" cannot use a listCollections cursor created by "Alice".
    assert.eq(1, testDB.auth("Alice", "pwd"));
    res = assert.commandWorked(testDB.runCommand({listCollections: 1, cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    testDB.logout();
    assert.eq(1, testDB.auth("Mallory", "pwd"));
    assert.commandFailedWithCode(
        testDB.runCommand({getMore: cursorId, collection: "$cmd.listCollections"}),
        ErrorCodes.Unauthorized,
        "read from another user's listCollections cursor");
    testDB.logout();

    // Test that "Mallory" cannot use a listIndexes cursor created by "Alice".
    assert.eq(1, testDB.auth("Alice", "pwd"));
    res = assert.commandWorked(testDB.runCommand({listIndexes: "foo", cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    testDB.logout();
    assert.eq(1, testDB.auth("Mallory", "pwd"));
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                 ErrorCodes.Unauthorized,
                                 "read from another user's listIndexes cursor");
    testDB.logout();

    //
    // Test that a user can call getMore on an indexStats cursor they created, unless the
    // indexStats privilege has been revoked in the meantime.
    //

    assert.eq(1, adminDB.auth("admin", "admin"));
    assert.commandWorked(testDB.runCommand({
        createRole: "indexStatsOnly",
        privileges: [{resource: {db: testDBName, collection: "foo"}, actions: ["indexStats"]}],
        roles: []
    }));
    assert.commandWorked(
        testDB.runCommand({createUser: "Bob", pwd: "pwd", roles: ["indexStatsOnly"]}));
    adminDB.logout();

    assert.eq(1, testDB.auth("Bob", "pwd"));
    res = assert.commandWorked(testDB.runCommand(
        {aggregate: "foo", pipeline: [{$indexStats: {}}], cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "foo"}));

    res = assert.commandWorked(testDB.runCommand(
        {aggregate: "foo", pipeline: [{$indexStats: {}}], cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    testDB.logout();

    assert.eq(1, adminDB.auth("admin", "admin"));
    assert.commandWorked(
        testDB.runCommand({revokeRolesFromUser: "Bob", roles: ["indexStatsOnly"]}));
    adminDB.logout();

    assert.eq(1, testDB.auth("Bob", "pwd"));
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                 ErrorCodes.Unauthorized,
                                 "read from a cursor without required privileges");
    testDB.logout();

    //
    // Test that a user can call getMore on a listCollections cursor they created, unless the
    // readWrite privilege has been revoked in the meantime.
    //

    assert.eq(1, adminDB.auth("admin", "admin"));

    assert.commandWorked(testDB.runCommand({createUser: "Tom", pwd: "pwd", roles: ["readWrite"]}));
    adminDB.logout();

    assert.eq(1, testDB.auth("Tom", "pwd"));
    res = assert.commandWorked(testDB.runCommand({listCollections: 1, cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    assert.commandWorked(
        testDB.runCommand({getMore: cursorId, collection: "$cmd.listCollections"}));

    res = assert.commandWorked(testDB.runCommand({listCollections: 1, cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    testDB.logout();

    assert.eq(1, adminDB.auth("admin", "admin"));
    assert.commandWorked(testDB.runCommand({revokeRolesFromUser: "Tom", roles: ["readWrite"]}));
    adminDB.logout();

    assert.eq(1, testDB.auth("Tom", "pwd"));
    assert.commandFailedWithCode(
        testDB.runCommand({getMore: cursorId, collection: "$cmd.listCollections"}),
        ErrorCodes.Unauthorized,
        "read from a cursor without required privileges");
    testDB.logout();
    //
    // Test that a user can call getMore on a listIndexes cursor they created, unless the
    // readWrite privilege has been revoked in the meantime.
    //

    assert.eq(1, adminDB.auth("admin", "admin"));

    assert.commandWorked(testDB.runCommand({createUser: "Bill", pwd: "pwd", roles: ["readWrite"]}));
    adminDB.logout();

    assert.eq(1, testDB.auth("Bill", "pwd"));
    res = assert.commandWorked(testDB.runCommand({listIndexes: "foo", cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "foo"}));

    res = assert.commandWorked(testDB.runCommand({listIndexes: "foo", cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    testDB.logout();

    assert.eq(1, adminDB.auth("admin", "admin"));
    assert.commandWorked(testDB.runCommand({revokeRolesFromUser: "Bill", roles: ["readWrite"]}));
    adminDB.logout();

    assert.eq(1, testDB.auth("Bill", "pwd"));
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                 ErrorCodes.Unauthorized,
                                 "read from a cursor without required privileges");
    testDB.logout();

    //
    // Test that a user can run a getMore on an aggregate cursor they created, unless some
    // privileges required for the pipeline have been revoked in the meantime.
    //

    assert.eq(1, testDB.auth("Alice", "pwd"));
    res = assert.commandWorked(testDB.runCommand(
        {aggregate: "foo", pipeline: [{$match: {_id: 0}}, {$out: "out"}], cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "foo"}));

    res = assert.commandWorked(testDB.runCommand(
        {aggregate: "foo", pipeline: [{$match: {_id: 0}}, {$out: "out"}], cursor: {batchSize: 0}}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    testDB.logout();

    assert.eq(1, adminDB.auth("admin", "admin"));
    testDB.revokeRolesFromUser("Alice", ["readWrite"]);
    testDB.grantRolesToUser("Alice", ["read"]);
    adminDB.logout();

    assert.eq(1, testDB.auth("Alice", "pwd"));
    assert.commandFailedWithCode(
        testDB.runCommand(
            {aggregate: "foo", pipeline: [{$match: {_id: 0}}, {$out: "out"}], cursor: {}}),
        ErrorCodes.Unauthorized,
        "user should no longer have write privileges");
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                 ErrorCodes.Unauthorized,
                                 "wrote from a cursor without required privileges");
    testDB.logout();

    //
    // Test that if there were multiple users authenticated when the cursor was created, then at
    // least one of them must be authenticated in order to run getMore on the cursor.
    //

    assert.eq(1, adminDB.auth("admin", "admin"));
    assert.commandWorked(testDB.bar.insert({_id: 0}));

    // Create a user "fooUser" on the test database that can read the "foo" collection.
    assert.commandWorked(testDB.runCommand({
        createRole: "readFoo",
        privileges: [{resource: {db: testDBName, collection: "foo"}, actions: ["find"]}],
        roles: []
    }));
    assert.commandWorked(
        testDB.runCommand({createUser: "fooUser", pwd: "pwd", roles: ["readFoo"]}));

    // Create a user "fooBarUser" on the admin database that can read the "foo" and "bar"
    // collections.
    assert.commandWorked(adminDB.runCommand({
        createRole: "readFooBar",
        privileges: [
            {resource: {db: testDBName, collection: "foo"}, actions: ["find"]},
            {resource: {db: testDBName, collection: "bar"}, actions: ["find"]}
        ],
        roles: []
    }));
    assert.commandWorked(
        adminDB.runCommand({createUser: "fooBarUser", pwd: "pwd", roles: ["readFooBar"]}));

    adminDB.logout();

    // Test that a cursor created by "fooUser" and "fooBarUser" can be used by "fooUser".
    assert.eq(1, testDB.auth("fooUser", "pwd"));
    assert.eq(1, adminDB.auth("fooBarUser", "pwd"));
    res = assert.commandWorked(testDB.runCommand({find: "foo", batchSize: 0}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    adminDB.logout();
    assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "foo"}));
    testDB.logout();

    // Test that a cursor created by "fooUser" and "fooBarUser" cannot be used by "fooUser" if
    // "fooUser" does not have the privilege to read the collection.
    assert.eq(1, testDB.auth("fooUser", "pwd"));
    assert.eq(1, adminDB.auth("fooBarUser", "pwd"));
    res = assert.commandWorked(testDB.runCommand({find: "bar", batchSize: 0}));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    adminDB.logout();
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "bar"}),
                                 ErrorCodes.Unauthorized,
                                 "read from a cursor without required privileges");
    testDB.logout();

    // Test that an aggregate cursor created by "fooUser" and "fooBarUser" cannot be used by
    // "fooUser" if "fooUser" does not have all privileges required by the pipeline.
    assert.eq(1, testDB.auth("fooUser", "pwd"));
    assert.eq(1, adminDB.auth("fooBarUser", "pwd"));
    res = assert.commandWorked(testDB.runCommand({
        aggregate: "foo",
        pipeline: [
            {$match: {_id: 0}},
            {$lookup: {from: "bar", localField: "_id", foreignField: "_id", as: "bar"}}
        ],
        cursor: {batchSize: 0}
    }));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "foo"}));

    res = assert.commandWorked(testDB.runCommand({
        aggregate: "foo",
        pipeline: [
            {$match: {_id: 0}},
            {$lookup: {from: "bar", localField: "_id", foreignField: "_id", as: "bar"}}
        ],
        cursor: {batchSize: 0}
    }));
    cursorId = res.cursor.id;
    assert.neq(0, cursorId);
    adminDB.logout();
    assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                 ErrorCodes.Unauthorized,
                                 "read from a cursor without required privileges");
    testDB.logout();
}

// Run the test on a standalone.
let conn = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
runTest(conn);
MongoRunner.stopMongod(conn);

// Run the test on a sharded cluster.
let cluster = new ShardingTest(
    {shards: 1, mongos: 1, keyFile: "jstests/libs/key1", other: {shardOptions: {auth: ""}}});
runTest(cluster);
cluster.stop();
}());
