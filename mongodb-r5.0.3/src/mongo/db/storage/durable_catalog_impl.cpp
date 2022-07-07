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

#include "mongo/db/storage/durable_catalog_impl.h"

#include <fmt/format.h>
#include <memory>
#include <stdlib.h>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/durable_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/random.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
// This is a global resource, which protects accesses to the catalog metadata (instance-wide).
// It is never used with KVEngines that support doc-level locking so this should never conflict
// with anything else.

const char kIsFeatureDocumentFieldName[] = "isFeatureDoc";
const char kNamespaceFieldName[] = "ns";
const char kNonRepairableFeaturesFieldName[] = "nonRepairable";
const char kRepairableFeaturesFieldName[] = "repairable";
const char kInternalIdentPrefix[] = "internal-";
const char kResumableIndexBuildIdentStem[] = "resumable-index-build-";

void appendPositionsOfBitsSet(uint64_t value, StringBuilder* sb) {
    invariant(sb);

    *sb << "[ ";
    bool firstIteration = true;
    while (value) {
        const int lowestSetBitPosition = countTrailingZeros64(value);
        if (!firstIteration) {
            *sb << ", ";
        }
        *sb << lowestSetBitPosition;
        value ^= (1ULL << lowestSetBitPosition);
        firstIteration = false;
    }
    *sb << " ]";
}

// Does not escape letters, digits, '.', or '_'.
// Otherwise escapes to a '.' followed by a zero-filled 2- or 3-digit decimal number.
// Note that this escape table does not produce a 1:1 mapping to and from dbname, and
// collisions are possible.
// For example:
//     "db.123", "db\0143", and "db\073" all escape to "db.123".
//       {'d','b','1','2','3'} => "d" + "b" + "." + "1" + "2" + "3" => "db.123"
//       {'d','b','\x0c','3'}  => "d" + "b" + ".12" + "3"           => "db.123"
//       {'d','b','\x3b'}      => "d" + "b" + ".123"                => "db.123"
constexpr std::array<StringData, 256> escapeTable = {
    ".00"_sd,  ".01"_sd,  ".02"_sd,  ".03"_sd,  ".04"_sd,  ".05"_sd,  ".06"_sd,  ".07"_sd,
    ".08"_sd,  ".09"_sd,  ".10"_sd,  ".11"_sd,  ".12"_sd,  ".13"_sd,  ".14"_sd,  ".15"_sd,
    ".16"_sd,  ".17"_sd,  ".18"_sd,  ".19"_sd,  ".20"_sd,  ".21"_sd,  ".22"_sd,  ".23"_sd,
    ".24"_sd,  ".25"_sd,  ".26"_sd,  ".27"_sd,  ".28"_sd,  ".29"_sd,  ".30"_sd,  ".31"_sd,
    ".32"_sd,  ".33"_sd,  ".34"_sd,  ".35"_sd,  ".36"_sd,  ".37"_sd,  ".38"_sd,  ".39"_sd,
    ".40"_sd,  ".41"_sd,  ".42"_sd,  ".43"_sd,  ".44"_sd,  ".45"_sd,  "."_sd,    ".47"_sd,
    "0"_sd,    "1"_sd,    "2"_sd,    "3"_sd,    "4"_sd,    "5"_sd,    "6"_sd,    "7"_sd,
    "8"_sd,    "9"_sd,    ".58"_sd,  ".59"_sd,  ".60"_sd,  ".61"_sd,  ".62"_sd,  ".63"_sd,
    ".64"_sd,  "A"_sd,    "B"_sd,    "C"_sd,    "D"_sd,    "E"_sd,    "F"_sd,    "G"_sd,
    "H"_sd,    "I"_sd,    "J"_sd,    "K"_sd,    "L"_sd,    "M"_sd,    "N"_sd,    "O"_sd,
    "P"_sd,    "Q"_sd,    "R"_sd,    "S"_sd,    "T"_sd,    "U"_sd,    "V"_sd,    "W"_sd,
    "X"_sd,    "Y"_sd,    "Z"_sd,    ".91"_sd,  ".92"_sd,  ".93"_sd,  ".94"_sd,  "_"_sd,
    ".96"_sd,  "a"_sd,    "b"_sd,    "c"_sd,    "d"_sd,    "e"_sd,    "f"_sd,    "g"_sd,
    "h"_sd,    "i"_sd,    "j"_sd,    "k"_sd,    "l"_sd,    "m"_sd,    "n"_sd,    "o"_sd,
    "p"_sd,    "q"_sd,    "r"_sd,    "s"_sd,    "t"_sd,    "u"_sd,    "v"_sd,    "w"_sd,
    "x"_sd,    "y"_sd,    "z"_sd,    ".123"_sd, ".124"_sd, ".125"_sd, ".126"_sd, ".127"_sd,
    ".128"_sd, ".129"_sd, ".130"_sd, ".131"_sd, ".132"_sd, ".133"_sd, ".134"_sd, ".135"_sd,
    ".136"_sd, ".137"_sd, ".138"_sd, ".139"_sd, ".140"_sd, ".141"_sd, ".142"_sd, ".143"_sd,
    ".144"_sd, ".145"_sd, ".146"_sd, ".147"_sd, ".148"_sd, ".149"_sd, ".150"_sd, ".151"_sd,
    ".152"_sd, ".153"_sd, ".154"_sd, ".155"_sd, ".156"_sd, ".157"_sd, ".158"_sd, ".159"_sd,
    ".160"_sd, ".161"_sd, ".162"_sd, ".163"_sd, ".164"_sd, ".165"_sd, ".166"_sd, ".167"_sd,
    ".168"_sd, ".169"_sd, ".170"_sd, ".171"_sd, ".172"_sd, ".173"_sd, ".174"_sd, ".175"_sd,
    ".176"_sd, ".177"_sd, ".178"_sd, ".179"_sd, ".180"_sd, ".181"_sd, ".182"_sd, ".183"_sd,
    ".184"_sd, ".185"_sd, ".186"_sd, ".187"_sd, ".188"_sd, ".189"_sd, ".190"_sd, ".191"_sd,
    ".192"_sd, ".193"_sd, ".194"_sd, ".195"_sd, ".196"_sd, ".197"_sd, ".198"_sd, ".199"_sd,
    ".200"_sd, ".201"_sd, ".202"_sd, ".203"_sd, ".204"_sd, ".205"_sd, ".206"_sd, ".207"_sd,
    ".208"_sd, ".209"_sd, ".210"_sd, ".211"_sd, ".212"_sd, ".213"_sd, ".214"_sd, ".215"_sd,
    ".216"_sd, ".217"_sd, ".218"_sd, ".219"_sd, ".220"_sd, ".221"_sd, ".222"_sd, ".223"_sd,
    ".224"_sd, ".225"_sd, ".226"_sd, ".227"_sd, ".228"_sd, ".229"_sd, ".230"_sd, ".231"_sd,
    ".232"_sd, ".233"_sd, ".234"_sd, ".235"_sd, ".236"_sd, ".237"_sd, ".238"_sd, ".239"_sd,
    ".240"_sd, ".241"_sd, ".242"_sd, ".243"_sd, ".244"_sd, ".245"_sd, ".246"_sd, ".247"_sd,
    ".248"_sd, ".249"_sd, ".250"_sd, ".251"_sd, ".252"_sd, ".253"_sd, ".254"_sd, ".255"_sd};

