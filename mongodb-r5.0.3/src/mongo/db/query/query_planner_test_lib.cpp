/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file contains tests for mongo/db/query/query_planner.cpp
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/db/query/query_planner_test_lib.h"

#include <ostream>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace {

using namespace mongo;

using std::string;


bool filterMatches(const BSONObj& testFilter,
                   const BSONObj& testCollation,
                   const QuerySolutionNode* trueFilterNode) {
    if (nullptr == trueFilterNode->filter) {
        return false;
    }

    std::unique_ptr<CollatorInterface> testCollator;
    if (!testCollation.isEmpty()) {
        CollatorFactoryMock collatorFactoryMock;
        auto collator = collatorFactoryMock.makeFromBSON(testCollation);
        if (!collator.isOK()) {
            return false;
        }
        testCollator = std::move(collator.getValue());
    }

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(testCollator));
    StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(testFilter, expCtx);
    if (!statusWithMatcher.isOK()) {
        return false;
    }
    const std::unique_ptr<MatchExpression> root = std::move(statusWithMatcher.getValue());
    MatchExpression::sortTree(root.get());
    std::unique_ptr<MatchExpression> trueFilter(trueFilterNode->filter->shallowClone());
    MatchExpression::sortTree(trueFilter.get());
    return trueFilter->equivalent(root.get());
}

void appendIntervalBound(BSONObjBuilder& bob, BSONElement& el) {
    if (el.type() == String) {
        std::string data = el.String();
        if (data == "MaxKey") {
            bob.appendMaxKey("");
        } else if (data == "MinKey") {
            bob.appendMinKey("");
        } else {
            bob.appendAs(el, "");
        }
    } else {
        bob.appendAs(el, "");
    }
}

bool intervalMatches(const BSONObj& testInt, const Interval trueInt) {
    BSONObjIterator it(testInt);
    if (!it.more()) {
        return false;
    }
    BSONElement low = it.next();
    if (!it.more()) {
        return false;
    }
    BSONElement high = it.next();
    if (!it.more()) {
        return false;
    }
    bool startInclusive = it.next().Bool();
    if (!it.more()) {
        return false;
    }
    bool endInclusive = it.next().Bool();
    if (it.more()) {
        return false;
    }

    BSONObjBuilder bob;
    appendIntervalBound(bob, low);
    appendIntervalBound(bob, high);
    Interval toCompare(bob.obj(), startInclusive, endInclusive);
    return trueInt.equals(toCompare);
}

bool bsonObjFieldsAreInSet(BSONObj obj, const std::set<std::string>& allowedFields) {
    BSONObjIterator i(obj);
    while (i.more()) {
        BSONElement child = i.next();
        if (!allowedFields.count(child.fieldName())) {
            LOGV2_ERROR(23932, "Unexpected field", "field"_attr = child.fieldName());
            return false;
        }
    }

    return true;
}

}  // namespace

