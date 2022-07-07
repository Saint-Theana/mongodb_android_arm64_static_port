/**
 * Test that tenant migration donor and recipient correctly copy each other cluster time keys into
 * their config.external_validation_keys collection.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const kInternalKeysNs = "admin.system.keys";
const kExternalKeysNs = "config.external_validation_keys";

/**
 * Asserts that the donor and recipient have copied each other's cluster time keys into
 * config.external_validation_keys.
 */
function assertCopiedExternalKeys(tenantMigrationTest, migrationId) {
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    recipientPrimary.getCollection(kInternalKeysNs).find().forEach(internalKeyDoc => {
        assert.neq(null, donorPrimary.getCollection(kExternalKeysNs).findOne({
            keyId: internalKeyDoc._id,
            key: internalKeyDoc.key,
            expiresAt: internalKeyDoc.expiresAt,
            migrationId,
        }));
    });

    donorPrimary.getCollection(kInternalKeysNs).find().forEach(internalKeyDoc => {
        assert.neq(null, recipientPrimary.getCollection(kExternalKeysNs).findOne({
            keyId: internalKeyDoc._id,
            key: internalKeyDoc.key,
            expiresAt: internalKeyDoc.expiresAt,
            migrationId,
        }));
    });
}

function runMigrationAndAssertExternalKeysCopied(tenantMigrationTest, tenantId) {
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
    };
    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));
    assertCopiedExternalKeys(tenantMigrationTest, migrationId);
}

function assertHasExternalKeys(conn, migrationId) {
    const keys = conn.getCollection(kExternalKeysNs).find({migrationId}).toArray();
    assert.gt(keys.length, 0, tojson(keys));
}

const kTenantId1 = "testTenantId1";
const kTenantId2 = "testTenantId2";
const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

