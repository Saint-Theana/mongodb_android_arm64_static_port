/**
 * Commands supporting read and write concern.
 */
var kCommandsSupportingReadConcern = new Set([
    "aggregate",
    "count",
    "distinct",
    "find",
]);

/**
 * Write commands supporting snapshot readConcern in a transaction.
 */
var kWriteCommandsSupportingSnapshotInTransaction = new Set([
    "delete",
    "findAndModify",
    "findandmodify",
    "insert",
    "update",
]);

/**
 * Commands supporting snapshot readConcern outside of transactions.
 */
var kCommandsSupportingSnapshot = new Set([
    "aggregate",
    "distinct",
    "find",
]);

var kCommandsSupportingWriteConcern = new Set([
    "_configsvrAddShard",
    "_configsvrAddShardToZone",
    "_configsvrCommitChunkMerge",
    "_configsvrCommitChunksMerge",
    "_configsvrCommitChunkMigration",
    "_configsvrCommitChunkSplit",
    "_configsvrCreateDatabase",
    "_configsvrEnableSharding",
    "_configsvrMoveChunk",
    "_configsvrMovePrimary",
    "_configsvrRemoveShard",
    "_configsvrRemoveShardFromZone",
    "_configsvrShardCollection",
    "_configsvrUpdateZoneKeyRange",
    "_mergeAuthzCollections",
    "_recvChunkStart",
    "abortTransaction",
    "appendOplogNote",
    "applyOps",
    "aggregate",
    "captrunc",
    "cleanupOrphaned",
    "clone",
    "cloneCollectionAsCapped",
    "collMod",
    "commitTransaction",
    "convertToCapped",
    "create",
    "createIndexes",
    "createRole",
    "createUser",
    "delete",
    "deleteIndexes",
    "drop",
    "dropAllRolesFromDatabase",
    "dropAllUsersFromDatabase",
    "dropDatabase",
    "dropIndexes",
    "dropRole",
    "dropUser",
    "emptycapped",
    "findAndModify",
    "findandmodify",
    "godinsert",
    "grantPrivilegesToRole",
    "grantRolesToRole",
    "grantRolesToUser",
    "insert",
    "mapReduce",
    "mapreduce",
    "moveChunk",
    "renameCollection",
    "revokePrivilegesFromRole",
    "revokeRolesFromRole",
    "revokeRolesFromUser",
    "setFeatureCompatibilityVersion",
    "update",
    "updateRole",
    "updateUser",
]);

var kCommandsSupportingWriteConcernInTransaction =
    new Set(["abortTransaction", "commitTransaction"]);
