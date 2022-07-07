/**
 * Tests that the donor's state will transition to kError and reshardCollection will fail if there
 * is an ongoing index build.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");
load("jstests/sharding/libs/resharding_test_util.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

const reshardingTest = new ReshardingTest({numDonors: 1});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

// Insert a document so that the index build will hit the failpoint.
assert.commandWorked(inputCollection.insert([
    {_id: 0, oldKey: -10, newKey: -10},
]));

const mongos = inputCollection.getMongo();
const recipientShardNames = reshardingTest.recipientShardNames;

const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor_host = topology.shards[donorShardNames[0]].primary;
const donor0 = new Mongo(donor_host);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

// Create an inProgress index build.
const createIndexThread = new Thread(function(host) {
    const con = new Mongo(host).getCollection("reshardingDb.coll");
    return con.createIndexes([{newKey: 1}]);
}, donor_host);
let createIndexFailpoint = configureFailPoint(donor0, "hangIndexBuildBeforeCommit");
createIndexThread.start();
createIndexFailpoint.wait();

let removeDonorDocFailpoint = configureFailPoint(donor0, "removeDonorDocFailpoint");
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // TODO SERVER-51696: Review if these checks can be made in a cpp unittest instead.
        ReshardingTestUtil.assertDonorAbortsLocally(
            donor0,
            donorShardNames[0],
            inputCollection.getFullName(),
            ErrorCodes.BackgroundOperationInProgressForNamespace);

        removeDonorDocFailpoint.off();

        // Note: even though the recipient state machine does not exist by the time the donor
        // errors, recipients should still acknowledge they saw the coordinator's abort.
        ReshardingTestUtil.assertAllParticipantsReportDoneToCoordinator(
            configsvr, inputCollection.getFullName());
    },
    {expectedErrorCode: ErrorCodes.BackgroundOperationInProgressForNamespace});

// Resume index build.
createIndexFailpoint.off();
createIndexThread.join();
assert.commandWorked(createIndexThread.returnData());

reshardingTest.teardown();
})();
