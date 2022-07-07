(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

/**
 * One-shard cluster test do not need to be tested in the multiversion suites.
 * @tags: [multiversion_incompatible]
 */
var s = new ShardingTest({name: "version2", shards: 1});

assert.commandWorked(s.s0.adminCommand({enablesharding: "alleyinsider"}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "alleyinsider.foo", key: {num: 1}}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "alleyinsider.bar", key: {num: 1}}));

var a = s.shard0.getDB("admin");

// Setup from one client
assert.eq(a.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).global.i, 0);

var fooEpoch = s.getDB('config').collections.findOne({_id: "alleyinsider.foo"}).lastmodEpoch;
assert.commandWorked(a.runCommand({
    setShardVersion: "alleyinsider.foo",
    configdb: s._configDB,
    authoritative: true,
    version: new Timestamp(1, 0),
    versionEpoch: fooEpoch,
    shard: s.shard0.shardName,
    shardHost: s.s.host,
}));

printjson(s.config.chunks.findOne());

assert.eq(a.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).global.t, 1);

// From a different client
var a2 = connect(`mongodb://${s.rs0.getPrimary().name}/admin`);

assert.eq(a2.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).global.t,
          1,
          "a2 global 1");

s.stop();
})();
