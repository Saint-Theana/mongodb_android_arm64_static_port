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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_key_validate.h"

#include <boost/optional.hpp>
#include <cmath>
#include <limits>
#include <set>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"

namespace mongo {
namespace index_key_validate {

std::function<void(std::set<StringData>&)> filterAllowedIndexFieldNames;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {
// When the skipIndexCreateFieldNameValidation failpoint is enabled, validation for index field
// names will be disabled. This will allow for creation of indexes with invalid field names in their
// specification.
MONGO_FAIL_POINT_DEFINE(skipIndexCreateFieldNameValidation);

static std::set<StringData> allowedFieldNames = {
    IndexDescriptor::k2dIndexBitsFieldName,
    IndexDescriptor::k2dIndexMaxFieldName,
    IndexDescriptor::k2dIndexMinFieldName,
    IndexDescriptor::k2dsphereCoarsestIndexedLevel,
    IndexDescriptor::k2dsphereFinestIndexedLevel,
    IndexDescriptor::k2dsphereVersionFieldName,
    IndexDescriptor::kBackgroundFieldName,
    IndexDescriptor::kCollationFieldName,
    IndexDescriptor::kDefaultLanguageFieldName,
    IndexDescriptor::kDropDuplicatesFieldName,
    IndexDescriptor::kExpireAfterSecondsFieldName,
    IndexDescriptor::kGeoHaystackBucketSize,
    IndexDescriptor::kHiddenFieldName,
    IndexDescriptor::kIndexNameFieldName,
    IndexDescriptor::kIndexVersionFieldName,
    IndexDescriptor::kKeyPatternFieldName,
    IndexDescriptor::kLanguageOverrideFieldName,
    IndexDescriptor::kNamespaceFieldName,
    IndexDescriptor::kPartialFilterExprFieldName,
    IndexDescriptor::kPathProjectionFieldName,
    IndexDescriptor::kSparseFieldName,
    IndexDescriptor::kStorageEngineFieldName,
    IndexDescriptor::kTextVersionFieldName,
    IndexDescriptor::kUniqueFieldName,
    IndexDescriptor::kWeightsFieldName,
    // Index creation under legacy writeMode can result in an index spec with an _id field.
    "_id"};

static const std::set<StringData> allowedIdIndexFieldNames = {
    IndexDescriptor::kCollationFieldName,
    IndexDescriptor::kIndexNameFieldName,
    IndexDescriptor::kIndexVersionFieldName,
    IndexDescriptor::kKeyPatternFieldName,
    IndexDescriptor::kNamespaceFieldName,
    // Index creation under legacy writeMode can result in an index spec with an _id field.
    "_id"};

/**
 * Returns Status::OK() if indexes of version 'indexVersion' are allowed to be created, and
 * returns ErrorCodes::CannotCreateIndex otherwise.
 */
Status isIndexVersionAllowedForCreation(IndexVersion indexVersion, const BSONObj& indexSpec) {
    switch (indexVersion) {
        case IndexVersion::kV1:
        case IndexVersion::kV2:
            return Status::OK();
    }
    return {ErrorCodes::CannotCreateIndex,
            str::stream() << "Invalid index specification " << indexSpec
                          << "; cannot create an index with v=" << static_cast<int>(indexVersion)};
}
}  // namespace

Status validateKeyPattern(const BSONObj& key, IndexDescriptor::IndexVersion indexVersion) {
    const ErrorCodes::Error code = ErrorCodes::CannotCreateIndex;

    if (key.objsize() > 2048)
        return Status(code, "Index key pattern too large.");

    if (key.isEmpty())
        return Status(code, "Index keys cannot be empty.");

    auto pluginName = IndexNames::findPluginName(key);
    if (pluginName.size()) {
        if (!IndexNames::isKnownName(pluginName))
            return Status(code, str::stream() << "Unknown index plugin '" << pluginName << '\'');
    }

    BSONObjIterator it(key);
    while (it.more()) {
        BSONElement keyElement = it.next();

        switch (indexVersion) {
            case IndexVersion::kV1: {
                if (keyElement.type() == BSONType::Object || keyElement.type() == BSONType::Array) {
                    return {code,
                            str::stream() << "Values in index key pattern cannot be of type "
                                          << typeName(keyElement.type()) << " for index version v:"
                                          << static_cast<int>(indexVersion)};
                }

                if (pluginName == IndexNames::WILDCARD) {
                    return {code,
                            str::stream() << "'" << pluginName
                                          << "' index plugin is not allowed with index version v:"
                                          << static_cast<int>(indexVersion)};
                }
                break;
            }
            case IndexVersion::kV2: {
                if (keyElement.isNumber()) {
                    double value = keyElement.number();
                    if (std::isnan(value)) {
                        return {code, "Values in the index key pattern cannot be NaN."};
                    } else if (value == 0.0) {
                        return {code, "Values in the index key pattern cannot be 0."};
                    } else if (value < 0.0 && pluginName == IndexNames::WILDCARD) {
                        return {code,
                                "A numeric value in a $** index key pattern must be positive."};
                    }
                } else if (keyElement.type() != BSONType::String) {
                    return {code,
                            str::stream()
                                << "Values in v:2 index key pattern cannot be of type "
                                << typeName(keyElement.type())
                                << ". Only numbers > 0, numbers < 0, and strings are allowed."};
                }

                break;
            }
            default:
                MONGO_UNREACHABLE;
        }

        if (keyElement.type() == String && pluginName != keyElement.str()) {
            return Status(code, "Can't use more than one index plugin for a single index.");
        } else if (keyElement.type() == String && keyElement.str() == IndexNames::WILDCARD) {
            return Status(code,
                          str::stream() << "The key pattern value for an '" << IndexNames::WILDCARD
                                        << "' index must be a non-zero number, not a string.");
        }

        // Check if the wildcard index is compounded. If it is the key is invalid because
        // compounded wildcard indexes are disallowed.
        if (pluginName == IndexNames::WILDCARD && key.nFields() != 1) {
            return Status(code, "wildcard indexes do not allow compounding");
        }

        // Ensure that the fields on which we are building the index are valid: a field must not
        // begin with a '$' unless it is part of a wildcard, DBRef or text index, and a field path
        // cannot contain an empty field. If a field cannot be created or updated, it should not be
        // indexable.

        FieldRef keyField(keyElement.fieldName());

        const size_t numParts = keyField.numParts();
        if (numParts == 0) {
            return Status(code, "Index keys cannot be an empty field.");
        }

        // "$**" is acceptable for a text index or wildcard index.
        if ((keyElement.fieldNameStringData() == "$**") &&
            ((keyElement.isNumber()) || (keyElement.valuestrsafe() == IndexNames::TEXT)))
            continue;

        if ((keyElement.fieldNameStringData() == "_fts") &&
            keyElement.valuestrsafe() != IndexNames::TEXT) {
            return Status(code, "Index key contains an illegal field name: '_fts'");
        }

        for (size_t i = 0; i != numParts; ++i) {
            const StringData part = keyField.getPart(i);

            // Check if the index key path contains an empty field.
            if (part.empty()) {
                return Status(code, "Index keys cannot contain an empty field.");
            }

            if (part[0] != '$')
                continue;

            // Check if the '$'-prefixed field is part of a DBRef: since we don't have the
            // necessary context to validate whether this is a proper DBRef, we allow index
            // creation on '$'-prefixed names that match those used in a DBRef.
            const bool mightBePartOfDbRef =
                (i != 0) && (part == "$db" || part == "$id" || part == "$ref");

            const bool isPartOfWildcard =
                (i == numParts - 1) && (part == "$**") && (pluginName == IndexNames::WILDCARD);

            if (!mightBePartOfDbRef && !isPartOfWildcard) {
                return Status(code,
                              "Index key contains an illegal field name: "
                              "field name starts with '$'.");
            }
        }
    }

    return Status::OK();
}

BSONObj removeUnknownFields(const BSONObj& indexSpec) {
    BSONObjBuilder builder;
    for (const auto& indexSpecElem : indexSpec) {
        StringData fieldName = indexSpecElem.fieldNameStringData();
        if (allowedFieldNames.count(fieldName)) {
            builder.append(indexSpecElem);
        } else {
            LOGV2_WARNING(23878,
                          "Removing field '{fieldName}' from index spec: {indexSpec}",
                          "Removing unknown field from index spec",
                          "fieldName"_attr = redact(fieldName),
                          "indexSpec"_attr = redact(indexSpec));
        }
    }
    return builder.obj();
}

StatusWith<BSONObj> validateIndexSpec(OperationContext* opCtx, const BSONObj& indexSpec) {
    bool hasKeyPatternField = false;
    bool hasIndexNameField = false;
    bool hasNamespaceField = false;
    bool hasVersionField = false;
    bool hasCollationField = false;
    bool hasWeightsField = false;
    bool apiStrict = opCtx && APIParameters::get(opCtx).getAPIStrict().value_or(false);

    auto fieldNamesValidStatus = validateIndexSpecFieldNames(indexSpec);
    if (!fieldNamesValidStatus.isOK()) {
        return fieldNamesValidStatus;
    }

    boost::optional<IndexVersion> resolvedIndexVersion;
    std::string indexType;

    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (IndexDescriptor::kKeyPatternFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kKeyPatternFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }

            std::vector<StringData> keys;
            for (auto&& keyElem : indexSpecElem.Obj()) {
                auto keyElemFieldName = keyElem.fieldNameStringData();
                if (std::find(keys.begin(), keys.end(), keyElemFieldName) != keys.end()) {
                    return {ErrorCodes::BadValue,
                            str::stream() << "The field '" << keyElemFieldName
                                          << "' appears multiple times in the index key pattern "
                                          << indexSpecElem.Obj()};
                }
                keys.push_back(keyElemFieldName);
            }

            // TODO SERVER-51871: When 5.0 becomes last-lts, this check should be moved into
            // 'validateKeyPattern()'. It must currently be done here so that haystack indexes
            // continue to replicate correctly before the upgrade to FCV "4.9" is complete.
            const auto keyPattern = indexSpecElem.Obj();
            indexType = IndexNames::findPluginName(keyPattern);
            if (indexType == IndexNames::GEO_HAYSTACK) {
                return {ErrorCodes::CannotCreateIndex,
                        str::stream()
                            << "GeoHaystack indexes cannot be created in version 4.9 and above"};
            }

            if (apiStrict && indexType == IndexNames::TEXT) {
                return {ErrorCodes::APIStrictError,
                        str::stream()
                            << indexType << " indexes cannot be created with apiStrict: true"};
            }

            // Here we always validate the key pattern according to the most recent rules, in order
            // to enforce that all new indexes have well-formed key patterns.
            Status keyPatternValidateStatus =
                validateKeyPattern(keyPattern, IndexDescriptor::kLatestIndexVersion);
            if (!keyPatternValidateStatus.isOK()) {
                return keyPatternValidateStatus;
            }

            for (const auto& keyElement : indexSpecElem.Obj()) {
                if (keyElement.type() == String && keyElement.str().empty()) {
                    return {ErrorCodes::CannotCreateIndex,
                            str::stream()
                                << "Values in the index key pattern cannot be empty strings"};
                }
            }

            hasKeyPatternField = true;
        } else if (IndexDescriptor::kIndexNameFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::String) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kIndexNameFieldName
                            << "' must be a string, but got " << typeName(indexSpecElem.type())};
            }

            hasIndexNameField = true;
        } else if (IndexDescriptor::kHiddenFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::Bool) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kIndexNameFieldName
                            << "' must be a bool, but got " << typeName(indexSpecElem.type())};
            }

        } else if (IndexDescriptor::kNamespaceFieldName == indexSpecElemFieldName) {
            hasNamespaceField = true;
        } else if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
            if (!indexSpecElem.isNumber()) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kIndexVersionFieldName
                            << "' must be a number, but got " << typeName(indexSpecElem.type())};
            }

            auto requestedIndexVersionAsInt = representAs<int>(indexSpecElem.number());
            if (!requestedIndexVersionAsInt) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "Index version must be representable as a 32-bit integer, but got "
                            << indexSpecElem.toString(false, false)};
            }

            const IndexVersion requestedIndexVersion =
                static_cast<IndexVersion>(*requestedIndexVersionAsInt);
            auto creationAllowedStatus =
                isIndexVersionAllowedForCreation(requestedIndexVersion, indexSpec);
            if (!creationAllowedStatus.isOK()) {
                return creationAllowedStatus;
            }

            hasVersionField = true;
            resolvedIndexVersion = requestedIndexVersion;
        } else if (IndexDescriptor::kCollationFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kCollationFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }

            if (indexSpecElem.Obj().isEmpty()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "The field '" << IndexDescriptor::kCollationFieldName
                                      << "' cannot be an empty object."};
            }

            hasCollationField = true;
        } else if (IndexDescriptor::kPartialFilterExprFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kPartialFilterExprFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }

            // Just use the simple collator, even though the index may have a separate collation
            // specified or may inherit the default collation from the collection. It's legal to
            // parse with the wrong collation, since the collation can be set on a MatchExpression
            // after the fact. Here, we don't bother checking the collation after the fact, since
            // this invocation of the parser is just for validity checking. It's also legal to parse
            // with an empty namespace string, because we are only doing validity checking and not
            // resolving the expression against a given namespace.
            auto simpleCollator = nullptr;
            boost::intrusive_ptr<ExpressionContext> expCtx(
                new ExpressionContext(opCtx, simpleCollator, NamespaceString()));

            // Special match expression features (e.g. $jsonSchema, $expr, ...) are not allowed in a
            // partialFilterExpression on index creation.
            auto statusWithMatcher =
                MatchExpressionParser::parse(indexSpecElem.Obj(),
                                             std::move(expCtx),
                                             ExtensionsCallbackNoop(),
                                             MatchExpressionParser::kBanAllSpecialFeatures);
            if (!statusWithMatcher.isOK()) {
                return statusWithMatcher.getStatus();
            }
        } else if (IndexDescriptor::kPathProjectionFieldName == indexSpecElemFieldName) {
            const auto key = indexSpec.getObjectField(IndexDescriptor::kKeyPatternFieldName);
            if (IndexNames::findPluginName(key) != IndexNames::WILDCARD) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "The field '" << IndexDescriptor::kPathProjectionFieldName
                            << "' is only allowed in an '" << IndexNames::WILDCARD << "' index"};
            }
            if (indexSpecElem.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "The field '" << IndexDescriptor::kPathProjectionFieldName
                                      << "' must be a non-empty object, but got "
                                      << typeName(indexSpecElem.type())};
            }
            if (!key.hasField("$**")) {
                return {ErrorCodes::FailedToParse,
                        str::stream()
                            << "The field '" << IndexDescriptor::kPathProjectionFieldName
                            << "' is only allowed when '" << IndexDescriptor::kKeyPatternFieldName
                            << "' is {\"$**\": ±1}"};
            }

            if (indexSpecElem.embeddedObject().isEmpty()) {
                return {ErrorCodes::FailedToParse,
                        str::stream() << "The '" << IndexDescriptor::kPathProjectionFieldName
                                      << "' field can't be an empty object"};
            }
            try {
                // We use WildcardKeyGenerator::createProjectionExec to parse and validate the path
                // projection spec.
                WildcardKeyGenerator::createProjectionExecutor(key, indexSpecElem.embeddedObject());
            } catch (const DBException& ex) {
                return ex.toStatus(str::stream() << "Failed to parse: "
                                                 << IndexDescriptor::kPathProjectionFieldName);
            }
        } else if (IndexDescriptor::kWeightsFieldName == indexSpecElemFieldName) {
            if (!indexSpecElem.isABSONObj() && indexSpecElem.type() != String) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << indexSpecElemFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }
            hasWeightsField = true;
        } else if (IndexDescriptor::kGeoHaystackBucketSize == indexSpecElemFieldName) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream()
                        << "The 'bucketSize' parameter is disallowed because "
                           "geoHaystack indexes are no longer supported in version 4.9 and above"};
        } else if ((IndexDescriptor::kBackgroundFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kUniqueFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kSparseFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::k2dsphereCoarsestIndexedLevel == indexSpecElemFieldName ||
                    IndexDescriptor::k2dsphereFinestIndexedLevel == indexSpecElemFieldName ||
                    IndexDescriptor::kDropDuplicatesFieldName == indexSpecElemFieldName) &&
                   !indexSpecElem.isNumber() && !indexSpecElem.isBoolean()) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "The field '" << indexSpecElemFieldName << " has value "
                                  << indexSpecElem.toString()
                                  << ", which is not convertible to bool"};
        } else if ((IndexDescriptor::kDefaultLanguageFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kLanguageOverrideFieldName == indexSpecElemFieldName) &&
                   indexSpecElem.type() != BSONType::String) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "The field '" << indexSpecElemFieldName
                                  << "' must be a string, but got "
                                  << typeName(indexSpecElem.type())};
        } else if ((IndexDescriptor::k2dsphereVersionFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kTextVersionFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::k2dIndexBitsFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::k2dIndexMinFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::k2dIndexMaxFieldName == indexSpecElemFieldName) &&
                   !indexSpecElem.isNumber()) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "The field '" << indexSpecElemFieldName
                                  << "' must be a number, but got "
                                  << typeName(indexSpecElem.type())};
        } else {
            // We can assume field name is valid at this point. Validation of fieldname is handled
            // prior to this in validateIndexSpecFieldNames().
            continue;
        }
    }

    if (!resolvedIndexVersion) {
        resolvedIndexVersion = IndexDescriptor::getDefaultIndexVersion();
    }

    if (!hasKeyPatternField) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "The '" << IndexDescriptor::kKeyPatternFieldName
                              << "' field is a required property of an index specification"};
    }

    if (!hasIndexNameField) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "The '" << IndexDescriptor::kIndexNameFieldName
                              << "' field is a required property of an index specification"};
    }

    if (hasCollationField && *resolvedIndexVersion < IndexVersion::kV2) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Invalid index specification " << indexSpec
                              << "; cannot create an index with the '"
                              << IndexDescriptor::kCollationFieldName << "' option and "
                              << IndexDescriptor::kIndexVersionFieldName << "="
                              << static_cast<int>(*resolvedIndexVersion)};
    }

    if (indexType != IndexNames::TEXT && hasWeightsField) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Invalid index specification " << indexSpec << "; the field '"
                              << IndexDescriptor::kWeightsFieldName
                              << "' can only be specified with text indexes"};
    }

    BSONObj modifiedSpec = indexSpec;

    // Ignore any 'ns' field in the index spec because this field is dropped post-4.0. Don't remove
    // the field during repair, as repair may run on old data files (version 3.6 and 4.0) that
    // require the field to be present.
    if (hasNamespaceField && !storageGlobalParams.repair) {
        modifiedSpec = modifiedSpec.removeField(IndexDescriptor::kNamespaceFieldName);
    }

    if (!hasVersionField) {
        // We create a new index specification with the 'v' field set as 'defaultIndexVersion' if
        // the field was omitted.
        BSONObj versionObj = BSON(IndexDescriptor::kIndexVersionFieldName
                                  << static_cast<int>(*resolvedIndexVersion));
        modifiedSpec = modifiedSpec.addField(versionObj.firstElement());
    }

    return modifiedSpec;
}

