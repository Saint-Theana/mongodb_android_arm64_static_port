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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/db/index/expression_keys_private.h"

#include <utility>

#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/s2.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2regioncoverer.h"

namespace {

using namespace mongo;

namespace dps = ::mongo::dotted_path_support;
//
// Helper functions for getS2Keys
//

Status S2GetKeysForElement(const BSONElement& element,
                           const S2IndexingParams& params,
                           vector<S2CellId>* out) {
    GeometryContainer geoContainer;
    Status status = geoContainer.parseFromStorage(element);
    if (!status.isOK())
        return status;

    S2RegionCoverer coverer;
    params.configureCoverer(geoContainer, &coverer);

    // Don't index big polygon
    if (geoContainer.getNativeCRS() == STRICT_SPHERE) {
        return Status(ErrorCodes::BadValue, "can't index geometry with strict winding order");
    }

    // Only certain geometries can be indexed in the old index format S2_INDEX_VERSION_1.  See
    // definition of S2IndexVersion for details.
    if (params.indexVersion == S2_INDEX_VERSION_1 && !geoContainer.isSimpleContainer()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "given geometry can't be indexed in the old index format");
    }

    // Project the geometry into spherical space
    if (!geoContainer.supportsProject(SPHERE)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "can't project geometry into spherical CRS: "
                                    << element.toString(false));
    }
    geoContainer.projectInto(SPHERE);

    invariant(geoContainer.hasS2Region());

    coverer.GetCovering(geoContainer.getS2Region(), out);
    return Status::OK();
}

/*
 * We take the cartesian product of all keys when appending.
 */
void appendToS2Keys(const std::vector<KeyString::HeapBuilder>& existingKeys,
                    std::vector<KeyString::HeapBuilder>* out,
                    KeyString::Version keyStringVersion,
                    Ordering ordering,
                    const std::function<void(KeyString::HeapBuilder&)>& fn) {
    if (existingKeys.empty()) {
        /*
         * This is the base case when the keys for the first field are generated.
         */
        out->emplace_back(keyStringVersion, ordering);
        fn(out->back());
    }
    for (const auto& ks : existingKeys) {
        /*
         * We copy all of the existing keys and perform 'fn' on each copy.
         */
        out->emplace_back(ks);
        fn(out->back());
    }
}

/**
 * Fills 'out' with the S2 keys that should be generated for 'elements' in a 2dsphere index.
 *
 * Returns true if an indexed element of the document uses multiple cells for its covering, and
 * returns false otherwise.
 */
bool getS2GeoKeys(const BSONObj& document,
                  const BSONElementSet& elements,
                  const S2IndexingParams& params,
                  const std::vector<KeyString::HeapBuilder>& keysToAdd,
                  std::vector<KeyString::HeapBuilder>* out,
                  KeyString::Version keyStringVersion,
                  Ordering ordering) {
    bool everGeneratedMultipleCells = false;
    for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
        vector<S2CellId> cells;
        Status status = S2GetKeysForElement(*i, params, &cells);
        uassert(16755,
                str::stream() << "Can't extract geo keys: " << document << "  " << status.reason(),
                status.isOK());

        uassert(16756,
                "Unable to generate keys for (likely malformed) geometry: " + document.toString(),
                cells.size() > 0);

        for (vector<S2CellId>::const_iterator it = cells.begin(); it != cells.end(); ++it) {
            S2CellIdToIndexKeyStringAppend(
                *it, params.indexVersion, keysToAdd, out, keyStringVersion, ordering);
        }

        everGeneratedMultipleCells = everGeneratedMultipleCells || cells.size() > 1;
    }

    if (0 == out->size()) {
        appendToS2Keys(keysToAdd, out, keyStringVersion, ordering, [](KeyString::HeapBuilder& ks) {
            ks.appendNull();
        });
    }
    return everGeneratedMultipleCells;
}

/**
 * Fills 'out' with the keys that should be generated for an array value 'obj' in a 2dsphere index.
 * A key is generated for each element of the array value 'obj'.
 */
