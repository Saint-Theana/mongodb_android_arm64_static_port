// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// FCV4.4 is required for creating a collection with a long name.
// @tags: [
//   assumes_against_mongod_not_mongos,
//   assumes_no_implicit_collection_creation_after_drop,
//   requires_capped,
// ]

// Tests for the "create" command.
(function() {
"use strict";

load("jstests/libs/get_index_helpers.js");

// "create" command rejects invalid options.
db.create_collection.drop();
assert.commandFailedWithCode(db.createCollection("create_collection", {unknown: 1}), 40415);

// Cannot create a collection with null characters.
assert.commandFailedWithCode(db.createCollection("\0ab"), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(db.createCollection("a\0b"), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(db.createCollection("ab\0"), ErrorCodes.InvalidNamespace);

// The collection name length limit was upped in 4.4, try creating a collection with a longer
// name than previously allowed.
const longCollName = 'a'.repeat(200);
db[longCollName].drop();
assert.commandWorked(db.createCollection(longCollName));

//
// Tests for "idIndex" field.
//

// "idIndex" field not allowed with "viewOn".
db.create_collection.drop();
assert.commandWorked(db.createCollection("create_collection"));
assert.commandFailedWithCode(db.runCommand({
    create: "create_view",
    viewOn: "create_collection",
    idIndex: {key: {_id: 1}, name: "_id_"}
}),
                             ErrorCodes.InvalidOptions);

// "idIndex" field not allowed with "autoIndexId".
db.create_collection.drop();
assert.commandFailedWithCode(
    db.createCollection("create_collection",
                        {autoIndexId: false, idIndex: {key: {_id: 1}, name: "_id_"}}),
    ErrorCodes.InvalidOptions);

// "idIndex" field must be an object.
db.create_collection.drop();
assert.commandFailedWithCode(db.createCollection("create_collection", {idIndex: 1}),
                             ErrorCodes.TypeMismatch);

// "idIndex" field cannot be empty.
db.create_collection.drop();
assert.commandFailedWithCode(db.createCollection("create_collection", {idIndex: {}}),
                             ErrorCodes.FailedToParse);

// "idIndex" field must be a specification for an _id index.
db.create_collection.drop();
assert.commandFailedWithCode(
    db.createCollection("create_collection", {idIndex: {key: {a: 1}, name: "a_1"}}),
    ErrorCodes.BadValue);

// "idIndex" field must have "key" equal to {_id: 1}.
db.create_collection.drop();
assert.commandFailedWithCode(
    db.createCollection("create_collection", {idIndex: {key: {a: 1}, name: "_id_"}}),
    ErrorCodes.BadValue);

// The name of an _id index gets corrected to "_id_".
db.create_collection.drop();
assert.commandWorked(
    db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "a_1"}}));
var indexSpec = GetIndexHelpers.findByKeyPattern(db.create_collection.getIndexes(), {_id: 1});
assert.neq(indexSpec, null);
assert.eq(indexSpec.name, "_id_", tojson(indexSpec));

// "idIndex" field must only contain fields that are allowed for an _id index.
db.create_collection.drop();
assert.commandFailedWithCode(
    db.createCollection("create_collection",
                        {idIndex: {key: {_id: 1}, name: "_id_", sparse: true}}),
    ErrorCodes.InvalidIndexSpecificationOption);

// "create" creates v=2 _id index when "v" is not specified in "idIndex".
db.create_collection.drop();
assert.commandWorked(
    db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_"}}));
indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.v, 2, tojson(indexSpec));

// "create" creates v=1 _id index when "idIndex" has "v" equal to 1.
db.create_collection.drop();
assert.commandWorked(
    db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.v, 1, tojson(indexSpec));

// "create" creates v=2 _id index when "idIndex" has "v" equal to 2.
db.create_collection.drop();
assert.commandWorked(
    db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_", v: 2}}));
indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.v, 2, tojson(indexSpec));

// "collation" field of "idIndex" must match collection default collation.
db.create_collection.drop();
assert.commandFailedWithCode(
    db.createCollection("create_collection",
                        {idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}}),
    ErrorCodes.BadValue);

db.create_collection.drop();
assert.commandFailedWithCode(db.createCollection("create_collection", {
    collation: {locale: "fr_CA"},
    idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}
}),
                             ErrorCodes.BadValue);

db.create_collection.drop();
assert.commandFailedWithCode(db.createCollection("create_collection", {
    collation: {locale: "fr_CA"},
    idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "simple"}}
}),
                             ErrorCodes.BadValue);

db.create_collection.drop();
assert.commandWorked(db.createCollection("create_collection", {
    collation: {locale: "en_US", strength: 3},
    idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}
}));
indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.collation.locale, "en_US", tojson(indexSpec));

// If "collation" field is not present in "idIndex", _id index inherits collection default
// collation.
db.create_collection.drop();
assert.commandWorked(db.createCollection(
    "create_collection", {collation: {locale: "en_US"}, idIndex: {key: {_id: 1}, name: "_id_"}}));
indexSpec = GetIndexHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.collation.locale, "en_US", tojson(indexSpec));

//
// Tests the combination of the "capped", "size" and "max" fields in createCollection().
//

// When "capped" is true, the "size" field needs to be present.
assert.commandFailedWithCode(db.createCollection('capped_no_size_no_max', {capped: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.createCollection('capped_no_size', {capped: true, max: 10}),
                             ErrorCodes.InvalidOptions);
db.no_capped.drop();
assert.commandWorked(db.createCollection('no_capped'), {capped: false});
db.capped_no_max.drop();
assert.commandWorked(db.createCollection('capped_no_max', {capped: true, size: 256}));
db.capped_with_max_and_size.drop();
assert.commandWorked(
    db.createCollection('capped_with_max_and_size', {capped: true, max: 10, size: 256}));

// When the "size" field is present, "capped" needs to be true.
assert.commandFailedWithCode(db.createCollection('size_no_capped', {size: 256}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.createCollection('size_capped_false', {capped: false, size: 256}),
                             ErrorCodes.InvalidOptions);
})();