Status validateIdIndexSpec(const BSONObj& indexSpec) {
    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (!allowedIdIndexFieldNames.count(indexSpecElemFieldName)) {
            return {
                ErrorCodes::InvalidIndexSpecificationOption,
                str::stream() << "The field '" << indexSpecElemFieldName
                              << "' is not valid for an _id index specification. Specification: "
                              << indexSpec};
        }
    }

    auto keyPatternElem = indexSpec[IndexDescriptor::kKeyPatternFieldName];
    // validateIndexSpec() should have already verified that 'keyPatternElem' is an object.
    invariant(keyPatternElem.type() == BSONType::Object);
    if (SimpleBSONObjComparator::kInstance.evaluate(keyPatternElem.Obj() != BSON("_id" << 1))) {
        return {ErrorCodes::BadValue,
                str::stream() << "The field '" << IndexDescriptor::kKeyPatternFieldName
                              << "' for an _id index must be {_id: 1}, but got "
                              << keyPatternElem.Obj()};
    }

    if (!indexSpec[IndexDescriptor::kHiddenFieldName].eoo()) {
        return Status(ErrorCodes::BadValue, "can't hide _id index");
    }

    return Status::OK();
}

/**
 * Top-level index spec field names are validated here. When adding a new field with a document as
 * value, is the the sub-module's responsibility to ensure that the content is valid and that only
 * expected fields are present at creation time
 */
