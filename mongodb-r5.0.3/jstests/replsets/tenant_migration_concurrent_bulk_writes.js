/**
 * Tests that bulk writes during a tenant migration correctly report write errors and
 * retries writes that returned TenantMigrationCommitted.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls, incompatible_with_macos, requires_persistence]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kMaxBatchSize = 2;
const kCollName = "testColl";
const kTenantDefinedDbName = "0";
const kNumWriteOps = 6;  // num of writes to run in bulk.
const kNumWriteBatchesWithoutMigrationConflict =
    2;  // num of write batches we allow to complete before migration blocks writes.
const kNumUpdatesWithoutMigrationConflict = 2;
const kMaxSleepTimeMS = 1000;
const kBatchTypes = {
    insert: 1,
    update: 2,
    remove: 3
};

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();
const donorRst = new ReplSetTest({
    nodes: 1,
    name: 'donor',
    nodeOptions: Object.assign(migrationX509Options.donor, {
        setParameter: {
            internalInsertMaxBatchSize:
                kMaxBatchSize, /* Decrease internal max batch size so we can still show writes are
                                 batched without inserting hundreds of documents. */
            // Allow non-timestamped reads on donor after migration completes for testing.
            'failpoint.tenantMigrationDonorAllowsNonTimestampedReads': tojson({mode: 'alwaysOn'}),
        }
    })
});
donorRst.startSet();
donorRst.initiate();

const recipientRst = new ReplSetTest({
    nodes: 1,
    name: 'recipient',
    nodeOptions: Object.assign(migrationX509Options.recipient, {
        setParameter: {
            internalInsertMaxBatchSize: kMaxBatchSize /* Decrease internal max batch size so we can
                                                         still show writes are batched without
                                                         inserting hundreds of documents. */
        },
    })
});
recipientRst.startSet();
recipientRst.initiate();

const kRecipientConnString = recipientRst.getURL();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    recipientRst.stopSet();
    return;
}

function bulkInsertDocsOrdered(primaryHost, dbName, collName, numDocs) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);
    let bulk = primaryDB[collName].initializeOrderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulk.insert({x: i});
    }

    let res;
    try {
        res = bulk.execute();
    } catch (e) {
        res = e;
    }
    return {res: res.getRawResponse(), ops: bulk.getOperations()};
}

function bulkInsertDocsUnordered(primaryHost, dbName, collName, numDocs) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);
    let bulk = primaryDB[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulk.insert({x: i});
    }

    let res;
    try {
        res = bulk.execute();
    } catch (e) {
        res = e;
    }
    return {res: res.getRawResponse(), ops: bulk.getOperations()};
}

function bulkMultiUpdateDocsOrdered(primaryHost, dbName, collName, numDocs) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);

    // Insert initial docs to be updated.
    let insertBulk = primaryDB[collName].initializeOrderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        insertBulk.insert({x: i});
    }
    insertBulk.execute();

    let updateBulk = primaryDB[collName].initializeOrderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        updateBulk.find({x: i}).update({$set: {ordered_update: true}});
    }

    let res;
    try {
        res = updateBulk.execute();
    } catch (e) {
        res = e;
    }
    return {res: res.getRawResponse ? res.getRawResponse() : res, ops: updateBulk.getOperations()};
}

function bulkMultiUpdateDocsUnordered(primaryHost, dbName, collName, numDocs) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);

    // Insert initial docs to be updated.
    let insertBulk = primaryDB[collName].initializeOrderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        insertBulk.insert({x: i});
    }
    insertBulk.execute();

    let updateBulk = primaryDB[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        updateBulk.find({x: i}).update({$set: {unordered_update: true}});
    }

    let res;
    try {
        res = updateBulk.execute();
    } catch (e) {
        res = e;
    }
    return {res: res.getRawResponse ? res.getRawResponse() : res, ops: updateBulk.getOperations()};
}

(() => {
    jsTestLog("Testing unordered bulk insert against a tenant migration that commits.");

    const tenantId = "bulkUnorderedInserts-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkInsertDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    bulkWriteThread.start();
    writeFp.wait();

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    writeFp.off();
    bulkWriteThread.join();

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length,
              (kNumWriteOps - (kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict)));

    let expectedErrorIndex = kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict;
    writeErrors.forEach((err, arrIndex) => {
        assert.eq(err.code, ErrorCodes.TenantMigrationCommitted);
        assert.eq(err.index, expectedErrorIndex++);
        if (arrIndex == 0) {
            assert(err.errmsg);
        } else {
            assert(!err.errmsg);
        }
    });
})();

(() => {
    jsTestLog(
        "Testing unordered bulk insert against a tenant migration that blocks a few inserts and commits.");

    const tenantId = "bulkUnorderedInserts-blocks-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkInsertDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    TenantMigrationTest.assertCommitted(migrationThread.returnData());

    let bulkWriteRes = bulkWriteThread.returnData();
    let writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length,
              (kNumWriteOps - (kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict)));

    let expectedErrorIndex = kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict;
    writeErrors.forEach((err, index) => {
        assert.eq(err.code, ErrorCodes.TenantMigrationCommitted);
        assert.eq(err.index, expectedErrorIndex++);
        if (index == 0) {
            assert.eq(err.errmsg,
                      "Write or read must be re-routed to the new owner of this tenant");
        } else {
            assert.eq(err.errmsg, "");
        }
    });
})();

