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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/visit_helper.h"

namespace mongo {
namespace repl {

/**
 * The first oplog entry is a no-op with this message in its "msg" field.
 */
constexpr auto kInitiatingSetMsg = "initiating set"_sd;

/**
 * A parsed DurableReplOperation along with information about the operation that should only exist
 * in-memory.
 *
 * ReplOperation should always be used over DurableReplOperation when passing around ReplOperations
 * in server code.
 */

class ReplOperation : public DurableReplOperation {
public:
    static ReplOperation parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject) {
        ReplOperation o;
        o.parseProtected(ctxt, bsonObject);
        return o;
    }
    const BSONObj& getPreImageDocumentKey() const {
        return _preImageDocumentKey;
    }
    void setPreImageDocumentKey(BSONObj value) {
        _preImageDocumentKey = std::move(value);
    }

    const BSONObj& getPreImage() const {
        return _fullPreImage;
    }

    void setPreImage(BSONObj value) {
        _fullPreImage = std::move(value);
    }

private:
    BSONObj _preImageDocumentKey;
    BSONObj _fullPreImage;
};

/**
 * Mutable class used on primary to build up oplog entries progressively.
 */
class MutableOplogEntry : public OplogEntryBase {
public:
    // Current oplog version, should be the value of the v field in all oplog entries.
    static constexpr int kOplogVersion = 2;

    // Helpers to generate ReplOperation.
    static ReplOperation makeInsertOperation(const NamespaceString& nss,
                                             UUID uuid,
                                             const BSONObj& docToInsert);
    static ReplOperation makeUpdateOperation(const NamespaceString nss,
                                             UUID uuid,
                                             const BSONObj& update,
                                             const BSONObj& criteria);
    static ReplOperation makeDeleteOperation(const NamespaceString& nss,
                                             UUID uuid,
                                             const BSONObj& docToDelete);

    static ReplOperation makeCreateCommand(const NamespaceString nss,
                                           const mongo::CollectionOptions& options,
                                           const BSONObj& idIndex);

    static ReplOperation makeCreateIndexesCommand(const NamespaceString nss,
                                                  CollectionUUID uuid,
                                                  const BSONObj& indexDoc);

    static BSONObj makeCreateCollCmdObj(const NamespaceString& collectionName,
                                        const mongo::CollectionOptions& options,
                                        const BSONObj& idIndex);

    static StatusWith<MutableOplogEntry> parse(const BSONObj& object);

    MutableOplogEntry() : OplogEntryBase() {}

    void setSessionId(boost::optional<LogicalSessionId> value) & {
        getOperationSessionInfo().setSessionId(std::move(value));
    }

    void setStatementIds(const std::vector<StmtId>& stmtIds) & {
        if (stmtIds.empty()) {
            OplogEntryBase::setStatementIds(boost::none);
        } else if (stmtIds.size() == 1) {
            OplogEntryBase::setStatementIds({{stmtIds.front()}});
        } else {
            OplogEntryBase::setStatementIds({{stmtIds}});
        }
    }

    std::vector<StmtId> getStatementIds() const {
        if (!OplogEntryBase::getStatementIds()) {
            return {};
        }
        return stdx::visit(
            visit_helper::Overloaded{[](StmtId stmtId) { return std::vector<StmtId>{stmtId}; },
                                     [](const std::vector<StmtId>& stmtIds) { return stmtIds; }},
            *OplogEntryBase::getStatementIds());
    }

    void setTxnNumber(boost::optional<std::int64_t> value) & {
        getOperationSessionInfo().setTxnNumber(std::move(value));
    }

    void setOpType(OpTypeEnum value) & {
        getDurableReplOperation().setOpType(std::move(value));
    }

    void setNss(NamespaceString value) & {
        getDurableReplOperation().setNss(std::move(value));
    }

    void setUuid(boost::optional<UUID> value) & {
        getDurableReplOperation().setUuid(std::move(value));
    }

    void setObject(BSONObj value) & {
        getDurableReplOperation().setObject(std::move(value));
    }

    void setObject2(boost::optional<BSONObj> value) & {
        getDurableReplOperation().setObject2(std::move(value));
    }

    void setUpsert(boost::optional<bool> value) & {
        getDurableReplOperation().setUpsert(std::move(value));
    }

