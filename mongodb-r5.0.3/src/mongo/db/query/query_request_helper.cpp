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

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_request_helper.h"

#include <memory>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/dbmessage.h"

namespace mongo {

namespace query_request_helper {
namespace {

/**
 * Initializes options based on the value of the 'options' bit vector.
 *
 * This contains flags such as tailable, exhaust, and noCursorTimeout.
 */
void initFromInt(int options, FindCommandRequest* findCommand) {
    bool tailable = (options & QueryOption_CursorTailable) != 0;
    bool awaitData = (options & QueryOption_AwaitData) != 0;
    if (awaitData) {
        findCommand->setAwaitData(true);
    }
    if (tailable) {
        findCommand->setTailable(true);
    }

    if ((options & QueryOption_NoCursorTimeout) != 0) {
        findCommand->setNoCursorTimeout(true);
    }
    if ((options & QueryOption_PartialResults) != 0) {
        findCommand->setAllowPartialResults(true);
    }
}

/**
 * Updates the projection object with a $meta projection for the showRecordId option.
 */
void addShowRecordIdMetaProj(FindCommandRequest* findCommand) {
    if (findCommand->getProjection()["$recordId"]) {
        // There's already some projection on $recordId. Don't overwrite it.
        return;
    }

    BSONObjBuilder projBob;
    projBob.appendElements(findCommand->getProjection());
    BSONObj metaRecordId = BSON("$recordId" << BSON("$meta" << query_request_helper::metaRecordId));
    projBob.append(metaRecordId.firstElement());
    findCommand->setProjection(projBob.obj());
}

/**
 * Add the meta projection to this object if needed.
 */
void addMetaProjection(FindCommandRequest* findCommand) {
    if (findCommand->getShowRecordId()) {
        addShowRecordIdMetaProj(findCommand);
    }
}

Status initFullQuery(const BSONObj& top, FindCommandRequest* findCommand, bool* explain) {
    BSONObjIterator i(top);

    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        if (name == "$orderby" || name == "orderby") {
            if (Object == e.type()) {
                findCommand->setSort(e.embeddedObject().getOwned());
            } else if (Array == e.type()) {
                findCommand->setSort(e.embeddedObject());

                // TODO: Is this ever used?  I don't think so.
                // Quote:
                // This is for languages whose "objects" are not well ordered (JSON is well
                // ordered).
                // [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
                // note: this is slow, but that is ok as order will have very few pieces
                BSONObjBuilder b;
                char p[2] = "0";

                while (1) {
                    BSONObj j = findCommand->getSort().getObjectField(p);
                    if (j.isEmpty()) {
                        break;
                    }
                    BSONElement e = j.firstElement();
                    if (e.eoo()) {
                        return Status(ErrorCodes::BadValue, "bad order array");
                    }
                    if (!e.isNumber()) {
                        return Status(ErrorCodes::BadValue, "bad order array [2]");
                    }
                    b.append(e);
                    (*p)++;
                    if (!(*p <= '9')) {
                        return Status(ErrorCodes::BadValue, "too many ordering elements");
                    }
                }

                findCommand->setSort(b.obj());
            } else {
                return Status(ErrorCodes::BadValue, "sort must be object or array");
            }
        } else if (name.startsWith("$")) {
            name = name.substr(1);  // chop first char
            if (name == "explain") {
                // Won't throw.
                *explain = e.trueValue();
            } else if (name == "min") {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$min must be a BSONObj");
                }
                findCommand->setMin(e.embeddedObject().getOwned());
            } else if (name == "max") {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$max must be a BSONObj");
                }
                findCommand->setMax(e.embeddedObject().getOwned());
            } else if (name == "hint") {
                if (e.isABSONObj()) {
                    findCommand->setHint(e.embeddedObject().getOwned());
                } else if (String == e.type()) {
                    findCommand->setHint(e.wrap());
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "$hint must be either a string or nested object");
                }
            } else if (name == "returnKey") {
                // Won't throw.
                if (e.trueValue()) {
                    findCommand->setReturnKey(true);
                }
            } else if (name == "showDiskLoc") {
                // Won't throw.
                if (e.trueValue()) {
                    findCommand->setShowRecordId(true);
                    addShowRecordIdMetaProj(findCommand);
                }
            } else if (name == "maxTimeMS") {
                StatusWith<int> maxTimeMS = parseMaxTimeMS(e);
                if (!maxTimeMS.isOK()) {
                    return maxTimeMS.getStatus();
                }
                findCommand->setMaxTimeMS(maxTimeMS.getValue());
            }
        }
    }

    return Status::OK();
}

