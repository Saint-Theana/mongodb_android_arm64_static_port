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

#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/jsobj.h"
#include "mongo/logv2/log_format.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/net/cidr.h"

namespace mongo {

const int DEFAULT_UNIX_PERMS = 0700;
constexpr size_t DEFAULT_MAX_CONN = 1000000;

enum class ClusterRole { None, ShardServer, ConfigServer };

struct ServerGlobalParams {
    std::string binaryName;  // mongod or mongos
    std::string cwd;         // cwd of when process started

    int port = DefaultDBPort;  // --port
    enum {
        ConfigServerPort = 27019,
        CryptDServerPort = 27020,
        DefaultDBPort = 27017,
        ShardServerPort = 27018,
    };

    static std::string getPortSettingHelpText();

    std::vector<std::string> bind_ips;  // --bind_ip
    bool enableIPv6 = false;
    bool rest = false;  // --rest

    int listenBacklog = 0;  // --listenBacklog, real default is SOMAXCONN

    AtomicWord<bool> quiet{false};  // --quiet

    ClusterRole clusterRole = ClusterRole::None;  // --configsvr/--shardsvr

    bool cpu = false;  // --cpu show cpu time periodically

    bool objcheck = true;  // --objcheck

    int defaultProfile = 0;  // --profile
    boost::optional<BSONObj> defaultProfileFilter;
    int slowMS = 100;                      // --time in ms that is "slow"
    double sampleRate = 1.0;               // --samplerate rate at which to sample slow queries
    int defaultLocalThresholdMillis = 15;  // --localThreshold in ms to consider a node local
    bool moveParanoia = false;             // for move chunk paranoia

    bool noUnixSocket = false;    // --nounixsocket
    bool doFork = false;          // --fork
    std::string socket = "/tmp";  // UNIX domain socket directory
    std::string transportLayer;   // --transportLayer (must be either "asio" or "legacy")

    size_t maxConns = DEFAULT_MAX_CONN;  // Maximum number of simultaneous open connections.
    std::vector<stdx::variant<CIDR, std::string>> maxConnsOverride;
    int reservedAdminThreads = 0;

    int unixSocketPermissions = DEFAULT_UNIX_PERMS;  // permissions for the UNIX domain socket

    std::string keyFile;           // Path to keyfile, or empty if none.
    std::string pidFile;           // Path to pid file, or empty if none.
    std::string timeZoneInfoPath;  // Path to time zone info directory, or empty if none.

    std::string logpath;  // Path to log file, if logging to a file; otherwise, empty.
    logv2::LogTimestampFormat logTimestampFormat = logv2::LogTimestampFormat::kISO8601Local;

    bool logAppend = false;         // True if logging to a file in append mode.
    bool logRenameOnRotate = true;  // True if logging should rename log files on rotate
    bool logWithSyslog = false;     // True if logging to syslog; must not be set if logpath is set.
    int syslogFacility;             // Facility used when appending messages to the syslog.

#ifndef _WIN32
    int forkReadyFd = -1;  // for `--fork`. Write to it and close it when daemon service is up.
#endif

    /**
     * Switches to enable experimental (unsupported) features.
     */
    struct ExperimentalFeatures {
        ExperimentalFeatures() : storageDetailsCmdEnabled(false) {}
        bool storageDetailsCmdEnabled;  // -- enableExperimentalStorageDetailsCmd
    } experimental;

    time_t started = ::time(nullptr);

    BSONArray argvArray;
    BSONObj parsedOpts;

    enum AuthState { kEnabled, kDisabled, kUndefined };

    AuthState authState = AuthState::kUndefined;

    bool transitionToAuth = false;  // --transitionToAuth, mixed mode for rolling auth upgrade

    ClusterAuthMode startupClusterAuthMode;

    // for the YAML config, sharding._overrideShardIdentity. Can only be used when in
    // queryableBackupMode.
    BSONObj overrideShardIdentity;

    // True if the current binary version is an LTS Version.
    static constexpr bool kIsLTSBinaryVersion = false;

