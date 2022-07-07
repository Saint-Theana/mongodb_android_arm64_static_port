/**
 * Tests whether the recipient returns an appropriate error code to the donor when the recipient
 * primary is made to step down before creating the oplog buffer collection.
 *
 * @tags: [requires_fcv_49, requires_replication, incompatible_with_windows_tls,
 * incompatible_with_eft, incompatible_with_macos, requires_persistence]
 */

(function() {

"use strict";
load("jstests/libs/uuid_util.js");        // For extractUUIDFromObject().
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 2}});

if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const kMigrationId = UUID();
const kTenantId = 'testTenantId';
const kReadPreference = {
    mode: "primary"
};
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(kMigrationId),
    tenantId: kTenantId,
    readPreference: kReadPreference
};

const fpBeforeCreatingOplogBuffer =
    configureFailPoint(tenantMigrationTest.getRecipientPrimary(),
                       "fpAfterRetrievingStartOpTimesMigrationRecipientInstance",
                       {action: "hang"});

jsTestLog("Starting tenant migration with migrationId: " + kMigrationId +
          ", tenantId: " + kTenantId);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

jsTestLog("Waiting until the recipient primary is about to create an oplog buffer collection.");
fpBeforeCreatingOplogBuffer.wait();

jsTestLog("Stepping a new primary up.");
assert.commandWorked(tenantMigrationTest.getRecipientRst().getSecondaries()[0].adminCommand(
    {replSetStepUp: ReplSetTest.kForeverSecs, force: true}));

fpBeforeCreatingOplogBuffer.off();

jsTestLog("Waiting for migration to complete.");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

tenantMigrationTest.stop();
})();