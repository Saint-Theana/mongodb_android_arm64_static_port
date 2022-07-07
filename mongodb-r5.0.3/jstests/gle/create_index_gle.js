load('jstests/replsets/rslib.js');
(function() {
"use strict";

var st = new ShardingTest({
    name: "zzz",
    shards: {
        rs0: {
            nodes: {n0: {}, n1: {rsConfig: {priority: 0}}},
            oplogSize: 10,
        }
    },
    verbose: 3,
    other: {rsOptions: {verbose: 1}}
});
var replTest = st.rs0;
// If featureFlagUseSecondaryDelaySecs is enabled, we must use the 'secondaryDelaySecs' field
// name in our config. Otherwise, we use 'slaveDelay'.
const delayFieldName = selectDelayFieldName(replTest);

var config = replTest.getReplSetConfig();
// Add a delay long enough so getLastError would actually 'wait' for write concern.
config.members[1][delayFieldName] = 3;
config.version = replTest.getReplSetConfigFromNode().version + 1;

reconfig(replTest, config, true);

assert.soon(function() {
    var secConn = replTest.getSecondary();
    var config = secConn.getDB('local').system.replset.findOne();
    return config.members[1][delayFieldName] == 3;
});

replTest.awaitSecondaryNodes();

var testDB = st.s.getDB('test');
testDB.adminCommand({connPoolSync: 1});

var secConn = replTest.getSecondary();
var testDB2 = secConn.getDB('test');

testDB.user.insert({x: 1});

testDB.user.createIndex({x: 1});
assert.gleOK(testDB.runCommand({getLastError: 1, w: 2}));

replTest.awaitReplication();

var priIdx = testDB.user.getIndexes();
var secIdx = testDB2.user.getIndexes();

assert.eq(priIdx.length, secIdx.length, 'pri: ' + tojson(priIdx) + ', sec: ' + tojson(secIdx));

st.stop();
}());