Status initFindCommandRequest(int ntoskip,
                              int ntoreturn,
                              int queryOptions,
                              const BSONObj& queryObj,
                              const BSONObj& proj,
                              bool fromQueryMessage,
                              FindCommandRequest* findCommand,
                              bool* explain) {
    if (!proj.isEmpty()) {
        findCommand->setProjection(proj.getOwned());
    }
    if (ntoskip) {
        findCommand->setSkip(ntoskip);
    }

    if (ntoreturn) {
        if (ntoreturn < 0) {
            if (ntoreturn == std::numeric_limits<int>::min()) {
                // ntoreturn is negative but can't be negated.
                return Status(ErrorCodes::BadValue, "bad ntoreturn value in query");
            }
            findCommand->setNtoreturn(-ntoreturn);
            findCommand->setSingleBatch(true);
        } else {
            findCommand->setNtoreturn(ntoreturn);
        }
    }

    // An ntoreturn of 1 is special because it also means to return at most one batch.
    if (findCommand->getNtoreturn().value_or(0) == 1) {
        findCommand->setSingleBatch(true);
    }

    // Initialize flags passed as 'queryOptions' bit vector.
    initFromInt(queryOptions, findCommand);

    if (fromQueryMessage) {
        BSONElement queryField = queryObj["query"];
        if (!queryField.isABSONObj()) {
            queryField = queryObj["$query"];
        }
        if (queryField.isABSONObj()) {
            findCommand->setFilter(queryField.embeddedObject().getOwned());
            Status status = initFullQuery(queryObj, findCommand, explain);
            if (!status.isOK()) {
                return status;
            }
        } else {
            findCommand->setFilter(queryObj.getOwned());
        }
        // It's not possible to specify readConcern in a legacy query message, so initialize it to
        // an empty readConcern object, ie. equivalent to `readConcern: {}`.  This ensures that
        // mongos passes this empty readConcern to shards.
        findCommand->setReadConcern(BSONObj());
    } else {
        // This is the debugging code path.
        findCommand->setFilter(queryObj.getOwned());
    }

    return validateFindCommandRequest(*findCommand);
}

}  // namespace

Status validateGetMoreCollectionName(StringData collectionName) {
    if (collectionName.empty()) {
        return Status(ErrorCodes::InvalidNamespace, "Collection names cannot be empty");
    }
    if (collectionName[0] == '.') {
        return Status(ErrorCodes::InvalidNamespace,
                      "Collection names cannot start with '.': " + collectionName);
    }
    if (collectionName.find('\0') != std::string::npos) {
        return Status(ErrorCodes::InvalidNamespace,
                      "Collection names cannot have embedded null characters");
    }

    return Status::OK();
}