void getS2LiteralKeysArray(const BSONObj& obj,
                           const CollatorInterface* collator,
                           const std::vector<KeyString::HeapBuilder>& keysToAdd,
                           std::vector<KeyString::HeapBuilder>* out,
                           KeyString::Version keyStringVersion,
                           Ordering ordering) {
    BSONObjIterator objIt(obj);
    if (!objIt.more()) {
        // Empty arrays are indexed as undefined.
        appendToS2Keys(keysToAdd, out, keyStringVersion, ordering, [](KeyString::HeapBuilder& ks) {
            ks.appendUndefined();
        });
    } else {
        // Non-empty arrays are exploded.
        while (objIt.more()) {
            const auto elem = objIt.next();
            appendToS2Keys(
                keysToAdd, out, keyStringVersion, ordering, [&](KeyString::HeapBuilder& ks) {
                    if (collator) {
                        ks.appendBSONElement(elem, [&](StringData stringData) {
                            return collator->getComparisonString(stringData);
                        });
                    } else {
                        ks.appendBSONElement(elem);
                    }
                });
        }
    }
}

/**
 * Fills 'out' with the keys that should be generated for a value 'elt' in a 2dsphere index. If
 * 'elt' is an array value, then a key is generated for each element of the array value 'obj'.
 *
 * Returns true if 'elt' is an array value and returns false otherwise.
 */
bool getS2OneLiteralKey(const BSONElement& elt,
                        const CollatorInterface* collator,
                        const std::vector<KeyString::HeapBuilder>& keysToAdd,
                        std::vector<KeyString::HeapBuilder>* out,
                        KeyString::Version keyStringVersion,
                        Ordering ordering) {
    if (Array == elt.type()) {
        getS2LiteralKeysArray(elt.Obj(), collator, keysToAdd, out, keyStringVersion, ordering);
        return true;
    } else {
        // One thing, not an array, index as-is.
        appendToS2Keys(keysToAdd, out, keyStringVersion, ordering, [&](KeyString::HeapBuilder& ks) {
            if (collator) {
                ks.appendBSONElement(elt, [&](StringData stringData) {
                    return collator->getComparisonString(stringData);
                });
            } else {
                ks.appendBSONElement(elt);
            }
        });
    }
    return false;
}

/**
 * Fills 'out' with the non-geo keys that should be generated for 'elements' in a 2dsphere index. If
 * any element in 'elements' is an array value, then a key is generated for each element of that
 * array value.
 *
 * Returns true if any element of 'elements' is an array value and returns false otherwise.
 */
bool getS2LiteralKeys(const BSONElementSet& elements,
                      const CollatorInterface* collator,
                      const std::vector<KeyString::HeapBuilder>& keysToAdd,
                      std::vector<KeyString::HeapBuilder>* out,
                      KeyString::Version keyStringVersion,
                      Ordering ordering) {
    bool foundIndexedArrayValue = false;
    if (0 == elements.size()) {
        // Missing fields are indexed as null.
        appendToS2Keys(keysToAdd, out, keyStringVersion, ordering, [](KeyString::HeapBuilder& ks) {
            ks.appendNull();
        });
    } else {
        for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
            const bool thisElemIsArray =
                getS2OneLiteralKey(*i, collator, keysToAdd, out, keyStringVersion, ordering);
            foundIndexedArrayValue = foundIndexedArrayValue || thisElemIsArray;
        }
    }
    return foundIndexedArrayValue;
}

}  // namespace

