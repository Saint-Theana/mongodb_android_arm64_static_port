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

#pragma once

#include <string>

#include <boost/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/db/catalog/collection_options_gen.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/uuid.h"

namespace mongo {

class CollatorFactoryInterface;
class CreateCommand;

/**
 * A CollectionUUID is a 128-bit unique identifier, per RFC 4122, v4. for a database collection.
 * Newly created collections are assigned a new randomly generated CollectionUUID. In a replica-set
 * or a sharded cluster, all nodes will use the same UUID for a given collection. The UUID stays
 * with the collection until it is dropped, so even across renames. A copied collection must have
 * its own new unique UUID though.
 */
using CollectionUUID = UUID;

using OptionalCollectionUUID = boost::optional<CollectionUUID>;

struct CollectionOptions {
    /**
     * Returns true if the options indicate the namespace is a view.
     */
    bool isView() const;

    /**
     * The 'uuid' member is a collection property stored in the catalog with user-settable options,
     * but is not valid for the user to specify as collection option. So, parsing commands must
     * reject the 'uuid' property, but parsing stored options must accept it.
     */
    enum ParseKind { parseForCommand, parseForStorage };

    /**
     * Confirms that collection options can be converted to BSON and back without errors.
     */
    Status validateForStorage() const;

    /**
     * Parses the collection 'options' into the appropriate struct fields.
     *
     * When 'kind' is set to ParseKind::parseForStorage, the 'uuid' field is parsed,
     * otherwise the 'uuid' field is not parsed.
     *
     * When 'kind' is set to ParseKind::parseForCommand, the 'idIndex' field is parsed,
     * otherwise the 'idIndex' field is not parsed.
     */
    static StatusWith<CollectionOptions> parse(const BSONObj& options,
                                               ParseKind kind = parseForCommand);

    /**
     * Converts a client "create" command invocation.
     */
    static CollectionOptions fromCreateCommand(const CreateCommand& cmd);

    /**
     * Serialize to BSON. The 'includeUUID' parameter is used for the listCollections command to do
     * special formatting for the uuid. Aside from the UUID, if 'includeFields' is non-empty, only
     * the specified fields will be included.
     */
    void appendBSON(BSONObjBuilder* builder,
                    bool includeUUID,
                    const StringDataSet& includeFields) const;
    BSONObj toBSON(bool includeUUID = true, const StringDataSet& includeFields = {}) const;

    /**
     * Returns true if given options matches to this.
     *
     * Uses the collatorFactory to normalize the collation property being compared.
     *
     * Note: ignores idIndex property.
     */
    bool matchesStorageOptions(const CollectionOptions& other,
                               CollatorFactoryInterface* collatorFactory) const;

    // Collection UUID. If not set, specifies that the storage engine should generate the UUID (for
    // a new collection). For an existing collection parsed for storage, it will always be present.
    OptionalCollectionUUID uuid;

    bool capped = false;
    long long cappedSize = 0;
    long long cappedMaxDocs = 0;

    // The behavior of _id index creation when collection created
    void setNoIdIndex() {
        autoIndexId = NO;
    }
    enum {
        DEFAULT,  // currently yes for most collections, NO for some system ones
        YES,      // create _id index
        NO        // do not create _id index
    } autoIndexId = DEFAULT;

    bool temp = false;
    bool recordPreImages = false;

    // Storage engine collection options. Always owned or empty.
    BSONObj storageEngine;

    // Default options for indexes created on the collection.
    IndexOptionDefaults indexOptionDefaults;

    // Index specs for the _id index.
    BSONObj idIndex;

    // Always owned or empty.
    BSONObj validator;
    boost::optional<ValidationActionEnum> validationAction;
    boost::optional<ValidationLevelEnum> validationLevel;

    // The namespace's default collation.
    BSONObj collation;

    // Whether this collection is clustered on _id.
    bool clusteredIndex = false;

    // If present, the number of seconds after which old data should be deleted. Only for
    // collections which are clustered on _id.
    boost::optional<int64_t> expireAfterSeconds;

    // View-related options.
    // The namespace of the view or collection that "backs" this view, or the empty string if this
    // collection is not a view.
    std::string viewOn;
    // The aggregation pipeline that defines this view.
    BSONObj pipeline;

    // The options that define the time-series collection, or boost::none if not a time-series
    // collection.
    boost::optional<TimeseriesOptions> timeseries;
};
}  // namespace mongo