    struct FeatureCompatibility {
        /**
         * The combination of the fields (version, targetVersion, previousVersion) in the
         * featureCompatibilityVersion document in the server configuration collection
         * (admin.system.version) are represented by this enum and determine this node's behavior.
         *
         * Features can be gated for specific versions, or ranges of versions above or below some
         * minimum or maximum version, respectively.
         *
         * While upgrading from version X to Y or downgrading from Y to X, the server supports the
         * features of the older of the two versions.
         *
         * For versions X and Y, the legal enums and featureCompatibilityVersion documents are:
         *
         * kFullyDowngradedToX
         * (X, Unset, Unset): Only version X features are available, and new and existing storage
         *                    engine entries use the X format
         *
         * kUpgradingFromXToY
         * (X, Y, Unset): Only version X features are available, but new storage engine entries
         *                use the Y format, and existing entries may have either the X or
         *                Y format
         *
         * kVersionX
         * (X, Unset, Unset): X features are available, and new and existing storage engine
         *                    entries use the X format
         *
         * kDowngradingFromXToY
         * (Y, Y, X): Only Y features are available and new storage engine entries use the
         *            Y format, but existing entries may have either the Y or X format
         *
         * kUnsetDefault44Behavior
         * (Unset, Unset, Unset): This is the case on startup before the fCV document is loaded into
         *                        memory. isVersionInitialized() will return false, and getVersion()
         *                        will return the default (kUnsetDefault44Behavior).
         *
         */
        enum class Version {
            // The order of these enums matter: sort by (version, targetVersion, previousVersion).
            kInvalid,
            kUnsetDefault44Behavior,
            kFullyDowngradedTo44,    // { version: 4.4 }
            kDowngradingFrom47To44,  // { version: 4.4, targetVersion: 4.4, previousVersion: 4.7 }
            kDowngradingFrom48To44,  // { version: 4.4, targetVersion: 4.4, previousVersion: 4.8 }
            kDowngradingFrom49To44,  // { version: 4.4, targetVersion: 4.4, previousVersion: 4.9 }
            kDowngradingFrom50To44,  // { version: 4.4, targetVersion: 4.4, previousVersion: 5.0 }
            kUpgradingFrom44To47,    // { version: 4.4, targetVersion: 4.7 }
            kUpgradingFrom44To48,    // { version: 4.4, targetVersion: 4.8 }
            kUpgradingFrom44To49,    // { version: 4.4, targetVersion: 4.9 }
            kUpgradingFrom44To50,    // { version: 4.4, targetVersion: 5.0 }
            kVersion47,              // { version: 4.7 }
            kDowngradingFrom48To47,  // { version: 4.7, targetVersion: 4.7, previousVersion: 4.8 }
            kUpgradingFrom47To48,    // { version: 4.7, targetVersion: 4.8 }
            kVersion48,              // { version: 4.8 }
            kDowngradingFrom49To48,  // { version: 4.8, targetVersion: 4.8, previousVersion: 4.9 }
            kUpgradingFrom48To49,    // { version: 4.8, targetVersion: 4.9 }
            kVersion49,              // { version: 4.9 }
            kDowngradingFrom50To49,  // { version: 4.9, targetVersion: 4.9, previousVersion: 5.0 }
            kUpgradingFrom49To50,    // { version: 4.9, targetVersion: 5.0 }
            kVersion50,              // { version: 5.0 }
        };

        // These constants should only be used for generic FCV references. Generic references are
        // FCV references that are expected to exist across LTS binary versions.
        static constexpr Version kLatest = Version::kVersion50;
        static constexpr Version kLastContinuous = Version::kVersion49;
        static constexpr Version kLastLTS = Version::kFullyDowngradedTo44;

