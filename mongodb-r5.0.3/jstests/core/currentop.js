/**
 * Tests that long-running operations show up in currentOp and report the locks they are holding.
 *
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   assumes_superuser_permissions,
 *   # fsync command is not available on embedded
 *   incompatible_with_embedded,
 *   uses_parallel_shell,
 *   # The aggregation stage $currentOp cannot run with a readConcern other than 'local'
 *   assumes_read_concern_unchanged
 * ]
 */

(function() {
"use strict";
const coll = db.jstests_currentop;
coll.drop();

// We fsync+lock the server to cause all subsequent write operations to block.
assert.commandWorked(db.fsyncLock());

const awaitInsertShell = startParallelShell(function() {
    assert.commandWorked(db.jstests_currentop.insert({}));
});

// Wait until the write appears in the currentOp output reporting that it is waiting for a lock.
assert.soon(
    function() {
        const ops = db.currentOp({
            $and: [
                {"locks.Global": "w", waitingForLock: true},
                // Depending on whether CurOp::setNS_inlock() has been called, the "ns" field
                // may either be the full collection name or the command namespace.
                {
                    $or: [
                        {ns: coll.getFullName()},
                        {ns: db.$cmd.getFullName(), "command.insert": coll.getName()}
                    ]
                },
                {type: "op"}
            ]
        });
        return ops.inprog.length === 1;
    },
    function() {
        return "Failed to find blocked insert in currentOp() output: " + tojson(db.currentOp());
    });

// Unlock the server and make sure the write finishes.
const fsyncResponse = assert.commandWorked(db.fsyncUnlock());
assert.eq(fsyncResponse.lockCount, 0);
awaitInsertShell();
}());