    void setPreImageOpTime(boost::optional<OpTime> value) {
        getDurableReplOperation().setPreImageOpTime(std::move(value));
    }

    const boost::optional<OpTime>& getPreImageOpTime() const {
        return getDurableReplOperation().getPreImageOpTime();
    }

    void setTimestamp(Timestamp value) & {
        getOpTimeBase().setTimestamp(std::move(value));
    }

    void setTerm(boost::optional<std::int64_t> value) & {
        getOpTimeBase().setTerm(std::move(value));
    }

    void setDestinedRecipient(boost::optional<ShardId> value) {
        getDurableReplOperation().setDestinedRecipient(std::move(value));
    }

    const boost::optional<ShardId>& getDestinedRecipient() const {
        return getDurableReplOperation().getDestinedRecipient();
    }

    /**
     * Sets the OpTime of the oplog entry.
     */
    void setOpTime(const OpTime& opTime) &;

    /**
     * Returns the OpTime of the oplog entry.
     */
    OpTime getOpTime() const;

    /**
     * Same as setFromMigrate but only set when it is true.
     */
    void setFromMigrateIfTrue(bool value) & {
        if (value)
            setFromMigrate(value);
    }
};

/**
 * A parsed oplog entry that privately inherits from the MutableOplogEntry.
 * This class is immutable. All setters are hidden.
 */
class DurableOplogEntry : private MutableOplogEntry {
public:
    // Make field names accessible.
    using MutableOplogEntry::k_idFieldName;
    using MutableOplogEntry::kDestinedRecipientFieldName;
    using MutableOplogEntry::kDurableReplOperationFieldName;
    using MutableOplogEntry::kFromMigrateFieldName;
    using MutableOplogEntry::kFromTenantMigrationFieldName;
    using MutableOplogEntry::kHashFieldName;
    using MutableOplogEntry::kNssFieldName;
    using MutableOplogEntry::kObject2FieldName;
    using MutableOplogEntry::kObjectFieldName;
    using MutableOplogEntry::kOperationSessionInfoFieldName;
    using MutableOplogEntry::kOplogVersion;
    using MutableOplogEntry::kOpTypeFieldName;
    using MutableOplogEntry::kPostImageOpTimeFieldName;
    using MutableOplogEntry::kPreImageOpTimeFieldName;
    using MutableOplogEntry::kPrevWriteOpTimeInTransactionFieldName;
    using MutableOplogEntry::kSessionIdFieldName;
    using MutableOplogEntry::kStatementIdsFieldName;
    using MutableOplogEntry::kTermFieldName;
    using MutableOplogEntry::kTimestampFieldName;
    using MutableOplogEntry::kTxnNumberFieldName;
    using MutableOplogEntry::kUpsertFieldName;
    using MutableOplogEntry::kUuidFieldName;
    using MutableOplogEntry::kVersionFieldName;
    using MutableOplogEntry::kWallClockTimeFieldName;

    // Make serialize() and getters accessible.
    using MutableOplogEntry::get_id;
    using MutableOplogEntry::getDestinedRecipient;
    using MutableOplogEntry::getDurableReplOperation;
    using MutableOplogEntry::getFromMigrate;
    using MutableOplogEntry::getFromTenantMigration;
    using MutableOplogEntry::getHash;
    using MutableOplogEntry::getNeedsRetryImage;
    using MutableOplogEntry::getNss;
    using MutableOplogEntry::getObject;
    using MutableOplogEntry::getObject2;
    using MutableOplogEntry::getOperationSessionInfo;
    using MutableOplogEntry::getOpType;
    using MutableOplogEntry::getPostImageOpTime;
    using MutableOplogEntry::getPreImageOpTime;
    using MutableOplogEntry::getPrevWriteOpTimeInTransaction;
    using MutableOplogEntry::getSessionId;
    using MutableOplogEntry::getStatementIds;
    using MutableOplogEntry::getTerm;
    using MutableOplogEntry::getTimestamp;
    using MutableOplogEntry::getTxnNumber;
    using MutableOplogEntry::getUpsert;
    using MutableOplogEntry::getUuid;
    using MutableOplogEntry::getVersion;
    using MutableOplogEntry::getWallClockTime;
    using MutableOplogEntry::serialize;

