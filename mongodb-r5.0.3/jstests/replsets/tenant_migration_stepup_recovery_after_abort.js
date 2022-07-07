/**
 * Tests the the donor correctly recovers the abort reason and the migration after stepup.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

// Set the delay before a state doc is garbage collected to be short to speed up the test.
const kGarbageCollectionParams = {
    tenantMigrationGarbageCollectionDelayMS: 3 * 1000,
    ttlMonitorSleepSecs: 1,
};

const donorRst = new ReplSetTest({
    nodes: 3,
    name: "donor",
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor,
                               {setParameter: kGarbageCollectionParams})
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), donorRst, sharedOptions: {setParameter: kGarbageCollectionParams}});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    return;
}

const tenantId = "testTenantId";
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: tenantId,
};

const donorPrimary = tenantMigrationTest.getDonorPrimary();

assert.commandWorked(donorPrimary.getCollection(tenantId + "_testDb.testColl").insert({_id: 0}));

const donorFp = configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");

TenantMigrationTest.assertAborted(
    tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */),
    ErrorCodes.InternalError);
donorFp.off();

assert.commandWorked(
    donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));

TenantMigrationTest.assertAborted(
    tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */),
    ErrorCodes.InternalError);

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);

donorRst.stopSet();
tenantMigrationTest.stop();
})();
