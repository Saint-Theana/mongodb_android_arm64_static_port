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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/db/query/planner_analysis.h"

#include <set>
#include <vector>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/logv2/log.h"

namespace mongo {

using std::endl;
using std::string;
using std::unique_ptr;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

//
// Helpers for bounds explosion AKA quick-and-dirty SERVER-1205.
//

namespace {

/**
 * Walk the tree 'root' and output all leaf nodes into 'leafNodes'.
 */
void getLeafNodes(QuerySolutionNode* root, vector<QuerySolutionNode*>* leafNodes) {
    if (0 == root->children.size()) {
        leafNodes->push_back(root);
    } else {
        for (size_t i = 0; i < root->children.size(); ++i) {
            getLeafNodes(root->children[i], leafNodes);
        }
    }
}

/**
 * Determines if the query solution node 'node' is a FETCH node with an IXSCAN child node.
 */
bool isFetchNodeWithIndexScanChild(const QuerySolutionNode* node) {
    return (STAGE_FETCH == node->getType() && node->children.size() == 1 &&
            STAGE_IXSCAN == node->children[0]->getType());
}

/**
 * Walks the tree 'root' and outputs all nodes that can be considered for explosion for sort.
 * Outputs FETCH nodes with an IXSCAN node as a child as well as singular IXSCAN leaves without a
 * FETCH as a parent into 'explodableNodes'.
 */
void getExplodableNodes(QuerySolutionNode* root, vector<QuerySolutionNode*>* explodableNodes) {
    if (STAGE_IXSCAN == root->getType() || isFetchNodeWithIndexScanChild(root)) {
        explodableNodes->push_back(root);
    } else {
        for (auto&& childNode : root->children) {
            getExplodableNodes(childNode, explodableNodes);
        }
    }
}

/**
 * Returns the IXSCAN node from the tree 'node' that can be either a IXSCAN node or a FETCH node
 * with an IXSCAN node as a child.
 */
const IndexScanNode* getIndexScanNode(const QuerySolutionNode* node) {
    if (STAGE_IXSCAN == node->getType()) {
        return static_cast<const IndexScanNode*>(node);
    } else if (isFetchNodeWithIndexScanChild(node)) {
        return static_cast<const IndexScanNode*>(node->children[0]);
    }
    MONGO_UNREACHABLE;
    return nullptr;
}

/**
 * Returns true if every interval in 'oil' is a point, false otherwise.
 */
bool isUnionOfPoints(const OrderedIntervalList& oil) {
    // We can't explode if there are empty bounds. Don't consider the
    // oil a union of points if there are no intervals.
    if (0 == oil.intervals.size()) {
        return false;
    }

    for (size_t i = 0; i < oil.intervals.size(); ++i) {
        if (!oil.intervals[i].isPoint()) {
            return false;
        }
    }

    return true;
}

/**
 * Should we try to expand the index scan(s) in 'solnRoot' to pull out an indexed sort?
 *
 * Returns the node which should be replaced by the merge sort of exploded scans
 * in the out-parameter 'toReplace'.
 */
bool structureOKForExplode(QuerySolutionNode* solnRoot, QuerySolutionNode** toReplace) {
    // For now we only explode if we *know* we will pull the sort out.  We can look at
    // more structure (or just explode and recalculate properties and see what happens)
    // but for now we just explode if it's a sure bet.
    //
    // TODO: Can also try exploding if root is AND_HASH (last child dictates order.),
    // or other less obvious cases...

    // Skip over a sharding filter stage.
    if (STAGE_SHARDING_FILTER == solnRoot->getType()) {
        solnRoot = solnRoot->children[0];
    }

    if (STAGE_IXSCAN == solnRoot->getType()) {
        *toReplace = solnRoot;
        return true;
    }

    if (isFetchNodeWithIndexScanChild(solnRoot)) {
        *toReplace = solnRoot->children[0];
        return true;
    }

    // If we have a STAGE_OR, we can explode only when all children are either IXSCANs or FETCHes
    // that have an IXSCAN as a child.
    if (STAGE_OR == solnRoot->getType()) {
        for (auto&& child : solnRoot->children) {
            if (STAGE_IXSCAN != child->getType() && !isFetchNodeWithIndexScanChild(child)) {
                return false;
            }
        }
        *toReplace = solnRoot;
        return true;
    }

    return false;
}

// vectors of vectors can be > > annoying.
typedef vector<Interval> PointPrefix;

/**
 * The first 'fieldsToExplode' fields of 'bounds' are points.  Compute the Cartesian product
 * of those fields and place it in 'prefixOut'.
 */
void makeCartesianProduct(const IndexBounds& bounds,
                          size_t fieldsToExplode,
                          vector<PointPrefix>* prefixOut) {
    vector<PointPrefix> prefixForScans;

    // We dump the Cartesian product of bounds into prefixForScans, starting w/the first
    // field's points.
    verify(fieldsToExplode >= 1);
    const OrderedIntervalList& firstOil = bounds.fields[0];
    verify(firstOil.intervals.size() >= 1);
    for (size_t i = 0; i < firstOil.intervals.size(); ++i) {
        const Interval& ival = firstOil.intervals[i];
        verify(ival.isPoint());
        PointPrefix pfix;
        pfix.push_back(ival);
        prefixForScans.push_back(pfix);
    }

    // For each subsequent field...
    for (size_t i = 1; i < fieldsToExplode; ++i) {
        vector<PointPrefix> newPrefixForScans;
        const OrderedIntervalList& oil = bounds.fields[i];
        verify(oil.intervals.size() >= 1);
        // For each point interval in that field (all ivals must be points)...
        for (size_t j = 0; j < oil.intervals.size(); ++j) {
            const Interval& ival = oil.intervals[j];
            verify(ival.isPoint());
            // Make a new scan by appending it to all scans in prefixForScans.
            for (size_t k = 0; k < prefixForScans.size(); ++k) {
                PointPrefix pfix = prefixForScans[k];
                pfix.push_back(ival);
                newPrefixForScans.push_back(pfix);
            }
        }
        // And update prefixForScans.
        newPrefixForScans.swap(prefixForScans);
    }

    prefixOut->swap(prefixForScans);
}

/**
 * Takes the provided 'node', either an IndexScanNode or FetchNode with a direct child that is an
 * IndexScanNode. Returns a list of nodes which are logically equivalent to 'node' if joined by a
 * MergeSort through the out-parameter 'explosionResult'. These nodes are owned by the caller.
 *
 * fieldsToExplode is a count of how many fields in the scan's bounds are the union of point
 * intervals.  This is computed beforehand and provided as a small optimization.
 *
 * Example:
 *
 * For the query find({a: {$in: [1,2]}}).sort({b: 1}) using the index {a:1, b:1}:
 * 'node' will be a scan with multi-interval bounds a: [[1, 1], [2, 2]], b: [MinKey, MaxKey]
 * 'sort' will be {b: 1}
 * 'fieldsToExplode' will be 1 (as only one field isUnionOfPoints).
 *
 * On return, 'explosionResult' will contain the following two scans:
 * a: [[1, 1]], b: [MinKey, MaxKey]
 * a: [[2, 2]], b: [MinKey, MaxKey]
 */
void explodeNode(const QuerySolutionNode* node,
                 const BSONObj& sort,
                 size_t fieldsToExplode,
                 vector<QuerySolutionNode*>* explosionResult) {
    // Get the 'isn' from either the FetchNode or IndexScanNode.
    const IndexScanNode* isn = getIndexScanNode(node);

    // Turn the compact bounds in 'isn' into a bunch of points...
    vector<PointPrefix> prefixForScans;
    makeCartesianProduct(isn->bounds, fieldsToExplode, &prefixForScans);

    for (size_t i = 0; i < prefixForScans.size(); ++i) {
        const PointPrefix& prefix = prefixForScans[i];
        verify(prefix.size() == fieldsToExplode);

        // Copy boring fields into new child.
        IndexScanNode* child = new IndexScanNode(isn->index);
        child->direction = isn->direction;
        child->addKeyMetadata = isn->addKeyMetadata;
        child->queryCollator = isn->queryCollator;

        // Copy the filter, if there is one.
        if (isn->filter.get()) {
            child->filter = isn->filter->shallowClone();
        }

        // Create child bounds.
        child->bounds.fields.resize(isn->bounds.fields.size());
        for (size_t j = 0; j < fieldsToExplode; ++j) {
            child->bounds.fields[j].intervals.push_back(prefix[j]);
            child->bounds.fields[j].name = isn->bounds.fields[j].name;
        }
        for (size_t j = fieldsToExplode; j < isn->bounds.fields.size(); ++j) {
            child->bounds.fields[j] = isn->bounds.fields[j];
        }

        // If the explosion is on a FetchNode, make a copy and add the 'isn' as a child.
        if (STAGE_FETCH == node->getType()) {
            auto origFetchNode = static_cast<const FetchNode*>(node);
            auto newFetchNode = std::make_unique<FetchNode>();

            // Copy the FETCH's filter, if it exists.
            if (origFetchNode->filter.get()) {
                newFetchNode->filter = origFetchNode->filter->shallowClone();
            }

            // Add the 'child' IXSCAN under the FETCH stage, and the FETCH stage to the result set.
            newFetchNode->children.push_back(child);
            explosionResult->push_back(newFetchNode.release());
        } else {
            explosionResult->push_back(child);
        }
    }
}

/**
 * In the tree '*root', replace 'oldNode' with 'newNode'.
 */
void replaceNodeInTree(QuerySolutionNode** root,
                       QuerySolutionNode* oldNode,
                       QuerySolutionNode* newNode) {
    if (*root == oldNode) {
        *root = newNode;
    } else {
        for (size_t i = 0; i < (*root)->children.size(); ++i) {
            replaceNodeInTree(&(*root)->children[i], oldNode, newNode);
        }
    }
}

void geoSkipValidationOn(const std::set<StringData>& twoDSphereFields,
                         QuerySolutionNode* solnRoot) {
    // If there is a GeoMatchExpression in the tree on a field with a 2dsphere index,
    // we can skip validation since it was validated on insertion. This only applies to
    // 2dsphere index version >= 3.
    //
    // This does not mean that there is necessarily an IXSCAN using this 2dsphere index,
    // only that there exists a 2dsphere index on this field.
    MatchExpression* expr = solnRoot->filter.get();
    if (expr) {
        StringData nodeField = expr->path();
        if (expr->matchType() == MatchExpression::GEO &&
            twoDSphereFields.find(nodeField) != twoDSphereFields.end()) {
            GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(expr);
            gme->setCanSkipValidation(true);
        }
    }

    for (QuerySolutionNode* child : solnRoot->children) {
        geoSkipValidationOn(twoDSphereFields, child);
    }
}

/**
 * If any field is missing from the list of fields the projection wants, we are not covered.
 */
auto providesAllFields(const vector<std::string>& fields, const QuerySolutionNode& solnRoot) {
    for (size_t i = 0; i < fields.size(); ++i) {
        if (!solnRoot.hasField(fields[i]))
            return false;
    }
    return true;
}

/**
 * If 'solnRoot' is returning index key data from a single index, returns the associated index key
 * pattern. Otherwise, returns an empty object.
 */
auto produceCoveredKeyObj(QuerySolutionNode* solnRoot) {
    vector<QuerySolutionNode*> leafNodes;
    getLeafNodes(solnRoot, &leafNodes);

    // Both the IXSCAN and DISTINCT stages provide covered key data.
    if (1 == leafNodes.size()) {
        if (STAGE_IXSCAN == leafNodes[0]->getType()) {
            IndexScanNode* ixn = static_cast<IndexScanNode*>(leafNodes[0]);
            return ixn->index.keyPattern;
        } else if (STAGE_DISTINCT_SCAN == leafNodes[0]->getType()) {
            DistinctNode* dn = static_cast<DistinctNode*>(leafNodes[0]);
            return dn->index.keyPattern;
        }
    }
    return BSONObj();
}

/**
 * Adds a stage to generate the sort key metadata if there's no sort stage but we have a sortKey
 * meta-projection.
 */
std::unique_ptr<QuerySolutionNode> addSortKeyGeneratorStageIfNeeded(
    const CanonicalQuery& query, bool hasSortStage, std::unique_ptr<QuerySolutionNode> solnRoot) {
    if (!hasSortStage && query.metadataDeps()[DocumentMetadataFields::kSortKey]) {
        auto keyGenNode = std::make_unique<SortKeyGeneratorNode>();
        keyGenNode->sortSpec = query.getFindCommandRequest().getSort();
        keyGenNode->children.push_back(solnRoot.release());
        return keyGenNode;
    }
    return solnRoot;
}

/**
 * When projection needs to be added to the solution tree, this function chooses between the default
 * implementation and one of the fast paths.
 */
std::unique_ptr<ProjectionNode> analyzeProjection(const CanonicalQuery& query,
                                                  std::unique_ptr<QuerySolutionNode> solnRoot,
                                                  const bool hasSortStage) {
    LOGV2_DEBUG(20949, 5, "PROJECTION: Current plan", "plan"_attr = redact(solnRoot->toString()));

    // If the projection requires the entire document we add a fetch stage if not present. Otherwise
    // we add a fetch stage if we are not covered.
    if (!solnRoot->fetched() &&
        (query.getProj()->requiresDocument() ||
         (!providesAllFields(query.getProj()->getRequiredFields(), *solnRoot)))) {
        auto fetch = std::make_unique<FetchNode>();
        fetch->children.push_back(solnRoot.release());
        solnRoot = std::move(fetch);
    }

    // There are two projection fast paths available for simple inclusion projections that don't
    // need a sort key, don't have any dotted-path inclusions, don't have a positional projection,
    // and don't have the 'requiresDocument' property: the ProjectionNodeSimple fast-path for plans
    // that have a fetch stage and the ProjectionNodeCovered for plans with an index scan that the
    // projection can cover. Plans that don't meet all the requirements for these fast path
    // projections will all use ProjectionNodeDefault, which is able to handle all projections,
    // covered or otherwise.
    if (query.getProj()->isSimple()) {
        // If the projection is simple, but not covered, use 'ProjectionNodeSimple'.
        if (solnRoot->fetched()) {
            return std::make_unique<ProjectionNodeSimple>(
                addSortKeyGeneratorStageIfNeeded(query, hasSortStage, std::move(solnRoot)),
                *query.root(),
                *query.getProj());
        } else {
            // If we're here we're not fetched so we're covered. Let's see if we can get out of
            // using the default projType. If 'solnRoot' is an index scan we can use the faster
            // covered impl.
            BSONObj coveredKeyObj = produceCoveredKeyObj(solnRoot.get());
            if (!coveredKeyObj.isEmpty()) {
                return std::make_unique<ProjectionNodeCovered>(
                    addSortKeyGeneratorStageIfNeeded(query, hasSortStage, std::move(solnRoot)),
                    *query.root(),
                    *query.getProj(),
                    std::move(coveredKeyObj));
            }
        }
    }

    return std::make_unique<ProjectionNodeDefault>(
        addSortKeyGeneratorStageIfNeeded(query, hasSortStage, std::move(solnRoot)),
        *query.root(),
        *query.getProj());
}

/**
 * Given the solution tree 'root', attempts to push a projection at the root of the tree beneath a
 * SORT node. Returns the tree with this optimization applied, or the unmodified tree if the
 * optimization was not legal.
 *
 * Applying the projection before the sort is beneficial when it reduces the amount of data that
 * needs to be sorted.
 */
std::unique_ptr<QuerySolutionNode> tryPushdownProjectBeneathSort(
    std::unique_ptr<QuerySolutionNode> root) {
    if (!isProjectionStageType(root->getType())) {
        // There's no projection to push down.
        return root;
    }

    auto projectNode = static_cast<ProjectionNode*>(root.get());
    if (projectNode->proj.hasExpressions()) {
        // If the projection has any expressions, then we refrain from moving it underneath the
        // sort. It's possible that the addition of computed fields increases the size of the data
        // to sort, in which case it would be better to sort first and then project.
        return root;
    }

    // There could be a situation when there is a SKIP stage between PROJECT and SORT:
    //   PROJECT => SKIP => SORT
    // In this case we still want to push PROJECT beneath SORT.
    bool hasSkipBetween = false;
    auto sortNodeCandidate = projectNode->children[0];
    if (sortNodeCandidate->getType() == STAGE_SKIP) {
        hasSkipBetween = true;
        sortNodeCandidate = sortNodeCandidate->children[0];
    }

    if (!isSortStageType(sortNodeCandidate->getType())) {
        return root;
    }

    auto sortNode = static_cast<SortNode*>(sortNodeCandidate);

    // Don't perform this optimization if the sort is a top-k sort. We would be wasting work
    // computing projections for documents that are discarded since they are not in the top-k set.
    if (sortNode->limit > 0) {
        return root;
    }

    // It is only legal to push down the projection it if preserves all of the fields on which we
    // need to sort.
    for (auto&& sortComponent : sortNode->pattern) {
        if (!projectNode->hasField(sortComponent.fieldNameStringData().toString())) {
            return root;
        }
    }

    // Perform the swap. We are starting with the following structure:
    //   PROJECT => SORT => CHILD
    // Or if there is a SKIP stage between PROJECT and SORT:
    //   PROJECT => SKIP => SORT => CHILD
    //
    // This needs to be transformed to the following:
    //   SORT => PROJECT => CHILD
    // Or to the following in case of SKIP:
    //   SKIP => SORT => PROJECT => CHILD
    //
    // First, detach the bottom of the tree. This part is CHILD in the comment above.
    std::unique_ptr<QuerySolutionNode> restOfTree{sortNode->children[0]};
    invariant(sortNode->children.size() == 1u);
    sortNode->children.clear();

    // Next, detach the input from the projection and assume ownership of it.
    // The projection input is either this structure:
    //   SORT
    // Or this if we have SKIP:
    //   SKIP => SORT
    std::unique_ptr<QuerySolutionNode> ownedProjectionInput{projectNode->children[0]};
    sortNode = nullptr;
    invariant(projectNode->children.size() == 1u);
    projectNode->children.clear();

    // Attach the lower part of the tree as the child of the projection.
    // We want to get the following structure:
    //   PROJECT => CHILD
    std::unique_ptr<QuerySolutionNode> ownedProjectionNode = std::move(root);
    ownedProjectionNode->children.push_back(restOfTree.release());

    // Attach the projection as the child of the sort stage.
    if (hasSkipBetween) {
        // In this case 'ownedProjectionInput' points to the structure:
        //   SKIP => SORT
        // And to attach PROJECT => CHILD to it, we need to access children of SORT stage.
        ownedProjectionInput->children[0]->children.push_back(ownedProjectionNode.release());
    } else {
        // In this case 'ownedProjectionInput' points to the structure:
        //   SORT
        // And we can just add PROJECT => CHILD to its children.
        ownedProjectionInput->children.push_back(ownedProjectionNode.release());
    }

    // Re-compute properties so that they reflect the new structure of the tree.
    ownedProjectionInput->computeProperties();

    return ownedProjectionInput;
}

bool canUseSimpleSort(const QuerySolutionNode& solnRoot,
                      const CanonicalQuery& cq,
                      const QueryPlannerParams& plannerParams) {
    const bool splitLimitedSortEligible = cq.getFindCommandRequest().getNtoreturn() &&
        !cq.getFindCommandRequest().getSingleBatch() &&
        plannerParams.options & QueryPlannerParams::SPLIT_LIMITED_SORT;

    // The simple sort stage discards any metadata other than sort key metadata. It can only be used
    // if there are no metadata dependencies, or the only metadata dependency is a 'kSortKey'
    // dependency.
    const bool metadataDepsCompatible = cq.metadataDeps().none() ||
        (cq.metadataDeps().count() == 1u && cq.metadataDeps()[DocumentMetadataFields::kSortKey]);

    return solnRoot.fetched() && metadataDepsCompatible &&
        // For performance, the simple sort stage discards any incoming record ids. Carrying the
        // record ids along through the sorting process is wasted work when these ids will never be
        // consumed later in the execution of the query. If the record ids are needed, however, then
        // we can't use the simple sort stage.
        !(plannerParams.options & QueryPlannerParams::PRESERVE_RECORD_ID)
        // Disable for queries which have an ntoreturn value and are eligible for the "split limited
        // sort" hack. Such plans require record ids to be present for deduping, but the simple sort
        // stage discards record ids.
        && !splitLimitedSortEligible;
}

}  // namespace

// static
void QueryPlannerAnalysis::analyzeGeo(const QueryPlannerParams& params,
                                      QuerySolutionNode* solnRoot) {
    // Get field names of all 2dsphere indexes with version >= 3.
    std::set<StringData> twoDSphereFields;
    for (const IndexEntry& indexEntry : params.indices) {
        if (indexEntry.type != IndexType::INDEX_2DSPHERE) {
            continue;
        }

        S2IndexingParams params;
        ExpressionParams::initialize2dsphereParams(
            indexEntry.infoObj, indexEntry.collator, &params);

        if (params.indexVersion < S2_INDEX_VERSION_3) {
            continue;
        }

        for (auto elt : indexEntry.keyPattern) {
            if (elt.type() == BSONType::String && elt.String() == "2dsphere") {
                twoDSphereFields.insert(elt.fieldName());
            }
        }
    }
    if (twoDSphereFields.size() > 0) {
        geoSkipValidationOn(twoDSphereFields, solnRoot);
    }
}

BSONObj QueryPlannerAnalysis::getSortPattern(const BSONObj& indexKeyPattern) {
    BSONObjBuilder sortBob;
    BSONObjIterator kpIt(indexKeyPattern);
    while (kpIt.more()) {
        BSONElement elt = kpIt.next();
        if (elt.type() == mongo::String) {
            break;
        }
        // The canonical check as to whether a key pattern element is "ascending" or "descending" is
        // (elt.number() >= 0). This is defined by the Ordering class.
        int sortOrder = (elt.number() >= 0) ? 1 : -1;
        sortBob.append(elt.fieldName(), sortOrder);
    }
    return sortBob.obj();
}

// static
bool QueryPlannerAnalysis::explodeForSort(const CanonicalQuery& query,
                                          const QueryPlannerParams& params,
                                          QuerySolutionNode** solnRoot) {
    vector<QuerySolutionNode*> explodableNodes;

    QuerySolutionNode* toReplace;
    if (!structureOKForExplode(*solnRoot, &toReplace)) {
        return false;
    }

    // Find explodable nodes in the subtree rooted at 'toReplace'.
    getExplodableNodes(toReplace, &explodableNodes);

    const BSONObj& desiredSort = query.getFindCommandRequest().getSort();

    // How many scan leaves will result from our expansion?
    size_t totalNumScans = 0;

    // The value of entry i is how many scans we want to blow up for explodableNodes[i]. We
    // calculate this in the loop below and might as well reuse it if we blow up that scan.
    vector<size_t> fieldsToExplode;

    // The sort order we're looking for has to possibly be provided by each of the index scans
    // upon explosion.
    for (auto&& explodableNode : explodableNodes) {
        // We can do this because structureOKForExplode is only true if the leaves are index
        // scans.
        IndexScanNode* isn = const_cast<IndexScanNode*>(getIndexScanNode(explodableNode));
        const IndexBounds& bounds = isn->bounds;

        // Not a point interval prefix, can't try to rewrite.
        if (bounds.isSimpleRange) {
            return false;
        }

        if (isn->index.multikey && isn->index.multikeyPaths.empty()) {
            // The index is multikey but has no path-level multikeyness metadata. In this case, the
            // index can never provide a sort.
            return false;
        }

        // How many scans will we create if we blow up this ixscan?
        size_t numScans = 1;

        // Skip every field that is a union of point intervals and build the resulting sort
        // order from the remaining fields.
        BSONObjIterator kpIt(isn->index.keyPattern);
        size_t boundsIdx = 0;
        while (kpIt.more()) {
            const OrderedIntervalList& oil = bounds.fields[boundsIdx];
            if (!isUnionOfPoints(oil)) {
                break;
            }
            numScans *= oil.intervals.size();
            kpIt.next();
            ++boundsIdx;
        }

        // There's no sort order left to gain by exploding.  Just go home.  TODO: verify nothing
        // clever we can do here.
        if (!kpIt.more()) {
            return false;
        }

        // Only explode if there's at least one field to explode for this scan.
        if (0 == boundsIdx) {
            return false;
        }

        // The rest of the fields define the sort order we could obtain by exploding
        // the bounds.
        BSONObjBuilder resultingSortBob;
        while (kpIt.more()) {
            auto elem = kpIt.next();
            if (isn->multikeyFields.find(elem.fieldNameStringData()) != isn->multikeyFields.end()) {
                // One of the indexed fields providing the sort is multikey. It is not correct for a
                // field with multikey components to provide a sort, so bail out.
                return false;
            }
            resultingSortBob.append(elem);
        }

        // See if it's the order we're looking for.
        BSONObj possibleSort = resultingSortBob.obj();
        if (!desiredSort.isPrefixOf(possibleSort, SimpleBSONElementComparator::kInstance)) {
            // We can't get the sort order from the index scan. See if we can
            // get the sort by reversing the scan.
            BSONObj reversePossibleSort = QueryPlannerCommon::reverseSortObj(possibleSort);
            if (!desiredSort.isPrefixOf(reversePossibleSort,
                                        SimpleBSONElementComparator::kInstance)) {
                // Can't get the sort order from the reversed index scan either. Give up.
                return false;
            } else {
                // We can get the sort order we need if we reverse the scan.
                QueryPlannerCommon::reverseScans(isn);
            }
        }

        // An index whose collation does not match the query's cannot provide a sort if sort-by
        // fields can contain collatable values.
        if (!CollatorInterface::collatorsMatch(isn->index.collator, query.getCollator())) {
            auto fieldsWithStringBounds =
                IndexScanNode::getFieldsWithStringBounds(bounds, isn->index.keyPattern);
            for (auto&& element : desiredSort) {
                if (fieldsWithStringBounds.count(element.fieldNameStringData()) > 0) {
                    // The field can contain collatable values and therefore we cannot use the index
                    // to provide the sort.
                    return false;
                }
            }
        }

        // Do some bookkeeping to see how many ixscans we'll create total.
        totalNumScans += numScans;

        // And for this scan how many fields we expand.
        fieldsToExplode.push_back(boundsIdx);
    }

    // Too many ixscans spoil the performance.
    if (totalNumScans > (size_t)internalQueryMaxScansToExplode.load()) {
        (*solnRoot)->hitScanLimit = true;
        LOGV2_DEBUG(
            20950,
            5,
            "Could expand ixscans to pull out sort order but resulting scan count is too high",
            "numScans"_attr = totalNumScans);
        return false;
    }

    // If we're here, we can (probably?  depends on how restrictive the structure check is)
    // get our sort order via ixscan blow-up.
    MergeSortNode* merge = new MergeSortNode();
    merge->sort = desiredSort;
    for (size_t i = 0; i < explodableNodes.size(); ++i) {
        explodeNode(explodableNodes[i], desiredSort, fieldsToExplode[i], &merge->children);
    }

    merge->computeProperties();

    // Replace 'toReplace' with the new merge sort node.
    replaceNodeInTree(solnRoot, toReplace, merge);
    // And get rid of the node that got replaced.
    delete toReplace;

    return true;
}

// static
QuerySolutionNode* QueryPlannerAnalysis::analyzeSort(const CanonicalQuery& query,
                                                     const QueryPlannerParams& params,
                                                     QuerySolutionNode* solnRoot,
                                                     bool* blockingSortOut) {
    *blockingSortOut = false;

    const FindCommandRequest& findCommand = query.getFindCommandRequest();
    const BSONObj& sortObj = findCommand.getSort();

    if (sortObj.isEmpty()) {
        return solnRoot;
    }

    // TODO: We could check sortObj for any projections other than :1 and :-1
    // and short-cut some of this.

    // If the sort is $natural, we ignore it, assuming that the caller has detected that and
    // outputted a collscan to satisfy the desired order.
    if (sortObj[query_request_helper::kNaturalSortField]) {
        return solnRoot;
    }

    // See if solnRoot gives us the sort.  If so, we're done.
    auto providedSorts = solnRoot->providedSorts();
    if (providedSorts.contains(sortObj)) {
        return solnRoot;
    }

    // Sort is not provided.  See if we provide the reverse of our sort pattern.
    // If so, we can reverse the scan direction(s).
    BSONObj reverseSort = QueryPlannerCommon::reverseSortObj(sortObj);
    if (providedSorts.contains(reverseSort)) {
        QueryPlannerCommon::reverseScans(solnRoot);
        LOGV2_DEBUG(20951,
                    5,
                    "Reversing ixscan to provide sort",
                    "newPlan"_attr = redact(solnRoot->toString()));
        return solnRoot;
    }

    // Sort not provided, can't reverse scans to get the sort.  One last trick: We can "explode"
    // index scans over point intervals to an OR of sub-scans in order to pull out a sort.
    // Let's try this.
    if (explodeForSort(query, params, &solnRoot)) {
        return solnRoot;
    }

    // If we're here, we need to add a sort stage.

    if (!solnRoot->fetched()) {
        const bool sortIsCovered =
            std::all_of(sortObj.begin(), sortObj.end(), [solnRoot](BSONElement e) {
                // Note that hasField() will return 'false' in the case that this field is a string
                // and there is a non-simple collation on the index. This will lead to encoding of
                // the field from the document on fetch, despite having read the encoded value from
                // the index.
                return solnRoot->hasField(e.fieldName());
            });

        if (!sortIsCovered) {
            FetchNode* fetch = new FetchNode();
            fetch->children.push_back(solnRoot);
            solnRoot = fetch;
        }
    }

    std::unique_ptr<SortNode> sortNode;
    if (canUseSimpleSort(*solnRoot, query, params)) {
        sortNode = std::make_unique<SortNodeSimple>();
    } else {
        sortNode = std::make_unique<SortNodeDefault>();
    }
    sortNode->pattern = sortObj;
    sortNode->children.push_back(solnRoot);
    sortNode->addSortKeyMetadata = query.metadataDeps()[DocumentMetadataFields::kSortKey];
    solnRoot = sortNode.release();
    auto sortNodeRaw = static_cast<SortNode*>(solnRoot);
    // When setting the limit on the sort, we need to consider both
    // the limit N and skip count M. The sort should return an ordered list
    // N + M items so that the skip stage can discard the first M results.
    if (findCommand.getLimit()) {
        // We have a true limit. The limit can be combined with the SORT stage.
        sortNodeRaw->limit = static_cast<size_t>(*findCommand.getLimit()) +
            static_cast<size_t>(findCommand.getSkip().value_or(0));
    } else if (findCommand.getNtoreturn()) {
        // We have an ntoreturn specified by an OP_QUERY style find. This is used
        // by clients to mean both batchSize and limit.
        //
        // Overflow here would be bad and could cause a nonsense limit. Cast
        // skip and limit values to unsigned ints to make sure that the
        // sum is never stored as signed. (See SERVER-13537).
        sortNodeRaw->limit = static_cast<size_t>(*findCommand.getNtoreturn()) +
            static_cast<size_t>(findCommand.getSkip().value_or(0));

        // This is a SORT with a limit. The wire protocol has a single quantity called "numToReturn"
        // which could mean either limit or batchSize.  We have no idea what the client intended.
        // One way to handle the ambiguity of a limited OR stage is to use the SPLIT_LIMITED_SORT
        // hack.
        //
        // If singleBatch is true (meaning that 'ntoreturn' was initially passed to the server as a
        // negative value), then we treat numToReturn as a limit.  Since there is no limit-batchSize
        // ambiguity in this case, we do not use the SPLIT_LIMITED_SORT hack.
        //
        // If numToReturn is really a limit, then we want to add a limit to this SORT stage, and
        // hence perform a topK.
        //
        // If numToReturn is really a batchSize, then we want to perform a regular blocking sort.
        //
        // Since we don't know which to use, just join the two options with an OR, with the topK
        // first. If the client wants a limit, they'll get the efficiency of topK. If they want a
        // batchSize, the other OR branch will deliver the missing results. The OR stage handles
        // deduping.
        //
        // We must also add an ENSURE_SORTED node above the OR to ensure that the final results are
        // in correct sorted order, which may not be true if the data is concurrently modified.
        //
        // Not allowed for geo or text, because we assume elsewhere that those stages appear just
        // once.
        if (!findCommand.getSingleBatch() &&
            params.options & QueryPlannerParams::SPLIT_LIMITED_SORT &&
            !QueryPlannerCommon::hasNode(query.root(), MatchExpression::TEXT) &&
            !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO) &&
            !QueryPlannerCommon::hasNode(query.root(), MatchExpression::GEO_NEAR)) {
            // If we're here then the SPLIT_LIMITED_SORT hack is turned on, and the query is of a
            // type that allows the hack.
            //
            // The EnsureSortedStage consumes sort key metadata, so we must instruct the sort to
            // attach it.
            sortNodeRaw->addSortKeyMetadata = true;

            auto orNode = std::make_unique<OrNode>();
            orNode->children.push_back(solnRoot);
            auto sortClone = static_cast<SortNode*>(sortNodeRaw->clone());
            sortClone->limit = 0;
            orNode->children.push_back(sortClone);

            // Add ENSURE_SORTED above the OR.
            auto ensureSortedNode = std::make_unique<EnsureSortedNode>();
            ensureSortedNode->pattern = sortNodeRaw->pattern;
            ensureSortedNode->children.push_back(orNode.release());
            solnRoot = ensureSortedNode.release();
        }
    } else {
        sortNodeRaw->limit = 0;
    }