std::string escapeDbName(StringData dbname) {
    std::string escaped;
    escaped.reserve(dbname.size());
    for (unsigned char c : dbname) {
        StringData ce = escapeTable[c];
        escaped.append(ce.begin(), ce.end());
    }
    return escaped;
}

bool indexTypeSupportsPathLevelMultikeyTracking(StringData accessMethod) {
    return accessMethod == IndexNames::BTREE || accessMethod == IndexNames::GEO_2DSPHERE;
}

/**
 * Returns true if writes to the catalog entry for the input namespace require being timestamped.
 */
bool requiresTimestampForCatalogWrite(OperationContext* opCtx, const NamespaceString& nss) {
    if (!nss.isReplicated() || nss.coll().startsWith("tmp.mr.") || nss.isDropPendingNamespace()) {
        return false;
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->isReplEnabled()) {
        return false;
    }

    if (replCoord->canAcceptWritesFor(opCtx, nss)) {
        return false;
    }

    // If there is a timestamp already assigned, there's no need to explicitly assign a timestamp.
    if (opCtx->recoveryUnit()->isTimestamped()) {
        return false;
    }

    // Nodes in `startup` do not need to timestamp writes.
    // Nodes in the oplog application phase of initial sync (`startup2`) must not timestamp writes
    // before the `initialDataTimestamp`.  Nodes in initial sync may also be in the `removed` state
    // due to DNS resolution errors; they may continue writing during that time.
    const auto memberState = replCoord->getMemberState();
    if (memberState.startup() || memberState.startup2() || memberState.removed()) {
        return false;
    }

    // When rollback completes, index builds may be restarted, which requires untimestamped catalog
    // writes. Additionally, it's illegal to timestamp a write later than the timestamp associated
    // with the node exiting the rollback state.
    if (memberState.rollback()) {
        return false;
    }

    return true;
}

}  // namespace

using std::string;
using std::unique_ptr;

class DurableCatalogImpl::AddIdentChange : public RecoveryUnit::Change {
public:
    AddIdentChange(DurableCatalogImpl* catalog, RecordId catalogId)
        : _catalog(catalog), _catalogId(catalogId) {}

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        stdx::lock_guard<Latch> lk(_catalog->_catalogIdToEntryMapLock);
        _catalog->_catalogIdToEntryMap.erase(_catalogId);
    }

    DurableCatalogImpl* const _catalog;
    const RecordId _catalogId;
};

class DurableCatalogImpl::RemoveIdentChange : public RecoveryUnit::Change {
public:
    RemoveIdentChange(DurableCatalogImpl* catalog, RecordId catalogId, const Entry& entry)
        : _catalog(catalog), _catalogId(catalogId), _entry(entry) {}

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        stdx::lock_guard<Latch> lk(_catalog->_catalogIdToEntryMapLock);
        _catalog->_catalogIdToEntryMap[_catalogId] = _entry;
    }

    DurableCatalogImpl* const _catalog;
    const RecordId _catalogId;
    const Entry _entry;
};

class DurableCatalogImpl::AddIndexChange : public RecoveryUnit::Change {
public:
    AddIndexChange(RecoveryUnit* ru, StorageEngineInterface* engine, StringData ident)
        : _recoveryUnit(ru), _engine(engine), _ident(ident.toString()) {}

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        // Intentionally ignoring failure.
        auto kvEngine = _engine->getEngine();
        kvEngine->dropIdent(_recoveryUnit, _ident).ignore();
    }

    RecoveryUnit* const _recoveryUnit;
    StorageEngineInterface* _engine;
    const std::string _ident;
};

