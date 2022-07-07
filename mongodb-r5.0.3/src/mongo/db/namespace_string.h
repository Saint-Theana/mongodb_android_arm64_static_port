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

#include <algorithm>
#include <boost/optional.hpp>
#include <iosfwd>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {

class NamespaceString {
public:
    constexpr static size_t MaxDatabaseNameLen =
        128;  // max str len for the db name, including null char
    constexpr static size_t MaxNSCollectionLenFCV42 = 120U;
    constexpr static size_t MaxNsCollectionLen = 255;

    // Reserved system namespaces

    // Namespace for the admin database
    static constexpr StringData kAdminDb = "admin"_sd;

    // Namespace for the local database
    static constexpr StringData kLocalDb = "local"_sd;

    // Namespace for the sharding config database
    static constexpr StringData kConfigDb = "config"_sd;

    // Name for the system views collection
    static constexpr StringData kSystemDotViewsCollectionName = "system.views"_sd;

    // Names of privilege document collections
    static constexpr StringData kSystemUsers = "system.users"_sd;
    static constexpr StringData kSystemRoles = "system.roles"_sd;

    // Prefix for orphan collections
    static constexpr StringData kOrphanCollectionPrefix = "orphan."_sd;
    static constexpr StringData kOrphanCollectionDb = "local"_sd;

    // Prefix for collections that store the local resharding oplog buffer.
    static constexpr StringData kReshardingLocalOplogBufferPrefix =
        "localReshardingOplogBuffer."_sd;

    // Prefix for resharding conflict stash collections.
    static constexpr StringData kReshardingConflictStashPrefix = "localReshardingConflictStash."_sd;

    // Prefix for temporary resharding collection.
    static constexpr StringData kTemporaryReshardingCollectionPrefix = "system.resharding."_sd;

    // Prefix for time-series buckets collection.
    static constexpr StringData kTimeseriesBucketsCollectionPrefix = "system.buckets."_sd;

    // Namespace for storing configuration data, which needs to be replicated if the server is
    // running as a replica set. Documents in this collection should represent some configuration
    // state of the server, which needs to be recovered/consulted at startup. Each document in this
    // namespace should have its _id set to some string, which meaningfully describes what it
    // represents. For example, 'shardIdentity' and 'featureCompatibilityVersion'.
    static const NamespaceString kServerConfigurationNamespace;

    // Namespace for storing the logical sessions information
    static const NamespaceString kLogicalSessionsNamespace;

    // Namespace for storing the transaction information for each session
    static const NamespaceString kSessionTransactionsTableNamespace;

    // Name for a shard's collections metadata collection, each document of which indicates the
    // state of a specific collection
    static const NamespaceString kShardConfigCollectionsNamespace;

    // Name for a shard's databases metadata collection, each document of which indicates the state
    // of a specific database
    static const NamespaceString kShardConfigDatabasesNamespace;

    // Namespace for storing keys for signing and validating cluster times created by the cluster
    // that this node is in.
    static const NamespaceString kKeysCollectionNamespace;

    // Namespace for storing keys for validating cluster times created by other clusters.
    static const NamespaceString kExternalKeysCollectionNamespace;

    // Namespace of the the oplog collection.
    static const NamespaceString kRsOplogNamespace;

    // Namespace for storing the persisted state of transaction coordinators.
    static const NamespaceString kTransactionCoordinatorsNamespace;

    // Namespace for storing the persisted state of migration coordinators.
    static const NamespaceString kMigrationCoordinatorsNamespace;

    // Namespace for storing the persisted state of tenant migration donors.
    static const NamespaceString kTenantMigrationDonorsNamespace;

    // Namespace for storing the persisted state of tenant migration recipient service instances.
    static const NamespaceString kTenantMigrationRecipientsNamespace;

    // Namespace for view on local.oplog.rs for tenant migrations.
    static const NamespaceString kTenantMigrationOplogView;

    // Namespace for replica set configuration settings.
    static const NamespaceString kSystemReplSetNamespace;

    // Namespace for index build entries.
    static const NamespaceString kIndexBuildEntryNamespace;

    // Namespace for pending range deletions.
    static const NamespaceString kRangeDeletionNamespace;