namespace mongo {

using std::pair;
using std::string;
using std::vector;

// static
void ExpressionKeysPrivate::get2DKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                      const BSONObj& obj,
                                      const TwoDIndexingParams& params,
                                      KeyStringSet* keys,
                                      KeyString::Version keyStringVersion,
                                      Ordering ordering,
                                      boost::optional<RecordId> id) {
    BSONElementMultiSet bSet;

    // Get all the nested location fields, but don't return individual elements from
    // the last array, if it exists.
    dps::extractAllElementsAlongPath(obj, params.geo.c_str(), bSet, false);

    if (bSet.empty())
        return;

    auto keysSequence = keys->extract_sequence();
    for (BSONElementMultiSet::iterator setI = bSet.begin(); setI != bSet.end(); ++setI) {
        BSONElement geo = *setI;

        if (geo.eoo() || !geo.isABSONObj())
            continue;

        //
        // Grammar for location lookup:
        // locs ::= [loc,loc,...,loc]|{<k>:loc,<k>:loc,...,<k>:loc}|loc
        // loc  ::= { <k1> : #, <k2> : # }|[#, #]|{}
        //
        // Empty locations are ignored, preserving single-location semantics
        //

        BSONObj embed = geo.embeddedObject();
        if (embed.isEmpty())
            continue;

        // Differentiate between location arrays and locations
        // by seeing if the first element value is a number
        bool singleElement = embed.firstElement().isNumber();

        BSONObjIterator oi(embed);

        while (oi.more()) {
            BSONObj locObj;

            if (singleElement) {
                locObj = embed;
            } else {
                BSONElement locElement = oi.next();

                uassert(16804,
                        str::stream()
                            << "location object expected, location array not in correct format",
                        locElement.isABSONObj());

                locObj = locElement.embeddedObject();
                if (locObj.isEmpty())
                    continue;
            }

            KeyString::PooledBuilder keyString(pooledBufferBuilder, keyStringVersion, ordering);
            params.geoHashConverter->hash(locObj, &obj).appendHashMin(&keyString);

            // Go through all the other index keys
            for (vector<pair<string, int>>::const_iterator i = params.other.begin();
                 i != params.other.end();
                 ++i) {
                // Get *all* fields for the index key
                BSONElementSet eSet;
                dps::extractAllElementsAlongPath(obj, i->first, eSet);

                if (eSet.size() == 0)
                    keyString.appendNull();
                else if (eSet.size() == 1)
                    keyString.appendBSONElement(*(eSet.begin()));
                else {
                    // If we have more than one key, store as an array of the objects
                    keyString.appendSetAsArray(eSet);
                }
            }

            if (id) {
                keyString.appendRecordId(*id);
            }
            keysSequence.push_back(keyString.release());
            if (singleElement)
                break;
        }
    }
    keys->adopt_sequence(std::move(keysSequence));
}

// static
void ExpressionKeysPrivate::getFTSKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                       const BSONObj& obj,
                                       const fts::FTSSpec& ftsSpec,
                                       KeyStringSet* keys,
                                       KeyString::Version keyStringVersion,
                                       Ordering ordering,
                                       boost::optional<RecordId> id) {
    fts::FTSIndexFormat::getKeys(
        pooledBufferBuilder, ftsSpec, obj, keys, keyStringVersion, ordering, id);
}

// static
void ExpressionKeysPrivate::getHashKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                        const BSONObj& obj,
                                        const BSONObj& keyPattern,
                                        HashSeed seed,
                                        int hashVersion,
                                        bool isSparse,
                                        const CollatorInterface* collator,
                                        KeyStringSet* keys,
                                        KeyString::Version keyStringVersion,
                                        Ordering ordering,
                                        bool ignoreArraysAlongPath,
                                        boost::optional<RecordId> id) {
    static const BSONObj nullObj = BSON("" << BSONNULL);
    auto hasFieldValue = false;
    KeyString::PooledBuilder keyString(pooledBufferBuilder, keyStringVersion, ordering);
    for (auto&& indexEntry : keyPattern) {
        auto indexPath = indexEntry.fieldNameStringData();
        auto* cstr = indexPath.rawData();
        auto fieldVal = dps::extractElementAtPathOrArrayAlongPath(obj, cstr);

        // If we hit an array while traversing the path, 'cstr' will point to the path component
        // immediately following the array, or the null termination byte if the terminal path
        // component was an array. In the latter case, 'remainingPath' will be empty.
        auto remainingPath = StringData(cstr);

        // If 'ignoreArraysAlongPath' is set, we want to use the behaviour prior to SERVER-44050,
        // which is to allow arrays along the field path (except the terminal path). This is done so
        // that the document keys inserted prior to SERVER-44050 can be deleted or updated after the
        // upgrade, allowing users to recover from the possible index corruption. The old behaviour
        // before SERVER-44050 was to store 'null' index key if we encountered an array along the
        // index field path. We will use the same logic in the context of removing index keys.
        if (ignoreArraysAlongPath && fieldVal.type() == BSONType::Array && !remainingPath.empty()) {
            fieldVal = nullObj.firstElement();
        }

        // Otherwise, throw if an array was encountered at any point along the path.
        uassert(16766,
                str::stream() << "Error: hashed indexes do not currently support array values. "
                                 "Found array at path: "
                              << indexPath.substr(0,
                                                  indexPath.size() - remainingPath.size() -
                                                      !remainingPath.empty()),
                fieldVal.type() != BSONType::Array);

        BSONObj fieldValObj;
        if (fieldVal.eoo()) {
            fieldVal = nullObj.firstElement();
        } else {
            BSONObjBuilder bob;
            CollationIndexKey::collationAwareIndexKeyAppend(fieldVal, collator, &bob);
            fieldValObj = bob.obj();
            fieldVal = fieldValObj.firstElement();
            hasFieldValue = true;
        }

        if (indexEntry.isNumber()) {
            keyString.appendBSONElement(fieldVal);
        } else {
            keyString.appendNumberLong(makeSingleHashKey(fieldVal, seed, hashVersion));
        }
    }
    if (isSparse && !hasFieldValue) {
        return;
    }
    if (id) {
        keyString.appendRecordId(*id);
    }
    keys->insert(keyString.release());
}