bool DurableCatalogImpl::FeatureTracker::isFeatureDocument(BSONObj obj) {
    BSONElement firstElem = obj.firstElement();
    if (firstElem.fieldNameStringData() == kIsFeatureDocumentFieldName) {
        return firstElem.booleanSafe();
    }
    return false;
}

Status DurableCatalogImpl::FeatureTracker::isCompatibleWithCurrentCode(
    OperationContext* opCtx) const {
    FeatureBits versionInfo = getInfo(opCtx);

    uint64_t unrecognizedNonRepairableFeatures =
        versionInfo.nonRepairableFeatures & ~_usedNonRepairableFeaturesMask;
    if (unrecognizedNonRepairableFeatures) {
        StringBuilder sb;
        sb << "The data files use features not recognized by this version of mongod; the NR feature"
              " bits in positions ";
        appendPositionsOfBitsSet(unrecognizedNonRepairableFeatures, &sb);
        sb << " aren't recognized by this version of mongod";
        return {ErrorCodes::MustUpgrade, sb.str()};
    }

    uint64_t unrecognizedRepairableFeatures =
        versionInfo.repairableFeatures & ~_usedRepairableFeaturesMask;
    if (unrecognizedRepairableFeatures) {
        StringBuilder sb;
        sb << "The data files use features not recognized by this version of mongod; the R feature"
              " bits in positions ";
        appendPositionsOfBitsSet(unrecognizedRepairableFeatures, &sb);
        sb << " aren't recognized by this version of mongod";
        return {ErrorCodes::CanRepairToDowngrade, sb.str()};
    }

    return Status::OK();
}

std::unique_ptr<DurableCatalogImpl::FeatureTracker> DurableCatalogImpl::FeatureTracker::get(
    OperationContext* opCtx, DurableCatalogImpl* catalog, RecordId rid) {
    auto record = catalog->_rs->dataFor(opCtx, rid);
    BSONObj obj = record.toBson();
    invariant(isFeatureDocument(obj));
    return std::unique_ptr<DurableCatalogImpl::FeatureTracker>(
        new DurableCatalogImpl::FeatureTracker(catalog, rid));
}

std::unique_ptr<DurableCatalogImpl::FeatureTracker> DurableCatalogImpl::FeatureTracker::create(
    OperationContext* opCtx, DurableCatalogImpl* catalog) {
    return std::unique_ptr<DurableCatalogImpl::FeatureTracker>(
        new DurableCatalogImpl::FeatureTracker(catalog, RecordId()));
}

bool DurableCatalogImpl::FeatureTracker::isNonRepairableFeatureInUse(
    OperationContext* opCtx, NonRepairableFeature feature) const {
    FeatureBits versionInfo = getInfo(opCtx);
    return versionInfo.nonRepairableFeatures & static_cast<NonRepairableFeatureMask>(feature);
}

void DurableCatalogImpl::FeatureTracker::markNonRepairableFeatureAsInUse(
    OperationContext* opCtx, NonRepairableFeature feature) {
    FeatureBits versionInfo = getInfo(opCtx);
    versionInfo.nonRepairableFeatures |= static_cast<NonRepairableFeatureMask>(feature);
    putInfo(opCtx, versionInfo);
}

void DurableCatalogImpl::FeatureTracker::markNonRepairableFeatureAsNotInUse(
    OperationContext* opCtx, NonRepairableFeature feature) {
    FeatureBits versionInfo = getInfo(opCtx);
    versionInfo.nonRepairableFeatures &= ~static_cast<NonRepairableFeatureMask>(feature);
    putInfo(opCtx, versionInfo);
}

bool DurableCatalogImpl::FeatureTracker::isRepairableFeatureInUse(OperationContext* opCtx,
                                                                  RepairableFeature feature) const {
    FeatureBits versionInfo = getInfo(opCtx);
    return versionInfo.repairableFeatures & static_cast<RepairableFeatureMask>(feature);
}

void DurableCatalogImpl::FeatureTracker::markRepairableFeatureAsInUse(OperationContext* opCtx,
                                                                      RepairableFeature feature) {
    FeatureBits versionInfo = getInfo(opCtx);
    versionInfo.repairableFeatures |= static_cast<RepairableFeatureMask>(feature);
    putInfo(opCtx, versionInfo);
}

void DurableCatalogImpl::FeatureTracker::markRepairableFeatureAsNotInUse(
    OperationContext* opCtx, RepairableFeature feature) {
    FeatureBits versionInfo = getInfo(opCtx);
    versionInfo.repairableFeatures &= ~static_cast<RepairableFeatureMask>(feature);
    putInfo(opCtx, versionInfo);
}

