/**
 * Tests hedging metrics in the serverStatus output.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

function setCommandDelay(nodeConn, command, delay, ns) {
    assert.commandWorked(nodeConn.adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {
            failInternalCommands: true,
            blockConnection: true,
            blockTimeMS: delay,
            failCommands: [command],
            namespace: ns
        },
    }));
}

function clearCommandDelay(nodeConn) {
    assert.commandWorked(nodeConn.adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));
}

/*
 * Returns true if there is operation with the given comment running on the
 * given connection.
 */
function checkForOpWithComment(conn, comment) {
    const ret =
        conn.getDB("admin")
            .aggregate([{$currentOp: {localOps: true}}, {$match: {"command.comment": comment}}])
            .toArray();

    jsTestLog(`Checked currentOp with comment ${comment}: ${tojson(ret)}`);

    if (ret.length == 0) {
        return false;
    }

    if (ret.every(op => op.killPending)) {
        // CurrentOp actually blocks kills from proceeding.
        return false;
    }

    return true;
}

const st = new ShardingTest({
    mongos: [{
        setParameter: {
            logComponentVerbosity: tojson({network: {verbosity: 2}}),
            // Force the mongos's replica set monitors to always include all the eligible nodes.
            "failpoint.scanningServerSelectorIgnoreLatencyWindow": tojson({mode: "alwaysOn"}),
            "failpoint.sdamServerSelectorIgnoreLatencyWindow": tojson({mode: "alwaysOn"}),
            // Force the mongos to send requests to hosts in alphabetical order of host names.
            "failpoint.networkInterfaceSendRequestsToTargetHostsInAlphabeticalOrder":
                tojson({mode: "alwaysOn"}),
            maxTimeMSForHedgedReads: 10
        }
    }],
    shards: 1,
    rs: {nodes: 2, setParameter: {logComponentVerbosity: tojson({command: {verbosity: 1}})}}
});
const dbName = "hedged_reads";
const collName = "test";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);

const kBlockCmdTimeMS = 5 * 60 * 1000;
const kWaitKillOpTimeoutMS = 5 * 1000;
const numDocs = 10;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

jsTest.log(
    "Verify that maxTimeMS expiration of the additional request does not affect the command result");
// The hedged read will have the maxTimeMS set to 10ms, hence need to sleep longer than that.
assert.commandWorked(testDB.runCommand({
    count: collName,
    query: {$where: "sleep(100); return true;", x: {$gte: 0}},
    $readPreference: {mode: "nearest"}
}));

let sortedNodes = [...st.rs0.nodes].sort((node1, node2) => node1.host.localeCompare(node2.host));

jsTest.log("Verify that the initial request is canceled when the hedged request responds first");
try {
    // Make the initial request block.
    setCommandDelay(sortedNodes[0], "count", kBlockCmdTimeMS, ns);

    // Make the hedged request block for a while to allow the operation to start on the other node.
    setCommandDelay(sortedNodes[1], "count", 100, ns);

    const comment = "test_kill_initial_request_" + ObjectId();
    assert.commandWorked(testDB.runCommand({
        count: collName,
        query: {x: {$gte: 0}},
        $readPreference: {mode: "nearest"},
        comment: comment
    }));

    assert.soon(
        function() {
            return !checkForOpWithComment(sortedNodes[0], comment);
        },
        "Timed out waiting for the operation run by the initial request to be killed",
        kWaitKillOpTimeoutMS);
} finally {
    clearCommandDelay(sortedNodes[0]);
    clearCommandDelay(sortedNodes[1]);
}

jsTest.log(
    "Verify that the additional request is canceled when the initial request responds first");
try {
    // Make the additional/hedged request block, set maxTimeMSForHedgedReads to the block time
    // to prevent the remote host from killing the operation by itself.
    assert.commandWorked(
        st.s.adminCommand({setParameter: 1, maxTimeMSForHedgedReads: kBlockCmdTimeMS}));
    setCommandDelay(sortedNodes[1], "count", kBlockCmdTimeMS, ns);

    // Make the initial request block for a while to allow the operation to start on the other node.
    setCommandDelay(sortedNodes[0], "count", 100, ns);

    const comment = "test_kill_additional_request_" + ObjectId();
    assert.commandWorked(testDB.runCommand({
        count: collName,
        query: {x: {$gte: 0}},
        $readPreference: {mode: "nearest"},
        comment: comment
    }));

    assert.soon(
        function() {
            return !checkForOpWithComment(sortedNodes[1], comment);
        },
        "Timed out waiting for the operation run by the additional request to be killed",
        kWaitKillOpTimeoutMS);
} finally {
    clearCommandDelay(sortedNodes[0]);
    clearCommandDelay(sortedNodes[1]);
    assert.commandWorked(st.s.adminCommand({setParameter: 1, maxTimeMSForHedgedReads: 100}));
}

// Need causally consistent reads to verify the document count
let session = testDB.getMongo().startSession({causalConsistency: true});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

let bulk = sessionColl.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Verify that the getMore on hedge request do not inherit maxTimeMS");
try {
    // force to open hedge read cursor on sortedNodes[1]
    setCommandDelay(sortedNodes[0], "find", 500, ns);

    // $where with sleep is used because blocking command via failCommand does not affect the opCtx
    // deadlines as it blocks and unblocks the command before it starts execution.
    const comment = "test_getmore_on_additional_request_" + ObjectId();
    let findRes = assert.commandWorked(sessionDB.runCommand({
        find: collName,
        filter: {$where: "sleep(2000); return true;", x: {$gte: 0}},
        $readPreference: {mode: "nearest"},
        batchSize: 0,
        comment: comment
    }));

    const cursorId = findRes.cursor.id;
    assert.neq(0, cursorId);

    // confirm that getMore does not time out.
    let getMoreRes =
        assert.commandWorked(sessionDB.runCommand({getMore: cursorId, collection: collName}));
    assert.eq(getMoreRes.cursor.nextBatch.length, numDocs);
} finally {
    clearCommandDelay(sortedNodes[0]);
}

st.stop();
}());
