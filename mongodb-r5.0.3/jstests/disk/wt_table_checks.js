/**
 * Tests that MongoDB sets the WiredTiger table logging settings correctly under different
 * circumstances.
 *
 * @tags: [requires_wiredtiger, requires_journaling]
 */
(function() {

load('jstests/disk/libs/wt_file_helper.js');

// Create a bunch of collections under various database names.
let conn = MongoRunner.runMongod({});
const dbpath = conn.dbpath;

for (let i = 0; i < 10; i++) {
    assert.commandWorked(conn.getDB(i.toString()).createCollection(i.toString()));
}

MongoRunner.stopMongod(conn);

/**
 * Test 1. The regular case, where no table logging setting modifications are needed.
 */
jsTest.log("Test 1.");

conn = startMongodOnExistingPath(dbpath, {});
checkLog.containsJson(conn, 4366408, {loggingEnabled: true});
MongoRunner.stopMongod(conn);

/**
 * Test 2. Repair checks all of the table logging settings.
 */
jsTest.log("Test 2.");

assertRepairSucceeds(dbpath, conn.port, {});

// Cannot use checkLog here as the server is no longer running.
let logContents = rawMongoProgramOutput();
assert(logContents.indexOf(
           "Modifying the table logging settings for all existing WiredTiger tables") > 0);

/**
 * Test 3. Explicitly create the '_wt_table_checks' file to force all of the table logging setting
 * modifications to be made.
 */
jsTest.log("Test 3.");

let files = listFiles(dbpath);
for (f in files) {
    assert(!files[f].name.includes("_wt_table_checks"));
}

writeFile(dbpath + "/_wt_table_checks", "");

conn = startMongodOnExistingPath(dbpath, {});
checkLog.containsJson(
    conn, 4366405, {loggingEnabled: true, repair: false, hasPreviouslyIncompleteTableChecks: true});
MongoRunner.stopMongod(conn);

/**
 * Test 4. Change into a single replica set, which requires all of the table logging settings to be
 * updated. But simulate an interruption/crash while starting up during the table logging check
 * phase.
 *
 * The next start up will detect an unclean shutdown causing all of the table logging settings to be
 * updated.
 */
jsTest.log("Test 4.");

conn = startMongodOnExistingPath(dbpath, {
    replSet: "mySet",
    setParameter:
        "failpoint.crashAfterUpdatingFirstTableLoggingSettings=" + tojson({"mode": "alwaysOn"})
});
assert(!conn);

// Cannot use checkLog here as the server is no longer running.
logContents = rawMongoProgramOutput();
assert(logContents.indexOf(
           "Crashing due to 'crashAfterUpdatingFirstTableLoggingSettings' fail point") > 0);

// The '_wt_table_checks' still exists, so all table logging settings should be modified.
conn = startMongodOnExistingPath(dbpath, {});
checkLog.containsJson(
    conn, 4366405, {loggingEnabled: true, repair: false, hasPreviouslyIncompleteTableChecks: true});
MongoRunner.stopMongod(conn);

/**
 * Test 5. Change into a single node replica set, which requires all of the table logging settings
 * to be updated as the node was successfully started up as a standalone the last time.
 */
jsTest.log("Test 5.");

conn = startMongodOnExistingPath(dbpath, {replSet: "mySet"});
checkLog.containsJson(conn, 4366406, {loggingEnabled: false});
MongoRunner.stopMongod(conn);
}());
