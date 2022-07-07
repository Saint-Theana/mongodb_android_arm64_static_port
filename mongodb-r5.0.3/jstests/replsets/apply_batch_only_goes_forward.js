/**
 * This test will ensure that a failed a batch apply will become consistent only when passing
 * the end boundary (minvalid) in subsequent applies.
 *
 * To do this we:
 * -- Set minvalid manually on primary (node0) way ahead (5 minutes)
 * -- Restart primary (node0)
 * -- Ensure restarted primary (node0) comes up in recovering
 * -- Ensure node0 denylists new primary as a sync source and keeps the old minvalid
 * -- Success!
 *
 * This test requires persistence to test that a restarted primary will stay in the RECOVERING state
 * when minvalid is set to the future. An ephemeral storage engine will not have a minvalid after
 * restarting, so will initial sync in this scenario, invalidating the test.
 * @tags: [requires_persistence]
 */

// Skip db hash check because replset cannot reach consistent state.
TestData.skipCheckDBHashes = true;

(function() {
"use strict";

function tsToDate(ts) {
    return new Date(ts.getTime() * 1000);
}

var replTest =
    new ReplSetTest({name: "apply_batch_only_goes_forward", nodes: [{}, {}, {arbiter: true}]});

var nodes = replTest.startSet();
replTest.initiate();
var primary = replTest.getPrimary();
var pTest = primary.getDB("test");
var pLocal = primary.getDB("local");
var mMinvalid = pLocal["replset.minvalid"];

var secondary = replTest.getSecondary();
var sTest = secondary.getDB("test");
var sLocal = secondary.getDB("local");
var sMinvalid = sLocal["replset.minvalid"];
var stepDownSecs = 30;
var stepDownCmd = {replSetStepDown: stepDownSecs, force: true};

// Write op
assert.commandWorked(
    pTest.foo.save({}, {writeConcern: {w: 'majority', wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);
assert.commandWorked(
    pTest.foo.save({}, {writeConcern: {w: 'majority', wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

// Set minvalid to something far in the future for the current primary, to simulate recovery.
// Note: This is so far in the future (5 days) that it will never become secondary.
var farFutureTS = new Timestamp(
    Math.floor(new Date().getTime() / 1000) + (60 * 60 * 24 * 5 /* in five days*/), 0);

jsTest.log("future TS: " + tojson(farFutureTS) + ", date:" + tsToDate(farFutureTS));
// We do an update in case there is a minvalid document on the primary already.
// If the doc doesn't exist then upsert:true will create it, and the writeConcern ensures
// that update returns details of the write, like whether an update or insert was performed.
const minValidUpdate = {
    $set: {ts: farFutureTS}
};
jsTestLog("Current minvalid is " + tojson(mMinvalid.findOne()));
jsTestLog("Updating minValid to: " + tojson(minValidUpdate));
printjson(assert.commandWorked(mMinvalid.update(
    {},
    minValidUpdate,
    {upsert: true, writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS}})));

jsTest.log('Restarting primary ' + primary.host +
           ' with updated minValid. This node will go into RECOVERING upon restart. ' +
           'Secondary ' + secondary.host + ' will become new primary.');
clearRawMongoProgramOutput();
replTest.restart(primary);
printjson(sLocal.adminCommand("hello"));
replTest.waitForState(primary, ReplSetTest.State.RECOVERING);

replTest.awaitNodesAgreeOnPrimary();
// Secondary is now primary... Do a write to advance the optime on the primary so that it will be
// considered as a sync source -  this is more relevant to PV0 because we do not write a new
// entry to the oplog on becoming primary.
assert.commandWorked(replTest.getPrimary().getDB("test").foo.save(
    {}, {writeConcern: {w: 1, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

// Sync source selection will log this message if it does not detect min valid in the sync
// source candidate's oplog.
assert.soon(function() {
    return rawMongoProgramOutput().match(
        'it does not contain the necessary operations for us to reach a consistent state');
});

assert.soon(function() {
    var mv;
    try {
        mv = mMinvalid.findOne();
    } catch (e) {
        return false;
    }
    var msg = "ts !=, " + tojson(farFutureTS) + "(" + tsToDate(farFutureTS) +
        "), mv:" + tojson(mv) + " - " + tsToDate(mv.ts);
    assert.eq(farFutureTS, mv.ts, msg);
    return true;
});

// Shut down the set and finish the test.
replTest.stopSet();
})();
