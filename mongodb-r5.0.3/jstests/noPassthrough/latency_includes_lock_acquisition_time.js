/**
 * Test that latency reported in the profiler and logs include lock acquisition time for various
 * CRUD operations.
 * @tags: [requires_profiling]
 */
(function() {
"use strict";

load("jstests/libs/logv2_helpers.js");

/**
 * Configures the server to wait for 'millis' while acquiring locks in the CRUD path, then
 * invokes the no-arguments function 'func', then disables the aforementioned lock wait
 * behavior.
 */
function runWithWait(millis, func) {
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "setAutoGetCollectionWait",
        mode: "alwaysOn",
        data: {waitForMillis: millis}
    }));
    func();
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "setAutoGetCollectionWait",
        mode: "off",
    }));
}

load("jstests/libs/profiler.js");

let hangMillis = 200;
let padding = hangMillis / 10;

let conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");
let testDB = conn.getDB("test");
let testColl = testDB.lock_acquisition_time;

function runTests() {
    // Profile all operations.
    assert.commandWorked(testDB.setProfilingLevel(0));
    testDB.system.profile.drop();
    assert.commandWorked(testDB.setProfilingLevel(2));

    // Test that insert profiler/logs include lock acquisition time. Rather than parsing the log
    // lines, we are just verifying that the log line appears, which implies that the recorded
    // latency exceeds slowms.
    runWithWait(hangMillis, function() {
        assert.commandWorked(testColl.insert({a: 1}));
    });
    let profileEntry;
    if (conn.writeMode() === "commands") {
        profileEntry = getLatestProfilerEntry(testDB, {
            ns: testColl.getFullName(),
            "command.insert": testColl.getName(),
        });
    } else {
        profileEntry = getLatestProfilerEntry(testDB, {
            op: "insert",
            ns: testColl.getFullName(),
        });
    }
    assert.gte(profileEntry.millis, hangMillis - padding);
    checkLog.contains(
        conn,
        !isJsonLog(conn)
            ? (conn.writeMode() === "commands" ? "insert { insert: \"lock_acquisition_time\""
                                               : "insert test.lock_acquisition_time")
            : (conn.writeMode() === "commands"
                   ? /"ns":"test.lock_acquisition_time".*"command":{"insert"/
                   : /"type":"insert","ns":"test.lock_acquisition_time/));

    // Test that update profiler/logs include lock acquisition time.
    runWithWait(hangMillis, function() {
        assert.commandWorked(testColl.update({}, {$set: {b: 1}}));
    });
    profileEntry = getLatestProfilerEntry(testDB, {
        ns: testColl.getFullName(),
        "command.u": {$eq: {$set: {b: 1}}},
    });
    assert.gte(profileEntry.millis, hangMillis - padding);
    checkLog.contains(conn,
                      !isJsonLog(conn)
                          ? "update { update: \"lock_acquisition_time\""
                          : /"ns":"test.\$cmd".*"command":{"update":"lock_acquisition_time"/);

    // Test that find profiler/logs include lock acquisition time.
    runWithWait(hangMillis, function() {
        assert.eq(1, testColl.find({b: 1}).itcount());
    });
    profileEntry = getLatestProfilerEntry(testDB, {
        ns: testColl.getFullName(),
        "command.find": testColl.getName(),
    });
    assert.gte(profileEntry.millis, hangMillis - padding);
    checkLog.contains(conn,
                      !isJsonLog(conn) ? "find { find: \"lock_acquisition_time\""
                                       : '"command":{"find":"lock_acquisition_time"');

    // Test that getMore profiler/logs include lock acquisition time.
    assert.commandWorked(testColl.insert([{a: 2}, {a: 3}]));
    runWithWait(hangMillis, function() {
        // Include a batchSize in order to ensure that a getMore is issued.
        assert.eq(3, testColl.find().batchSize(2).itcount());
    });
    profileEntry = getLatestProfilerEntry(testDB, {
        ns: testColl.getFullName(),
        "command.getMore": {$exists: true},
    });
    assert.gte(profileEntry.millis, hangMillis - padding);
    checkLog.contains(conn,
                      !isJsonLog(conn) ? "originatingCommand: { find: \"lock_acquisition_time\""
                                       : '"originatingCommand":{"find":"lock_acquisition_time"');
    assert.commandWorked(testColl.remove({a: {$gt: 1}}));

    // Test that aggregate profiler/logs include lock acquisition time.
    runWithWait(hangMillis, function() {
        assert.eq(1, testColl.aggregate([{$match: {b: 1}}]).itcount());
    });
    profileEntry = getLatestProfilerEntry(testDB, {
        ns: testColl.getFullName(),
        "command.aggregate": testColl.getName(),
    });
    assert.gte(profileEntry.millis, hangMillis - padding);
    checkLog.contains(conn,
                      !isJsonLog(conn) ? "aggregate { aggregate: \"lock_acquisition_time\""
                                       : '"command":{"aggregate":"lock_acquisition_time"');

    // Test that count profiler/logs include lock acquisition time.
    runWithWait(hangMillis, function() {
        assert.eq(1, testColl.count());
    });
    profileEntry = getLatestProfilerEntry(testDB, {
        ns: testColl.getFullName(),
        "command.count": testColl.getName(),
    });
    assert.gte(profileEntry.millis, hangMillis - padding);
    checkLog.contains(conn,
                      !isJsonLog(conn) ? "count { count: \"lock_acquisition_time\""
                                       : '"command":{"count":"lock_acquisition_time"');

    // Test that distinct profiler/logs include lock acquisition time.
    runWithWait(hangMillis, function() {
        assert.eq([1], testColl.distinct("a"));
    });
    profileEntry = getLatestProfilerEntry(testDB, {
        ns: testColl.getFullName(),
        "command.distinct": testColl.getName(),
    });
    assert.gte(profileEntry.millis, hangMillis - padding);
    checkLog.contains(conn,
                      !isJsonLog(conn) ? "distinct { distinct: \"lock_acquisition_time\""
                                       : '"command":{"distinct":"lock_acquisition_time"');

    // Test that delete profiler/logs include lock acquisition time.
    runWithWait(hangMillis, function() {
        assert.commandWorked(testColl.remove({b: 1}));
    });
    profileEntry = getLatestProfilerEntry(testDB, {
        ns: testColl.getFullName(),
        "command.q": {b: 1},
    });
    assert.gte(profileEntry.millis, hangMillis - padding);
    checkLog.contains(conn,
                      !isJsonLog(conn) ? "delete { delete: \"lock_acquisition_time\""
                                       : '"command":{"delete":"lock_acquisition_time"');
}

// Run the tests once with read and write commands and once with legacy ops.
runTests();
conn.forceWriteMode("compatibility");
conn.forceReadMode("legacy");
runTests();
MongoRunner.stopMongod(conn);
}());
