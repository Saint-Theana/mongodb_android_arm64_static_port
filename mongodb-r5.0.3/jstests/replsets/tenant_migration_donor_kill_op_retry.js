/**
 * Tests that the donor will retry its steps if its OperationContext is interrupted by a killOp.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kGarbageCollectionDelayMS = 5 * 1000;
const kDelayMS = 100000;  // Set some arbitrarily large blockTimeMS to let recipientSyncData command
                          // hang until we use kill op to kill it.
const kTenantIdPrefix = "testTenantId";
let testNum = 0;
const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();
const garbageCollectionOpts = {
    // Set the delay before a donor state doc is garbage collected to be short to speed
    // up the test.
    tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
    ttlMonitorSleepSecs: 1
};

function makeTenantId() {
    return kTenantIdPrefix + testNum++;
}

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

{
    // This section tests behavior in the middle of a tenant migration.
    let fpNames = [
        "pauseTenantMigrationBeforeInsertingDonorStateDoc",
        "pauseTenantMigrationDonorWhileUpdatingStateDoc",
        "pauseTenantMigrationBeforeStoringExternalClusterTimeKeyDocs"
    ];
    for (let fpName of fpNames) {
        jsTestLog("Setting failpoint \"" + fpName +
                  "\" to test that the migration will retry the " +
                  "operation at the failpoint if a killOp is issued.");

        const migrationOpts = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: tenantMigrationTest.getRecipientConnString(),
            tenantId: makeTenantId(),

        };
        const donorPrimary = tenantMigrationTest.getDonorPrimary();
        let fp = configureFailPoint(donorPrimary, fpName);

        const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());

        const runMigrationThread =
            new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
        runMigrationThread.start();
        fp.wait();

        const res = assert.commandWorked(donorPrimary.adminCommand({
            currentOp: true,
            $all: true,
            desc: {$regex: 'TenantMigrationDonorService'},
            opid: {$exists: true}
        }));

        const opid = res.inprog[0].opid;
        assert.commandWorked(donorPrimary.adminCommand({killOp: 1, op: opid}));

        fp.off();
        runMigrationThread.join();

        TenantMigrationTest.assertCommitted(runMigrationThread.returnData());
        assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    }
}

{
    // This section tests the behavior during TenantMigrationDonorService creation.
    let fpNames = [
        "pauseTenantMigrationBeforeCreatingStateDocumentTTLIndex",
        "pauseTenantMigrationBeforeCreatingExternalKeysTTLIndex"
    ];
    for (let fpName of fpNames) {
        tenantMigrationTest.getDonorRst().stopSet();
        tenantMigrationTest.getDonorRst().startSet(Object.assign({}, migrationX509Options.donor, {
            setParameter: {['failpoint.' + fpName]: tojson({mode: 'alwaysOn'})}
        }));
        // The failpoints in this test run hang the TenantMigrationDonorService during service
        // rebuild, so we need to skip waiting on PrimaryOnlyServices.
        tenantMigrationTest.getDonorRst().initiate(
            null, null, {doNotWaitForPrimaryOnlyServices: true});
        TenantMigrationUtil.createTenantMigrationRecipientRoleIfNotExist(
            tenantMigrationTest.getDonorRst());

        jsTestLog(
            "Setting failpoint \"" + fpName +
            "\" during the creation of a ReplSetTest to test that the migration will retry the " +
            "operation at the failpoint if a killOp is issued.");

        const migrationOpts = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: tenantMigrationTest.getRecipientConnString(),
            tenantId: makeTenantId(),

        };
        const donorPrimary = tenantMigrationTest.getDonorPrimary();
        const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
        const runMigrationThread =
            new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
        runMigrationThread.start();

        const res = assert.commandWorked(donorPrimary.adminCommand({
            currentOp: true,
            $all: true,
            desc: {$regex: 'TenantMigrationDonorService'},
            opid: {$exists: true}
        }));
        const opid = res.inprog[0].opid;
        assert.commandWorked(donorPrimary.adminCommand({killOp: 1, op: opid}));

        assert.commandWorked(donorPrimary.adminCommand({configureFailPoint: fpName, mode: "off"}));

        runMigrationThread.join();

        TenantMigrationTest.assertCommitted(runMigrationThread.returnData());
        assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    }
}

{
    // This section is testing behavior during garbage collection.
    tenantMigrationTest.getDonorRst().stopSet();
    tenantMigrationTest.getDonorRst().startSet(
        Object.assign({}, migrationX509Options.donor, {setParameter: garbageCollectionOpts}));
    tenantMigrationTest.getDonorRst().initiate();
    TenantMigrationUtil.createTenantMigrationRecipientRoleIfNotExist(
        tenantMigrationTest.getDonorRst());

    tenantMigrationTest.getRecipientRst().stopSet();
    tenantMigrationTest.getRecipientRst().startSet(
        Object.assign({}, migrationX509Options.recipient, {setParameter: garbageCollectionOpts}));
    tenantMigrationTest.getRecipientRst().initiate();
    TenantMigrationUtil.createTenantMigrationDonorRoleIfNotExist(
        tenantMigrationTest.getRecipientRst());

    let fpNames = [
        "pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable",
        "pauseTenantMigrationBeforeMarkingExternalKeysGarbageCollectable"
    ];
    for (let fpName of fpNames) {
        jsTestLog(
            "Setting failpoint \"" + fpName +
            "\" during migration garbage collection to test that the migration will retry the " +
            "operation at the failpoint if a killOp is issued.");
        const migrationId = UUID();
        const tenantId = makeTenantId();
        const migrationOpts = {
            migrationIdString: extractUUIDFromObject(migrationId),
            recipientConnString: tenantMigrationTest.getRecipientConnString(),
            tenantId: tenantId,
        };

        let fp = configureFailPoint(tenantMigrationTest.getDonorPrimary(), fpName);

        TenantMigrationTest.assertCommitted(
            tenantMigrationTest.runMigration(migrationOpts,
                                             false /* retry on retriable errors */,
                                             false /* Automatically forget migration */));

        const donorPrimary = tenantMigrationTest.getDonorPrimary();
        const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
        const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                                 migrationOpts.migrationIdString,
                                                 donorRstArgs);
        forgetMigrationThread.start();

        fp.wait();

        const res = assert.commandWorked(donorPrimary.adminCommand({
            currentOp: true,
            $all: true,
            desc: {$regex: 'TenantMigrationDonorService'},
            opid: {$exists: true}
        }));
        const opid = res.inprog[0].opid;
        assert.commandWorked(donorPrimary.adminCommand({killOp: 1, op: opid}));

        fp.off();
        forgetMigrationThread.join();
        tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);
    }
}
tenantMigrationTest.stop();
})();
