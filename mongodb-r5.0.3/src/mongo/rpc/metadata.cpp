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

#include "mongo/rpc/metadata.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/vector_clock.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"

namespace mongo {
namespace rpc {

BSONObj makeEmptyMetadata() {
    return BSONObj();
}

void readRequestMetadata(OperationContext* opCtx,
                         const BSONObj& metadataObj,
                         bool cmdRequiresAuth) {
    BSONElement readPreferenceElem;
    BSONElement configSvrElem;
    BSONElement trackingElem;
    BSONElement clientElem;
    BSONElement helloClientElem;
    BSONElement impersonationElem;
    BSONElement clientOperationKeyElem;

    for (const auto& metadataElem : metadataObj) {
        auto fieldName = metadataElem.fieldNameStringData();
        if (fieldName == "$readPreference") {
            readPreferenceElem = metadataElem;
        } else if (fieldName == ConfigServerMetadata::fieldName()) {
            configSvrElem = metadataElem;
        } else if (fieldName == ClientMetadata::fieldName()) {
            clientElem = metadataElem;
        } else if (fieldName == TrackingMetadata::fieldName()) {
            trackingElem = metadataElem;
        } else if (fieldName == kImpersonationMetadataSectionName) {
            impersonationElem = metadataElem;
        } else if (fieldName == "clientOperationKey"_sd) {
            clientOperationKeyElem = metadataElem;
        }
    }

    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

    if (clientOperationKeyElem &&
        (TestingProctor::instance().isEnabled() ||
         authSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                       ActionType::internal))) {
        auto opKey = uassertStatusOK(UUID::parse(clientOperationKeyElem));
        opCtx->setOperationKey(std::move(opKey));
    }

    if (readPreferenceElem) {
        ReadPreferenceSetting::get(opCtx) =
            uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(readPreferenceElem));
    }

    readImpersonatedUserMetadata(impersonationElem, opCtx);

    // We check for "$client" but not "client" here, because currentOp can filter on "client" as
    // a top-level field.
    if (clientElem) {
        // The '$client' field is populated by mongos when it sends requests to shards on behalf of
        // its own requests. This may or may not be relevant for SERVER-50804.
        ClientMetadata::setFromMetadataForOperation(opCtx, clientElem);
    }

    ConfigServerMetadata::get(opCtx) =
        uassertStatusOK(ConfigServerMetadata::readFromMetadata(configSvrElem));

    TrackingMetadata::get(opCtx) =
        uassertStatusOK(TrackingMetadata::readFromMetadata(trackingElem));

    VectorClock::get(opCtx)->gossipIn(opCtx, metadataObj, !cmdRequiresAuth);
}

namespace {
const auto docSequenceFieldsForCommands = StringMap<std::string>{
    {"insert", "documents"},  //
    {"update", "updates"},
    {"delete", "deletes"},
};

bool isArrayOfObjects(BSONElement array) {
    if (array.type() != Array)
        return false;

    for (auto elem : array.Obj()) {
        if (elem.type() != Object)
            return false;
    }

    return true;
}
}  // namespace

OpMsgRequest upconvertRequest(StringData db, BSONObj cmdObj, int queryFlags) {
    cmdObj = cmdObj.getOwned();  // Usually this is a no-op since it is already owned.

    auto readPrefContainer = BSONObj();
    const StringData firstFieldName = cmdObj.firstElementFieldName();
    if (firstFieldName == "$query" || firstFieldName == "query") {
        // Commands sent over OP_QUERY specify read preference by putting it at the top level and
        // putting the command in a nested field called either query or $query.

        // Check if legacyCommand has an invalid $maxTimeMS option.
        uassert(ErrorCodes::InvalidOptions,
                "cannot use $maxTimeMS query option with commands; use maxTimeMS command option "
                "instead",
                !cmdObj.hasField("$maxTimeMS"));

        if (auto readPref = cmdObj["$readPreference"])
            readPrefContainer = readPref.wrap();

        cmdObj = cmdObj.firstElement().Obj().shareOwnershipWith(cmdObj);
    } else if (auto queryOptions = cmdObj["$queryOptions"]) {
        // Mongos rewrites commands with $readPreference to put it in a field nested inside of
        // $queryOptions. Its command implementations often forward commands in that format to
        // shards. This function is responsible for rewriting it to a format that the shards
        // understand.
        readPrefContainer = queryOptions.Obj().shareOwnershipWith(cmdObj);
        cmdObj = cmdObj.removeField("$queryOptions");
    }

    if (!readPrefContainer.isEmpty()) {
        cmdObj = BSONObjBuilder(std::move(cmdObj)).appendElements(readPrefContainer).obj();
    } else if (!cmdObj.hasField("$readPreference") && (queryFlags & QueryOption_SecondaryOk)) {
        BSONObjBuilder bodyBuilder(std::move(cmdObj));
        ReadPreferenceSetting(ReadPreference::SecondaryPreferred).toContainingBSON(&bodyBuilder);
        cmdObj = bodyBuilder.obj();
    }

    uassert(40621, "$db is not allowed in OP_QUERY requests", !cmdObj.hasField("$db"));

    // Try to move supported array fields into document sequences.
    auto docSequenceIt = docSequenceFieldsForCommands.find(cmdObj.firstElementFieldName());
    auto docSequenceElem = docSequenceIt == docSequenceFieldsForCommands.end()
        ? BSONElement()
        : cmdObj[docSequenceIt->second];
    if (!isArrayOfObjects(docSequenceElem))
        return OpMsgRequest::fromDBAndBody(db, std::move(cmdObj));

    auto docSequenceName = docSequenceElem.fieldNameStringData();

    // Note: removing field before adding "$db" to avoid the need to copy the potentially large
    // array.
    auto out = OpMsgRequest::fromDBAndBody(db, cmdObj.removeField(docSequenceName));
    out.sequences.push_back({docSequenceName.toString()});
    for (auto elem : docSequenceElem.Obj()) {
        out.sequences[0].objs.push_back(elem.Obj().shareOwnershipWith(cmdObj));
    }
    return out;
}

}  // namespace rpc
}  // namespace mongo
