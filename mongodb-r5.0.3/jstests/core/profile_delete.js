// @tags: [
//   does_not_support_stepdowns,
//   requires_non_retryable_writes,
//   requires_profiling,
// ]

// Confirms that profiled delete execution contains all expected metrics with proper values.

(function() {
"use strict";

load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

// Setup test db and collection.
var testDB = db.getSiblingDB("profile_delete");
assert.commandWorked(testDB.dropDatabase());
var coll = testDB.getCollection("test");

testDB.setProfilingLevel(2);

//
// Confirm metrics for single document delete.
//
var i;
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.remove({a: {$gte: 2}, b: {$gte: 2}},
                                 db.getMongo().writeMode() === "commands"
                                     ? {justOne: true, collation: {locale: "fr"}}
                                     : {justOne: true}));

var profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ns, coll.getFullName(), tojson(profileObj));
assert.eq(profileObj.op, "remove", tojson(profileObj));
if (db.getMongo().writeMode() === "commands") {
    assert.eq(profileObj.command.collation, {locale: "fr"}, tojson(profileObj));
}
assert.eq(profileObj.ndeleted, 1, tojson(profileObj));
assert.eq(profileObj.keysExamined, 1, tojson(profileObj));
assert.eq(profileObj.docsExamined, 1, tojson(profileObj));
assert.eq(profileObj.keysDeleted, 2, tojson(profileObj));
assert.eq(profileObj.planSummary, "IXSCAN { a: 1 }", tojson(profileObj));
assert(profileObj.execStats.hasOwnProperty("stage"), tojson(profileObj));
assert(profileObj.hasOwnProperty("millis"), tojson(profileObj));
assert(profileObj.hasOwnProperty("numYield"), tojson(profileObj));
assert(profileObj.hasOwnProperty("locks"), tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm metrics for multiple document delete.
//
coll.drop();
for (i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

assert.commandWorked(coll.remove({a: {$gte: 2}}));
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.ndeleted, 8, tojson(profileObj));
assert.eq(profileObj.keysDeleted, 8, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm "fromMultiPlanner" metric.
//
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

assert.commandWorked(coll.remove({a: 3, b: 3}));
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.fromMultiPlanner, true, tojson(profileObj));
assert.eq(profileObj.appName, "MongoDB Shell", tojson(profileObj));

//
// Confirm killing a remove operation will not log 'ndeleted' to the profiler.
//
assert(coll.drop());

for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({a: 1}));
}

const deleteResult = testDB.runCommand({
    delete: coll.getName(),
    deletes: [{q: {$where: "sleep(1000);return true", a: 1}, limit: 0}],
    maxTimeMS: 1
});

// This command will time out before completing.
assert.commandFailedWithCode(deleteResult, ErrorCodes.MaxTimeMSExpired);

profileObj = getLatestProfilerEntry(testDB);

// 'ndeleted' should not be defined.
assert(!profileObj.hasOwnProperty("ndeleted"), profileObj);
})();
