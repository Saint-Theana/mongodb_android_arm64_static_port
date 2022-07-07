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

#include "mongo/db/update/v1_log_builder.h"

#include "mongo/db/update/runtime_update_path.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/util/str.h"

namespace mongo {

using mutablebson::Element;

namespace {
const char kSet[] = "$set";
const char kUnset[] = "$unset";
}  // namespace

V1LogBuilder::V1LogBuilder(mutablebson::Element logRoot, bool includeVersionField)
    : _logRoot(logRoot),
      _setAccumulator(_logRoot.getDocument().end()),
      _unsetAccumulator(_setAccumulator) {
    invariant(logRoot.isType(mongo::Object));
    invariant(!logRoot.hasChildren());

    if (includeVersionField) {
        auto version = logRoot.getDocument().makeElementInt(
            kUpdateOplogEntryVersionFieldName,
            static_cast<int>(UpdateOplogEntryVersion::kUpdateNodeV1));
        invariant(_logRoot.pushFront(version).isOK());
    }
}

Status V1LogBuilder::addToSection(Element newElt, Element* section, const char* sectionName) {
    // If we don't already have this section, try to create it now.
    if (!section->ok()) {
        mutablebson::Document& doc = _logRoot.getDocument();

        // We should not already have an element with the section name under the root.
        dassert(_logRoot[sectionName] == doc.end());

        // Construct a new object element to represent this section in the log.
        const Element newElement = doc.makeElementObject(sectionName);
        if (!newElement.ok())
            return Status(ErrorCodes::InternalError,
                          "V1LogBuilder: failed to construct Object Element for $set/$unset");

        // Enqueue the new section under the root, and record it as our out parameter.
        Status result = _logRoot.pushBack(newElement);
        if (!result.isOK())
            return result;
        *section = newElement;
    }

    // Whatever transpired, we should now have an ok accumulator for the section, and not
    // have a replacement accumulator.
    dassert(section->ok());

    // Enqueue the provided element to the section and propagate the result.
    return section->pushBack(newElt);
}

Status V1LogBuilder::addToSets(Element elt) {
    return addToSection(elt, &_setAccumulator, kSet);
}

Status V1LogBuilder::addToSetsWithNewFieldName(StringData name, const mutablebson::Element val) {
    mutablebson::Element elemToSet = _logRoot.getDocument().makeElementWithNewFieldName(name, val);
    if (!elemToSet.ok())
        return Status(ErrorCodes::InternalError,
                      str::stream()
                          << "Could not create new '" << name << "' element from existing element '"
                          << val.getFieldName() << "' of type " << typeName(val.getType()));

    return addToSets(elemToSet);
}

Status V1LogBuilder::addToSetsWithNewFieldName(StringData name, const BSONElement& val) {
    mutablebson::Element elemToSet = _logRoot.getDocument().makeElementWithNewFieldName(name, val);
    if (!elemToSet.ok())
        return Status(ErrorCodes::InternalError,
                      str::stream()
                          << "Could not create new '" << name << "' element from existing element '"
                          << val.fieldName() << "' of type " << typeName(val.type()));

    return addToSets(elemToSet);
}

Status V1LogBuilder::addToUnsets(StringData path) {
    mutablebson::Element logElement = _logRoot.getDocument().makeElementBool(path, true);
    if (!logElement.ok())
        return Status(ErrorCodes::InternalError,
                      str::stream() << "Cannot create $unset oplog entry for path" << path);

    return addToSection(logElement, &_unsetAccumulator, kUnset);
}

Status V1LogBuilder::logUpdatedField(const RuntimeUpdatePath& path, mutablebson::Element elt) {
    return addToSetsWithNewFieldName(path.fieldRef().dottedField(), elt);
}

Status V1LogBuilder::logCreatedField(const RuntimeUpdatePath& path,
                                     int idxOfFirstNewComponent,
                                     mutablebson::Element elt) {
    return addToSetsWithNewFieldName(path.fieldRef().dottedField(), elt);
}

Status V1LogBuilder::logCreatedField(const RuntimeUpdatePath& path,
                                     int idxOfFirstNewComponent,
                                     BSONElement elt) {
    return addToSetsWithNewFieldName(path.fieldRef().dottedField(), elt);
}

Status V1LogBuilder::logDeletedField(const RuntimeUpdatePath& path) {
    return addToUnsets(path.fieldRef().dottedField());
}
}  // namespace mongo
