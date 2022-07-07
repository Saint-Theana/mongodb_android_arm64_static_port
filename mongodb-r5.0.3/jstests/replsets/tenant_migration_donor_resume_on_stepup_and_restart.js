/**
 * Tests that tenant migrations resume successfully on donor stepup and restart.
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
const kMigrationFpNames = [
    "pauseTenantMigrationBeforeLeavingDataSyncState",
    "pauseTenantMigrationBeforeLeavingBlockingState",
    "abortTenantMigrationBeforeLeavingBlockingState",
    ""
];

// Set the delay before a state doc is garbage collected to be short to speed up the test but long
// enough for the state doc to still be around after stepup or restart.
const kGarbageCollectionDelayMS = 30 * 1000;

// Set the TTL monitor to run at a smaller interval to speed up the test.
const kTTLMonitorSleepSecs = 1;

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

/**
 * Runs the donorStartMigration command to start a migration, and interrupts the migration on the
 * donor using the 'interruptFunc', and asserts that migration eventually commits.
 */
function testDonorStartMigrationInterrupt(interruptFunc, donorRestarted) {
    const donorRst =
        new ReplSetTest({nodes: 3, name: "donorRst", nodeOptions: migrationX509Options.donor});

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }
    let donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

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
                                          true /* retryOnRetryableErrors */);
    runMigrationThread.start();

    // Wait for donorStartMigration command to start.
    assert.soon(() => donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"})
                          .inprog.length > 0);

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst);

    TenantMigrationTest.assertCommitted(runMigrationThread.returnData());
    tenantMigrationTest.waitForDonorNodesToReachState(donorRst.nodes,
                                                      migrationId,
                                                      migrationOpts.tenantId,
                                                      TenantMigrationTest.DonorState.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    donorPrimary = tenantMigrationTest.getDonorPrimary();  // Could change after interrupt.
    const donorStats = tenantMigrationTest.getTenantMigrationStats(donorPrimary);
    jsTestLog(`Stats at the donor primary: ${tojson(donorStats)}`);
    if (donorRestarted) {
        // If full restart happened the count could be lost completely.
        assert.gte(1, donorStats.totalSuccessfulMigrationsDonated);
    } else {
        // The double counting happens when the failover happens after migration completes
        // but before the state doc GC mark is persisted. While this test is targeting this
        // scenario it is low probability in production.
        assert(1 == donorStats.totalSuccessfulMigrationsDonated ||
               2 == donorStats.totalSuccessfulMigrationsDonated);
    }
    // Skip checking the stats on the recipient since enableRecipientTesting is false
    // so the recipient is forced to respond to recipientSyncData without starting the
    // migration.

    tenantMigrationTest.stop();
    donorRst.stopSet();
}

/**
 * Starts a migration and waits for it to commit, then runs the donorForgetMigration, and interrupts
 * the donor using the 'interruptFunc', and asserts that the migration state is eventually garbage
 * collected.
 */
function testDonorForgetMigrationInterrupt(interruptFunc) {
    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donorRst",
        nodeOptions: Object.assign({}, migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
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
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

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
                                             true /* retryOnRetryableErrors */);
    forgetMigrationThread.start();

    // Wait for donorForgetMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        return res.inprog[0].expireAt != null;
    });
    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst);

    assert.commandWorkedOrFailedWithCode(
        tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString),
        ErrorCodes.NoSuchTenantMigration);

    assert.commandWorked(forgetMigrationThread.returnData());
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, migrationOpts.tenantId);

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
}

/**
 * Starts a migration and sets the passed in failpoint, then runs the donorAbortMigration, and
 * interrupts the donor using the 'interruptFunc', and asserts that the migration state is
 * eventually garbage collected.
 */
function testDonorAbortMigrationInterrupt(interruptFunc, fpName, isShutdown = false) {
    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donorRst",
        nodeOptions: Object.assign({}, migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
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

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: recipientRst.getURL(),
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    let donorPrimary = tenantMigrationTest.getDonorPrimary();

    // If we passed in a valid failpoint we set it, otherwise we let the migration run normally.
    let fp;
    if (fpName) {
        fp = configureFailPoint(donorPrimary, fpName);
    }

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    const tryAbortThread = new Thread(TenantMigrationUtil.tryAbortMigrationAsync,
                                      {migrationIdString: migrationOpts.migrationIdString},
                                      donorRstArgs,
                                      true /* retryOnRetryableErrors */);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        return res.inprog[0].receivedCancellation;
    });

    interruptFunc(donorRst);

    if (fp && !isShutdown) {
        // Turn off failpoint in order to allow the migration to resume after stepup.
        fp.off();
    }

    tryAbortThread.join();

    let res = tryAbortThread.returnData();
    assert.commandWorkedOrFailedWithCode(res, ErrorCodes.TenantMigrationCommitted);

    donorPrimary = tenantMigrationTest.getDonorPrimary();
    let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});

    if (!res.ok) {
        assert.eq(donorDoc.state, TenantMigrationTest.DonorState.kCommitted);
    } else {
        assert.eq(donorDoc.state, TenantMigrationTest.DonorState.kAborted);
    }

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
}

/**
 * Starts a migration and sets the passed in failpoint, then either waits for the failpoint or lets
 * the migration run successfully and interrupts the donor using the 'interruptFunc'. After
 * restarting, check the to see if the donorDoc data has persisted.
 */
