// Tests whether the noBalance flag disables balancing for collections
// @tags: [requires_sharding]

load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({shards: 2, mongos: 1});

// First, test that shell helpers require an argument
assert.throws(sh.disableBalancing, [], "sh.disableBalancing requires a collection");
assert.throws(sh.enableBalancing, [], "sh.enableBalancing requires a collection");

var shardAName = st.shard0.shardName;
var shardBName = st.shard1.shardName;

var collA = st.s.getCollection(jsTest.name() + ".collA");
var collB = st.s.getCollection(jsTest.name() + ".collB");

// Shard two collections
st.shardColl(collA, {_id: 1}, false);
st.shardColl(collB, {_id: 1}, false);

// Split into a lot of chunks so balancing can occur
var totalNumChunks = 10;
var numChunksPerShard = totalNumChunks / 2;
for (var i = 0; i < totalNumChunks - 1; i++) {  // 10 chunks total
    collA.getMongo().getDB("admin").runCommand({split: collA + "", middle: {_id: i}});
    collA.getMongo().getDB("admin").runCommand({split: collB + "", middle: {_id: i}});
}

// Disable balancing on one collection
sh.disableBalancing(collB);

jsTest.log("Balancing disabled on " + collB);
printjson(collA.getDB().getSiblingDB("config").collections.find().toArray());

st.startBalancer();

// Make sure collA gets balanced
assert.soon(function() {
    var shardAChunks =
        findChunksUtil
            .findChunksByNs(st.s.getDB("config"), collA.getFullName(), {shard: shardAName})
            .itcount();
    var shardBChunks =
        findChunksUtil
            .findChunksByNs(st.s.getDB("config"), collA.getFullName(), {shard: shardBName})
            .itcount();
    printjson({shardA: shardAChunks, shardB: shardBChunks});
    return (shardAChunks == numChunksPerShard) && (shardAChunks == shardBChunks);
}, "" + collA + " chunks not balanced!", 5 * 60 * 1000);

jsTest.log("Chunks for " + collA + " are balanced.");

// Check that the collB chunks were not moved
var shardAChunks =
    findChunksUtil.findChunksByNs(st.s.getDB("config"), collB.getFullName(), {shard: shardAName})
        .itcount();
var shardBChunks =
    findChunksUtil.findChunksByNs(st.s.getDB("config"), collB.getFullName(), {shard: shardBName})
        .itcount();
printjson({shardA: shardAChunks, shardB: shardBChunks});
assert(shardAChunks == 0 || shardBChunks == 0);

// Re-enable balancing for collB
sh.enableBalancing(collB);

// Make sure that collB is now balanced
assert.soon(function() {
    var shardAChunks =
        findChunksUtil
            .findChunksByNs(st.s.getDB("config"), collB.getFullName(), {shard: shardAName})
            .itcount();
    var shardBChunks =
        findChunksUtil
            .findChunksByNs(st.s.getDB("config"), collB.getFullName(), {shard: shardBName})
            .itcount();
    printjson({shardA: shardAChunks, shardB: shardBChunks});
    return (shardAChunks == numChunksPerShard) && (shardAChunks == shardBChunks);
}, "" + collB + " chunks not balanced!", 5 * 60 * 1000);

jsTest.log("Chunks for " + collB + " are balanced.");

// Re-disable balancing for collB
sh.disableBalancing(collB);

// Wait for the balancer to fully finish the last migration and write the changelog
// MUST set db var here, ugly but necessary
db = st.s0.getDB("config");
st.waitForBalancer(true, 60000);

// Make sure auto-migrates on insert don't move chunks
var lastMigration = sh._lastMigration(collB);

var bulk = collB.initializeUnorderedBulkOp();
// Reduce the amount of data on live-record buildvariant
var n = (TestData.undoRecorderPath ? 100000 : 1000000);
for (var i = 0; i < n; i++) {
    bulk.insert({_id: i, hello: "world"});
}
assert.commandWorked(bulk.execute());

printjson(lastMigration);
printjson(sh._lastMigration(collB));

if (lastMigration == null) {
    assert.eq(null, sh._lastMigration(collB));
} else {
    assert.eq(lastMigration.time, sh._lastMigration(collB).time);
}

st.stop();
