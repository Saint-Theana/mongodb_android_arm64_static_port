/**
 * Tests the applyOps command on a capped collection that needs capped deletes.
 *
 * @tags: [requires_capped, requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "apply_ops_capped_collection";

const primary = rst.getPrimary();
const db = primary.getDB(dbName);

assert.commandWorked(db.runCommand({create: collName, capped: true, size: 1024 * 1024, max: 10}));

function nss(dbName, collName) {
    return `${dbName}.${collName}`;
}

let ops = [];
for (let i = 0; i < 20; i++) {
    ops.push({op: "i", ns: nss(dbName, collName), ts: Timestamp(i, 0), o: {_id: i}});
}

assert.commandWorked(db.runCommand({applyOps: ops}));

rst.stopSet();
}());