    // Namespace containing pending range deletions snapshots for rename operations.
    static const NamespaceString kRangeDeletionForRenameNamespace;

    // Namespace for the coordinator's resharding operation state.
    static const NamespaceString kConfigReshardingOperationsNamespace;

    // Namespace for the donor shard's local resharding operation state.
    static const NamespaceString kDonorReshardingOperationsNamespace;

    // Namespace for the recipient shard's local resharding operation state.
    static const NamespaceString kRecipientReshardingOperationsNamespace;

    // Namespace for persisting sharding DDL coordinators state documents
    static const NamespaceString kShardingDDLCoordinatorsNamespace;

    // Namespace for persisting sharding DDL rename participant state documents
    static const NamespaceString kShardingRenameParticipantsNamespace;

    // Namespace for balancer settings and default read and write concerns.
    static const NamespaceString kConfigSettingsNamespace;

    // Namespace for vector clock state.
    static const NamespaceString kVectorClockNamespace;

    // Namespace for storing oplog applier progress for resharding.
    static const NamespaceString kReshardingApplierProgressNamespace;

    // Namespace for storing config.transactions cloner progress for resharding.
    static const NamespaceString kReshardingTxnClonerProgressNamespace;

    // Namespace for storing config.collectionCriticalSections documents
    static const NamespaceString kCollectionCriticalSectionsNamespace;

    // Dummy namespace used for forcing secondaries to handle an oplog entry on its own batch.
    static const NamespaceString kForceOplogBatchBoundaryNamespace;

    // Namespace used for storing retryable findAndModify images.
    static const NamespaceString kConfigImagesNamespace;

    /**
     * Constructs an empty NamespaceString.
     */
    NamespaceString() : _ns(), _dotIndex(std::string::npos) {}

    /**
     * Constructs a NamespaceString from the fully qualified namespace named in "ns".
     */
    explicit NamespaceString(StringData ns) {
        _ns = ns.toString();  // copy to our buffer
        _dotIndex = _ns.find('.');
        uassert(ErrorCodes::InvalidNamespace,
                "namespaces cannot have embedded null characters",
                _ns.find('\0') == std::string::npos);
    }

    /**
     * Constructs a NamespaceString for the given database and collection names.
     * "dbName" must not contain a ".", and "collectionName" must not start with one.
     */
    NamespaceString(StringData dbName, StringData collectionName)
        : _ns(dbName.size() + collectionName.size() + 1, '\0') {
        uassert(ErrorCodes::InvalidNamespace,
                "'.' is an invalid character in the database name: " + dbName,
                dbName.find('.') == std::string::npos);
        uassert(ErrorCodes::InvalidNamespace,
                "Collection names cannot start with '.': " + collectionName,
                collectionName.empty() || collectionName[0] != '.');

        std::string::iterator it = std::copy(dbName.begin(), dbName.end(), _ns.begin());
        *it = '.';
        ++it;
        it = std::copy(collectionName.begin(), collectionName.end(), it);
        _dotIndex = dbName.size();

        dassert(it == _ns.end());
        dassert(_ns[_dotIndex] == '.');

        uassert(ErrorCodes::InvalidNamespace,
                "namespaces cannot have embedded null characters",
                _ns.find('\0') == std::string::npos);
    }

    /**
     * Constructs the namespace '<dbName>.$cmd.aggregate', which we use as the namespace for
     * aggregation commands with the format {aggregate: 1}.
     */
    static NamespaceString makeCollectionlessAggregateNSS(StringData dbName);

    /**
     * Constructs a NamespaceString representing a listCollections namespace. The format for this
     * namespace is "<dbName>.$cmd.listCollections".
     */
    static NamespaceString makeListCollectionsNSS(StringData dbName);

    /**
     * NOTE: DollarInDbNameBehavior::allow is deprecated.
     *
     * Please use DollarInDbNameBehavior::disallow and check explicitly for any DB names that must
     * contain a $.
     */
    enum class DollarInDbNameBehavior {
        Disallow,
        Allow,  // Deprecated
    };

    StringData db() const {
        return _dotIndex == std::string::npos ? _ns : StringData(_ns.data(), _dotIndex);
    }