// static
long long int ExpressionKeysPrivate::makeSingleHashKey(const BSONElement& e, HashSeed seed, int v) {
    massert(16767, "Only HashVersion 0 has been defined", v == 0);
    return BSONElementHasher::hash64(e, seed);
}

// static
void ExpressionKeysPrivate::getHaystackKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                            const BSONObj& obj,
                                            const std::string& geoField,
                                            const std::vector<std::string>& otherFields,
                                            double bucketSize,
                                            KeyStringSet* keys,
                                            KeyString::Version keyStringVersion,
                                            Ordering ordering,
                                            boost::optional<RecordId> id) {
    BSONElement loc = dps::extractElementAtPath(obj, geoField);

    if (loc.eoo()) {
        return;
    }

    // NOTE: We explicitly test nFields >= 2 to support legacy users who may have indexed
    // (intentionally or unintentionally) objects/arrays with more than two fields.
    uassert(16775,
            str::stream() << "cannot extract [lng, lat] array or object from " << obj,
            loc.isABSONObj() && loc.Obj().nFields() >= 2);

    string root;
    {
        BSONObjIterator i(loc.Obj());
        BSONElement x = i.next();
        BSONElement y = i.next();
        root = makeHaystackString(hashHaystackElement(x, bucketSize),
                                  hashHaystackElement(y, bucketSize));
    }

    invariant(otherFields.size() == 1);

    BSONElementSet all;

    // The object we're indexing may be an array.
    dps::extractAllElementsAlongPath(obj, otherFields[0], all);

    if (all.size() == 0) {
        // We're indexing a document that doesn't have the secondary non-geo field present.
        // XXX: do we want to add this even if all.size() > 0?  result:empty search terms
        // match everything instead of only things w/empty search terms)
        KeyString::PooledBuilder keyString(pooledBufferBuilder, keyStringVersion, ordering);
        keyString.appendString(root);
        keyString.appendNull();
        if (id) {
            keyString.appendRecordId(*id);
        }
        keys->insert(keyString.release());
    } else {
        // Ex:If our secondary field is type: "foo" or type: {a:"foo", b:"bar"},
        // all.size()==1.  We can query on the complete field.
        // Ex: If our secondary field is type: ["A", "B"] all.size()==2 and all has values
        // "A" and "B".  The query looks for any of the fields in the array.
        auto keysSequence = keys->extract_sequence();
        for (const auto& elem : all) {
            KeyString::PooledBuilder keyString(pooledBufferBuilder, keyStringVersion, ordering);
            keyString.appendString(root);
            keyString.appendBSONElement(elem);
            if (id) {
                keyString.appendRecordId(*id);
            }
            keysSequence.push_back(keyString.release());
        }
        keys->adopt_sequence(std::move(keysSequence));
    }
}

// static
int ExpressionKeysPrivate::hashHaystackElement(const BSONElement& e, double bucketSize) {
    uassert(16776, "geo field is not a number", e.isNumber());
    double d = e.numberDouble();
    d += 180;
    d /= bucketSize;
    return static_cast<int>(d);
}

// static
std::string ExpressionKeysPrivate::makeHaystackString(int hashedX, int hashedY) {
    str::stream ss;
    ss << hashedX << "_" << hashedY;
    return ss;
}