Status validateIndexSpecFieldNames(const BSONObj& indexSpec) {
    if (MONGO_unlikely(skipIndexCreateFieldNameValidation.shouldFail())) {
        return Status::OK();
    }

    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (!allowedFieldNames.count(indexSpecElemFieldName)) {
            return {ErrorCodes::InvalidIndexSpecificationOption,
                    str::stream() << "The field '" << indexSpecElemFieldName
                                  << "' is not valid for an index specification. Specification: "
                                  << indexSpec};
        }
    }

    return Status::OK();
}

StatusWith<BSONObj> validateIndexSpecCollation(OperationContext* opCtx,
                                               const BSONObj& indexSpec,
                                               const CollatorInterface* defaultCollator) {
    if (auto collationElem = indexSpec[IndexDescriptor::kCollationFieldName]) {
        // validateIndexSpec() should have already verified that 'collationElem' is an object.
        invariant(collationElem.type() == BSONType::Object);

        auto collator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                            ->makeFromBSON(collationElem.Obj());
        if (!collator.isOK()) {
            return collator.getStatus();
        }

        if (collator.getValue()) {
            // If the collator factory returned a non-null collator, then inject the entire
            // collation specification into the index specification. This is necessary to fill
            // in any options that the user omitted.
            BSONObjBuilder bob;

            for (auto&& indexSpecElem : indexSpec) {
                if (IndexDescriptor::kCollationFieldName != indexSpecElem.fieldNameStringData()) {
                    bob.append(indexSpecElem);
                }
            }
            bob.append(IndexDescriptor::kCollationFieldName,
                       collator.getValue()->getSpec().toBSON());

            return bob.obj();
        } else {
            // If the collator factory returned a null collator (representing the "simple"
            // collation), then we simply omit the "collation" from the index specification.
            // This is desirable to make the representation for the "simple" collation
            // consistent between v=1 and v=2 indexes.
            return indexSpec.removeField(IndexDescriptor::kCollationFieldName);
        }
    } else if (defaultCollator) {
        // validateIndexSpec() should have added the "v" field if it was not present and
        // verified that 'versionElem' is a number.
        auto versionElem = indexSpec[IndexDescriptor::kIndexVersionFieldName];
        invariant(versionElem.isNumber());

        if (IndexVersion::kV2 <= static_cast<IndexVersion>(versionElem.numberInt())) {
            // The user did not specify an explicit collation for this index and the collection
            // has a default collator. If we're building a v=2 index, then we should inherit the
            // collection default. However, if we're building a v=1 index, then we're implicitly
            // building an index that's using the "simple" collation.
            BSONObjBuilder bob;

            bob.appendElements(indexSpec);
            bob.append(IndexDescriptor::kCollationFieldName, defaultCollator->getSpec().toBSON());

            return bob.obj();
        }
    }
    return indexSpec;
}