    // Make helper functions accessible.
    using MutableOplogEntry::getOpTime;
    using MutableOplogEntry::makeCreateCommand;
    using MutableOplogEntry::makeCreateIndexesCommand;
    using MutableOplogEntry::makeDeleteOperation;
    using MutableOplogEntry::makeInsertOperation;
    using MutableOplogEntry::makeUpdateOperation;

    enum class CommandType {
        kNotCommand,
        kCreate,
        kRenameCollection,
        kDbCheck,
        kDrop,
        kCollMod,
        kApplyOps,
        kDropDatabase,
        kEmptyCapped,
        kCreateIndexes,
        kStartIndexBuild,
        kCommitIndexBuild,
        kAbortIndexBuild,
        kDropIndexes,
        kCommitTransaction,
        kAbortTransaction,
        kImportCollection,
    };

    // Get the in-memory size in bytes of a ReplOperation.
    static size_t getDurableReplOperationSize(const DurableReplOperation& op);

    static StatusWith<DurableOplogEntry> parse(const BSONObj& object);

    DurableOplogEntry(OpTime opTime,
                      const boost::optional<int64_t> hash,
                      OpTypeEnum opType,
                      const NamespaceString& nss,
                      const boost::optional<UUID>& uuid,
                      const boost::optional<bool>& fromMigrate,
                      int version,
                      const BSONObj& oField,
                      const boost::optional<BSONObj>& o2Field,
                      const OperationSessionInfo& sessionInfo,
                      const boost::optional<bool>& isUpsert,
                      const mongo::Date_t& wallClockTime,
                      const std::vector<StmtId>& statementIds,
                      const boost::optional<OpTime>& prevWriteOpTimeInTransaction,
                      const boost::optional<OpTime>& preImageOpTime,
                      const boost::optional<OpTime>& postImageOpTime,
                      const boost::optional<ShardId>& destinedRecipient,
                      const boost::optional<Value>& idField,
                      const boost::optional<RetryImageEnum>& needsRetryImage);

    // DEPRECATED: This constructor can throw. Use static parse method instead.
    explicit DurableOplogEntry(BSONObj raw);

    DurableOplogEntry() = delete;

    /**
     * Returns if the oplog entry is for a command operation.
     */
    bool isCommand() const;

    /**
     * Returns if the oplog entry is part of a transaction that has not yet been prepared or
     * committed.  The actual "prepare" or "commit" oplog entries do not have a "partialTxn" field
     * and so this method will always return false for them.
     */
    bool isPartialTransaction() const {
        if (getCommandType() != CommandType::kApplyOps) {
            return false;
        }
        return getObject()[ApplyOpsCommandInfoBase::kPartialTxnFieldName].booleanSafe();
    }

    /**
     * Returns whether if the oplog entry is the last applyOps in a multiple-entry transaction.
     */
    bool isEndOfLargeTransaction() const;

    /**
     * Returns if this is a prepared 'commitTransaction' oplog entry.
     */
    bool isPreparedCommit() const {
        return getCommandType() == DurableOplogEntry::CommandType::kCommitTransaction;
    }

    /**
     * Returns whether the oplog entry represents an applyOps which is a self-contained atomic
     * operation, or the last applyOps of an unprepared transaction, as opposed to part of a
     * prepared transaction or a non-final applyOps in a transaction.
     */
    bool isTerminalApplyOps() const {
        return getCommandType() == DurableOplogEntry::CommandType::kApplyOps && !shouldPrepare() &&
            !isPartialTransaction() && !getObject().getBoolField("prepare");
    }

    /**
     * Returns whether the oplog entry represents a single oplog entry transaction.
     */
    bool isSingleOplogEntryTransaction() const;

    /**
     * Returns whether the oplog entry represents an applyOps with a command inside. This is only
     * for transactions with only one oplog entry.
     */
    bool isSingleOplogEntryTransactionWithCommand() const;

    /**
     * Returns true if the oplog entry is for a CRUD operation.
     */
    static bool isCrudOpType(OpTypeEnum opType);
    bool isCrudOpType() const;

    /**
     * Returns true if the oplog entry is for a command related to indexes.
     * i.e createIndexes, dropIndexes, startIndexBuild, commitIndexBuild, abortIndexBuild.
     */
    bool isIndexCommandType() const;

    /**
     * Returns if the operation should be prepared. Must be called on an 'applyOps' entry.
     */
    bool shouldPrepare() const;