    StringData coll() const {
        return _dotIndex == std::string::npos
            ? StringData()
            : StringData(_ns.c_str() + _dotIndex + 1, _ns.size() - 1 - _dotIndex);
    }

    const std::string& ns() const {
        return _ns;
    }

    const std::string& toString() const {
        return ns();
    }

    size_t size() const {
        return _ns.size();
    }

    bool isEmpty() const {
        return _ns.empty();
    }

    //
    // The following methods assume isValid() is true for this NamespaceString.
    //

    bool isHealthlog() const {
        return isLocal() && coll() == "system.healthlog";
    }
    bool isSystem() const {
        return coll().startsWith("system.");
    }
    bool isNormalCollection() const {
        return !isSystem() && !(isLocal() && coll().startsWith("replset."));
    }
    bool isAdminDB() const {
        return db() == kAdminDb;
    }
    bool isLocal() const {
        return db() == kLocalDb;
    }
    bool isSystemDotProfile() const {
        return coll() == "system.profile";
    }
    bool isSystemDotViews() const {
        return coll() == kSystemDotViewsCollectionName;
    }
    bool isServerConfigurationCollection() const {
        return (db() == kAdminDb) && (coll() == "system.version");
    }
    bool isPrivilegeCollection() const {
        if (!isAdminDB()) {
            return false;
        }
        return (coll() == kSystemUsers) || (coll() == kSystemRoles);
    }
    bool isConfigDB() const {
        return db() == kConfigDb;
    }
    bool isCommand() const {
        return coll() == "$cmd";
    }
    bool isOplog() const {
        return oplog(_ns);
    }
    bool isOnInternalDb() const {
        if (db() == kAdminDb)
            return true;
        if (db() == kLocalDb)
            return true;
        if (db() == kConfigDb)
            return true;
        return false;
    }

    bool isOrphanCollection() const {
        return db() == kOrphanCollectionDb && coll().startsWith(kOrphanCollectionPrefix);
    }

    /**
     * Returns whether the specified namespace is used for internal purposes only and can
     * never be marked as anything other than UNSHARDED.
     */
    bool isNamespaceAlwaysUnsharded() const;

    /**
     * Returns whether the specified namespace is config.cache.chunks.<>.
     */
    bool isConfigDotCacheDotChunks() const;

    /**
     * Returns whether the specified namespace is config.localReshardingOplogBuffer.<>.
     */
    bool isReshardingLocalOplogBufferCollection() const;

    /**
     * Returns whether the specified namespace is config.localReshardingConflictStash.<>.
     */
    bool isReshardingConflictStashCollection() const;

    /**
     * Returns whether the specified namespace is <database>.system.resharding.<>.
     */
    bool isTemporaryReshardingCollection() const;

    /**
     * Returns whether the specified namespace is <database>.system.buckets.<>.
     */
    bool isTimeseriesBucketsCollection() const;

    /**
     * Returns the time-series buckets namespace for this view.
     */
    NamespaceString makeTimeseriesBucketsNamespace() const;

    /**
     * Returns the time-series view namespace for this buckets namespace.
     */
    NamespaceString getTimeseriesViewNamespace() const;

    /**
     * Returns whether a namespace is replicated, based only on its string value. One notable
     * omission is that map reduce `tmp.mr` collections may or may not be replicated. Callers must
     * decide how to handle that case separately.
     *
     * Note: This function considers "replicated" to be any namespace that should be timestamped.
     * Not all collections that are timestamped are replicated explicitly through the oplog.
     * Drop-pending collections are a notable example. Please use
     * ReplicationCoordinator::isOplogDisabledForNS to determine if a namespace gets logged in the
     * oplog.
     */
    bool isReplicated() const;

    /**
     * The namespace associated with some ClientCursors does not correspond to a particular
     * namespace. For example, this is true for listCollections cursors and $currentOp agg cursors.
     * Returns true if the namespace string is for a "collectionless" cursor.
     */
    bool isCollectionlessCursorNamespace() const {
        return coll().startsWith("$cmd."_sd);
    }

    bool isCollectionlessAggregateNS() const;
    bool isListCollectionsCursorNS() const;

    /**
     * Returns true if a client can modify this namespace even though it is under ".system."
     * For example <dbname>.system.users is ok for regular clients to update.
     */
    bool isLegalClientSystemNS(const ServerGlobalParams::FeatureCompatibility& currentFCV) const;

