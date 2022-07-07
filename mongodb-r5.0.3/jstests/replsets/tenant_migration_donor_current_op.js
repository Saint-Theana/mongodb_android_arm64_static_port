/**
 * Tests currentOp command during a tenant migration.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft, incompatible_with_windows_tls, incompatible_with_macos]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

// An object that mirrors the donor migration states.
const migrationStates = {
    kUninitialized: 0,
    kAbortingIndexBuilds: 1,
    kDataSync: 2,
    kBlocking: 3,
    kCommitted: 4,
    kAborted: 5
};

const kTenantId = 'testTenantId';
const kReadPreference = {
    mode: "primary"
};

(() => {
    jsTestLog("Testing currentOp output for migration in data sync state");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    let fp = configureFailPoint(donorPrimary,
                                "pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kAbortingIndexBuilds);
    assert.eq(res.inprog[0].migrationCompleted, false);
    assert(res.inprog[0].migrationStart instanceof Date);

    fp.off();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for migration in data sync state");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kDataSync);
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);
    assert.eq(res.inprog[0].migrationCompleted, false);
    assert(res.inprog[0].migrationStart instanceof Date);

    fp.off();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for migration in blocking state");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kBlocking);
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);
    assert(res.inprog[0].blockTimestamp instanceof Timestamp);
    assert.eq(res.inprog[0].migrationCompleted, false);
    assert(res.inprog[0].migrationStart instanceof Date);

    fp.off();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.stop();
})();

(() => {
    jsTestLog("Testing currentOp output for aborted migration");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    TenantMigrationTest.assertAborted(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));

    const res = assert.commandWorked(
        donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kAborted);
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);
    assert(res.inprog[0].blockTimestamp instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);
    assert.eq(typeof res.inprog[0].abortReason.code, "number");
    assert.eq(typeof res.inprog[0].abortReason.codeName, "string");
    assert.eq(typeof res.inprog[0].abortReason.errmsg, "string");
    assert.eq(res.inprog[0].migrationCompleted, false);
    assert(res.inprog[0].migrationStart instanceof Date);

    tenantMigrationTest.stop();
})();

// Check currentOp while in committed state before and after a migration has completed.
(() => {
    jsTestLog("Testing currentOp output for committed migration");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: kReadPreference
    };
    assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));

    let res = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kCommitted);
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);
    assert(res.inprog[0].blockTimestamp instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);
    assert.eq(res.inprog[0].migrationCompleted, false);
    assert(res.inprog[0].migrationStart instanceof Date);

    jsTestLog("Testing currentOp output for a committed migration after donorForgetMigration");

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    res = donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"});
    assert.eq(res.inprog.length, 1);
    assert.eq(bsonWoCompare(res.inprog[0].instanceID, migrationId), 0);
    assert.eq(bsonWoCompare(res.inprog[0].tenantId, kTenantId), 0);
    assert.eq(res.inprog[0].recipientConnectionString,
              tenantMigrationTest.getRecipientRst().getURL());
    assert.eq(bsonWoCompare(res.inprog[0].readPreference, kReadPreference), 0);
    assert.eq(res.inprog[0].lastDurableState, migrationStates.kCommitted);
    assert(res.inprog[0].startMigrationDonorTimestamp instanceof Timestamp);
    assert(res.inprog[0].blockTimestamp instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.ts instanceof Timestamp);
    assert(res.inprog[0].commitOrAbortOpTime.t instanceof NumberLong);
    assert(res.inprog[0].expireAt instanceof Date);
    assert.eq(res.inprog[0].migrationCompleted, true);
    assert(res.inprog[0].migrationStart instanceof Date);

    tenantMigrationTest.stop();
})();
})();
