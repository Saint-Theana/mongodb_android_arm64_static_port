/**
 * this tests some of the ground work
 */
(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

var s = new ShardingTest({shards: 2});
var db = s.getDB("test");

assert.commandWorked(db.foo.insert({num: 1, name: "eliot"}));
assert.commandWorked(db.foo.insert({num: 2, name: "sara"}));
assert.commandWorked(db.foo.insert({num: -1, name: "joe"}));

assert.commandWorked(db.foo.createIndex({num: 1}));

assert.eq(3, db.foo.find().length(), "A");

const shardCommand = {
    shardcollection: "test.foo",
    key: {num: 1}
};

assert.commandFailed(s.s0.adminCommand(shardCommand));

assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', s.shard1.shardName);

assert.eq(3, db.foo.find().length(), "after partitioning count failed");

assert.commandWorked(s.s0.adminCommand(shardCommand));
assert.commandFailed(s.s0.adminCommand({shardCollection: 'test', key: {x: 1}}));
assert.commandFailed(s.s0.adminCommand({shardCollection: '.foo', key: {x: 1}}));

assert(s.config.collections.findOne({_id: "test.foo"}), "No collection entry found for test.foo");
s.config.collections.find().forEach(printjson);

assert.eq(1, findChunksUtil.countChunksForNs(s.config, "test.foo"), "num chunks A");
assert.eq(3, db.foo.find().length(), "after sharding, no split count failed");

s.stop();
})();
