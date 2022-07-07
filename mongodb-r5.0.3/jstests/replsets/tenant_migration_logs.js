/**
 * Tests that migration certificates do not show up in the logs.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/libs/uuid_util.js");

function assertNoCertificateOrPrivateKeyLogsForCmd(conn, cmdName) {
    assert(checkLog.checkContainsOnce(conn, new RegExp(`Slow query.*${cmdName}`)),
           "did not find slow query logs for the command");
    assert(!checkLog.checkContainsOnce(conn, /BEGIN CERTIFICATE.*END CERTIFICATE/),
           "found certificate in the logs");
    assert(!checkLog.checkContainsOnce(conn, /BEGIN PRIVATE KEY.*END PRIVATE KEY/),
           "found private key in the logs");
}

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

// Verify that migration certificates are not logged as part of slow query logging.
(() => {
    const donorDefaultSlowMs =
        assert.commandWorked(donorPrimary.adminCommand({profile: 0, slowms: 0})).slowms;
    const recipientDefaultSlowMs =
        assert.commandWorked(recipientPrimary.adminCommand({profile: 0, slowms: 0})).slowms;

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: "slowCommands",
    };

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    assertNoCertificateOrPrivateKeyLogsForCmd(donorPrimary, "donorStartMigration");
    assertNoCertificateOrPrivateKeyLogsForCmd(recipientPrimary, "recipientSyncData");

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    assertNoCertificateOrPrivateKeyLogsForCmd(donorPrimary, "donorForgetMigration");
    assertNoCertificateOrPrivateKeyLogsForCmd(recipientPrimary, "recipientForgetMigration");

    assert.commandWorked(donorPrimary.adminCommand({profile: 0, slowms: donorDefaultSlowMs}));
    assert.commandWorked(
        recipientPrimary.adminCommand({profile: 0, slowms: recipientDefaultSlowMs}));
})();

tenantMigrationTest.stop();
})();
