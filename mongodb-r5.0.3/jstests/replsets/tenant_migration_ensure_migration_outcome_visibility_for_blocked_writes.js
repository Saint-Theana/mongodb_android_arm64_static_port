/**
 * Tests that writes that are blocked during a tenant migration will still be able to find out
 * migration outcome even if the migration's in memory state has been garbage collected.
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

const kGarbageCollectionParams = {
    // Set the delay before a donor state doc is garbage collected to be short to speed up the test.
    tenantMigrationGarbageCollectionDelayMS: 30 * 1000,
    // Set the TTL monitor to run at a smaller interval to speed up the test.
    ttlMonitorSleepSecs: 1,
};

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

const donorRst = new ReplSetTest({
    nodes: 1,
    name: 'donor',
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor,
                               {setParameter: kGarbageCollectionParams})
});

function insertDocument(primaryHost, dbName, collName) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);
    let res = primaryDB.runCommand({insert: collName, documents: [{x: 1}]});
    return res;
}

(() => {
    jsTestLog(
        "Testing blocked writes can see migration outcome for a migration that has been committed and garbage collected.");

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({
        name: jsTestName(),
        donorRst,
        enableRecipientTesting: false,
        sharedOptions: {setParameter: kGarbageCollectionParams}
    });
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }

    const migrationId = UUID();
    const tenantId = "migrationOutcome-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    const writeFp = configureFailPoint(primaryDB, "hangWriteBeforeWaitingForMigrationDecision");
    const writeThread = new Thread(insertDocument, primary.host, dbName, kCollName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    migrationThread.start();
    blockFp.wait();

    writeThread.start();
    writeFp.wait();

    blockFp.off();

    migrationThread.join();

    TenantMigrationTest.assertCommitted(migrationThread.returnData());

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);

    writeFp.off();
    writeThread.join();
    const writeRes = writeThread.returnData();

    assert.commandFailedWithCode(writeRes, ErrorCodes.TenantMigrationCommitted);

    tenantMigrationTest.stop();
    donorRst.stopSet();
})();

(() => {
    jsTestLog(
        "Testing blocked writes can see migration outcome for a migration that has been aborted and garbage collected.");

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest(
        {name: jsTestName(), donorRst, sharedOptions: {setParameter: kGarbageCollectionParams}});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }

    const migrationId = UUID();
    const tenantId = "migrationOutcome-aborted";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    const writeFp = configureFailPoint(primaryDB, "hangWriteBeforeWaitingForMigrationDecision");
    const writeThread = new Thread(insertDocument, primary.host, dbName, kCollName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const abortFp = configureFailPoint(primaryDB, "abortTenantMigrationBeforeLeavingBlockingState");
    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    migrationThread.start();
    blockFp.wait();

    writeThread.start();
    writeFp.wait();

    blockFp.off();

    migrationThread.join();

    TenantMigrationTest.assertAborted(migrationThread.returnData());
    abortFp.off();

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);

    writeFp.off();
    writeThread.join();
    const writeRes = writeThread.returnData();

    assert.commandFailedWithCode(writeRes, ErrorCodes.TenantMigrationAborted);

    tenantMigrationTest.stop();
    donorRst.stopSet();
})();
})();