    /**
     * Returns the _id of the document being modified. Must be called on CRUD ops.
     */
    BSONElement getIdElement() const;

    /**
     * Returns the document representing the operation to apply. This is the 'o' field for all
     * operations, including updates. For updates this is not guaranteed to include the _id or the
     * shard key.
     */
    BSONObj getOperationToApply() const;

    /**
     * Returns an object containing the _id of the target document for a CRUD operation. In a
     * sharded cluster this object also contains the shard key. This object may contain more fields
     * in the target document than the _id and shard key.
     * For insert/delete operations, this will be the document in the 'o' field.
     * For update operations, this will be the document in the 'o2' field.
     * Should not be called for non-CRUD operations.
     */
    BSONObj getObjectContainingDocumentKey() const;

    /**
     * Returns the type of command of the oplog entry. If it is not a command, returns kNotCommand.
     */
    CommandType getCommandType() const;

    /**
     * Returns the size of the original document used to create this DurableOplogEntry.
     */
    int getRawObjSizeBytes() const;

    /**
     * Returns the original document used to create this DurableOplogEntry.
     */
    const BSONObj& getRaw() const {
        return _raw;
    }

    /**
     * Serializes the oplog entry to a string.
     */
    std::string toString() const;

    BSONObj toBSON() const {
        return _raw;
    }

private:
    BSONObj _raw;  // Owned.
    CommandType _commandType = CommandType::kNotCommand;
};

DurableOplogEntry::CommandType parseCommandType(const BSONObj& objectField);

/**
 * Data structure that holds a DurableOplogEntry and other different run time state variables.
 */
class OplogEntry {
public:
    using CommandType = DurableOplogEntry::CommandType;
    static constexpr auto k_idFieldName = DurableOplogEntry::k_idFieldName;
    static constexpr auto kDestinedRecipientFieldName =
        DurableOplogEntry::kDestinedRecipientFieldName;
    static constexpr auto kDurableReplOperationFieldName =
        DurableOplogEntry::kDurableReplOperationFieldName;
    static constexpr auto kFromMigrateFieldName = DurableOplogEntry::kFromMigrateFieldName;
    static constexpr auto kFromTenantMigrationFieldName =
        DurableOplogEntry::kFromTenantMigrationFieldName;
    static constexpr auto kHashFieldName = DurableOplogEntry::kHashFieldName;
    static constexpr auto kNssFieldName = DurableOplogEntry::kNssFieldName;
    static constexpr auto kObject2FieldName = DurableOplogEntry::kObject2FieldName;
    static constexpr auto kObjectFieldName = DurableOplogEntry::kObjectFieldName;
    static constexpr auto kOperationSessionInfoFieldName =
        DurableOplogEntry::kOperationSessionInfoFieldName;
    static constexpr auto kOplogVersion = DurableOplogEntry::kOplogVersion;
    static constexpr auto kOpTypeFieldName = DurableOplogEntry::kOpTypeFieldName;
    static constexpr auto kPostImageOpTimeFieldName = DurableOplogEntry::kPostImageOpTimeFieldName;
    static constexpr auto kPreImageOpTimeFieldName = DurableOplogEntry::kPreImageOpTimeFieldName;
    static constexpr auto kPrevWriteOpTimeInTransactionFieldName =
        DurableOplogEntry::kPrevWriteOpTimeInTransactionFieldName;
    static constexpr auto kSessionIdFieldName = DurableOplogEntry::kSessionIdFieldName;
    static constexpr auto kStatementIdFieldName = DurableOplogEntry::kStatementIdsFieldName;
    static constexpr auto kTermFieldName = DurableOplogEntry::kTermFieldName;
    static constexpr auto kTimestampFieldName = DurableOplogEntry::kTimestampFieldName;
    static constexpr auto kTxnNumberFieldName = DurableOplogEntry::kTxnNumberFieldName;
    static constexpr auto kUpsertFieldName = DurableOplogEntry::kUpsertFieldName;
    static constexpr auto kUuidFieldName = DurableOplogEntry::kUuidFieldName;
    static constexpr auto kVersionFieldName = DurableOplogEntry::kVersionFieldName;
    static constexpr auto kWallClockTimeFieldName = DurableOplogEntry::kWallClockTimeFieldName;

    OplogEntry(DurableOplogEntry oplog);
    OplogEntry(const BSONObj& oplog);

