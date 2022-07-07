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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/record_id_helpers.h"

#include <limits>

#include "mongo/bson/bson_validate.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/debug_util.h"

namespace mongo {
namespace record_id_helpers {

StatusWith<RecordId> keyForOptime(const Timestamp& opTime) {
    // Make sure secs and inc wouldn't be negative if treated as signed. This ensures that they
    // don't sort differently when put in a RecordId. It also avoids issues with Null/Invalid
    // RecordIds
    if (opTime.getSecs() > uint32_t(std::numeric_limits<int32_t>::max()))
        return StatusWith<RecordId>(ErrorCodes::BadValue, "ts secs too high");

    if (opTime.getInc() > uint32_t(std::numeric_limits<int32_t>::max()))
        return StatusWith<RecordId>(ErrorCodes::BadValue, "ts inc too high");

    const RecordId out = RecordId(opTime.getSecs(), opTime.getInc());
    if (out <= RecordId::minLong())
        return StatusWith<RecordId>(ErrorCodes::BadValue, "ts too low");
    if (out >= RecordId::maxLong())
        return StatusWith<RecordId>(ErrorCodes::BadValue, "ts too high");

    return StatusWith<RecordId>(out);
}

/**
 * data and len must be the arguments from RecordStore::insert() on an oplog collection.
 */
StatusWith<RecordId> extractKeyOptime(const char* data, int len) {
    // Use the latest BSON validation version. Oplog entries are allowed to contain decimal data
    // even if decimal is disabled.
    if (kDebugBuild)
        invariant(validateBSON(data, len).isOK());

    const BSONObj obj(data);
    const BSONElement elem = obj["ts"];
    if (elem.eoo())
        return StatusWith<RecordId>(ErrorCodes::BadValue, "no ts field");
    if (elem.type() != bsonTimestamp)
        return StatusWith<RecordId>(ErrorCodes::BadValue, "ts must be a Timestamp");

    return keyForOptime(elem.timestamp());
}

StatusWith<RecordId> keyForDoc(const BSONObj& doc) {
    // Build a KeyString as the RecordId using the "_id" field.
    BSONElement idElem;
    bool foundId = doc.getObjectID(idElem);
    if (!foundId) {
        return {ErrorCodes::BadValue,
                str::stream() << "Document " << redact(doc) << " is missing the '_id' field"};
    }

    return keyForElem(idElem);
}

RecordId keyForElem(const BSONElement& elem) {
    // Intentionally discard the TypeBits since the type information will be stored in the _id of
    // the original document. The consequence of this behavior is that _id values that compare
    // similarly, but are of different types may not be used concurrently.
    KeyString::Builder keyBuilder(KeyString::Version::kLatestVersion);
    keyBuilder.appendBSONElement(elem);
    return RecordId(keyBuilder.getBuffer(), keyBuilder.getSize());
}

RecordId keyForOID(OID oid) {
    KeyString::Builder keyBuilder(KeyString::Version::kLatestVersion);
    keyBuilder.appendOID(oid);
    return RecordId(keyBuilder.getBuffer(), keyBuilder.getSize());
}

void appendToBSONAs(RecordId rid, BSONObjBuilder* builder, StringData fieldName) {
    rid.withFormat([&](RecordId::Null) { builder->appendNull(fieldName); },
                   [&](int64_t val) { builder->append(fieldName, val); },
                   [&](const char* str, int len) {
                       KeyString::appendSingleFieldToBSONAs(str, len, fieldName, builder);
                   });
}

BSONObj toBSONAs(RecordId rid, StringData fieldName) {
    BSONObjBuilder builder;
    appendToBSONAs(rid, &builder, fieldName);
    return builder.obj();
}

namespace {
static constexpr int64_t kMinReservedLong = RecordId::kMaxRepr - (1024 * 1024);
// All RecordId strings that start with FF are considered reserved. This also happens to be an
// invalid start byte for a KeyString sequence, which is used to encode RecordId binary strings.
static constexpr char kReservedStrPrefix = static_cast<char>(0xFF);
}  // namespace

RecordId reservedIdFor(ReservationId res, KeyFormat keyFormat) {
    // There is only one reservation at the moment.
    invariant(res == ReservationId::kWildcardMultikeyMetadataId);
    if (keyFormat == KeyFormat::Long) {
        return RecordId(kMinReservedLong);
    } else {
        invariant(keyFormat == KeyFormat::String);
        constexpr char reservation[] = {kReservedStrPrefix, 0};
        return RecordId(reservation, sizeof(reservation));
    }
}

bool isReserved(RecordId id) {
    if (id.isNull()) {
        return false;
    }
    if (id.isLong()) {
        return id.getLong() >= kMinReservedLong && id.getLong() < RecordId::kMaxRepr;
    }
    // All RecordId strings that start with FF are considered reserved.
    auto strData = id.getStr();
    return strData.rawData()[0] == kReservedStrPrefix;
}

}  // namespace record_id_helpers
}  // namespace mongo