Status validateExpireAfterSeconds(std::int64_t expireAfterSeconds) {
    if (expireAfterSeconds < 0) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                              << "' option cannot be less than 0"};
    }

    const std::string tooLargeErr = str::stream()
        << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
        << "' option must be within an acceptable range, try a lower number";

    // There are two cases where we can encounter an issue here.
    // The first case is when we try to cast to millseconds from seconds, which could cause an
    // overflow. The second case is where 'expireAfterSeconds' is larger than the current epoch
    // time.
    if (expireAfterSeconds > std::numeric_limits<std::int64_t>::max() / 1000) {
        return {ErrorCodes::InvalidOptions, tooLargeErr};
    }
    auto expireAfterMillis = duration_cast<Milliseconds>(Seconds(expireAfterSeconds));
    if (expireAfterMillis > Date_t::now().toDurationSinceEpoch()) {
        return {ErrorCodes::InvalidOptions, tooLargeErr};
    }
    return Status::OK();
}

Status validateIndexSpecTTL(const BSONObj& indexSpec) {
    if (!indexSpec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName)) {
        return Status::OK();
    }

    const BSONElement expireAfterSecondsElt =
        indexSpec[IndexDescriptor::kExpireAfterSecondsFieldName];
    if (!expireAfterSecondsElt.isNumber()) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                              << "' option must be numeric, but received a type of '"
                              << typeName(expireAfterSecondsElt.type())
                              << "'. Index spec: " << indexSpec};
    }

    if (auto status = validateExpireAfterSeconds(expireAfterSecondsElt.safeNumberLong());
        !status.isOK()) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << status.reason() << "index spec: " << indexSpec};
    }

    const BSONObj key = indexSpec["key"].Obj();
    if (key.nFields() != 1) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "TTL indexes are single-field indexes, compound indexes do "
                                 "not support TTL. Index spec: "
                              << indexSpec};
    }

    return Status::OK();
}

bool isIndexAllowedInAPIVersion1(const IndexDescriptor& indexDesc) {
    const auto indexName = IndexNames::findPluginName(indexDesc.keyPattern());
    return indexName != IndexNames::TEXT && indexName != IndexNames::GEO_HAYSTACK &&
        !indexDesc.isSparse();
}

GlobalInitializerRegisterer filterAllowedIndexFieldNamesInitializer(
    "FilterAllowedIndexFieldNames", [](InitializerContext* service) {
        if (filterAllowedIndexFieldNames)
            filterAllowedIndexFieldNames(allowedFieldNames);
        return Status::OK();
    });

}  // namespace index_key_validate
}  // namespace mongo
