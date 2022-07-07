/**
 * Tests that resharding can successfully handle an abort request after the coordinator is in state
 * preparing-to-donate but before it has flushed its state change to prompt participant state
 * machine creation. See SERVER-56936 for more details.
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

const originalCollectionNs = "reshardingDb.coll";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollection = reshardingTest.createShardedCollection({
    ns: originalCollectionNs,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

const pauseAfterPreparingToDonateFP =
    configureFailPoint(configsvr, "reshardingPauseCoordinatorAfterPreparingToDonate");

reshardingTest.withReshardingInBackground(
    {

        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        pauseAfterPreparingToDonateFP.wait();
        assert.commandWorked(mongos.adminCommand({abortReshardCollection: originalCollectionNs}));
        // Signaling abort will cause the
        // pauseAfterPreparingToDonateFP to throw, implicitly
        // allowing the coordinator to make progress without
        // explicitly turning off the failpoint.
    },
    {expectedErrorCode: ErrorCodes.ReshardCollectionAborted});
pauseAfterPreparingToDonateFP.off();
reshardingTest.teardown();
})();