        // These constants should only be used for generic FCV references. Generic references are
        // FCV references that are expected to exist across LTS binary versions.
        // NOTE: DO NOT USE THEM FOR REGULAR FCV CHECKS.
        static constexpr Version kUpgradingFromLastLTSToLatest = Version::kUpgradingFrom44To50;
        static constexpr Version kUpgradingFromLastContinuousToLatest =
            Version::kUpgradingFrom49To50;
        static constexpr Version kDowngradingFromLatestToLastLTS = Version::kDowngradingFrom50To44;
        static constexpr Version kDowngradingFromLatestToLastContinuous =
            Version::kDowngradingFrom50To49;
        // kUpgradingFromLastLTSToLastContinuous is only ever set to a valid FCV when
        // kLastLTS and kLastContinuous are not equal. Otherwise, this value should be set to
        // kInvalid.
        static constexpr Version kUpgradingFromLastLTSToLastContinuous =
            Version::kUpgradingFrom44To49;

        /**
         * On startup, the featureCompatibilityVersion may not have been explicitly set yet. This
         * exposes the actual state of the featureCompatibilityVersion if it is uninitialized.
         */
        const bool isVersionInitialized() const {
            return _version.load() != Version::kUnsetDefault44Behavior;
        }

        /**
         * This safe getter for the featureCompatibilityVersion parameter ensures the parameter has
         * been initialized with a meaningful value.
         */
        const Version getVersion() const {
            invariant(isVersionInitialized());
            return _version.load();
        }

        bool isLessThanOrEqualTo(Version version, Version* versionReturn = nullptr) const {
            Version currentVersion = getVersion();
            if (versionReturn != nullptr) {
                *versionReturn = currentVersion;
            }
            return currentVersion <= version;
        }

        bool isGreaterThanOrEqualTo(Version version, Version* versionReturn = nullptr) const {
            Version currentVersion = getVersion();
            if (versionReturn != nullptr) {
                *versionReturn = currentVersion;
            }
            return currentVersion >= version;
        }

        bool isLessThan(Version version, Version* versionReturn = nullptr) const {
            Version currentVersion = getVersion();
            if (versionReturn != nullptr) {
                *versionReturn = currentVersion;
            }
            return currentVersion < version;
        }

        bool isGreaterThan(Version version, Version* versionReturn = nullptr) const {
            Version currentVersion = getVersion();
            if (versionReturn != nullptr) {
                *versionReturn = currentVersion;
            }
            return currentVersion > version;
        }

        // This function is to be used for generic FCV references only, and not for FCV-gating.
        bool isUpgradingOrDowngrading(boost::optional<Version> version = boost::none) const {
            if (version == boost::none) {
                version = getVersion();
            }
            return version != kLatest && version != kLastContinuous && version != kLastLTS;
        }

        void reset() {
            _version.store(Version::kUnsetDefault44Behavior);
        }

        void setVersion(Version version) {
            return _version.store(version);
        }

    private:
        AtomicWord<Version> _version{Version::kUnsetDefault44Behavior};

    } mutableFeatureCompatibility;

    // Const reference for featureCompatibilityVersion checks.
    const FeatureCompatibility& featureCompatibility = mutableFeatureCompatibility;

    // Feature validation differs depending on the role of a mongod in a replica set. Replica set
    // primaries can accept user-initiated writes and validate based on the feature compatibility
    // version. A secondary always validates in the upgraded mode so that it can sync new features,
    // even when in the downgraded feature compatibility mode.
    AtomicWord<bool> validateFeaturesAsPrimary{true};

    std::vector<std::string> disabledSecureAllocatorDomains;

    bool enableMajorityReadConcern = true;
};

extern ServerGlobalParams serverGlobalParams;

template <typename NameTrait>
struct TraitNamedDomain {
    static bool peg() {
        const auto& dsmd = serverGlobalParams.disabledSecureAllocatorDomains;
        const auto contains = [&](StringData dt) {
            return std::find(dsmd.begin(), dsmd.end(), dt) != dsmd.end();
        };
        static const bool ret = !(contains("*"_sd) || contains(NameTrait::DomainType));
        return ret;
    }
};
}  // namespace mongo