DurableCatalogImpl::FeatureTracker::FeatureBits DurableCatalogImpl::FeatureTracker::getInfo(
    OperationContext* opCtx) const {
    if (_rid.isNull()) {
        return {};
    }

    auto record = _catalog->_rs->dataFor(opCtx, _rid);
    BSONObj obj = record.toBson();
    invariant(isFeatureDocument(obj));

    BSONElement nonRepairableFeaturesElem;
    auto nonRepairableFeaturesStatus = bsonExtractTypedField(
        obj, kNonRepairableFeaturesFieldName, BSONType::NumberLong, &nonRepairableFeaturesElem);
    if (!nonRepairableFeaturesStatus.isOK()) {
        LOGV2_ERROR(22215,
                    "error: exception extracting typed field with obj:{obj}",
                    "Exception extracting typed field from obj",
                    "obj"_attr = redact(obj),
                    "fieldName"_attr = kNonRepairableFeaturesFieldName);
        fassert(40111, nonRepairableFeaturesStatus);
    }

    BSONElement repairableFeaturesElem;
    auto repairableFeaturesStatus = bsonExtractTypedField(
        obj, kRepairableFeaturesFieldName, BSONType::NumberLong, &repairableFeaturesElem);
    if (!repairableFeaturesStatus.isOK()) {
        LOGV2_ERROR(22216,
                    "error: exception extracting typed field with obj:{obj}",
                    "Exception extracting typed field from obj",
                    "obj"_attr = redact(obj),
                    "fieldName"_attr = kRepairableFeaturesFieldName);
        fassert(40112, repairableFeaturesStatus);
    }

    FeatureBits versionInfo;
    versionInfo.nonRepairableFeatures =
        static_cast<NonRepairableFeatureMask>(nonRepairableFeaturesElem.numberLong());
    versionInfo.repairableFeatures =
        static_cast<RepairableFeatureMask>(repairableFeaturesElem.numberLong());
    return versionInfo;
}

void DurableCatalogImpl::FeatureTracker::putInfo(OperationContext* opCtx,
                                                 const FeatureBits& versionInfo) {
    BSONObjBuilder bob;
    bob.appendBool(kIsFeatureDocumentFieldName, true);
    // We intentionally include the "ns" field with a null value in the feature document to prevent
    // older versions that do 'obj["ns"].String()' from starting up. This way only versions that are
    // aware of the feature document's existence can successfully start up.
    bob.appendNull(kNamespaceFieldName);
    bob.append(kNonRepairableFeaturesFieldName,
               static_cast<long long>(versionInfo.nonRepairableFeatures));
    bob.append(kRepairableFeaturesFieldName,
               static_cast<long long>(versionInfo.repairableFeatures));
    BSONObj obj = bob.done();

    if (_rid.isNull()) {
        // This is the first time a feature is being marked as in-use or not in-use, so we must
        // insert the feature document rather than update it.
        auto rid = _catalog->_rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp());
        fassert(40113, rid.getStatus());
        _rid = rid.getValue();
    } else {
        auto status = _catalog->_rs->updateRecord(opCtx, _rid, obj.objdata(), obj.objsize());
        fassert(40114, status);
    }
}

DurableCatalogImpl::DurableCatalogImpl(RecordStore* rs,
                                       bool directoryPerDb,
                                       bool directoryForIndexes,
                                       StorageEngineInterface* engine)
    : _rs(rs),
      _directoryPerDb(directoryPerDb),
      _directoryForIndexes(directoryForIndexes),
      _rand(_newRand()),
      _next(0),
      _engine(engine) {}

DurableCatalogImpl::~DurableCatalogImpl() {
    _rs = nullptr;
}

std::string DurableCatalogImpl::_newRand() {
    return str::stream() << SecureRandom().nextInt64();
}

bool DurableCatalogImpl::_hasEntryCollidingWithRand(WithLock) const {
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    for (auto it = _catalogIdToEntryMap.begin(); it != _catalogIdToEntryMap.end(); ++it) {
        if (StringData(it->second.ident).endsWith(_rand))
            return true;
    }
    return false;
}

std::string DurableCatalogImpl::newInternalIdent() {
    return _newInternalIdent("");
}

std::string DurableCatalogImpl::newInternalResumableIndexBuildIdent() {
    return _newInternalIdent(kResumableIndexBuildIdentStem);
}

std::string DurableCatalogImpl::_newInternalIdent(StringData identStem) {
    stdx::lock_guard<Latch> lk(_randLock);
    StringBuilder buf;
    buf << kInternalIdentPrefix;
    buf << identStem;
    buf << _next++ << '-' << _rand;
    return buf.str();
}

std::string DurableCatalogImpl::getFilesystemPathForDb(const std::string& dbName) const {
    if (_directoryPerDb) {
        return storageGlobalParams.dbpath + '/' + escapeDbName(dbName);
    } else {
        return storageGlobalParams.dbpath;
    }
}

std::string DurableCatalogImpl::_newUniqueIdent(NamespaceString nss, const char* kind) {
    // If this changes to not put _rand at the end, _hasEntryCollidingWithRand will need fixing.
    stdx::lock_guard<Latch> lk(_randLock);
    StringBuilder buf;
    if (_directoryPerDb) {
        buf << escapeDbName(nss.db()) << '/';
    }
    buf << kind;
    buf << (_directoryForIndexes ? '/' : '-');
    buf << _next++ << '-' << _rand;
    return buf.str();
}

void DurableCatalogImpl::init(OperationContext* opCtx) {
    // No locking needed since called single threaded.
    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();

        if (FeatureTracker::isFeatureDocument(obj)) {
            // There should be at most one version document in the catalog.
            invariant(!_featureTracker);

            // Initialize the feature tracker and skip over the version document because it doesn't
            // correspond to a namespace entry.
            _featureTracker = FeatureTracker::get(opCtx, this, record->id);
            continue;
        }

        // No rollback since this is just loading already committed data.
        string ident = obj["ident"].String();
        string ns = obj["ns"].String();
        _catalogIdToEntryMap[record->id] = Entry(record->id, ident, NamespaceString(ns));
    }

    if (!_featureTracker) {
        // If there wasn't a feature document, commit a default one to disk. All deployments will
        // end up with `kPathLevelMultikeyTracking` as every `_id` index build sets this.
        WriteUnitOfWork wuow(opCtx);
        _featureTracker = DurableCatalogImpl::FeatureTracker::create(opCtx, this);
        _featureTracker->markRepairableFeatureAsInUse(
            opCtx, FeatureTracker::RepairableFeature::kPathLevelMultikeyTracking);
        wuow.commit();
    }

    // In the unlikely event that we have used this _rand before generate a new one.
    stdx::lock_guard<Latch> lk(_randLock);
    while (_hasEntryCollidingWithRand(lk)) {
        _rand = _newRand();
    }
}