    /**
     * Returns true if this namespace refers to a drop-pending collection.
     */
    bool isDropPendingNamespace() const;

    /**
     * Returns true if operations on this namespace must be applied in their own oplog batch.
     */
    bool mustBeAppliedInOwnOplogBatch() const;

    /**
     * Returns the drop-pending namespace name for this namespace, provided the given optime.
     *
     * Example:
     *     test.foo -> test.system.drop.<timestamp seconds>i<timestamp increment>t<term>.foo
     */
    NamespaceString makeDropPendingNamespace(const repl::OpTime& opTime) const;

    /**
     * Returns the optime used to generate the drop-pending namespace.
     * Returns an error if this namespace is not drop-pending.
     */
    StatusWith<repl::OpTime> getDropPendingNamespaceOpTime() const;

    /**
     * Returns true if the namespace is valid. Special namespaces for internal use are considered as
     * valid.
     */
    bool isValid(DollarInDbNameBehavior behavior = DollarInDbNameBehavior::Allow) const {
        return validDBName(db(), behavior) && !coll().empty();
    }

    /**
     * NamespaceString("foo.bar").getSisterNS("blah") returns "foo.blah".
     */
    std::string getSisterNS(StringData local) const;

    NamespaceString getCommandNS() const {
        return {db(), "$cmd"};
    }

    void serializeCollectionName(BSONObjBuilder* builder, StringData fieldName) const;

    /**
     * @return true if the ns is an oplog one, otherwise false.
     */
    static bool oplog(StringData ns) {
        return ns.startsWith("local.oplog.");
    }

    /**
     * samples:
     *   good
     *      foo
     *      bar
     *      foo-bar
     *   bad:
     *      foo bar
     *      foo.bar
     *      foo"bar
     *
     * @param db - a possible database name
     * @param DollarInDbNameBehavior - please do not change the default value. DB names that must
     *                                 contain a $ should be checked explicitly.
     * @return if db is an allowed database name
     */
    static bool validDBName(StringData db,
                            DollarInDbNameBehavior behavior = DollarInDbNameBehavior::Disallow);

    /**
     * Takes a fully qualified namespace (ie dbname.collectionName), and returns true if
     * the collection name component of the namespace is valid.
     * samples:
     *   good:
     *      foo.bar
     *   bad:
     *      foo.
     *
     * @param ns - a full namespace (a.b)
     * @return if db.coll is an allowed collection name
     */
    static bool validCollectionComponent(StringData ns);

    /**
     * Takes a collection name and returns true if it is a valid collection name.
     * samples:
     *   good:
     *     foo
     *     system.views
     *   bad:
     *     $foo
     * @param coll - a collection name component of a namespace
     * @return if the input is a valid collection name
     */
    static bool validCollectionName(StringData coll);

    // Relops among `NamespaceString`.
    friend bool operator==(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() == b.ns();
    }
    friend bool operator!=(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() != b.ns();
    }
    friend bool operator<(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() < b.ns();
    }
    friend bool operator>(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() > b.ns();
    }
    friend bool operator<=(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() <= b.ns();
    }
    friend bool operator>=(const NamespaceString& a, const NamespaceString& b) {
        return a.ns() >= b.ns();
    }

    template <typename H>
    friend H AbslHashValue(H h, const NamespaceString& nss) {
        return H::combine(std::move(h), nss._ns);
    }

    friend auto logAttrs(const NamespaceString& nss) {
        return "namespace"_attr = nss;
    }

private:
    std::string _ns;
    size_t _dotIndex = 0;
};

/**
 * This class is intented to be used by commands which can accept either a collection name or
 * database + collection UUID. It will never be initialized with both.
 */
class NamespaceStringOrUUID {
public:
    NamespaceStringOrUUID(NamespaceString nss) : _nss(std::move(nss)) {}
    NamespaceStringOrUUID(std::string dbname, UUID uuid)
        : _uuid(std::move(uuid)), _dbname(std::move(dbname)) {}

    const boost::optional<NamespaceString>& nss() const {
        return _nss;
    }

    void setNss(const NamespaceString& nss) {
        _nss = nss;
    }

