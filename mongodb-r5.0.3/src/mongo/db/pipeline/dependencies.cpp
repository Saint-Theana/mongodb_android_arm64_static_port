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

#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/str.h"

namespace mongo {

BSONObj DepsTracker::toProjectionWithoutMetadata(
    TruncateToRootLevel truncationBehavior /*= TruncateToRootLevel::no*/) const {
    BSONObjBuilder bb;

    if (needWholeDocument) {
        return bb.obj();
    }

    if (fields.empty()) {
        // We need no user-level fields (as we would if this was logically a count). Since there is
        // no way of expressing a projection that indicates no depencies, we return an empty
        // projection.
        return bb.obj();
    }

    bool idSpecified = false;
    std::string last;
    for (const auto& field : fields) {
        if (str::startsWith(field, "_id") && (field.size() == 3 || field[3] == '.')) {
            idSpecified = true;
        }

        if (!last.empty() && str::startsWith(field, last)) {
            // we are including a parent of *it so we don't need to include this field
            // explicitly. This logic relies on on set iterators going in lexicographic order so
            // that a string is always directly before of all fields it prefixes.
            continue;
        }

        // Check that the field requested is a valid field name in the agg language. This
        // constructor will throw if it isn't.
        FieldPath fp(field);

        if (truncationBehavior == TruncateToRootLevel::yes) {
            last = fp.front().toString() + '.';
            bb.append(fp.front(), 1);
        } else {
            last = field + '.';
            bb.append(field, 1);
        }
    }

    if (!idSpecified) {
        bb.append("_id", 0);
    }

    return bb.obj();
}

void DepsTracker::setNeedsMetadata(DocumentMetadataFields::MetaType type, bool required) {
    uassert(40218,
            str::stream() << "query requires " << type << " metadata, but it is not available",
            !required || !_unavailableMetadata[type]);

    // If the metadata type is not required, then it should not be recorded as a metadata
    // dependency.
    invariant(required || !_metadataDeps[type]);
    _metadataDeps[type] = required;
}
}  // namespace mongo