(() => {
    jsTestLog("Testing unordered bulk insert against a tenant migration that aborts.");

    const tenantId = "bulkUnorderedInserts-aborted";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkInsertDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    const abortFp = configureFailPoint(primaryDB, "abortTenantMigrationBeforeLeavingBlockingState");

    // The failpoint below is used to ensure that a write to throw
    // TenantMigrationConflict in the op observer. Without this failpoint, the migration
    // could have already aborted by the time the write gets to the op observer.
    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    abortFp.off();

    TenantMigrationTest.assertAborted(migrationThread.returnData());

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length,
              (kNumWriteOps - (kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict)));

    let expectedErrorIndex = kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict;
    writeErrors.forEach((err, arrIndex) => {
        assert.eq(err.code, ErrorCodes.TenantMigrationAborted);
        assert.eq(err.index, expectedErrorIndex++);
        if (arrIndex == 0) {
            assert(err.errmsg);
        } else {
            assert(!err.errmsg);
        }
    });
})();

(() => {
    jsTestLog("Testing ordered bulk inserts against a tenant migration that commits.");

    const tenantId = "bulkOrderedInserts-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkInsertDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    bulkWriteThread.start();
    writeFp.wait();

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    writeFp.off();
    bulkWriteThread.join();

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration
    // started blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationCommitted);
})();

(() => {
    jsTestLog(
        "Testing ordered bulk insert against a tenant migration that blocks a few inserts and commits.");

    const tenantId = "bulkOrderedInserts-blocks-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkInsertDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    TenantMigrationTest.assertCommitted(migrationThread.returnData());
    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration
    // started blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationCommitted);
})();

(() => {
    jsTestLog("Testing ordered bulk write against a tenant migration that aborts.");

    const tenantId = "bulkOrderedInserts-aborted";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkInsertDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    const abortFp = configureFailPoint(primaryDB, "abortTenantMigrationBeforeLeavingBlockingState");

    // The failpoint below is used to ensure that a write to throw
    // TenantMigrationConflict in the op observer. Without this failpoint, the migration
    // could have already aborted by the time the write gets to the op observer.
    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    abortFp.off();

    TenantMigrationTest.assertAborted(migrationThread.returnData());

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration
    // started blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationAborted);
})();

(() => {
    jsTestLog("Testing unordered bulk multi update that blocks.");

    const tenantId = "bulkUnorderedMultiUpdates-blocks";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchUpdate", {}, {skip: kNumUpdatesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkMultiUpdateDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    TenantMigrationTest.assertCommitted(migrationThread.returnData());

    let bulkWriteRes = bulkWriteThread.returnData();
    assert.eq(bulkWriteRes.res.code, ErrorCodes.Interrupted, tojson(bulkWriteRes));
    assert.eq(
        bulkWriteRes.res.errmsg,
        "Operation interrupted by an internal data migration and could not be automatically retried",
        tojson(bulkWriteRes));
})();

(() => {
    jsTestLog("Testing ordered bulk multi update that blocks.");

    const tenantId = "bulkOrderedMultiUpdates-blocks";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchUpdate", {}, {skip: kNumUpdatesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkMultiUpdateDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    TenantMigrationTest.assertCommitted(migrationThread.returnData());

    let bulkWriteRes = bulkWriteThread.returnData();
    assert.eq(bulkWriteRes.res.code, ErrorCodes.Interrupted, tojson(bulkWriteRes));
    assert.eq(
        bulkWriteRes.res.errmsg,
        "Operation interrupted by an internal data migration and could not be automatically retried",
        tojson(bulkWriteRes));
})();

(() => {
    jsTestLog("Testing unordered multi updates against a tenant migration that has completed.");

    const tenantId = "bulkUnorderedMultiUpdates-completed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchUpdate", {}, {skip: kNumUpdatesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkMultiUpdateDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    bulkWriteThread.start();
    writeFp.wait();

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    writeFp.off();
    bulkWriteThread.join();

    const bulkWriteRes = bulkWriteThread.returnData();
    assert.eq(bulkWriteRes.res.code, ErrorCodes.Interrupted, tojson(bulkWriteRes));
    assert.eq(
        bulkWriteRes.res.errmsg,
        "Operation interrupted by an internal data migration and could not be automatically retried",
        tojson(bulkWriteRes));
})();

(() => {
    jsTestLog("Testing ordered multi updates against a tenant migration that has completed.");

    const tenantId = "bulkOrderedMultiUpdates-completed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchUpdate", {}, {skip: kNumUpdatesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkMultiUpdateDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    bulkWriteThread.start();
    writeFp.wait();

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    writeFp.off();
    bulkWriteThread.join();

    const bulkWriteRes = bulkWriteThread.returnData();
    assert.eq(bulkWriteRes.res.code, ErrorCodes.Interrupted, tojson(bulkWriteRes));
    assert.eq(
        bulkWriteRes.res.errmsg,
        "Operation interrupted by an internal data migration and could not be automatically retried",
        tojson(bulkWriteRes));
})();

tenantMigrationTest.stop();
donorRst.stopSet();
recipientRst.stopSet();
})();
