/**
 * Tests that a migration will correctly apply a transaction that contains a 'createCollection'
 * command followed by multiple inserts onto that collection.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const tenantId = "testTenantId";
const tenantDB = tenantMigrationTest.tenantDB(tenantId, "testDB");
const collName = "testColl";
const tenantNS = `${tenantDB}.${collName}`;
const transactionsNS = "config.transactions";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const hangAfterStartingOplogApplier = configureFailPoint(
    recipientPrimary, "fpAfterStartingOplogApplierMigrationRecipientInstance", {action: "hang"});

jsTestLog("Starting a migration");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
};
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

hangAfterStartingOplogApplier.wait();

jsTestLog("Running transaction while the migration is running");
const session = donorPrimary.startSession();
const sessionDb = session.getDatabase(tenantDB);

const [doc1, doc2, doc3] = [{_id: 1, a: 1}, {_id: 2, b: 2}, {_id: 3, c: 3}];

// Run a transaction that creates a collection.
session.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(sessionDb.createCollection(collName));
const sessionColl = sessionDb.getCollection(collName);
assert.commandWorked(sessionColl.insert(doc1));
assert.commandWorked(sessionColl.insert(doc2));
assert.commandWorked(sessionColl.insert(doc3));
assert.commandWorked(session.commitTransaction_forTesting());
session.endSession();

hangAfterStartingOplogApplier.off();

jsTestLog("Waiting for migration to complete");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

// Verify that all docs were successfully migrated onto the recipient.
assert.eq(1, recipientPrimary.getCollection(transactionsNS).find().itcount());
assert.eq(3, recipientPrimary.getCollection(tenantNS).find().itcount());
assertArrayEq([doc1, doc2, doc3], recipientPrimary.getCollection(tenantNS).find().toArray());

tenantMigrationTest.stop();
})();
