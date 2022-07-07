/**
 * Tests inserting various _id values and duplicates on a collection clustered by _id.
 *
 * @tags: [
 *     assumes_against_mongod_not_mongos,
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const collName = 'system.buckets.test';
const coll = db[collName];
coll.drop();

assert.commandWorked(db.createCollection(collName, {clusteredIndex: true}));

// Expect that duplicates are rejected.
let oid = new ObjectId();
assert.commandWorked(coll.insert({_id: oid}));
assert.commandFailedWithCode(coll.insert({_id: oid}), ErrorCodes.DuplicateKey);
assert.eq(1, coll.find({_id: oid}).itcount());

// Updates should work.
assert.commandWorked(coll.update({_id: oid}, {a: 1}));
assert.eq(1, coll.find({_id: oid}).itcount());

assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.eq(1, coll.find({_id: 0}).itcount());
assert.commandWorked(coll.insert({_id: "", a: 2}));
assert.eq(1, coll.find({_id: ""}).itcount());
assert.commandWorked(coll.insert({_id: NumberLong("9223372036854775807"), a: 3}));
assert.eq(1, coll.find({_id: NumberLong("9223372036854775807")}).itcount());
assert.commandWorked(coll.insert({_id: {a: 1, b: 1}, a: 4}));
assert.eq(1, coll.find({_id: {a: 1, b: 1}}).itcount());
assert.commandFailedWithCode(coll.insert({_id: {a: {b: 1}, c: 1}, a: 5}), ErrorCodes.BadValue);
assert.commandWorked(coll.insert({_id: -1, a: 6}));
assert.eq(1, coll.find({_id: -1}).itcount());
assert.commandWorked(coll.insert({_id: "123456789012", a: 7}));
assert.eq(1, coll.find({_id: "123456789012"}).itcount());
assert.commandWorked(coll.insert({a: 8}));
assert.eq(1, coll.find({a: 8}).itcount());
assert.commandWorked(coll.insert({_id: null, a: 9}));
assert.eq(1, coll.find({_id: null}).itcount());
assert.commandFailedWithCode(coll.insert({_id: "123456789012345678912387238478142876534", a: 10}),
                             ErrorCodes.BadValue);

assert.commandWorked(coll.createIndex({a: 1}));

// No support for numeric type differentiation.
assert.commandWorked(coll.insert({_id: 42.0}));
assert.commandFailedWithCode(coll.insert({_id: 42}), ErrorCodes.DuplicateKey);
assert.commandFailedWithCode(coll.insert({_id: NumberLong("42")}), ErrorCodes.DuplicateKey);
assert.eq(1, coll.find({_id: 42.0}).itcount());
assert.eq(1, coll.find({_id: 42}).itcount());
assert.eq(1, coll.find({_id: NumberLong("42")}).itcount());
})();
