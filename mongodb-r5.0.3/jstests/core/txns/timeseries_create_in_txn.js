/**
 * Tests that it is illegal to create a time-series collection within a transaction.
 * @tags: [
 *     uses_transactions,
 *     requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const session = db.getMongo().startSession();
// Use a custom database, to avoid conflict with other tests that use the system.js collection.
session.startTransaction();
const sessionDB = session.getDatabase('test');
assert.commandFailedWithCode(
    sessionDB.createCollection('timeseries_create_in_txn', {timeseries: {timeField: 'time'}}),
    ErrorCodes.OperationNotSupportedInTransaction);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
})();
