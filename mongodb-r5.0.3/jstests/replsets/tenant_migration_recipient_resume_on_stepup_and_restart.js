/**
 * Tests that tenant migrations resume successfully on recipient stepup and restart.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft, incompatible_with_windows_tls, incompatible_with_macos]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kMaxSleepTimeMS = 100;
const kTenantId = "testTenantId";

// Set the delay before a state doc is garbage collected to be short to speed up the test but long
// enough for the state doc to still be around after stepup or restart.
const kGarbageCollectionDelayMS = 30 * 1000;

// Set the TTL monitor to run at a smaller interval to speed up the test.
const kTTLMonitorSleepSecs = 1;

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

/**
 * Runs the donorStartMigration command to start a migration, and interrupts the migration on the
 * recipient using the 'interruptFunc' after the migration starts on the recipient side, and
 * asserts that migration eventually commits.
 * @param {recipientRestarted} bool is needed to properly assert the tenant migrations stat count.
 */
function testRecipientSyncDataInterrupt(interruptFunc, recipientRestarted) {
    const recipientRst = new ReplSetTest(
        {nodes: 3, name: "recipientRst", nodeOptions: migrationX509Options.recipient});
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        recipientRst.stopSet();
        return;
    }
    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    let recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const runMigrationThread = new Thread(TenantMigrationUtil.runMigrationAsync,
                                          migrationOpts,
                                          donorRstArgs,
                                          false /* retryOnRetryableErrors */);
    runMigrationThread.start();

    // Wait for recipientSyncData command to start.
    assert.soon(
        () => recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"})
                  .inprog.length > 0);

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(recipientRst);

    TenantMigrationTest.assertCommitted(runMigrationThread.returnData());
    tenantMigrationTest.waitForDonorNodesToReachState(donorRst.nodes,
                                                      migrationId,
                                                      migrationOpts.tenantId,
                                                      TenantMigrationTest.DonorState.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    tenantMigrationTest.awaitTenantMigrationStatsCounts(donorPrimary,
                                                        {totalSuccessfulMigrationsDonated: 1});
    recipientPrimary = tenantMigrationTest.getRecipientPrimary();  // Could change after interrupt.
    if (!recipientRestarted) {
        tenantMigrationTest.awaitTenantMigrationStatsCounts(recipientPrimary,
                                                            {totalSuccessfulMigrationsReceived: 1});
    } else {
        // In full restart the count could be lost completely.
        const stats = tenantMigrationTest.getTenantMigrationStats(recipientPrimary);
        assert(1 == stats.totalSuccessfulMigrationsReceived ||
               0 == stats.totalSuccessfulMigrationsReceived);
    }

    tenantMigrationTest.stop();
    recipientRst.stopSet();
}

/**
 * Starts a migration and waits for it to commit, then runs the donorForgetMigration, and interrupts
 * the recipient using the 'interruptFunc', and asserts that the migration state is eventually
 * garbage collected.
 */
function testRecipientForgetMigrationInterrupt(interruptFunc) {
    const donorRst = new ReplSetTest({
        nodes: 1,
        name: "donorRst",
        nodeOptions: Object.assign({}, migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });
    const recipientRst = new ReplSetTest({
        nodes: 3,
        name: "recipientRst",
        nodeOptions: Object.assign({}, migrationX509Options.recipient, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: recipientRst.getURL(),
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                             migrationOpts.migrationIdString,
                                             donorRstArgs,
                                             false /* retryOnRetryableErrors */);
    forgetMigrationThread.start();

    // Wait for recipientForgetMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"}));
        return res.inprog[0].expireAt != null;
    });
    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(recipientRst);

    assert.commandWorkedOrFailedWithCode(
        tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString),
        ErrorCodes.NoSuchTenantMigration);

    assert.commandWorked(forgetMigrationThread.returnData());
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, migrationOpts.tenantId);

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
}

(() => {
    jsTest.log("Test that the migration resumes on stepup");
    testRecipientSyncDataInterrupt((recipientRst) => {
        // Force the primary to step down but make it likely to step back up.
        const recipientPrimary = recipientRst.getPrimary();
        assert.commandWorked(recipientPrimary.adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));
    }, false);
})();

(() => {
    jsTest.log("Test that the migration resumes after restart");
    testRecipientSyncDataInterrupt((recipientRst) => {
        recipientRst.stopSet(null /* signal */, true /*forRestart */);
        recipientRst.startSet({restart: true});
        recipientRst.awaitSecondaryNodes();
        recipientRst.getPrimary();
    }, true);
})();

(() => {
    jsTest.log("Test that the recipientForgetMigration command can be retried on stepup");
    testRecipientForgetMigrationInterrupt((recipientRst) => {
        // Force the primary to step down but make it likely to step back up.
        const recipientPrimary = recipientRst.getPrimary();
        assert.commandWorked(recipientPrimary.adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));
    });
})();

(() => {
    jsTest.log("Test that the recipientForgetMigration command can be retried after restart");
    testRecipientForgetMigrationInterrupt((recipientRst) => {
        recipientRst.stopSet(null /* signal */, true /*forRestart */);
        recipientRst.startSet({restart: true});
        recipientRst.awaitSecondaryNodes();
        recipientRst.getPrimary();
    });
})();
})();
