/**
 * Test that 'queryHash' and 'planCacheKey' from explain() output have sensible values
 * across catalog changes.
 */
(function() {
"use strict";
load('jstests/libs/fixture_helpers.js');  // For and isMongos().

const collName = "query_hash_stability";
const coll = db[collName];
coll.drop();
// Be sure the collection exists.
assert.commandWorked(coll.insert({x: 5}));

/**
 * Given two explain plans (firstExplain, secondExplain), this function makes assertions about their
 * 'planCacheField' values (in particular, whether they are 'expectedToMatch').
 */
let assertPlanCacheField = function(
    {firstExplain, secondExplain, planCacheField, expectedToMatch}) {
    let compareFn = function(first, second) {
        assert.eq(typeof (first), "string");
        assert.eq(typeof (second), "string");
        assert.eq(first === second,
                  expectedToMatch,
                  "Mismatch for field " + planCacheField + " when comparing " +
                      tojson(firstExplain) + " with " + tojson(secondExplain));
    };

    // SERVER-56980: When running in a sharded environment, we group the values for 'planCacheField'
    // by shard. This is because in a multi-version environment, we want to ensure that we are
    // comparing the results produced by the same shard in the event that the planCacheKey format
    // changed in between versions.
    if (FixtureHelpers.isMongos(db)) {
        let buildShardMap = function(shardedPlan) {
            let explainMap = {};
            for (const shard of shardedPlan.queryPlanner.winningPlan.shards) {
                explainMap[shard.shardName] = shard[planCacheField];
            }
            return explainMap;
        };

        const firstExplainMap = buildShardMap(firstExplain);
        const secondExplainMap = buildShardMap(secondExplain);

        // Should have the same number of elements.
        assert.eq(Object.keys(firstExplainMap).length,
                  Object.keys(secondExplainMap).length,
                  "Expected " + tojson(firstExplainMap) + " and " + tojson(secondExplainMap) +
                      " to have the same number of elements");

        // Match the values for 'planCacheField' for each shard.
        for (const shardName of Object.keys(firstExplainMap)) {
            const firstPlanCacheValue = firstExplainMap[shardName];
            const secondPlanCacheValue = secondExplainMap[shardName];
            compareFn(firstPlanCacheValue, secondPlanCacheValue);
        }
    } else {
        const first = firstExplain['queryPlanner'][planCacheField];
        const second = secondExplain['queryPlanner'][planCacheField];
        compareFn(first, second);
    }
};

const query = {
    x: 3
};

const initialExplain = coll.find(query).explain();

// Add a sparse index.
assert.commandWorked(coll.createIndex({x: 1}, {sparse: true}));

const withIndexExplain = coll.find(query).explain();

// 'queryHash' shouldn't change across catalog changes.
assertPlanCacheField({
    firstExplain: initialExplain,
    secondExplain: withIndexExplain,
    planCacheField: 'queryHash',
    expectedToMatch: true
});

// We added an index so the plan cache key changed.
assertPlanCacheField({
    firstExplain: initialExplain,
    secondExplain: withIndexExplain,
    planCacheField: 'planCacheKey',
    expectedToMatch: false
});

// Drop the index.
assert.commandWorked(coll.dropIndex({x: 1}));
const postDropExplain = coll.find(query).explain();

// 'queryHash' shouldn't change across catalog changes.
assertPlanCacheField({
    firstExplain: initialExplain,
    secondExplain: postDropExplain,
    planCacheField: 'queryHash',
    expectedToMatch: true
});

// The 'planCacheKey' should be the same as what it was before we dropped the index.
assertPlanCacheField({
    firstExplain: initialExplain,
    secondExplain: postDropExplain,
    planCacheField: 'planCacheKey',
    expectedToMatch: true
});
})();
