/**
 * Tests that tenant migrations are correctly filtering DBs by tenantId.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, incompatible_with_windows_tls,
 * incompatible_with_eft, incompatible_with_macos, requires_persistence]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const tenantIdPrefix = "tenantId";
const baseDBName = "testDB";
const collName = "testColl";

let currId = 0;
const makeBaseTenantId = () => {
    return `${tenantIdPrefix}${currId++}`;
};

const runTest = (baseTenantId, dbName, shouldMatch) => {
    jsTestLog(`Running tenant migration with dbName ${dbName} and tenantId ${baseTenantId}`);

    assert.eq(shouldMatch, TenantMigrationUtil.isNamespaceForTenant(baseTenantId, dbName));
    tenantMigrationTest.insertDonorDB(dbName, collName);

    // Run a migration with the base tenant ID.
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: baseTenantId,
    };
    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    // verifyRecipientDB calls isNamespaceForTenant() to determine if the data should have been
    // migrated, so we can directly call it here.
    tenantMigrationTest.verifyRecipientDB(baseTenantId, dbName, collName);
    tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString);
};

const testCases = [
    {makeTenantId: baseId => baseId, shouldMatch: true},
    {makeTenantId: baseId => `${baseId}_`, shouldMatch: true},
    {makeTenantId: baseId => `${baseId}_${baseId}`, shouldMatch: true},
    {makeTenantId: baseId => `a${baseId}`, shouldMatch: false},
    {makeTenantId: baseId => `${baseId}a`, shouldMatch: false},
    {makeTenantId: baseId => `^${baseId}`, shouldMatch: false},
    {makeTenantId: baseId => `${baseId.toUpperCase()}`, shouldMatch: false},
    {makeTenantId: baseId => `${baseId.toLowerCase()}`, shouldMatch: false},
    {makeTenantId: baseId => `${baseId}${baseId}`, shouldMatch: false},
];

for (const {makeTenantId, shouldMatch} of testCases) {
    const baseTenantId = makeBaseTenantId();
    const tenantId = makeTenantId(baseTenantId);
    runTest(baseTenantId, `${tenantId}_${baseDBName}`, shouldMatch);
}

tenantMigrationTest.stop();
})();
