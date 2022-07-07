/**
 * Tests that a time-series collection handles a bucket being manually removed from the buckets
 * collection.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');

TimeseriesTest.run((insert) => {
    const coll = db.timeseries_bucket_manual_removal;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    const timeFieldName = 'time';

    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    const docs1 = [
        {
            _id: 0,
            [timeFieldName]: ISODate("2021-01-01T01:00:00Z"),
        },
        {
            _id: 1,
            [timeFieldName]: ISODate("2021-01-01T01:01:00Z"),
        },
    ];
    const docs2 = [
        {
            _id: 2,
            [timeFieldName]: ISODate("2021-01-01T01:02:00Z"),
        },
        {
            _id: 3,
            [timeFieldName]: ISODate("2021-01-01T01:03:00Z"),
        },
    ];

    assert.commandWorked(insert(coll, docs1));
    assert.docEq(coll.find().toArray(), docs1);
    let buckets = bucketsColl.find().toArray();
    assert.eq(buckets.length, 1, 'Expected one bucket but found ' + tojson(buckets));
    const bucketId = buckets[0]._id;

    assert.commandWorked(bucketsColl.remove({_id: bucketId}));
    assert.docEq(coll.find().toArray(), []);
    buckets = bucketsColl.find().toArray();
    assert.eq(buckets.length, 0, 'Expected no buckets but found ' + tojson(buckets));

    assert.commandWorked(bucketsColl.remove({_id: bucketId}));
    assert.docEq(coll.find().toArray(), []);
    buckets = bucketsColl.find().toArray();
    assert.eq(buckets.length, 0, 'Expected no buckets but found ' + tojson(buckets));

    assert.commandWorked(coll.insert(docs2, {ordered: false}));
    assert.docEq(coll.find().toArray(), docs2);
    buckets = bucketsColl.find().toArray();
    assert.eq(buckets.length, 1, 'Expected one bucket but found ' + tojson(buckets));
    assert.neq(buckets[0]._id, bucketId);
});
})();
