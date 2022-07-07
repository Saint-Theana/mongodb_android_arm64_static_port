/**
 * Tests that legacy writes to secondaries result in no answer and a disconnection.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const collname = "disconnect_on_legacy_write_to_secondary";
const coll = primary.getDB("test")[collname];
const secondaryDb = secondary.getDB("test");
const secondaryColl = secondaryDb[collname];

// Never retry on network error, because this test needs to detect the network error.
TestData.skipRetryOnNetworkError = true;
secondary.forceWriteMode('legacy');
assert.commandWorked(coll.insert([{_id: 'deleteme'}, {_id: 'updateme'}]));
rst.awaitReplication();

jsTestLog("Trying legacy insert on secondary");
secondaryColl.insert({_id: 'no_insert_on_secondary'});
let res = assert.throws(() => secondaryDb.adminCommand({ping: 1}));
assert(isNetworkError(res));
// We should automatically reconnect after the failed command.
assert.commandWorked(secondaryDb.adminCommand({ping: 1}));

jsTestLog("Trying legacy update on secondary");
secondaryColl.update({_id: 'updateme'}, {'$set': {x: 1}});
res = assert.throws(() => secondaryDb.adminCommand({ping: 1}));
assert(isNetworkError(res));
// We should automatically reconnect after the failed command.
assert.commandWorked(secondaryDb.adminCommand({ping: 1}));

jsTestLog("Trying legacy remove on secondary");
secondaryColl.remove({_id: 'deleteme'}, {'$set': {x: 1}});
res = assert.throws(() => secondaryDb.adminCommand({ping: 1}));
assert(isNetworkError(res));
// We should automatically reconnect after the failed command.
assert.commandWorked(secondaryDb.adminCommand({ping: 1}));

// Do the stepdown tests on a separate connection to avoid interfering with the
// ReplSetTest mechanism.
const primaryAdmin = primary.getDB("admin");
const primaryDataConn = new Mongo(primary.host);
const primaryDb = primaryDataConn.getDB("test");
const primaryColl = primaryDb[collname];
primaryDataConn.forceWriteMode('legacy');

function getNotPrimaryLegacyUnackWritesCounter() {
    return assert.commandWorked(primaryAdmin.adminCommand({serverStatus: 1}))
        .metrics.repl.network.notPrimaryLegacyUnacknowledgedWrites;
}

function runStepDownTest({description, failpoint, operation}) {
    jsTestLog("Enabling failpoint to block " + description + "s");
    let failPoint = configureFailPoint(primaryAdmin, failpoint);

    let failedLegacyUnackWritesBefore = getNotPrimaryLegacyUnackWritesCounter();

    jsTestLog("Trying legacy " + description + " on stepping-down primary");
    operation();
    failPoint.wait();
    jsTestLog("Within " + description + ": stepping down and disabling failpoint");
    assert.commandWorked(primaryAdmin.adminCommand({replSetStepDown: 60, force: true}));
    rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    failPoint.off();
    res = assert.throws(() => primaryDb.adminCommand({ping: 1}));
    assert(isNetworkError(res));
    // We should automatically reconnect after the failed command.
    assert.commandWorked(primaryDb.adminCommand({ping: 1}));

    // Validate the number of legacy unacknowledged writes failed due to step down resulted
    // in network disconnection.
    let failedLegacyUnackWritesAfter = getNotPrimaryLegacyUnackWritesCounter();
    assert.eq(failedLegacyUnackWritesAfter, failedLegacyUnackWritesBefore + 1);

    // Allow the primary to be re-elected, and wait for it.
    assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
    rst.getPrimary();
}
runStepDownTest({
    description: "insert",
    failpoint: "hangDuringBatchInsert",
    operation: () => primaryColl.insert({_id: 'no_insert_on_stepdown'})
});

runStepDownTest({
    description: "update",
    failpoint: "hangDuringBatchUpdate",
    operation: () => primaryColl.update({_id: 'updateme'}, {'$set': {x: 1}})
});

runStepDownTest({
    description: "remove",
    failpoint: "hangDuringBatchRemove",
    operation: () => primaryColl.remove({_id: 'deleteme'}, {'$set': {x: 1}})
});

rst.stopSet();
})();