(() => {
    jsTest.log("Test that the donor and recipient correctly copy each other's cluster time keys " +
               "when there is no failover.");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        tenantMigrationTest.stop();
        return;
    }

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId1,
    };
    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));
    assertCopiedExternalKeys(tenantMigrationTest, migrationId);

    // After another migration, the first's keys should still exist.
    runMigrationAndAssertExternalKeysCopied(tenantMigrationTest, kTenantId2);
    assertCopiedExternalKeys(tenantMigrationTest, migrationId);

    // Inserting an invalid key should fail.
    assert.commandFailedWithCode(
        tenantMigrationTest.getDonorPrimary().getCollection(kExternalKeysNs).insert({
            _id: "invalid key"
        }),
        ErrorCodes.TypeMismatch);

    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that the donor and recipient correctly copy each other's cluster time keys " +
               "when there is no failover but the recipient syncs data from a secondary.");
    const recipientRst = new ReplSetTest(
        {nodes: 3, name: "recipientRst", nodeOptions: migrationX509Options.recipient});
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        recipientRst.stopSet();
        tenantMigrationTest.stop();
        return;
    }

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId1,
        readPreference: {mode: "secondary"}
    };
    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));
    assertCopiedExternalKeys(tenantMigrationTest, migrationId);

    // After another migration, the first's keys should still exist.
    runMigrationAndAssertExternalKeysCopied(tenantMigrationTest, kTenantId2);
    assertCopiedExternalKeys(tenantMigrationTest, migrationId);

    recipientRst.stopSet();
    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that the donor and recipient correctly copy each other's cluster time keys " +
               "when there is donor failover.");
    const donorRst =
        new ReplSetTest({nodes: 3, name: "donorRst", nodeOptions: migrationX509Options.donor});
    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        tenantMigrationTest.stop();
        return;
    }

    let donorPrimary = donorRst.getPrimary();
    const fp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationAfterPersistingInitialDonorStateDoc");

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId1,
    };
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    assert.commandWorked(
        donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));

    fp.off();
    TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, true /* retryOnRetryableErrors */));

    assertCopiedExternalKeys(tenantMigrationTest, migrationId);

    // After another migration, the first's keys should still exist.
    runMigrationAndAssertExternalKeysCopied(tenantMigrationTest, kTenantId2);
    assertCopiedExternalKeys(tenantMigrationTest, migrationId);

    donorRst.stopSet();
    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that the donor and recipient correctly copy each other's cluster time keys " +
               "when there is recipient failover.");
    const recipientRst = new ReplSetTest(
        {nodes: 3, name: "recipientRst", nodeOptions: migrationX509Options.recipient});
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        recipientRst.stopSet();
        tenantMigrationTest.stop();
        return;
    }

    const recipientPrimary = recipientRst.getPrimary();
    const fp = configureFailPoint(recipientPrimary,
                                  "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
                                  {action: "hang"});

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId1,
    };
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    assert.commandWorked(
        recipientPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));

    fp.off();
    TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, true /* retryOnRetryableErrors */));

    assertCopiedExternalKeys(tenantMigrationTest, migrationId);

    // After another migration, the first's keys should still exist.
    runMigrationAndAssertExternalKeysCopied(tenantMigrationTest, kTenantId2);
    assertCopiedExternalKeys(tenantMigrationTest, migrationId);

    recipientRst.stopSet();
    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that the donor waits for copied external keys to replicate to every node");
    const donorRst = new ReplSetTest({
        nodes: [{}, {}, {rsConfig: {priority: 0}}],
        name: "donorRst",
        settings: {chainingAllowed: false},
        nodeOptions: migrationX509Options.donor
    });
    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        tenantMigrationTest.stop();
        return;
    }

    function runTest(tenantId, withFailover) {
        const migrationId = UUID();
        const migrationOpts = {
            migrationIdString: extractUUIDFromObject(migrationId),
            tenantId: tenantId,
        };

        // Stop replicating on one of the secondaries so the donor cannot satisfy write concerns
        // that require all nodes but can still commit majority writes. Pause the secondary with 0
        // priority so it can't become primary in the failover case.
        const delayedSecondary = donorRst.getSecondaries()[1];
        stopServerReplication(delayedSecondary);

        const barrierBeforeWaitingForKeyWC = configureFailPoint(
            donorRst.getPrimary(), "pauseTenantMigrationDonorBeforeWaitingForKeysToReplicate");

        assert.commandWorked(
            tenantMigrationTest.startMigration(migrationOpts, false /* retryOnRetryableErrors */));

        // Wait for the donor to begin waiting for replication of the copied keys.
        barrierBeforeWaitingForKeyWC.wait();
        barrierBeforeWaitingForKeyWC.off();
        sleep(500);

        // The migration should be unable to progress past the aborting index builds state because
        // it cannot replicate the copied keys to every donor node.
        let res = assert.commandWorked(
            tenantMigrationTest.runDonorStartMigration(migrationOpts,
                                                       false /* waitForMigrationToComplete */,
                                                       false /* retryOnRetryableErrors */));
        assert.eq("aborting index builds", res.state, tojson(res));

        if (withFailover) {
            // The secondary with a non-zero priority will become the new primary.
            const newPrimary = donorRst.getSecondaries()[0];
            let newPrimaryBarrierBeforeWaitingForKeyWC = configureFailPoint(
                newPrimary, "pauseTenantMigrationDonorBeforeWaitingForKeysToReplicate");

            const oldPrimary = donorRst.getPrimary();
            assert.commandWorked(
                oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));

            newPrimaryBarrierBeforeWaitingForKeyWC.wait();
            newPrimaryBarrierBeforeWaitingForKeyWC.off();
            sleep(500);

            // The migration should still be stuck because it cannot replicate the keysto all donor
            // nodes.
            res = assert.commandWorked(
                tenantMigrationTest.runDonorStartMigration(migrationOpts,
                                                           false /* waitForMigrationToComplete */,
                                                           true /* retryOnRetryableErrors */));
            assert.eq("aborting index builds", res.state, tojson(res));
        }

        // Restart replication, verify the migration can now complete, and the keys are present on
        // all donor nodes.
        restartServerReplication(delayedSecondary);

        res = assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(
            migrationOpts, false /* retryOnRetryableErrors */));
        assert.eq(res.state, "committed", tojson(res));

        donorRst.nodes.forEach(node => {
            assertHasExternalKeys(node, migrationId);
        });
    }

    runTest(kTenantId1, false /* withFailover */);
    runTest(kTenantId2, true /* withFailover */);

    donorRst.stopSet();
    tenantMigrationTest.stop();
})();
})();