std::vector<DurableCatalog::Entry> DurableCatalogImpl::getAllCatalogEntries(
    OperationContext* opCtx) const {
    std::vector<DurableCatalog::Entry> ret;

    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        if (FeatureTracker::isFeatureDocument(obj)) {
            // Skip over the version document because it doesn't correspond to a collection.
            continue;
        }
        string ident = obj["ident"].String();
        string collName = obj["ns"].String();

        ret.emplace_back(record->id, ident, NamespaceString(collName));
    }

    return ret;
}

DurableCatalog::Entry DurableCatalogImpl::getEntry(RecordId catalogId) const {
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    auto it = _catalogIdToEntryMap.find(catalogId);
    invariant(it != _catalogIdToEntryMap.end());
    return it->second;
}

StatusWith<DurableCatalog::Entry> DurableCatalogImpl::_addEntry(OperationContext* opCtx,
                                                                NamespaceString nss,
                                                                const CollectionOptions& options) {
    invariant(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_IX));

    const string ident = _newUniqueIdent(nss, "collection");

    BSONObj obj;
    {
        BSONObjBuilder b;
        b.append("ns", nss.ns());
        b.append("ident", ident);
        BSONCollectionCatalogEntry::MetaData md;
        md.ns = nss.ns();
        md.options = options;
        b.append("md", md.toBSON());
        obj = b.obj();
    }
    StatusWith<RecordId> res = _rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = {res.getValue(), ident, nss};
    opCtx->recoveryUnit()->registerChange(std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(22207,
                1,
                "stored meta data for {nss_ns} @ {res_getValue}",
                "nss_ns"_attr = nss.ns(),
                "res_getValue"_attr = res.getValue());
    return {{res.getValue(), ident, nss}};
}

