
// Checking UUID consistency involves talking to the shard primary, but by the end of this test, the
// shard does not have a primary.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

(function() {
'use strict';

var st = new ShardingTest({shards: 1, rs: {nodes: 2}});

// This test expects the shard version after step down to be UNKNOWN. Disable checking for index
// consistency to ensure that the config server doesn't trigger a StaleShardVersion exception on
// shard0 and cause it to refresh its sharding metadata.
st._configServers.forEach(
    config => config.adminCommand({setParameter: 1, enableShardedIndexConsistencyCheck: false}));

assert.commandWorked(st.s0.adminCommand({enablesharding: 'TestDB'}));
st.ensurePrimaryShard('TestDB', st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({shardcollection: 'TestDB.TestColl', key: {x: 1}}));

var db = st.s0.getDB('TestDB');
var coll = db.TestColl;

// Insert documents for the test
assert.commandWorked(coll.insert({x: 1, value: 'Test value 1'}));
assert.commandWorked(coll.insert({x: 2, value: 'Test value 2'}));

// Establish a cursor on the primary (by not using secondaryOk read)
var findCursor = assert.commandWorked(db.runCommand({find: 'TestColl', batchSize: 1})).cursor;

var shardVersionBeforeStepdown =
    assert.commandWorked(st.rs0.getPrimary().adminCommand({getShardVersion: 'TestDB.TestColl'}))
        .global;
assert.neq(Timestamp(0, 0), shardVersionBeforeStepdown);

// Stepdown the primary of the shard and ensure that that cursor can still be read
assert.commandWorked(st.rs0.getPrimary().adminCommand({replSetStepDown: 60, force: 1}));

var getMoreCursor =
    assert.commandWorked(db.runCommand({getMore: findCursor.id, collection: 'TestColl'})).cursor;
assert.eq(0, getMoreCursor.id);
assert.eq(2, getMoreCursor.nextBatch[0].x);

// After stepdown, the shard version will be reset
var shardVersionAfterStepdown =
    assert.commandWorked(st.rs0.getPrimary().adminCommand({getShardVersion: 'TestDB.TestColl'}))
        .global;
assert.eq("UNKNOWN", shardVersionAfterStepdown);

st.stop();
})();
