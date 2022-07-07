/**
 * Tests that a migration will continuously retry sync source selection when there are no available
 * donor hosts. Also checks that a donor host is considered an uneligible sync source when it has a
 * majority OpTime earlier than the recipient's stored 'startApplyingDonorOpTime'.
 *
 * Tests that if a donor host becomes available, the recipient will successfully choose it as a
 * sync source and resume the migration.
 *
 * @tags: [requires_majority_read_concern, requires_fcv_49, incompatible_with_windows_tls,
 * incompatible_with_eft, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_recipient_sync_source.js");

// After this setUp() call, we should have a migration with 'secondary' read preference. The
// recipient should be continuously retrying sync source selection, unable to choose
// 'delayedSecondary' because it is too stale and 'donorSecondary' because it is down.
const {
    tenantMigrationTest,
    migrationOpts,
    donorSecondary,
    delayedSecondary,
    hangAfterCreatingConnections
} = setUpMigrationSyncSourceTest();

if (!tenantMigrationTest) {
    // Feature flag was not enabled.
    return;
}

const donorRst = tenantMigrationTest.getDonorRst();

jsTestLog("Restarting 'donorSecondary'");
donorRst.start(donorSecondary, null /* options */, true /* restart */);

// The recipient should eventually be able to connect to the donor secondary, after the node reaches
// 'secondary' state.
hangAfterCreatingConnections.wait();

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
const currOp = res.inprog[0];
// 'donorSecondary' should always be the chosen sync source, since read preference is 'secondary'
// and 'delayedSecondary' cannot be chosen because it is too stale.
assert.eq(donorSecondary.host,
          currOp.donorSyncSource,
          `the recipient should only be able to choose 'donorSecondary' as sync source`);

hangAfterCreatingConnections.off();
restartServerReplication(delayedSecondary);

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

donorRst.stopSet();
tenantMigrationTest.stop();
})();
