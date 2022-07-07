/**
 * Tests that tenant migration commands cannot be run on sharded clusters.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");

const st = new ShardingTest({shards: 1});
const donorRstShard = st.rs0;
const donorRstConfig = st.configRS;
const recipientRst = new ReplSetTest({nodes: 1});
recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), donorRst: donorRstShard, recipientRst});

if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    st.stop();
    recipientRst.stopSet();
    return;
}

// Run tenant migration commands on shards.
let donorPrimary = donorRstShard.getPrimary();

let cmdObj = {
    donorStartMigration: 1,
    tenantId: "kTenantTest",
    migrationId: UUID(),
    recipientConnectionString: tenantMigrationTest.getRecipientConnString(),
    readPreference: {mode: "primary"}
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    donorForgetMigration: 1,
    migrationId: UUID()
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    donorAbortMigration: 1,
    migrationId: UUID()
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    tenantId: "kTenantTest",
    readPreference: {mode: "primary"},
    startMigrationDonorTimestamp: Timestamp(1, 1)
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    recipientForgetMigration: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    tenantId: "kTenantTest",
    readPreference: {mode: "primary"},
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

// Run tenant migration commands on config servers.
donorPrimary = donorRstConfig.getPrimary();

cmdObj = {
    donorStartMigration: 1,
    tenantId: "kTenantTest",
    migrationId: UUID(),
    recipientConnectionString: tenantMigrationTest.getRecipientConnString(),
    readPreference: {mode: "primary"}
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    donorForgetMigration: 1,
    migrationId: UUID()
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    donorAbortMigration: 1,
    migrationId: UUID()
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    recipientSyncData: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    tenantId: "kTenantTest",
    readPreference: {mode: "primary"},
    startMigrationDonorTimestamp: Timestamp(1, 1)
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

cmdObj = {
    recipientForgetMigration: 1,
    migrationId: UUID(),
    donorConnectionString: tenantMigrationTest.getRecipientRst().getURL(),
    tenantId: "kTenantTest",
    readPreference: {mode: "primary"},
};
assert.commandFailedWithCode(donorPrimary.adminCommand(cmdObj), ErrorCodes.IllegalOperation);

tenantMigrationTest.stop();
recipientRst.stopSet();
st.stop();
})();