StatusWith<DurableCatalog::Entry> DurableCatalogImpl::_importEntry(OperationContext* opCtx,
                                                                   NamespaceString nss,
                                                                   const BSONObj& metadata) {
    invariant(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_IX));

    const string ident = metadata["ident"].String();
    StatusWith<RecordId> res =
        _rs->insertRecord(opCtx, metadata.objdata(), metadata.objsize(), Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = {res.getValue(), ident, nss};
    opCtx->recoveryUnit()->registerChange(std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(
        5095101, 1, "imported meta data", "nss"_attr = nss.ns(), "metadata"_attr = res.getValue());
    return {{res.getValue(), ident, nss}};
}

std::string DurableCatalogImpl::getIndexIdent(OperationContext* opCtx,
                                              RecordId catalogId,
                                              StringData idxName) const {
    BSONObj obj = _findEntry(opCtx, catalogId);
    BSONObj idxIdent = obj["idxIdent"].Obj();
    return idxIdent[idxName].String();
}

BSONObj DurableCatalogImpl::_findEntry(OperationContext* opCtx, RecordId catalogId) const {
    LOGV2_DEBUG(22208, 3, "looking up metadata for: {catalogId}", "catalogId"_attr = catalogId);
    RecordData data;
    if (!_rs->findRecord(opCtx, catalogId, &data)) {
        // since the in memory meta data isn't managed with mvcc
        // its possible for different transactions to see slightly
        // different things, which is ok via the locking above.
        return BSONObj();
    }

    return data.releaseToBson().getOwned();
}

std::shared_ptr<BSONCollectionCatalogEntry::MetaData> DurableCatalogImpl::getMetaData(
    OperationContext* opCtx, RecordId catalogId) const {
    BSONObj obj = _findEntry(opCtx, catalogId);
    LOGV2_DEBUG(22209, 3, " fetched CCE metadata: {obj}", "obj"_attr = obj);
    std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md;
    const BSONElement mdElement = obj["md"];
    if (mdElement.isABSONObj()) {
        LOGV2_DEBUG(22210, 3, "returning metadata: {mdElement}", "mdElement"_attr = mdElement);
        md = std::make_shared<BSONCollectionCatalogEntry::MetaData>();
        md->parse(mdElement.Obj());
    }
    return md;
}

void DurableCatalogImpl::putMetaData(OperationContext* opCtx,
                                     RecordId catalogId,
                                     BSONCollectionCatalogEntry::MetaData& md) {
    NamespaceString nss(md.ns);
    BSONObj obj = _findEntry(opCtx, catalogId);

    {
        // rebuilt doc
        BSONObjBuilder b;
        b.append("md", md.toBSON());

        BSONObjBuilder newIdentMap;
        BSONObj oldIdentMap;
        if (obj["idxIdent"].isABSONObj())
            oldIdentMap = obj["idxIdent"].Obj();

        for (size_t i = 0; i < md.indexes.size(); i++) {
            const auto index = md.indexes[i];
            string name = index.name();

            // All indexes with buildUUIDs must be ready:false.
            invariant(!(index.buildUUID && index.ready), str::stream() << md.toBSON(true));

            // fix ident map
            BSONElement e = oldIdentMap[name];
            if (e.type() == String) {
                newIdentMap.append(e);
                continue;
            }
            // missing, create new
            newIdentMap.append(name, _newUniqueIdent(nss, "index"));
        }
        b.append("idxIdent", newIdentMap.obj());

        // add whatever is left
        b.appendElementsUnique(obj);
        obj = b.obj();
    }

    if (requiresTimestampForCatalogWrite(opCtx, nss)) {
        opCtx->recoveryUnit()->setMustBeTimestamped();
    }

    LOGV2_DEBUG(22211, 3, "recording new metadata: {obj}", "obj"_attr = obj);
    Status status = _rs->updateRecord(opCtx, catalogId, obj.objdata(), obj.objsize());
    fassert(28521, status);
}

Status DurableCatalogImpl::_replaceEntry(OperationContext* opCtx,
                                         RecordId catalogId,
                                         const NamespaceString& toNss,
                                         BSONCollectionCatalogEntry::MetaData& md) {
    BSONObj old = _findEntry(opCtx, catalogId).getOwned();
    {
        BSONObjBuilder b;

        b.append("ns", toNss.ns());
        b.append("md", md.toBSON());

        b.appendElementsUnique(old);

        BSONObj obj = b.obj();
        Status status = _rs->updateRecord(opCtx, catalogId, obj.objdata(), obj.objsize());
        fassert(28522, status);
    }

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    const auto it = _catalogIdToEntryMap.find(catalogId);
    invariant(it != _catalogIdToEntryMap.end());

    NamespaceString fromName = it->second.nss;
    it->second.nss = toNss;
    opCtx->recoveryUnit()->onRollback([this, catalogId, fromName]() {
        stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
        const auto it = _catalogIdToEntryMap.find(catalogId);
        invariant(it != _catalogIdToEntryMap.end());
        it->second.nss = fromName;
    });

    if (requiresTimestampForCatalogWrite(opCtx, fromName)) {
        opCtx->recoveryUnit()->setMustBeTimestamped();
    }

    return Status::OK();
}

Status DurableCatalogImpl::_removeEntry(OperationContext* opCtx, RecordId catalogId) {
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    const auto it = _catalogIdToEntryMap.find(catalogId);
    if (it == _catalogIdToEntryMap.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found");
    }

    opCtx->recoveryUnit()->registerChange(
        std::make_unique<RemoveIdentChange>(this, catalogId, it->second));

    LOGV2_DEBUG(22212,
                1,
                "deleting metadata for {it_second_nss} @ {catalogId}",
                "it_second_nss"_attr = it->second.nss,
                "catalogId"_attr = catalogId);
    _rs->deleteRecord(opCtx, catalogId);
    _catalogIdToEntryMap.erase(it);

    return Status::OK();
}

std::vector<std::string> DurableCatalogImpl::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> v;

    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        if (FeatureTracker::isFeatureDocument(obj)) {
            // Skip over the version document because it doesn't correspond to a namespace entry and
            // therefore doesn't refer to any idents.
            continue;
        }
        v.push_back(obj["ident"].String());

        BSONElement e = obj["idxIdent"];
        if (!e.isABSONObj())
            continue;
        BSONObj idxIdent = e.Obj();

        BSONObjIterator sub(idxIdent);
        while (sub.more()) {
            BSONElement e = sub.next();
            v.push_back(e.String());
        }
    }

    return v;
}

bool DurableCatalogImpl::isUserDataIdent(StringData ident) const {
    // Indexes and collections are candidates for dropping when the storage engine's metadata does
    // not align with the catalog metadata.
    return ident.find("index-") != std::string::npos || ident.find("index/") != std::string::npos ||
        ident.find("collection-") != std::string::npos ||
        ident.find("collection/") != std::string::npos;
}

bool DurableCatalogImpl::isInternalIdent(StringData ident) const {
    return ident.find(kInternalIdentPrefix) != std::string::npos;
}

bool DurableCatalogImpl::isResumableIndexBuildIdent(StringData ident) const {
    invariant(isInternalIdent(ident), ident.toString());
    return ident.find(kResumableIndexBuildIdentStem) != std::string::npos;
}

bool DurableCatalogImpl::isCollectionIdent(StringData ident) const {
    // Internal idents prefixed "internal-" should not be considered collections, because
    // they are not eligible for orphan recovery through repair.
    return ident.find("collection-") != std::string::npos ||
        ident.find("collection/") != std::string::npos;
}