Status validateFindCommandRequest(const FindCommandRequest& findCommand) {
    // Min and Max objects must have the same fields.
    if (!findCommand.getMin().isEmpty() && !findCommand.getMax().isEmpty()) {
        if (!findCommand.getMin().isFieldNamePrefixOf(findCommand.getMax()) ||
            (findCommand.getMin().nFields() != findCommand.getMax().nFields())) {
            return Status(ErrorCodes::Error(51176), "min and max must have the same field names");
        }
    }

    if ((findCommand.getLimit() || findCommand.getBatchSize()) && findCommand.getNtoreturn()) {
        return Status(ErrorCodes::BadValue,
                      "'limit' or 'batchSize' fields can not be set with 'ntoreturn' field.");
    }

    if (query_request_helper::getTailableMode(findCommand) != TailableModeEnum::kNormal) {
        // Tailable cursors cannot have any sort other than {$natural: 1}.
        const BSONObj expectedSort = BSON(query_request_helper::kNaturalSortField << 1);
        if (!findCommand.getSort().isEmpty() &&
            SimpleBSONObjComparator::kInstance.evaluate(findCommand.getSort() != expectedSort)) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with a sort other than {$natural: 1}");
        }

        // Cannot indicate that you want a 'singleBatch' if the cursor is tailable.
        if (findCommand.getSingleBatch()) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with the 'singleBatch' option");
        }
    }

    if (findCommand.getRequestResumeToken()) {
        if (SimpleBSONObjComparator::kInstance.evaluate(
                findCommand.getHint() != BSON(query_request_helper::kNaturalSortField << 1))) {
            return Status(ErrorCodes::BadValue,
                          "hint must be {$natural:1} if 'requestResumeToken' is enabled");
        }
        if (!findCommand.getSort().isEmpty() &&
            SimpleBSONObjComparator::kInstance.evaluate(
                findCommand.getSort() != BSON(query_request_helper::kNaturalSortField << 1))) {
            return Status(ErrorCodes::BadValue,
                          "sort must be unset or {$natural:1} if 'requestResumeToken' is enabled");
        }
        if (!findCommand.getResumeAfter().isEmpty()) {
            if (findCommand.getResumeAfter().nFields() != 1 ||
                (findCommand.getResumeAfter()["$recordId"].type() != BSONType::NumberLong &&
                 findCommand.getResumeAfter()["$recordId"].type() != BSONType::String &&
                 findCommand.getResumeAfter()["$recordId"].type() != BSONType::jstNULL)) {
                return Status(
                    ErrorCodes::BadValue,
                    "Malformed resume token: the '_resumeAfter' object must contain"
                    " exactly one field named '$recordId', of type NumberLong, jstOID or jstNULL.");
            }
        }
    } else if (!findCommand.getResumeAfter().isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      "'requestResumeToken' must be true if 'resumeAfter' is"
                      " specified");
    }

    return Status::OK();
}

void refreshNSS(const NamespaceString& nss, FindCommandRequest* findCommand) {
    if (findCommand->getNamespaceOrUUID().uuid()) {
        auto& nssOrUUID = findCommand->getNamespaceOrUUID();
        nssOrUUID.setNss(nss);
    }
    invariant(findCommand->getNamespaceOrUUID().nss());
}

std::unique_ptr<FindCommandRequest> makeFromFindCommand(const BSONObj& cmdObj,
                                                        boost::optional<NamespaceString> nss,
                                                        bool apiStrict) {

    auto findCommand = std::make_unique<FindCommandRequest>(
        FindCommandRequest::parse(IDLParserErrorContext("FindCommandRequest", apiStrict), cmdObj));

    // If there is an explicit namespace specified overwite it.
    if (nss) {
        auto& nssOrUuid = findCommand->getNamespaceOrUUID();
        nssOrUuid.setNss(*nss);
    }

    addMetaProjection(findCommand.get());

    if (findCommand->getSkip() && *findCommand->getSkip() == 0) {
        findCommand->setSkip(boost::none);
    }
    if (findCommand->getLimit() && *findCommand->getLimit() == 0) {
        findCommand->setLimit(boost::none);
    }
    uassertStatusOK(validateFindCommandRequest(*findCommand));

    return findCommand;
}

std::unique_ptr<FindCommandRequest> makeFromFindCommandForTests(
    const BSONObj& cmdObj, boost::optional<NamespaceString> nss, bool apiStrict) {
    return makeFromFindCommand(cmdObj, nss, apiStrict);
}

bool isTextScoreMeta(BSONElement elt) {
    // elt must be foo: {$meta: "textScore"}
    if (mongo::Object != elt.type()) {
        return false;
    }
    BSONObj metaObj = elt.Obj();
    BSONObjIterator metaIt(metaObj);
    // must have exactly 1 element
    if (!metaIt.more()) {
        return false;
    }
    BSONElement metaElt = metaIt.next();
    if (metaElt.fieldNameStringData() != "$meta") {
        return false;
    }
    if (mongo::String != metaElt.type()) {
        return false;
    }
    if (StringData{metaElt.valuestr()} != metaTextScore) {
        return false;
    }
    // must have exactly 1 element
    if (metaIt.more()) {
        return false;
    }
    return true;
}

