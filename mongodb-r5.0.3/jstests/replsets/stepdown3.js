// Test that GLE asserts when the primary steps down while we're waiting for a replicated write.
//
// This test requires the fsync command to force a secondary to be stale.
// @tags: [requires_fsync]
(function() {
'use strict';

var replTest = new ReplSetTest({name: 'testSet', nodes: 2});
var nodes = replTest.startSet();
replTest.initiate();
var primary = replTest.getPrimary();

// The default WC is majority and fsyncLock will prevent satisfying any majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// do a write to allow stepping down of the primary;
// otherwise, the primary will refuse to step down
print("\ndo a write");
primary.getDB("test").foo.insert({x: 1});
replTest.awaitReplication();

// do another write, because the first one might be longer than 10 seconds ago
// on the secondary (due to starting up), and we need to be within 10 seconds
// to step down.
var options = {writeConcern: {w: 2, wtimeout: 30000}};
assert.commandWorked(primary.getDB("test").foo.insert({x: 2}, options));
// lock secondary, to pause replication
print("\nlock secondary");
var locked = replTest.getSecondary();
printjson(locked.getDB("admin").runCommand({fsync: 1, lock: 1}));

// do a write
print("\ndo a write");
primary.getDB("test").foo.insert({x: 3});

// step down the primary asyncronously
print("stepdown");
var command =
    "sleep(4000); assert.commandWorked(db.adminCommand( { replSetStepDown : 60, force : 1 } ));";
var awaitShell = startParallelShell(command, primary.port);

print("getlasterror; should return an error");
let result = primary.getDB("test").runCommand({getLastError: 1, w: 2, wtimeout: 10 * 60 * 1000});
assert(ErrorCodes.isNotPrimaryError(result.code));
print("result of gle:");
printjson(result);

awaitShell();

// unlock and shut down
printjson(locked.getDB("admin").fsyncUnlock());
replTest.stopSet();
})();