    *blockingSortOut = true;

    return solnRoot;
}

std::unique_ptr<QuerySolution> QueryPlannerAnalysis::analyzeDataAccess(
    const CanonicalQuery& query,
    const QueryPlannerParams& params,
    std::unique_ptr<QuerySolutionNode> solnRoot) {
    auto soln = std::make_unique<QuerySolution>();
    soln->indexFilterApplied = params.indexFiltersApplied;

    solnRoot->computeProperties();

    analyzeGeo(params, solnRoot.get());

    // solnRoot finds all our results.  Let's see what transformations we must perform to the
    // data.

    // If we're answering a query on a sharded system, we need to drop documents that aren't
    // logically part of our shard.
    if (params.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        if (!solnRoot->fetched()) {
            // See if we need to fetch information for our shard key.
            // NOTE: Solution nodes only list ordinary, non-transformed index keys for now

            bool fetch = false;
            for (auto&& shardKeyField : params.shardKey) {
                auto fieldAvailability = solnRoot->getFieldAvailability(shardKeyField.fieldName());
                if (fieldAvailability == FieldAvailability::kNotProvided) {
                    // One of the shard key fields is not provided by an index. We need to fetch the
                    // full documents prior to shard filtering.
                    fetch = true;
                    break;
                }
                if (fieldAvailability == FieldAvailability::kHashedValueProvided &&
                    shardKeyField.valueStringDataSafe() != IndexNames::HASHED) {
                    // The index scan provides the hash of a field, but the shard key field is _not_
                    // hashed. We need to fetch prior to shard filtering in order to recover the raw
                    // value of the field.
                    fetch = true;
                    break;
                }
            }

            if (fetch) {
                FetchNode* fetchNode = new FetchNode();
                fetchNode->children.push_back(solnRoot.release());
                solnRoot.reset(fetchNode);
            }
        }

        ShardingFilterNode* sfn = new ShardingFilterNode();
        sfn->children.push_back(solnRoot.release());
        solnRoot.reset(sfn);
    }

    bool hasSortStage = false;
    solnRoot.reset(analyzeSort(query, params, solnRoot.release(), &hasSortStage));

    // This can happen if we need to create a blocking sort stage and we're not allowed to.
    if (!solnRoot) {
        return nullptr;
    }

    // A solution can be blocking if it has a blocking sort stage or
    // a hashed AND stage.
    bool hasAndHashStage = solnRoot->hasNode(STAGE_AND_HASH);
    soln->hasBlockingStage = hasSortStage || hasAndHashStage;

    const FindCommandRequest& findCommand = query.getFindCommandRequest();

    if (findCommand.getSkip()) {
        auto skip = std::make_unique<SkipNode>();
        skip->skip = *findCommand.getSkip();
        skip->children.push_back(solnRoot.release());
        solnRoot = std::move(skip);
    }

    // Project the results.
    if (findCommand.getReturnKey()) {
        // We don't need a projection stage if returnKey was requested since the intended behavior
        // is that the projection is ignored when returnKey is specified.
        solnRoot = std::make_unique<ReturnKeyNode>(
            addSortKeyGeneratorStageIfNeeded(query, hasSortStage, std::move(solnRoot)),
            query.getProj()
                ? QueryPlannerCommon::extractSortKeyMetaFieldsFromProjection(*query.getProj())
                : std::vector<FieldPath>{});
    } else if (query.getProj()) {
        solnRoot = analyzeProjection(query, std::move(solnRoot), hasSortStage);
    } else {
        // Even if there's no projection, the client may want sort key metadata.
        solnRoot = addSortKeyGeneratorStageIfNeeded(query, hasSortStage, std::move(solnRoot));

        // If there's no projection, we must fetch, as the user wants the entire doc.
        if (!solnRoot->fetched() && !(params.options & QueryPlannerParams::IS_COUNT)) {
            FetchNode* fetch = new FetchNode();
            fetch->children.push_back(solnRoot.release());
            solnRoot.reset(fetch);
        }
    }

    // When there is both a blocking sort and a limit, the limit will
    // be enforced by the blocking sort.
    // Otherwise, we need to limit the results in the case of a hard limit
    // (ie. limit in raw query is negative)
    if (!hasSortStage) {
        // We don't have a sort stage. This means that, if there is a limit, we will have
        // to enforce it ourselves since it's not handled inside SORT.
        if (findCommand.getLimit()) {
            LimitNode* limit = new LimitNode();
            limit->limit = *findCommand.getLimit();
            limit->children.push_back(solnRoot.release());
            solnRoot.reset(limit);
        } else if (findCommand.getNtoreturn() && findCommand.getSingleBatch()) {
            // We have a "legacy limit", i.e. a negative ntoreturn value from an OP_QUERY style
            // find.
            LimitNode* limit = new LimitNode();
            limit->limit = *findCommand.getNtoreturn();
            limit->children.push_back(solnRoot.release());
            solnRoot.reset(limit);
        }
    }

    solnRoot = tryPushdownProjectBeneathSort(std::move(solnRoot));

    soln->setRoot(std::move(solnRoot));
    return soln;
}

}  // namespace mongo