    const DurableOplogEntry& getEntry() const {
        return _entry;
    }

    void setEntry(DurableOplogEntry oplog);

    /**
     * Note: will only parse fields included in DurableOplogEntry.
     */
    static StatusWith<OplogEntry> parse(const BSONObj& object);

    bool isForCappedCollection() const;
    void setIsForCappedCollection(bool isForCappedCollection);

    std::shared_ptr<DurableOplogEntry> getPreImageOp() const;
    void setPreImageOp(std::shared_ptr<DurableOplogEntry> preImageOp);
    void setPreImageOp(const BSONObj& preImageOp);

    std::shared_ptr<DurableOplogEntry> getPostImageOp() const;
    void setPostImageOp(std::shared_ptr<DurableOplogEntry> postImageOp);
    void setPostImageOp(const BSONObj& postImageOp);

    std::string toStringForLogging() const;

    /**
     * Returns the BSON representation for diagnostic purposes. To get a BSON meant for storing to
     * the oplog collection, use getEntry().toBSON() instead.
     */
    BSONObj toBSONForLogging() const;

    // Wrapper methods for DurableOplogEntry
    const boost::optional<mongo::Value>& get_id() const&;
    std::vector<StmtId> getStatementIds() const&;
    const OperationSessionInfo& getOperationSessionInfo() const;
    const boost::optional<mongo::LogicalSessionId>& getSessionId() const;
    const boost::optional<std::int64_t> getTxnNumber() const;
    const DurableReplOperation& getDurableReplOperation() const;
    mongo::repl::OpTypeEnum getOpType() const;
    const mongo::NamespaceString& getNss() const;
    const boost::optional<mongo::UUID>& getUuid() const;
    const mongo::BSONObj& getObject() const;
    const boost::optional<mongo::BSONObj>& getObject2() const;
    const boost::optional<bool> getUpsert() const;
    const boost::optional<mongo::repl::OpTime>& getPreImageOpTime() const;
    const boost::optional<mongo::ShardId>& getDestinedRecipient() const;
    const mongo::Timestamp& getTimestamp() const;
    const boost::optional<std::int64_t> getTerm() const;
    const mongo::Date_t& getWallClockTime() const;
    const boost::optional<std::int64_t> getHash() const&;
    std::int64_t getVersion() const;
    const boost::optional<bool> getFromMigrate() const&;
    const boost::optional<mongo::UUID>& getFromTenantMigration() const&;
    const boost::optional<mongo::repl::OpTime>& getPrevWriteOpTimeInTransaction() const&;
    const boost::optional<mongo::repl::OpTime>& getPostImageOpTime() const&;
    const boost::optional<RetryImageEnum> getNeedsRetryImage() const;
    OpTime getOpTime() const;
    bool isCommand() const;
    bool isPartialTransaction() const;
    bool isEndOfLargeTransaction() const;
    bool isPreparedCommit() const;
    bool isTerminalApplyOps() const;
    bool isSingleOplogEntryTransaction() const;
    bool isSingleOplogEntryTransactionWithCommand() const;
    bool isCrudOpType() const;
    bool isIndexCommandType() const;
    bool shouldPrepare() const;
    BSONElement getIdElement() const;
    BSONObj getOperationToApply() const;
    BSONObj getObjectContainingDocumentKey() const;
    OplogEntry::CommandType getCommandType() const;
    int getRawObjSizeBytes() const;

private:
    DurableOplogEntry _entry;


    // We use std::shared_ptr<DurableOplogEntry> rather than boost::optional<DurableOplogEntry> here
    // so that OplogEntries are cheaper to copy.
    std::shared_ptr<DurableOplogEntry> _preImageOp;
    std::shared_ptr<DurableOplogEntry> _postImageOp;

    bool _isForCappedCollection = false;
};

std::ostream& operator<<(std::ostream& s, const DurableOplogEntry& o);
std::ostream& operator<<(std::ostream& s, const OplogEntry& o);

inline bool operator==(const DurableOplogEntry& lhs, const DurableOplogEntry& rhs) {
    return SimpleBSONObjComparator::kInstance.evaluate(lhs.getRaw() == rhs.getRaw());
}

bool operator==(const OplogEntry& lhs, const OplogEntry& rhs);

std::ostream& operator<<(std::ostream& s, const ReplOperation& o);

}  // namespace repl
}  // namespace mongo
