/**
 * Tests collation with time-series collections.
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
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const timeFieldName = 'tm';
    const metaFieldName = 'mm';

    const metadata = ['8', '16', '64', '128'];

    const coll = db.timeseries_index_collation;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    coll.drop();  // implicitly drops bucketsColl.

    // Compares numeric strings as numbers rather than as strings.
    const numericOrdering = {locale: 'en_US', numericOrdering: true};

    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
        collation: numericOrdering
    }));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    for (let i = 0; i < metadata.length; ++i) {
        const doc = {_id: i, [timeFieldName]: ISODate(), [metaFieldName]: metadata[i]};
        jsTestLog('Inserting doc ' + i + ': ' + tojson(doc));
        assert.commandWorked(insert(coll, doc), 'failed to insert doc: ' + tojson(doc));
    }

    // Check bucket collection. We are using all unique metadata values so each measurement should
    // be in its own bucket.
    const bucketDocs = bucketsColl.find().toArray();
    assert.eq(metadata.length, bucketDocs.length, bucketDocs);

    // Create an index that should use the default collation, which is {numericOrdering: true}.
    const indexKey = {[metaFieldName]: 1};
    assert.commandWorked(coll.createIndex(indexKey, {name: 'index_numeric'}),
                         'failed to create index: ' + tojson(indexKey));

    const expectedNumericOrdering = metadata;
    const docsNumericOrdering = coll.find().hint('index_numeric').toArray();
    assert.eq(expectedNumericOrdering.length,
              docsNumericOrdering.length,
              'unexpected documents returned from index scan with numeric ordering: ' +
                  tojson(docsNumericOrdering) +
                  ' (expected metadata values: ' + tojson(expectedNumericOrdering) + ')');
    for (let i = 0; i < docsNumericOrdering.length; ++i) {
        assert.eq(expectedNumericOrdering[i],
                  docsNumericOrdering[i][metaFieldName],
                  'Unexpected metadata in doc ' + i +
                      ' (numeric ordering): ' + tojson(docsNumericOrdering));
    }

    // Create an index that does not use the default collation and compares numeric strings
    // lexicographically rather than as numbers.
    assert.commandWorked(
        coll.createIndex(
            indexKey, {name: 'index_string', collation: {locale: 'en_US', numericOrdering: false}}),
        'failed to create index: ' + tojson(indexKey));

    const expectedStringOrdering = ['128', '16', '64', '8'];
    const docsStringOrdering = coll.find().hint('index_string').toArray();
    assert.eq(expectedStringOrdering.length,
              docsStringOrdering.length,
              'unexpected documents returned from index scan with string ordering: ' +
                  tojson(docsStringOrdering));
    for (let i = 0; i < docsNumericOrdering.length; ++i) {
        assert.eq(expectedStringOrdering[i],
                  docsStringOrdering[i][metaFieldName],
                  'Unexpected metadata in doc ' + i +
                      ' (string ordering): ' + tojson(docsStringOrdering));
    }

    // Check that listIndexes returns specs with collation information.
    const indexSpecs = coll.getIndexes();
    assert.eq(2, indexSpecs.length, 'Unexpected index specs: ' + tojson(indexSpecs));
    for (let i = 0; i < indexSpecs.length; ++i) {
        if (indexSpecs[i].name === 'index_numeric') {
            assert.eq(true,
                      indexSpecs[i].collation.numericOrdering,
                      'Invalid index spec for index_numeric: ' + tojson(indexSpecs[i]));
        } else if (indexSpecs[i].name === 'index_string') {
            assert.eq(false,
                      indexSpecs[i].collation.numericOrdering,
                      'Invalid index spec for index_string: ' + tojson(indexSpecs[i]));
        } else {
            assert(false, 'Unexpected index spec: ' + tojson(indexSpecs[i]));
        }
    }
});
})();
