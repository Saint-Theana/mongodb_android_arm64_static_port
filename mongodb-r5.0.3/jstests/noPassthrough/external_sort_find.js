/**
 * Test that the find command can spill to disk while executing a blocking sort, if the client
 * explicitly allows disk usage.
 *
 * Must be run with read commands enabled, since legacy OP_QUERY reads do not support the
 * 'allowDiskUse' parameter.
 * @tags: [
 *   requires_find_command,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

// Only allow blocking sort execution to use 100 kB of memory.
const kMaxMemoryUsageBytes = 100 * 1024;

const kNumDocsWithinMemLimit = 70;
const kNumDocsExceedingMemLimit = 100;

const options = {
    setParameter: "internalQueryMaxBlockingSortMemoryUsageBytes=" + kMaxMemoryUsageBytes
};
const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

const testDb = conn.getDB("test");
const collection = testDb.external_sort_find;
const isSBEEnabled = checkSBEEnabled(testDb);

// Construct a document that is just over 1 kB.
const charToRepeat = "-";
const templateDoc = {
    padding: charToRepeat.repeat(1024)
};

// Insert data into the collection without exceeding the memory threshold.
for (let i = 0; i < kNumDocsWithinMemLimit; ++i) {
    templateDoc.sequenceNumber = i;
    assert.commandWorked(collection.insert(templateDoc));
}

// We should be able to successfully sort the collection with or without disk use allowed.
assert.eq(kNumDocsWithinMemLimit, collection.find().sort({sequenceNumber: -1}).itcount());
assert.eq(kNumDocsWithinMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

function getSortStats(allowDiskUse) {
    let cursor = collection.find().sort({sequenceNumber: -1});
    if (allowDiskUse) {
        cursor = cursor.allowDiskUse();
    }
    const stageName = isSBEEnabled ? "sort" : "SORT";
    const explain = cursor.explain("executionStats");
    return getPlanStage(explain.executionStats.executionStages, stageName);
}

// Explain should report that less than 100 kB of memory was used, and we did not spill to disk.
// Test that this result is the same whether or not 'allowDiskUse' is set.
let sortStats = getSortStats(false);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.lt(sortStats.totalDataSizeSorted, kMaxMemoryUsageBytes);
assert.eq(sortStats.usedDisk, false);
sortStats = getSortStats(true);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.lt(sortStats.totalDataSizeSorted, kMaxMemoryUsageBytes);
assert.eq(sortStats.usedDisk, false);

// Add enough data to exceed the memory threshold.
for (let i = kNumDocsWithinMemLimit; i < kNumDocsExceedingMemLimit; ++i) {
    templateDoc.sequenceNumber = i;
    assert.commandWorked(collection.insert(templateDoc));
}

// The sort should fail if disk use is not allowed, but succeed if disk use is allowed.
assert.commandFailedWithCode(
    testDb.runCommand({find: collection.getName(), sort: {sequenceNumber: -1}}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(kNumDocsExceedingMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

// Explain should report that the SORT stage failed if disk use is not allowed.
sortStats = getSortStats(false);

// SBE will not report the 'failed' field within sort stats.
if (isSBEEnabled) {
    assert(!sortStats.hasOwnProperty("failed"), sortStats);
} else {
    assert.eq(sortStats.failed, true, sortStats);
}
assert.eq(sortStats.usedDisk, false);
assert.lt(sortStats.totalDataSizeSorted, kMaxMemoryUsageBytes);
assert(!sortStats.inputStage.hasOwnProperty("failed"));

// Explain should report that >=100 kB of memory was used, and that we spilled to disk.
sortStats = getSortStats(true);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.gte(sortStats.totalDataSizeSorted, kMaxMemoryUsageBytes);
assert.eq(sortStats.usedDisk, true);

// If disk use is not allowed but there is a limit, we should be able to avoid exceeding the memory
// limit.
assert.eq(kNumDocsWithinMemLimit,
          collection.find().sort({sequenceNumber: -1}).limit(kNumDocsWithinMemLimit).itcount());

// Create a view on top of the collection. When a find command is run against the view without disk
// use allowed, the command should fail with the expected error code. When the find command allows
// disk use, however, the command should succeed.
assert.commandWorked(testDb.createView("identityView", collection.getName(), []));
const identityView = testDb.identityView;
assert.commandFailedWithCode(
    testDb.runCommand({find: identityView.getName(), sort: {sequenceNumber: -1}}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(kNumDocsExceedingMemLimit,
          identityView.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

MongoRunner.stopMongod(conn);
}());
