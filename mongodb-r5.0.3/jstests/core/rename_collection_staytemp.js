/*
 * @tags: [
 *   requires_non_retryable_commands,
 *   requires_replication,
 *   # applyOps is not supported on mongos
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 * ]
 */

orig = 'rename_stayTemp_orig';
dest = 'rename_stayTemp_dest';

db[orig].drop();
db[dest].drop();

function ns(coll) {
    return db[coll].getFullName();
}

function istemp(name) {
    var result = db.runCommand("listCollections", {filter: {name: name}});
    assert(result.ok);
    var collections = new DBCommandCursor(db, result).toArray();
    assert.eq(1, collections.length);
    return collections[0].options.temp ? true : false;
}

assert.commandWorked(
    db.runCommand({applyOps: [{op: "c", ns: db.getName() + ".$cmd", o: {create: orig, temp: 1}}]}));
assert(istemp(orig));

db.adminCommand({renameCollection: ns(orig), to: ns(dest)});
assert(!istemp(dest));

db[dest].drop();

assert.commandWorked(
    db.runCommand({applyOps: [{op: "c", ns: db.getName() + ".$cmd", o: {create: orig, temp: 1}}]}));
assert(istemp(orig));

db.adminCommand({renameCollection: ns(orig), to: ns(dest), stayTemp: true});
assert(istemp(dest));
