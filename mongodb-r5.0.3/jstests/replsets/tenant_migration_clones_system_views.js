/**
 * Tests that tenant migrations correctly clone 'system.views' collections that belong to the
 * tenant.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    tenantMigrationTest.stop();
    return;
}

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = "testTenantId";
const tenantDBName = tenantMigrationTest.tenantDB(tenantId, "testDB");
const donorTenantDB = donorPrimary.getDB(tenantDBName);
const collName = "testColl";
const donorTenantColl = donorTenantDB.getCollection(collName);

const viewName = 'tenantView';
const doc1 = {
    _id: 1,
    a: 1
};
const doc2 = {
    _id: 2,
    b: 2
};

// Create a view on the tenant DB and insert documents into the tenant collection.
assert.commandWorked(donorTenantDB.createView(viewName, collName, [{$match: {a: 1}}]));
assert.commandWorked(donorTenantColl.insert([doc1, doc2]));
donorRst.awaitReplication();

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId,
};

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

// Verify that the view was cloned correctly.
const recipientView = recipientPrimary.getDB(tenantDBName)[viewName];

const findRes = recipientView.find().toArray();
assert.eq(1, findRes.length, `find result: ${tojson(findRes)}`);
assert.eq([doc1], findRes);

tenantMigrationTest.stop();
})();