StatusWith<std::string> DurableCatalogImpl::newOrphanedIdent(OperationContext* opCtx,
                                                             std::string ident) {
    // The collection will be named local.orphan.xxxxx.
    std::string identNs = ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    NamespaceString ns = NamespaceString(NamespaceString::kOrphanCollectionDb,
                                         NamespaceString::kOrphanCollectionPrefix + identNs);

    // Generate a new UUID for the orphaned collection.
    CollectionOptions optionsWithUUID;
    optionsWithUUID.uuid.emplace(CollectionUUID::gen());
    BSONObj obj;
    {
        BSONObjBuilder b;
        b.append("ns", ns.ns());
        b.append("ident", ident);
        BSONCollectionCatalogEntry::MetaData md;
        md.ns = ns.ns();
        // Default options with newly generated UUID.
        md.options = optionsWithUUID;
        b.append("md", md.toBSON());
        obj = b.obj();
    }
    StatusWith<RecordId> res = _rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = Entry(res.getValue(), ident, ns);
    opCtx->recoveryUnit()->registerChange(std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(22213,
                1,
                "stored meta data for orphaned collection {ns} @ {res_getValue}",
                "ns"_attr = ns,
                "res_getValue"_attr = res.getValue());
    return {ns.ns()};
}

StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> DurableCatalogImpl::createCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& options,
    bool allocateDefaultSpace) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
    invariant(nss.coll().size() > 0);

    if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)) {
        throw WriteConflictException();
    }

    StatusWith<Entry> swEntry = _addEntry(opCtx, nss, options);
    if (!swEntry.isOK())
        return swEntry.getStatus();
    Entry& entry = swEntry.getValue();

    Status status = _engine->getEngine()->createRecordStore(opCtx, nss.ns(), entry.ident, options);
    if (!status.isOK())
        return status;

    // Mark collation feature as in use if the collection has a non-simple default collation.
    if (!options.collation.isEmpty()) {
        const auto feature = DurableCatalogImpl::FeatureTracker::NonRepairableFeature::kCollation;
        if (getFeatureTracker()->isNonRepairableFeatureInUse(opCtx, feature)) {
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx, feature);
        }
    }

    auto ru = opCtx->recoveryUnit();
    CollectionUUID uuid = options.uuid.get();
    opCtx->recoveryUnit()->onRollback([ru, catalog = this, nss, ident = entry.ident, uuid]() {
        // Intentionally ignoring failure
        catalog->_engine->getEngine()->dropIdent(ru, ident).ignore();
    });

    auto rs = _engine->getEngine()->getRecordStore(opCtx, nss.ns(), entry.ident, options);
    invariant(rs);

    return std::pair<RecordId, std::unique_ptr<RecordStore>>(entry.catalogId, std::move(rs));
}

Status DurableCatalogImpl::createIndex(OperationContext* opCtx,
                                       RecordId catalogId,
                                       const CollectionOptions& collOptions,
                                       const IndexDescriptor* spec) {
    std::string ident = getIndexIdent(opCtx, catalogId, spec->indexName());

    auto kvEngine = _engine->getEngine();
    const Status status = kvEngine->createSortedDataInterface(opCtx, collOptions, ident, spec);
    if (status.isOK()) {
        opCtx->recoveryUnit()->registerChange(
            std::make_unique<AddIndexChange>(opCtx->recoveryUnit(), _engine, ident));
    }
    return status;
}

StatusWith<DurableCatalog::ImportResult> DurableCatalogImpl::importCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& metadata,
    const BSONObj& storageMetadata,
    DurableCatalogImpl::ImportCollectionUUIDOption uuidOption) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
    invariant(nss.coll().size() > 0);

    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "Collection already exists. NS: " << nss,
            !CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));

    BSONCollectionCatalogEntry::MetaData md;
    const BSONElement mdElement = metadata["md"];
    uassert(ErrorCodes::BadValue, "Malformed catalog metadata", mdElement.isABSONObj());
    md.parse(mdElement.Obj());

    uassert(ErrorCodes::BadValue,
            "Attempted to import catalog entry without an ident",
            metadata.hasField("ident"));

    const auto& catalogEntry = [&] {
        if (uuidOption == ImportCollectionUUIDOption::kGenerateNew) {
            // Generate a new UUID for the collection.
            md.options.uuid = CollectionUUID::gen();
            BSONObjBuilder catalogEntryBuilder;
            // Generate a new "md" field after setting the new UUID.
            catalogEntryBuilder.append("md", md.toBSON());
            // Append the rest of the metadata.
            catalogEntryBuilder.appendElementsUnique(metadata);
            return catalogEntryBuilder.obj();
        }
        return metadata;
    }();

    // Before importing the idents belonging to the collection and indexes, change '_rand' if there
    // will be a conflict.
    std::set<std::string> indexIdents;
    {
        const std::string collectionIdent = catalogEntry["ident"].String();

        if (!catalogEntry["idxIdent"].eoo()) {
            for (const auto& indexIdent : catalogEntry["idxIdent"].Obj()) {
                indexIdents.insert(indexIdent.String());
            }
        }

        auto identsToImportConflict = [&](WithLock) -> bool {
            if (StringData(collectionIdent).endsWith(_rand)) {
                return true;
            }

            for (const std::string& ident : indexIdents) {
                if (StringData(ident).endsWith(_rand)) {
                    return true;
                }
            }
            return false;
        };

        stdx::lock_guard<Latch> lk(_randLock);
        while (_hasEntryCollidingWithRand(lk) || identsToImportConflict(lk)) {
            _rand = _newRand();
        }
    }

    StatusWith<Entry> swEntry = _importEntry(opCtx, nss, catalogEntry);
    if (!swEntry.isOK())
        return swEntry.getStatus();
    Entry& entry = swEntry.getValue();

    auto kvEngine = _engine->getEngine();
    Status status = kvEngine->importRecordStore(opCtx, entry.ident, storageMetadata);
    if (!status.isOK())
        return status;

    for (const std::string& indexIdent : indexIdents) {
        status = kvEngine->importSortedDataInterface(opCtx, indexIdent, storageMetadata);
        if (!status.isOK()) {
            return status;
        }
    }

    // Mark collation feature as in use if the collection has a non-simple default collation.
    if (!md.options.collation.isEmpty()) {
        const auto feature = DurableCatalogImpl::FeatureTracker::NonRepairableFeature::kCollation;
        if (getFeatureTracker()->isNonRepairableFeatureInUse(opCtx, feature)) {
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx, feature);
        }
    }

    opCtx->recoveryUnit()->onRollback(
        [opCtx, catalog = this, ident = entry.ident, indexIdents = indexIdents]() {
            catalog->_engine->getEngine()->dropIdentForImport(opCtx, ident);
            for (const auto& indexIdent : indexIdents) {
                catalog->_engine->getEngine()->dropIdentForImport(opCtx, indexIdent);
            }
        });

    auto rs = _engine->getEngine()->getRecordStore(opCtx, nss.ns(), entry.ident, md.options);
    invariant(rs);

    return DurableCatalog::ImportResult(entry.catalogId, std::move(rs), md.options.uuid.get());
}