function testStateDocPersistenceOnFailover(interruptFunc, fpName, isShutdown = false) {
    const donorRst =
        new ReplSetTest({nodes: 3, name: "donorRst", nodeOptions: migrationX509Options.donor});

    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };
    let donorPrimary = tenantMigrationTest.getDonorPrimary();

    // If we passed in a valid failpoint we set it, otherwise we let the migration run normally.
    let fp;
    if (fpName) {
        fp = configureFailPoint(donorPrimary, fpName);
        assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
        fp.wait();
    } else {
        TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));
    }

    let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    let donorDocBeforeFailover = configDonorsColl.findOne({tenantId: kTenantId});

    interruptFunc(tenantMigrationTest.getDonorRst());

    if (fp && !isShutdown) {
        // Turn off failpoint in order to allow the migration to resume after stepup.
        fp.off();
    }

    donorPrimary = tenantMigrationTest.getDonorPrimary();
    configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    let donorDocAfterFailover = configDonorsColl.findOne({tenantId: kTenantId});

    // Check persisted fields in the donor doc.
    assert.eq(donorDocBeforeFailover._id, donorDocAfterFailover._id);
    assert.eq(donorDocBeforeFailover.recipientConnString,
              donorDocAfterFailover.recipientConnString);
    assert.eq(donorDocBeforeFailover.readPreference, donorDocAfterFailover.readPreference);
    assert.eq(donorDocBeforeFailover.startMigrationDonorTimestamp,
              donorDocAfterFailover.startMigrationDonorTimestamp);
    assert.eq(donorDocBeforeFailover.migration, donorDocAfterFailover.migration);
    assert.eq(donorDocBeforeFailover.tenantId, donorDocAfterFailover.tenantId);
    assert.eq(donorDocBeforeFailover.donorCertificateForRecipient,
              donorDocAfterFailover.donorCertificateForRecipient);
    assert.eq(donorDocBeforeFailover.recipientCertificateForDonor,
              donorDocAfterFailover.recipientCertificateForDonor);
    assert.eq(donorDocBeforeFailover.migrationStart, donorDocAfterFailover.migrationStart);

    tenantMigrationTest.stop();
    donorRst.stopSet();
}

(() => {
    jsTest.log("Test that the migration resumes on stepup");
    testDonorStartMigrationInterrupt((donorRst) => {
        // Force the primary to step down but make it likely to step back up.
        const donorPrimary = donorRst.getPrimary();
        assert.commandWorked(
            donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
    }, false /* donor restarted */);
})();

(() => {
    jsTest.log("Test that the migration resumes after restart");
    testDonorStartMigrationInterrupt((donorRst) => {
        donorRst.stopSet(null /* signal */, true /*forRestart */);
        donorRst.startSet({restart: true});
    }, true /* donor restarted */);
})();

(() => {
    jsTest.log("Test that the donorForgetMigration command can be retried on stepup");
    testDonorForgetMigrationInterrupt((donorRst) => {
        // Force the primary to step down but make it likely to step back up.
        const donorPrimary = donorRst.getPrimary();
        assert.commandWorked(
            donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
    });
})();

(() => {
    jsTest.log("Test that the donorForgetMigration command can be retried after restart");
    testDonorForgetMigrationInterrupt((donorRst) => {
        donorRst.stopSet(null /* signal */, true /*forRestart */);
        donorRst.startSet({restart: true});
    });
})();

(() => {
    jsTest.log("Test that the donorAbortMigration command can be retried after restart");

    kMigrationFpNames.forEach(fpName => {
        if (!fpName) {
            jsTest.log("Testing without setting a failpoint.");
        } else {
            jsTest.log("Testing with failpoint: " + fpName);
        }

        testDonorAbortMigrationInterrupt((donorRst) => {
            donorRst.stopSet(null /* signal */, true /*forRestart */);
            donorRst.startSet({restart: true});
        }, fpName, true);
    });
})();

(() => {
    jsTest.log("Test that the donorAbortMigration command can be retried on stepup");
    kMigrationFpNames.forEach(fpName => {
        if (!fpName) {
            jsTest.log("Testing without setting a failpoint.");
        } else {
            jsTest.log("Testing with failpoint: " + fpName);
        }

        testDonorAbortMigrationInterrupt((donorRst) => {
            // Force the primary to step down but make it likely to step back up.
            const donorPrimary = donorRst.getPrimary();
            assert.commandWorked(donorPrimary.adminCommand(
                {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
            assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
        }, fpName);
    });
})();

(() => {
    jsTest.log("Test stateDoc data persistence on restart.");
    kMigrationFpNames.forEach(fpName => {
        if (!fpName) {
            jsTest.log("Testing without setting a failpoint.");
        } else {
            jsTest.log("Testing with failpoint: " + fpName);
        }

        testStateDocPersistenceOnFailover((donorRst) => {
            donorRst.stopSet(null /* signal */, true /*forRestart */);
            donorRst.startSet({restart: true});
        }, fpName, true);
    });
})();

(() => {
    jsTest.log("Test stateDoc data persistence on stepup.");
    kMigrationFpNames.forEach(fpName => {
        if (!fpName) {
            jsTest.log("Testing without setting a failpoint.");
        } else {
            jsTest.log("Testing with failpoint: " + fpName);
        }

        testStateDocPersistenceOnFailover((donorRst) => {
            // Force the primary to step down but make it likely to step back up.
            const donorPrimary = donorRst.getPrimary();
            assert.commandWorked(donorPrimary.adminCommand(
                {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
            assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
        }, fpName);
    });
})();
})();