    const boost::optional<UUID>& uuid() const {
        return _uuid;
    }

    /**
     * Returns database name if this object was initialized with a UUID.
     */
    const std::string& dbname() const {
        return _dbname;
    }

    void preferNssForSerialization() {
        _preferNssForSerialization = true;
    }

    /**
     * Returns database name derived from either '_nss' or '_dbname'.
     */
    StringData db() const {
        return _nss ? _nss->db() : StringData(_dbname);
    }

    std::string toString() const;

    void serialize(BSONObjBuilder* builder, StringData fieldName) const;

private:
    // At any given time exactly one of these optionals will be initialized.
    boost::optional<NamespaceString> _nss;
    boost::optional<UUID> _uuid;

    // When seralizing, if both '_nss' and '_uuid' are present, use '_nss'.
    bool _preferNssForSerialization = false;

    // Empty string when '_nss' is non-none, and contains the database name when '_uuid' is
    // non-none. Although the UUID specifies a collection uniquely, we must later verify that the
    // collection belongs to the database named here.
    std::string _dbname;
};

std::ostream& operator<<(std::ostream& stream, const NamespaceString& nss);
std::ostream& operator<<(std::ostream& stream, const NamespaceStringOrUUID& nsOrUUID);
StringBuilder& operator<<(StringBuilder& builder, const NamespaceString& nss);
StringBuilder& operator<<(StringBuilder& builder, const NamespaceStringOrUUID& nsOrUUID);

/**
 * "database.a.b.c" -> "database"
 */
inline StringData nsToDatabaseSubstring(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos) {
        massert(
            10078, "nsToDatabase: db too long", ns.size() < NamespaceString::MaxDatabaseNameLen);
        return ns;
    }
    massert(10088,
            "nsToDatabase: db too long",
            i < static_cast<size_t>(NamespaceString::MaxDatabaseNameLen));
    return ns.substr(0, i);
}

/**
 * "database.a.b.c" -> "database"
 *
 * TODO: make this return a StringData
 */
inline std::string nsToDatabase(StringData ns) {
    return nsToDatabaseSubstring(ns).toString();
}

/**
 * "database.a.b.c" -> "a.b.c"
 */
inline StringData nsToCollectionSubstring(StringData ns) {
    size_t i = ns.find('.');
    massert(16886, "nsToCollectionSubstring: no .", i != std::string::npos);
    return ns.substr(i + 1);
}

/**
 * foo = false
 * foo. = false
 * foo.a = true
 */
inline bool nsIsFull(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos)
        return false;
    if (i == ns.size() - 1)
        return false;
    return true;
}

/**
 * foo = true
 * foo. = false
 * foo.a = false
 */
inline bool nsIsDbOnly(StringData ns) {
    size_t i = ns.find('.');
    if (i == std::string::npos)
        return true;
    return false;
}

inline bool NamespaceString::validDBName(StringData db, DollarInDbNameBehavior behavior) {
    if (db.size() == 0 || db.size() >= 64)
        return false;

    for (StringData::const_iterator iter = db.begin(), end = db.end(); iter != end; ++iter) {
        switch (*iter) {
            case '\0':
            case '/':
            case '\\':
            case '.':
            case ' ':
            case '"':
                return false;
            case '$':
                if (behavior == DollarInDbNameBehavior::Disallow)
                    return false;
                continue;
#ifdef _WIN32
            // We prohibit all FAT32-disallowed characters on Windows
            case '*':
            case '<':
            case '>':
            case ':':
            case '|':
            case '?':
                return false;
#endif
            default:
                continue;
        }
    }
    return true;
}

inline bool NamespaceString::validCollectionComponent(StringData ns) {
    size_t idx = ns.find('.');
    if (idx == std::string::npos)
        return false;

    return validCollectionName(ns.substr(idx + 1)) || oplog(ns);
}

inline bool NamespaceString::validCollectionName(StringData coll) {
    if (coll.empty())
        return false;

    if (coll[0] == '.')
        return false;

    for (StringData::const_iterator iter = coll.begin(), end = coll.end(); iter != end; ++iter) {
        switch (*iter) {
            case '\0':
            case '$':
                return false;
            default:
                continue;
        }
    }

    return true;
}

}  // namespace mongo
