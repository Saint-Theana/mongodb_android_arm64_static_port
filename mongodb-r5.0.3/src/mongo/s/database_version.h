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

#include "mongo/s/database_version_gen.h"

namespace mongo {

/**
 * This class is used to represent a specific version of a Database.
 *
 * Currently it is implemented as a (uuid, [timestamp,] lastMod) triplet, where the
 * timestamp is optional in versions prior 4.9. The uuid is going to be removed soon,
 * since they are not comparable (that's the reason why there is a ComparableDatabaseVersion class).
 *
 * Once uuids are gone, relational operators should be implemented in this class.
 *
 */
class DatabaseVersion : public DatabaseVersionBase {
public:
    using DatabaseVersionBase::getTimestamp;

    DatabaseVersion() = default;

    explicit DatabaseVersion(const BSONObj& obj) {
        DatabaseVersionBase::parseProtected(IDLParserErrorContext("DatabaseVersion"), obj);
    }

    explicit DatabaseVersion(const DatabaseVersionBase& dbv) : DatabaseVersionBase(dbv) {}

    /**
     * Constructor of a DatabaseVersion based on epochs
     */
    explicit DatabaseVersion(mongo::UUID uuid)
        : DatabaseVersion(uuid, boost::none /* timestamp */) {}

    /**
     * Constructor of a DatabaseVersion based on epochs and timestamps
     */
    DatabaseVersion(mongo::UUID uuid, boost::optional<mongo::Timestamp> timestamp)
        : DatabaseVersionBase(1 /* lastMod */) {
        setUuid(uuid);
        setTimestamp(timestamp);
    }

    // Returns a new hardcoded DatabaseVersion value, which is used to distinguish databases that do
    // not have entries in the sharding catalog, namely 'config' and 'admin'.
    static DatabaseVersion makeFixed();

    // Returns a new DatabaseVersion with just the lastMod incremented. This indicates that the
    // database changed primary, as opposed to being dropped and recreated.
    DatabaseVersion makeUpdated() const;

    /**
     * It returns true if the uuid and lastmod of both versions are the same.
     */
    bool operator==(const DatabaseVersion& other) const {
        return getUuid() == other.getUuid() && getLastMod() == other.getLastMod();
    }

    bool operator!=(const DatabaseVersion& other) const {
        return !(*this == other);
    }

    bool isFixed() const {
        return getLastMod() == 0;
    }

    UUID getUuid() const {
        return *DatabaseVersionBase::getUuid();
    }
};


/**
 * The DatabaseVersion class contains a UUID that is not comparable,
 * in fact is impossible to compare two different DatabaseVersion that have different UUIDs.
 *
 * This class wrap a DatabaseVersion object to make it always comparable by timestamping it with a
 * node-local sequence number (_uuidDisambiguatingSequenceNum).
 *
 * This class class should go away once a cluster-wide comparable DatabaseVersion will be
 * implemented.
 */
class ComparableDatabaseVersion {
public:
    /**
     * Creates a ComparableDatabaseVersion that wraps the given DatabaseVersion.
     * Each object created through this method will have a local sequence number greater than the
     * previously created ones.
     *
     * If version is boost::none it creates a ComparableDatabaseVersion that doesn't have a valid
     * DatabaseVersion. This is useful in some scenarios in which the DatabaseVersion is provided
     * later through ComparableDatabaseVersion::setVersion.
     */
    static ComparableDatabaseVersion makeComparableDatabaseVersion(
        const boost::optional<DatabaseVersion>& version);

    /**
     * Creates a new instance which will artificially be greater than any
     * previously created ComparableDatabaseVersion and smaller than any instance
     * created afterwards. Used as means to cause the collections cache to
     * attempt a refresh in situations where causal consistency cannot be
     * inferred.
     */
    static ComparableDatabaseVersion makeComparableDatabaseVersionForForcedRefresh();

    /**
     * Empty constructor needed by the ReadThroughCache.
     *
     * Instances created through this constructor will be always less then the ones created through
     * the static constructor.
     */
    ComparableDatabaseVersion() = default;

    BSONObj toBSONForLogging() const;

    bool operator==(const ComparableDatabaseVersion& other) const;

    bool operator!=(const ComparableDatabaseVersion& other) const {
        return !(*this == other);
    }

    /**
     * In case the two compared instances have different UUIDs, the most recently created one will
     * be greater, otherwise the comparison will be driven by the lastMod field of the underlying
     * DatabaseVersion.
     */
    bool operator<(const ComparableDatabaseVersion& other) const;

    bool operator>(const ComparableDatabaseVersion& other) const {
        return other < *this;
    }

    bool operator<=(const ComparableDatabaseVersion& other) const {
        return !(*this > other);
    }

    bool operator>=(const ComparableDatabaseVersion& other) const {
        return !(*this < other);
    }

private:
    friend class CatalogCache;

    static AtomicWord<uint64_t> _uuidDisambiguatingSequenceNumSource;
    static AtomicWord<uint64_t> _forcedRefreshSequenceNumSource;

    ComparableDatabaseVersion(boost::optional<DatabaseVersion> version,
                              uint64_t uuidDisambiguatingSequenceNum,
                              uint64_t forcedRefreshSequenceNum)
        : _dbVersion(std::move(version)),
          _uuidDisambiguatingSequenceNum(uuidDisambiguatingSequenceNum),
          _forcedRefreshSequenceNum(forcedRefreshSequenceNum) {}

    void setDatabaseVersion(const DatabaseVersion& version);

    boost::optional<DatabaseVersion> _dbVersion;

    // Locally incremented sequence number that allows to compare two database versions with
    // different UUIDs. Each new comparableDatabaseVersion will have a greater sequence number then
    // the ones created before.
    uint64_t _uuidDisambiguatingSequenceNum{0};
    uint64_t _forcedRefreshSequenceNum{0};
};


}  // namespace mongo
