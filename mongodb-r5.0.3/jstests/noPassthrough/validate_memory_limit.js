/**
 * Test that the memory usage of validate is properly limited according to the
 * maxValidateMemoryUsageMB parameter.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

load("jstests/disk/libs/wt_file_helper.js");

const kIndexKeyLength = 1024 * 1024;

const baseName = "validate_memory_limit";
const dbpath = MongoRunner.dataPath + baseName + "/";
let conn = MongoRunner.runMongod({dbpath: dbpath});
let coll = conn.getDB("test").getCollection("corrupt");

function corruptIndex() {
    const uri = getUriForIndex(coll, "_id_");
    conn = truncateUriAndRestartMongod(uri, conn);
    coll = conn.getDB("test").getCollection("corrupt");
}

function checkValidate(errorPrefix, numMissingIndexEntries) {
    conn.getDB("test").adminCommand({setParameter: 1, maxValidateMemoryUsageMB: 1});
    const res = coll.validate();
    assert.commandWorked(res);
    assert(!res.valid);
    assert.containsPrefix(errorPrefix, res.errors);
    assert.eq(res.missingIndexEntries.length, numMissingIndexEntries);
}

function checkValidateRepair(expectRepair) {
    const res = coll.validate({repair: true});
    assert.commandWorked(res);
    assert(!res.valid, printjson(res));
    assert.eq(res.repaired, expectRepair, printjson(res));
}

const noneReportedPrefix =
    "Unable to report index entry inconsistencies due to memory limitations.";
const notAllReportedPrefix =
    "Not all index entry inconsistencies are reported due to memory limitations.";

// Insert a document with a key larger than maxValidateMemoryUsageMB so that validate does not
// report any missing index entries.
const indexKey = "a".repeat(kIndexKeyLength);
assert.commandWorked(coll.insert({_id: indexKey}));
corruptIndex();
checkValidate(noneReportedPrefix, 0);

// Can't repair successfully if there aren't any index inconsistencies reported.
checkValidateRepair(false);

// Insert a document with a small key so that validate reports one missing index entry.
assert.commandWorked(coll.insert({_id: 1}));
corruptIndex();
checkValidate(notAllReportedPrefix, 1);

// Repair, but incompletely if only some inconsistencies are reported.
checkValidateRepair(true);

MongoRunner.stopMongod(conn, null, {skipValidation: true});
})();