void setTailableMode(TailableModeEnum tailableMode, FindCommandRequest* findCommand) {
    if (tailableMode == TailableModeEnum::kTailableAndAwaitData) {
        findCommand->setAwaitData(true);
        findCommand->setTailable(true);
    } else if (tailableMode == TailableModeEnum::kTailable) {
        findCommand->setTailable(true);
    }
}

TailableModeEnum getTailableMode(const FindCommandRequest& findCommand) {
    return uassertStatusOK(
        tailableModeFromBools(findCommand.getTailable(), findCommand.getAwaitData()));
}

void validateCursorResponse(const BSONObj& outputAsBson) {
    if (getTestCommandsEnabled()) {
        CursorInitialReply::parse(IDLParserErrorContext("CursorInitialReply"), outputAsBson);
    }
}

//
// Old QueryRequest parsing code: SOON TO BE DEPRECATED.
//

StatusWith<std::unique_ptr<FindCommandRequest>> fromLegacyQueryMessage(const QueryMessage& qm,
                                                                       bool* explain) {
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceString(qm.ns));

    Status status = initFindCommandRequest(qm.ntoskip,
                                           qm.ntoreturn,
                                           qm.queryOptions,
                                           qm.query,
                                           qm.fields,
                                           true,
                                           findCommand.get(),
                                           explain);
    if (!status.isOK()) {
        return status;
    }

    return std::move(findCommand);
}

StatusWith<std::unique_ptr<FindCommandRequest>> fromLegacyQuery(NamespaceStringOrUUID nssOrUuid,
                                                                const BSONObj& queryObj,
                                                                const BSONObj& proj,
                                                                int ntoskip,
                                                                int ntoreturn,
                                                                int queryOptions,
                                                                bool* explain) {
    auto findCommand = std::make_unique<FindCommandRequest>(std::move(nssOrUuid));

    Status status = initFindCommandRequest(
        ntoskip, ntoreturn, queryOptions, queryObj, proj, true, findCommand.get(), explain);
    if (!status.isOK()) {
        return status;
    }

    return std::move(findCommand);
}