Status DurableCatalogImpl::renameCollection(OperationContext* opCtx,
                                            RecordId catalogId,
                                            const NamespaceString& toNss,
                                            BSONCollectionCatalogEntry::MetaData& md) {
    return _replaceEntry(opCtx, catalogId, toNss, md);
}

Status DurableCatalogImpl::dropCollection(OperationContext* opCtx, RecordId catalogId) {
    Entry entry;
    {
        stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
        entry = _catalogIdToEntryMap[catalogId];
    }

    invariant(opCtx->lockState()->isCollectionLockedForMode(entry.nss, MODE_X));
    invariant(getTotalIndexCount(opCtx, catalogId) == 0);

    // Remove metadata from mdb_catalog
    Status status = _removeEntry(opCtx, catalogId);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

BSONCollectionCatalogEntry::IndexMetaData DurableCatalogImpl::prepareIndexMetaDataForIndexBuild(
    OperationContext* opCtx,
    const IndexDescriptor* spec,
    boost::optional<UUID> buildUUID,
    bool isBackgroundSecondaryBuild) {
    BSONCollectionCatalogEntry::IndexMetaData imd;
    imd.spec = spec->infoObj();
    imd.ready = false;
    imd.multikey = false;
    imd.isBackgroundSecondaryBuild = isBackgroundSecondaryBuild;
    imd.buildUUID = buildUUID;

    if (indexTypeSupportsPathLevelMultikeyTracking(spec->getAccessMethodName())) {
        const auto feature = FeatureTracker::RepairableFeature::kPathLevelMultikeyTracking;
        if (!getFeatureTracker()->isRepairableFeatureInUse(opCtx, feature)) {
            getFeatureTracker()->markRepairableFeatureAsInUse(opCtx, feature);
        }
        imd.multikeyPaths = MultikeyPaths{static_cast<size_t>(spec->keyPattern().nFields())};
    }

    // Mark collation feature as in use if the index has a non-simple collation.
    if (imd.spec["collation"]) {
        const auto feature = FeatureTracker::NonRepairableFeature::kCollation;
        if (!getFeatureTracker()->isNonRepairableFeatureInUse(opCtx, feature)) {
            getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx, feature);
        }
    }

    return imd;
}

Status DurableCatalogImpl::dropAndRecreateIndexIdentForResume(OperationContext* opCtx,
                                                              const CollectionOptions& collOptions,
                                                              const IndexDescriptor* spec,
                                                              StringData ident) {
    auto status = _engine->getEngine()->dropSortedDataInterface(opCtx, ident);
    if (!status.isOK())
        return status;

    status = _engine->getEngine()->createSortedDataInterface(opCtx, collOptions, ident, spec);

    return status;
}

bool DurableCatalogImpl::isIndexMultikey(OperationContext* opCtx,
                                         RecordId catalogId,
                                         StringData indexName,
                                         MultikeyPaths* multikeyPaths) const {
    auto md = getMetaData(opCtx, catalogId);

    int offset = md->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot get multikey for index " << indexName << " @ " << catalogId
                            << " : " << md->toBSON());

    if (multikeyPaths && !md->indexes[offset].multikeyPaths.empty()) {
        *multikeyPaths = md->indexes[offset].multikeyPaths;
    }

    return md->indexes[offset].multikey;
}

int DurableCatalogImpl::getTotalIndexCount(OperationContext* opCtx, RecordId catalogId) const {
    auto md = getMetaData(opCtx, catalogId);
    if (!md)
        return 0;

    return static_cast<int>(md->indexes.size());
}

bool DurableCatalogImpl::isIndexPresent(OperationContext* opCtx,
                                        RecordId catalogId,
                                        StringData indexName) const {
    auto md = getMetaData(opCtx, catalogId);
    if (!md)
        return false;

    int offset = md->findIndexOffset(indexName);
    return offset >= 0;
}

bool DurableCatalogImpl::isIndexReady(OperationContext* opCtx,
                                      RecordId catalogId,
                                      StringData indexName) const {
    auto md = getMetaData(opCtx, catalogId);
    if (!md)
        return false;

    int offset = md->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot get ready status for index " << indexName << " @ "
                            << catalogId << " : " << md->toBSON());
    return md->indexes[offset].ready;
}

void DurableCatalogImpl::setRand_forTest(const std::string& rand) {
    stdx::lock_guard<Latch> lk(_randLock);
    _rand = rand;
}

std::string DurableCatalogImpl::getRand_forTest() const {
    stdx::lock_guard<Latch> lk(_randLock);
    return _rand;
}

}  // namespace mongo
