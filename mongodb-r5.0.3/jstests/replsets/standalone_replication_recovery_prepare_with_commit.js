/**
 * Tests that we can recover a transaction that was prepared and committed using the
 * 'recoverFromOplogAsStandalone' flag.
 *
 * This test only makes sense for storage engines that support recover to stable timestamp.
 * @tags: [requires_wiredtiger, requires_persistence, requires_journaling, requires_replication,
 * requires_majority_read_concern, uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";
load("jstests/replsets/libs/prepare_standalone_replication_recovery.js");

const testName = "standalone_replication_recovery_prepare_with_commit";

// TODO (SERVER-49862): Re-enable fast count validation if possible.
TestData.skipEnforceFastCountOnValidate = true;

testPrepareRecoverFromOplogAsStandalone(testName, /* commitBeforeRecovery */ true);
})();