StatusWith<BSONObj> asAggregationCommand(const FindCommandRequest& findCommand) {
    BSONObjBuilder aggregationBuilder;

    // First, check if this query has options that are not supported in aggregation.
    if (!findCommand.getMin().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kMinFieldName
                              << " not supported in aggregation."};
    }
    if (!findCommand.getMax().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kMaxFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getReturnKey()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kReturnKeyFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getShowRecordId()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kShowRecordIdFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getTailable()) {
        return {ErrorCodes::InvalidPipelineOperator,
                "Tailable cursors are not supported in aggregation."};
    }
    if (findCommand.getNoCursorTimeout()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kNoCursorTimeoutFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getAllowPartialResults()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kAllowPartialResultsFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getNtoreturn()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Cannot convert to an aggregation if ntoreturn is set."};
    }
    if (findCommand.getSort()[query_request_helper::kNaturalSortField]) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Sort option " << query_request_helper::kNaturalSortField
                              << " not supported in aggregation."};
    }
    // The aggregation command normally does not support the 'singleBatch' option, but we make a
    // special exception if 'limit' is set to 1.
    if (findCommand.getSingleBatch() && findCommand.getLimit().value_or(0) != 1LL) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kSingleBatchFieldName
                              << " not supported in aggregation."};
    }
    if (findCommand.getReadOnce()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kReadOnceFieldName
                              << " not supported in aggregation."};
    }

    if (findCommand.getAllowSpeculativeMajorityRead()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option "
                              << FindCommandRequest::kAllowSpeculativeMajorityReadFieldName
                              << " not supported in aggregation."};
    }

    if (findCommand.getRequestResumeToken()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kRequestResumeTokenFieldName
                              << " not supported in aggregation."};
    }

    if (!findCommand.getResumeAfter().isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << FindCommandRequest::kResumeAfterFieldName
                              << " not supported in aggregation."};
    }

    // Now that we've successfully validated this QR, begin building the aggregation command.
    aggregationBuilder.append("aggregate",
                              findCommand.getNamespaceOrUUID().nss()
                                  ? findCommand.getNamespaceOrUUID().nss()->coll()
                                  : "");

    // Construct an aggregation pipeline that finds the equivalent documents to this query request.
    BSONArrayBuilder pipelineBuilder(aggregationBuilder.subarrayStart("pipeline"));
    if (!findCommand.getFilter().isEmpty()) {
        BSONObjBuilder matchBuilder(pipelineBuilder.subobjStart());
        matchBuilder.append("$match", findCommand.getFilter());
        matchBuilder.doneFast();
    }
    if (!findCommand.getSort().isEmpty()) {
        BSONObjBuilder sortBuilder(pipelineBuilder.subobjStart());
        sortBuilder.append("$sort", findCommand.getSort());
        sortBuilder.doneFast();
    }
    if (findCommand.getSkip()) {
        BSONObjBuilder skipBuilder(pipelineBuilder.subobjStart());
        skipBuilder.append("$skip", *findCommand.getSkip());
        skipBuilder.doneFast();
    }
    if (findCommand.getLimit()) {
        BSONObjBuilder limitBuilder(pipelineBuilder.subobjStart());
        limitBuilder.append("$limit", *findCommand.getLimit());
        limitBuilder.doneFast();
    }
    if (!findCommand.getProjection().isEmpty()) {
        BSONObjBuilder projectBuilder(pipelineBuilder.subobjStart());
        projectBuilder.append("$project", findCommand.getProjection());
        projectBuilder.doneFast();
    }
    pipelineBuilder.doneFast();

    // The aggregation 'cursor' option is always set, regardless of the presence of batchSize.
    BSONObjBuilder batchSizeBuilder(aggregationBuilder.subobjStart("cursor"));
    if (findCommand.getBatchSize()) {
        batchSizeBuilder.append(FindCommandRequest::kBatchSizeFieldName,
                                *findCommand.getBatchSize());
    }
    batchSizeBuilder.doneFast();

    // Other options.
    aggregationBuilder.append("collation", findCommand.getCollation());
    int maxTimeMS = findCommand.getMaxTimeMS() ? static_cast<int>(*findCommand.getMaxTimeMS()) : 0;
    if (maxTimeMS > 0) {
        aggregationBuilder.append(cmdOptionMaxTimeMS, maxTimeMS);
    }
    if (!findCommand.getHint().isEmpty()) {
        aggregationBuilder.append(FindCommandRequest::kHintFieldName, findCommand.getHint());
    }
    if (findCommand.getReadConcern()) {
        aggregationBuilder.append("readConcern", *findCommand.getReadConcern());
    }
    if (!findCommand.getUnwrappedReadPref().isEmpty()) {
        aggregationBuilder.append(FindCommandRequest::kUnwrappedReadPrefFieldName,
                                  findCommand.getUnwrappedReadPref());
    }
    if (findCommand.getAllowDiskUse()) {
        aggregationBuilder.append(FindCommandRequest::kAllowDiskUseFieldName,
                                  static_cast<bool>(findCommand.getAllowDiskUse()));
    }
    if (findCommand.getLegacyRuntimeConstants()) {
        BSONObjBuilder rtcBuilder(
            aggregationBuilder.subobjStart(FindCommandRequest::kLegacyRuntimeConstantsFieldName));
        findCommand.getLegacyRuntimeConstants()->serialize(&rtcBuilder);
        rtcBuilder.doneFast();
    }
    if (findCommand.getLet()) {
        aggregationBuilder.append(FindCommandRequest::kLetFieldName, *findCommand.getLet());
    }
    return StatusWith<BSONObj>(aggregationBuilder.obj());
}

}  // namespace query_request_helper
}  // namespace mongo
