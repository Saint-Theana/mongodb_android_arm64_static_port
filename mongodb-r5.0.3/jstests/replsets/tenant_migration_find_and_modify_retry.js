/**
 * Tests a case where the retryable write oplog pre-fetch stage does not fetch the pre/post image
 * for a findAndModify operation because there is a newer txnNumber started in the same session when
 * the pre-fetch stage runs. As a newer transaction has started in the same session, the tenant
 * migration does not need to support retrying the findAndModify. We test that the tenant migration
 * can still succeed in this case.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}
const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "testColl";

tenantMigrationTest.insertDonorDB(kDbName, kCollName, [{x: 1}]);

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const hangBeforeRetrievingStartOpTime =
    configureFailPoint(recipientPrimary, "fpAfterComparingRecipientAndDonorFCV", {action: "hang"});
const hangAfterRetrievingStartOpTime = configureFailPoint(
    recipientPrimary, "fpAfterRetrievingStartOpTimesMigrationRecipientInstance", {action: "hang"});

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

hangBeforeRetrievingStartOpTime.wait();

const donorDB = tenantMigrationTest.getDonorPrimary().getDB(kDbName);

// Run a retryable findAndModify before the recipient gets the start fetching optime so that the
// recipient would start fetching oplog from this findAndModify's oplog entry and the post-image
// should be less than the startFetchingOpTime.
const lsid = {
    id: UUID()
};
let res = assert.commandWorked(donorDB.runCommand({
    findAndModify: kCollName,
    query: {x: 1},
    update: {$set: {y: 1}},
    new: true,
    txnNumber: NumberLong(0),
    lsid: lsid,
    writeConcern: {w: "majority"}
}));
jsTestLog("First findAndModify: " + tojson(res));

hangBeforeRetrievingStartOpTime.off();
hangAfterRetrievingStartOpTime.wait();

// Run another retryable findAndModify with a newer txnNumber using the same session before the
// recipient's retryable write pre-fetch stage. After this, the pre-fetch stage would not fetch the
// post-image of the last findAndModify because a newer txnNumber has started.
res = assert.commandWorked(donorDB.runCommand({
    findAndModify: kCollName,
    query: {x: 1},
    update: {$set: {y: 2}},
    new: true,
    txnNumber: NumberLong(1),
    lsid: lsid,
    writeConcern: {w: "majority"}
}));
jsTestLog("Second findAndModify: " + tojson(res));

// Resume the tenant migration.
hangAfterRetrievingStartOpTime.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

res = assert.commandWorked(recipientPrimary.getDB(kDbName).runCommand({
    findAndModify: kCollName,
    query: {x: 1},
    update: {$set: {y: 2}},
    new: true,
    txnNumber: NumberLong(1),
    lsid: lsid,
    writeConcern: {w: "majority"}
}));
jsTestLog("Retry findAndModify on recipient: " + tojson(res));
assert.eq(2, res.value.y);

tenantMigrationTest.stop();
})();
