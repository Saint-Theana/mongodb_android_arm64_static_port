/**
 * Tests the basic functionality of the resharding metrics section in server status.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const kNamespace = "reshardingDb.coll";

function verifyMetrics(metrics, expected) {
    for (var key in expected) {
        assert(metrics.hasOwnProperty(key), `Missing ${key} in ${tojson(metrics)}`);
        const expectedValue = expected[key];
        // The contract for this method is to treat `undefined` as an indication for non-important
        // or non-deterministic values.
        if (expectedValue === undefined)
            continue;
        assert.eq(metrics[key],
                  expectedValue,
                  `Expected the value for ${key} to be ${expectedValue}: ${tojson(metrics)}`);
    }
}

function testMetricsArePresent(mongo, expectedMetrics) {
    const stats = mongo.getDB('admin').serverStatus({});
    assert(stats.hasOwnProperty('shardingStatistics'), stats);
    const shardingStats = stats.shardingStatistics;
    assert(shardingStats.hasOwnProperty('resharding'),
           `Missing resharding section in ${tojson(shardingStats)}`);

    const metrics = shardingStats.resharding;
    verifyMetrics(metrics, expectedMetrics);
}

function verifyParticipantServerStatusOutput(reshardingTest, inputCollection, expectedMetrics) {
    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;

    const mongos = inputCollection.getMongo();
    const topology = DiscoverTopology.findConnectedNodes(mongos);

    testMetricsArePresent(new Mongo(topology.shards[donorShardNames[0]].primary), expectedMetrics);
    testMetricsArePresent(new Mongo(topology.shards[donorShardNames[1]].primary), expectedMetrics);
    testMetricsArePresent(new Mongo(topology.shards[recipientShardNames[0]].primary),
                          expectedMetrics);
    testMetricsArePresent(new Mongo(topology.shards[recipientShardNames[1]].primary),
                          expectedMetrics);
}

function verifyCoordinatorServerStatusOutput(inputCollection, expectedMetrics) {
    const mongos = inputCollection.getMongo();
    const topology = DiscoverTopology.findConnectedNodes(mongos);

    testMetricsArePresent(new Mongo(topology.configsvr.primary), expectedMetrics);
}

// Tests the currentOp output for each donor, each recipient, and the coordinator.
function checkCurrentOp(mongo, clusterName, role, expected) {
    function getCurrentOpReport(mongo, role) {
        return mongo.getDB("admin").currentOp(
            {ns: kNamespace, desc: {$regex: 'Resharding' + role + 'Service.*'}});
    }

    jsTest.log(`Testing currentOp output for ${role}s on ${clusterName}`);
    assert.soon(() => {
        const report = getCurrentOpReport(mongo, role);
        if (report.inprog.length === 1)
            return true;

        jsTest.log(tojson(report));
        return false;
    }, () => `: was unable to find resharding ${role} service in currentOp output`);

    verifyMetrics(getCurrentOpReport(mongo, role).inprog[0], expected);
}

function verifyCurrentOpOutput(reshardingTest, inputCollection) {
    // Wait for the resharding operation and the donor services to start.
    const mongos = inputCollection.getMongo();
    assert.soon(() => {
        const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
            ns: inputCollection.getFullName()
        });
        return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
    });

    const topology = DiscoverTopology.findConnectedNodes(mongos);

    reshardingTest.donorShardNames.forEach(function(shardName) {
        checkCurrentOp(new Mongo(topology.shards[shardName].primary), shardName, "Donor", {
            "type": "op",
            "op": "command",
            "ns": kNamespace,
            "originatingCommand": undefined,
            "totalOperationTimeElapsedSecs": undefined,
            "remainingOperationTimeEstimatedSecs": undefined,
            "countWritesDuringCriticalSection": 0,
            "totalCriticalSectionTimeElapsedSecs": undefined,
            "donorState": undefined,
            "opStatus": "running",
        });
    });

    reshardingTest.recipientShardNames.forEach(function(shardName) {
        checkCurrentOp(new Mongo(topology.shards[shardName].primary), shardName, "Recipient", {
            "type": "op",
            "op": "command",
            "ns": kNamespace,
            "originatingCommand": undefined,
            "totalOperationTimeElapsedSecs": undefined,
            "remainingOperationTimeEstimatedSecs": undefined,
            "approxDocumentsToCopy": undefined,
            "approxBytesToCopy": undefined,
            "documentsCopied": undefined,
            "bytesCopied": undefined,
            "totalCopyTimeElapsedSecs": undefined,
            "oplogEntriesFetched": undefined,
            "oplogEntriesApplied": undefined,
            "totalApplyTimeElapsedSecs": undefined,
            "recipientState": undefined,
            "opStatus": "running",
        });
    });

    checkCurrentOp(new Mongo(topology.configsvr.nodes[0]), "configsvr", "Coordinator", {
        "type": "op",
        "op": "command",
        "ns": kNamespace,
        "originatingCommand": undefined,
        "totalOperationTimeElapsedSecs": undefined,
        "remainingOperationTimeEstimatedSecs": undefined,
        "coordinatorState": undefined,
        "opStatus": "running",
    });
}

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: kNamespace,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

var initialServerStatusMetrics = {
    "countReshardingOperations": 0,
    "countReshardingSuccessful": 0,
    "countReshardingFailures": 0,
    "countReshardingCanceled": 0,
    "documentsCopied": 0,
    "bytesCopied": 0,
    "oplogEntriesApplied": 0,
    "countWritesDuringCriticalSection": 0,
    "lastOpEndingChunkImbalance": 0,
};

verifyParticipantServerStatusOutput(reshardingTest, inputCollection, initialServerStatusMetrics);
verifyCoordinatorServerStatusOutput(inputCollection, {lastOpEndingChunkImbalance: 0});

var documentsInserted = [
    {_id: "stays on shard0", oldKey: -10, newKey: -10},
    {_id: "moves to shard0", oldKey: 10, newKey: -10},
    {_id: "moves to shard1", oldKey: -10, newKey: 10},
    {_id: "stays on shard1", oldKey: 10, newKey: 10},
];

assert.commandWorked(inputCollection.insert(documentsInserted));

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(  //
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: 10}, shard: recipientShardNames[1]},
            {min: {newKey: 10}, max: {newKey: 20}, shard: recipientShardNames[1]},
            {min: {newKey: 20}, max: {newKey: 30}, shard: recipientShardNames[1]},
            {min: {newKey: 30}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    (tempNs) => {
        verifyCurrentOpOutput(reshardingTest, inputCollection);
    });

var finalServerStatusMetrics = {
    "countReshardingOperations": 1,
    "countReshardingSuccessful": 1,
    "countReshardingFailures": 0,
    "countReshardingCanceled": 0,
    "documentsCopied": 2,
    "bytesCopied": Object.bsonsize(documentsInserted[1]) + Object.bsonsize(documentsInserted[2]),
    "oplogEntriesApplied": 2,
    "countWritesDuringCriticalSection": 0,
    "lastOpEndingChunkImbalance": 0,
};

verifyParticipantServerStatusOutput(reshardingTest, inputCollection, finalServerStatusMetrics);
verifyCoordinatorServerStatusOutput(inputCollection, {lastOpEndingChunkImbalance: 3});

reshardingTest.teardown();
})();