void ExpressionKeysPrivate::getS2Keys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                      const BSONObj& obj,
                                      const BSONObj& keyPattern,
                                      const S2IndexingParams& params,
                                      KeyStringSet* keys,
                                      MultikeyPaths* multikeyPaths,
                                      KeyString::Version keyStringVersion,
                                      Ordering ordering,
                                      boost::optional<RecordId> id) {
    std::vector<KeyString::HeapBuilder> keysToAdd;

    // Does one of our documents have a geo field?
    bool haveGeoField = false;

    if (multikeyPaths) {
        invariant(multikeyPaths->empty());
        multikeyPaths->resize(keyPattern.nFields());
    }

    size_t posInIdx = 0;

    // We output keys in the same order as the fields we index.
    for (const auto& keyElem : keyPattern) {
        // First, we get the keys that this field adds.  Either they're added literally from
        // the value of the field, or they're transformed if the field is geo.
        BSONElementSet fieldElements;
        const bool expandArrayOnTrailingField = false;
        MultikeyComponents* arrayComponents = multikeyPaths ? &(*multikeyPaths)[posInIdx] : nullptr;
        dps::extractAllElementsAlongPath(
            obj, keyElem.fieldName(), fieldElements, expandArrayOnTrailingField, arrayComponents);

        // Trailing array values aren't being expanded, so we still need to determine whether the
        // last component of the indexed path 'keyElem.fieldName()' causes the index to be multikey.
        // We say that it does if
        //   (a) the last component of the indexed path ever refers to an array value (regardless of
        //       the number of array elements)
        //   (b) the last component of the indexed path ever refers to GeoJSON data that requires
        //       multiple cells for its covering.
        bool lastPathComponentCausesIndexToBeMultikey;
        std::vector<KeyString::HeapBuilder> updatedKeysToAdd;
        if (IndexNames::GEO_2DSPHERE == keyElem.valuestr()) {
            if (params.indexVersion >= S2_INDEX_VERSION_2) {
                // For >= V2,
                // geo: null,
                // geo: undefined
                // geo: []
                // should all behave like there is no geo field.  So we look for these cases and
                // throw out the field elements if we find them.
                if (1 == fieldElements.size()) {
                    BSONElement elt = *fieldElements.begin();
                    // Get the :null and :undefined cases.
                    if (elt.isNull() || Undefined == elt.type()) {
                        fieldElements.clear();
                    } else if (elt.isABSONObj()) {
                        // And this is the :[] case.
                        BSONObj obj = elt.Obj();
                        if (0 == obj.nFields()) {
                            fieldElements.clear();
                        }
                    }
                }

                // >= V2 2dsphere indices require that at least one geo field to be present in a
                // document in order to index it.
                if (fieldElements.size() > 0) {
                    haveGeoField = true;
                }
            }

            lastPathComponentCausesIndexToBeMultikey = getS2GeoKeys(obj,
                                                                    fieldElements,
                                                                    params,
                                                                    keysToAdd,
                                                                    &updatedKeysToAdd,
                                                                    keyStringVersion,
                                                                    ordering);
        } else {
            lastPathComponentCausesIndexToBeMultikey = getS2LiteralKeys(fieldElements,
                                                                        params.collator,
                                                                        keysToAdd,
                                                                        &updatedKeysToAdd,
                                                                        keyStringVersion,
                                                                        ordering);
        }

        // We expect there to be the missing field element present in the keys if data is
        // missing.  So, this should be non-empty.
        invariant(!updatedKeysToAdd.empty());

        if (multikeyPaths && lastPathComponentCausesIndexToBeMultikey) {
            const size_t pathLengthOfThisField = FieldRef{keyElem.fieldNameStringData()}.numParts();
            invariant(pathLengthOfThisField > 0);
            (*multikeyPaths)[posInIdx].insert(pathLengthOfThisField - 1);
        }

        keysToAdd = std::move(updatedKeysToAdd);
        ++posInIdx;
    }

    // Make sure that if we're >= V2 there's at least one geo field present in the doc.
    if (params.indexVersion >= S2_INDEX_VERSION_2) {
        if (!haveGeoField) {
            return;
        }
    }

    if (keysToAdd.size() > params.maxKeysPerInsert) {
        LOGV2_WARNING(23755,
                      "Insert of geo object generated a high number of keys. num keys: "
                      "{numKeys} obj inserted: {obj}",
                      "Insert of geo object generated a large number of keys",
                      "obj"_attr = redact(obj),
                      "numKeys"_attr = keysToAdd.size());
    }

    invariant(keys->empty());
    auto keysSequence = keys->extract_sequence();
    for (auto& ks : keysToAdd) {
        if (id) {
            ks.appendRecordId(*id);
        }
        keysSequence.push_back(ks.release());
    }
    keys->adopt_sequence(std::move(keysSequence));
}

}  // namespace mongo