namespace mongo {

/**
 * Looks in the children stored in the 'nodes' field of 'testSoln'
 * to see if thet match the 'children' field of 'trueSoln'.
 *
 * This does an unordered comparison, i.e. childrenMatch returns
 * true as long as the set of subtrees in testSoln's 'nodes' matches
 * the set of subtrees in trueSoln's 'children' vector.
 */
static bool childrenMatch(const BSONObj& testSoln,
                          const QuerySolutionNode* trueSoln,
                          bool relaxBoundsCheck) {
    BSONElement children = testSoln["nodes"];
    if (children.eoo() || !children.isABSONObj()) {
        return false;
    }

    // The order of the children array in testSoln might not match
    // the order in trueSoln, so we have to check all combos with
    // these nested loops.
    stdx::unordered_set<size_t> matchedNodeIndexes;
    BSONObjIterator i(children.Obj());
    while (i.more()) {
        BSONElement child = i.next();
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        // try to match against one of the QuerySolutionNode's children
        bool found = false;
        for (size_t j = 0; j < trueSoln->children.size(); ++j) {
            if (matchedNodeIndexes.find(j) != matchedNodeIndexes.end()) {
                // Do not match a child of the QuerySolutionNode more than once.
                continue;
            }
            if (QueryPlannerTestLib::solutionMatches(
                    child.Obj(), trueSoln->children[j], relaxBoundsCheck)) {
                found = true;
                matchedNodeIndexes.insert(j);
                break;
            }
        }

        // we couldn't match child
        if (!found) {
            return false;
        }
    }

    // Ensure we've matched all children of the QuerySolutionNode.
    return matchedNodeIndexes.size() == trueSoln->children.size();
}

bool QueryPlannerTestLib::boundsMatch(const BSONObj& testBounds,
                                      const IndexBounds trueBounds,
                                      bool relaxBoundsCheck) {
    // Iterate over the fields on which we have index bounds.
    BSONObjIterator fieldIt(testBounds);
    size_t fieldItCount = 0;
    while (fieldIt.more()) {
        BSONElement arrEl = fieldIt.next();
        if (arrEl.fieldNameStringData() != trueBounds.getFieldName(fieldItCount)) {
            return false;
        }
        if (arrEl.type() != Array) {
            return false;
        }
        // Iterate over an ordered interval list for
        // a particular field.
        BSONObjIterator oilIt(arrEl.Obj());
        size_t oilItCount = 0;
        while (oilIt.more()) {
            BSONElement intervalEl = oilIt.next();
            if (intervalEl.type() != Array) {
                return false;
            }
            Interval trueInt = trueBounds.getInterval(fieldItCount, oilItCount);
            if (!intervalMatches(intervalEl.Obj(), trueInt)) {
                return false;
            }
            ++oilItCount;
        }

        if (!relaxBoundsCheck && oilItCount != trueBounds.getNumIntervals(fieldItCount)) {
            return false;
        }

        ++fieldItCount;
    }

    return true;
}

// static
bool QueryPlannerTestLib::solutionMatches(const BSONObj& testSoln,
                                          const QuerySolutionNode* trueSoln,
                                          bool relaxBoundsCheck) {
    //
    // leaf nodes
    //
    if (STAGE_COLLSCAN == trueSoln->getType()) {
        const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(trueSoln);
        BSONElement el = testSoln["cscan"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj csObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(csObj, {"dir", "filter", "collation"}));

        BSONElement dir = csObj["dir"];
        if (dir.eoo() || !dir.isNumber()) {
            return false;
        }
        if (dir.numberInt() != csn->direction) {
            return false;
        }

        BSONElement filter = csObj["filter"];
        if (filter.eoo()) {
            return true;
        } else if (filter.isNull()) {
            return nullptr == csn->filter;
        } else if (!filter.isABSONObj()) {
            return false;
        }

        BSONObj collation;
        if (BSONElement collationElt = csObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        return filterMatches(filter.Obj(), collation, trueSoln);
    } else if (STAGE_IXSCAN == trueSoln->getType()) {
        const IndexScanNode* ixn = static_cast<const IndexScanNode*>(trueSoln);
        BSONElement el = testSoln["ixscan"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj ixscanObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(
            ixscanObj, {"pattern", "name", "bounds", "dir", "filter", "collation"}));

        BSONElement pattern = ixscanObj["pattern"];
        if (!pattern.eoo()) {
            if (!pattern.isABSONObj()) {
                return false;
            }
            if (SimpleBSONObjComparator::kInstance.evaluate(pattern.Obj() !=
                                                            ixn->index.keyPattern)) {
                return false;
            }
        }

        BSONElement name = ixscanObj["name"];
        if (!name.eoo()) {
            if (name.type() != BSONType::String) {
                return false;
            }
            if (name.valueStringData() != ixn->index.identifier.catalogName) {
                return false;
            }
        }

        if (name.eoo() && pattern.eoo()) {
            return false;
        }

        BSONElement bounds = ixscanObj["bounds"];
        if (!bounds.eoo()) {
            if (!bounds.isABSONObj()) {
                return false;
            } else if (!boundsMatch(bounds.Obj(), ixn->bounds, relaxBoundsCheck)) {
                return false;
            }
        }

        BSONElement dir = ixscanObj["dir"];
        if (!dir.eoo() && NumberInt == dir.type()) {
            if (dir.numberInt() != ixn->direction) {
                return false;
            }
        }

        BSONElement filter = ixscanObj["filter"];
        if (filter.eoo()) {
            return true;
        } else if (filter.isNull()) {
            return nullptr == ixn->filter;
        } else if (!filter.isABSONObj()) {
            return false;
        }

        BSONObj collation;
        if (BSONElement collationElt = ixscanObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        return filterMatches(filter.Obj(), collation, trueSoln);
    } else if (STAGE_GEO_NEAR_2D == trueSoln->getType()) {
        const GeoNear2DNode* node = static_cast<const GeoNear2DNode*>(trueSoln);
        BSONElement el = testSoln["geoNear2d"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj geoObj = el.Obj();
        return SimpleBSONObjComparator::kInstance.evaluate(geoObj == node->index.keyPattern);
    } else if (STAGE_GEO_NEAR_2DSPHERE == trueSoln->getType()) {
        const GeoNear2DSphereNode* node = static_cast<const GeoNear2DSphereNode*>(trueSoln);
        BSONElement el = testSoln["geoNear2dsphere"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj geoObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(geoObj, {"pattern", "bounds"}));

        BSONElement pattern = geoObj["pattern"];
        if (pattern.eoo() || !pattern.isABSONObj()) {
            return false;
        }
        if (SimpleBSONObjComparator::kInstance.evaluate(pattern.Obj() != node->index.keyPattern)) {
            return false;
        }

        BSONElement bounds = geoObj["bounds"];
        if (!bounds.eoo()) {
            if (!bounds.isABSONObj()) {
                return false;
            } else if (!boundsMatch(bounds.Obj(), node->baseBounds, relaxBoundsCheck)) {
                return false;
            }
        }

        return true;
    } else if (STAGE_TEXT_MATCH == trueSoln->getType()) {
        // {text: {search: "somestr", language: "something", filter: {blah: 1}}}
        const TextMatchNode* node = static_cast<const TextMatchNode*>(trueSoln);
        BSONElement el = testSoln["text"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj textObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(textObj,
                                        {"text",
                                         "search",
                                         "language",
                                         "caseSensitive",
                                         "diacriticSensitive",
                                         "prefix",
                                         "collation",
                                         "filter"}));

        BSONElement searchElt = textObj["search"];
        if (!searchElt.eoo()) {
            if (searchElt.String() != node->ftsQuery->getQuery()) {
                return false;
            }
        }

        BSONElement languageElt = textObj["language"];
        if (!languageElt.eoo()) {
            if (languageElt.String() != node->ftsQuery->getLanguage()) {
                return false;
            }
        }

        BSONElement caseSensitiveElt = textObj["caseSensitive"];
        if (!caseSensitiveElt.eoo()) {
            if (caseSensitiveElt.trueValue() != node->ftsQuery->getCaseSensitive()) {
                return false;
            }
        }

        BSONElement diacriticSensitiveElt = textObj["diacriticSensitive"];
        if (!diacriticSensitiveElt.eoo()) {
            if (diacriticSensitiveElt.trueValue() != node->ftsQuery->getDiacriticSensitive()) {
                return false;
            }
        }

        BSONElement indexPrefix = textObj["prefix"];
        if (!indexPrefix.eoo()) {
            if (!indexPrefix.isABSONObj()) {
                return false;
            }

            if (0 != indexPrefix.Obj().woCompare(node->indexPrefix)) {
                return false;
            }
        }

        BSONObj collation;
        if (BSONElement collationElt = textObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = textObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (nullptr != node->filter) {
                    return false;
                }
            } else if (!filter.isABSONObj()) {
                return false;
            } else if (!filterMatches(filter.Obj(), collation, trueSoln)) {
                return false;
            }
        }

        return true;
    }

    //
    // internal nodes
    //
    if (STAGE_FETCH == trueSoln->getType()) {
        const FetchNode* fn = static_cast<const FetchNode*>(trueSoln);

        BSONElement el = testSoln["fetch"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj fetchObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(fetchObj, {"collation", "filter", "node"}));

        BSONObj collation;
        if (BSONElement collationElt = fetchObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = fetchObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (nullptr != fn->filter) {
                    return false;
                }
            } else if (!filter.isABSONObj()) {
                return false;
            } else if (!filterMatches(filter.Obj(), collation, trueSoln)) {
                return false;
            }
        }

        BSONElement child = fetchObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }
        return solutionMatches(child.Obj(), fn->children[0], relaxBoundsCheck);
    } else if (STAGE_OR == trueSoln->getType()) {
        const OrNode* orn = static_cast<const OrNode*>(trueSoln);
        BSONElement el = testSoln["or"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj orObj = el.Obj();
        return childrenMatch(orObj, orn, relaxBoundsCheck);
    } else if (STAGE_AND_HASH == trueSoln->getType()) {
        const AndHashNode* ahn = static_cast<const AndHashNode*>(trueSoln);
        BSONElement el = testSoln["andHash"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj andHashObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(andHashObj, {"collation", "filter", "nodes"}));

        BSONObj collation;
        if (BSONElement collationElt = andHashObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = andHashObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (nullptr != ahn->filter) {
                    return false;
                }
            } else if (!filter.isABSONObj()) {
                return false;
            } else if (!filterMatches(filter.Obj(), collation, trueSoln)) {
                return false;
            }
        }

        return childrenMatch(andHashObj, ahn, relaxBoundsCheck);
    } else if (STAGE_AND_SORTED == trueSoln->getType()) {
        const AndSortedNode* asn = static_cast<const AndSortedNode*>(trueSoln);
        BSONElement el = testSoln["andSorted"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj andSortedObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(andSortedObj, {"collation", "filter", "nodes"}));

        BSONObj collation;
        if (BSONElement collationElt = andSortedObj["collation"]) {
            if (!collationElt.isABSONObj()) {
                return false;
            }
            collation = collationElt.Obj();
        }

        BSONElement filter = andSortedObj["filter"];
        if (!filter.eoo()) {
            if (filter.isNull()) {
                if (nullptr != asn->filter) {
                    return false;
                }
            } else if (!filter.isABSONObj()) {
                return false;
            } else if (!filterMatches(filter.Obj(), collation, trueSoln)) {
                return false;
            }
        }

        return childrenMatch(andSortedObj, asn, relaxBoundsCheck);
    } else if (isProjectionStageType(trueSoln->getType())) {
        const ProjectionNode* pn = static_cast<const ProjectionNode*>(trueSoln);

        BSONElement el = testSoln["proj"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj projObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(projObj, {"type", "spec", "node"}));

        BSONElement projType = projObj["type"];
        if (!projType.eoo()) {
            string projTypeStr = projType.str();
            switch (pn->getType()) {
                case StageType::STAGE_PROJECTION_DEFAULT:
                    if (projTypeStr != "default")
                        return false;
                    break;
                case StageType::STAGE_PROJECTION_COVERED:
                    if (projTypeStr != "coveredIndex")
                        return false;
                    break;
                case StageType::STAGE_PROJECTION_SIMPLE:
                    if (projTypeStr != "simple")
                        return false;
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }

        BSONElement spec = projObj["spec"];
        if (spec.eoo() || !spec.isABSONObj()) {
            return false;
        }
        BSONElement child = projObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        // Create an empty/dummy expression context without access to the operation context and
        // collator. This should be sufficient to parse a projection.
        auto expCtx =
            make_intrusive<ExpressionContext>(nullptr, nullptr, NamespaceString("test.dummy"));
        auto projection =
            projection_ast::parse(expCtx, spec.Obj(), ProjectionPolicies::findProjectionPolicies());
        auto specProjObj = projection_ast::astToDebugBSON(projection.root());
        auto solnProjObj = projection_ast::astToDebugBSON(pn->proj.root());
        return SimpleBSONObjComparator::kInstance.evaluate(specProjObj == solnProjObj) &&
            solutionMatches(child.Obj(), pn->children[0], relaxBoundsCheck);
    } else if (isSortStageType(trueSoln->getType())) {
        const SortNode* sn = static_cast<const SortNode*>(trueSoln);
        BSONElement el = testSoln["sort"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj sortObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(sortObj, {"pattern", "limit", "type", "node"}));

        BSONElement patternEl = sortObj["pattern"];
        if (patternEl.eoo() || !patternEl.isABSONObj()) {
            return false;
        }
        BSONElement limitEl = sortObj["limit"];
        if (!limitEl.isNumber()) {
            return false;
        }

        BSONElement sortType = sortObj["type"];
        if (sortType) {
            if (sortType.type() != BSONType::String) {
                return false;
            }

            auto sortTypeString = sortType.valueStringData();
            switch (sn->getType()) {
                case StageType::STAGE_SORT_DEFAULT: {
                    if (sortTypeString != "default") {
                        return false;
                    }
                    break;
                }
                case StageType::STAGE_SORT_SIMPLE: {
                    if (sortTypeString != "simple") {
                        return false;
                    }
                    break;
                }
                default: { MONGO_UNREACHABLE; }
            }
        }

        BSONElement child = sortObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        size_t expectedLimit = limitEl.numberInt();
        return SimpleBSONObjComparator::kInstance.evaluate(patternEl.Obj() == sn->pattern) &&
            (expectedLimit == sn->limit) &&
            solutionMatches(child.Obj(), sn->children[0], relaxBoundsCheck);
    } else if (STAGE_SORT_KEY_GENERATOR == trueSoln->getType()) {
        const SortKeyGeneratorNode* keyGenNode = static_cast<const SortKeyGeneratorNode*>(trueSoln);
        BSONElement el = testSoln["sortKeyGen"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj keyGenObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(keyGenObj, {"node"}));

        BSONElement child = keyGenObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return solutionMatches(child.Obj(), keyGenNode->children[0], relaxBoundsCheck);
    } else if (STAGE_SORT_MERGE == trueSoln->getType()) {
        const MergeSortNode* msn = static_cast<const MergeSortNode*>(trueSoln);
        BSONElement el = testSoln["mergeSort"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj mergeSortObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(mergeSortObj, {"nodes"}));
        return childrenMatch(mergeSortObj, msn, relaxBoundsCheck);
    } else if (STAGE_SKIP == trueSoln->getType()) {
        const SkipNode* sn = static_cast<const SkipNode*>(trueSoln);
        BSONElement el = testSoln["skip"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj skipObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(skipObj, {"n", "node"}));

        BSONElement skipEl = skipObj["n"];
        if (!skipEl.isNumber()) {
            return false;
        }
        BSONElement child = skipObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return (skipEl.numberInt() == sn->skip) &&
            solutionMatches(child.Obj(), sn->children[0], relaxBoundsCheck);
    } else if (STAGE_LIMIT == trueSoln->getType()) {
        const LimitNode* ln = static_cast<const LimitNode*>(trueSoln);
        BSONElement el = testSoln["limit"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj limitObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(limitObj, {"n", "node"}));

        BSONElement limitEl = limitObj["n"];
        if (!limitEl.isNumber()) {
            return false;
        }
        BSONElement child = limitObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return (limitEl.numberInt() == ln->limit) &&
            solutionMatches(child.Obj(), ln->children[0], relaxBoundsCheck);
    } else if (STAGE_SHARDING_FILTER == trueSoln->getType()) {
        const ShardingFilterNode* fn = static_cast<const ShardingFilterNode*>(trueSoln);

        BSONElement el = testSoln["sharding_filter"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj keepObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(keepObj, {"node"}));

        BSONElement child = keepObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return solutionMatches(child.Obj(), fn->children[0], relaxBoundsCheck);
    } else if (STAGE_ENSURE_SORTED == trueSoln->getType()) {
        const EnsureSortedNode* esn = static_cast<const EnsureSortedNode*>(trueSoln);

        BSONElement el = testSoln["ensureSorted"];
        if (el.eoo() || !el.isABSONObj()) {
            return false;
        }
        BSONObj esObj = el.Obj();
        invariant(bsonObjFieldsAreInSet(esObj, {"node", "pattern"}));

        BSONElement patternEl = esObj["pattern"];
        if (patternEl.eoo() || !patternEl.isABSONObj()) {
            return false;
        }
        BSONElement child = esObj["node"];
        if (child.eoo() || !child.isABSONObj()) {
            return false;
        }

        return SimpleBSONObjComparator::kInstance.evaluate(patternEl.Obj() == esn->pattern) &&
            solutionMatches(child.Obj(), esn->children[0], relaxBoundsCheck);
    }

    return false;
}

}  // namespace mongo
