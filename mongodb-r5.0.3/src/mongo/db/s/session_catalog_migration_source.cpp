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

#include "mongo/db/s/session_catalog_migration_source.h"

#include <memory>

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/session.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/write_concern.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

PseudoRandom hashGenerator(SecureRandom().nextInt64());

boost::optional<repl::OplogEntry> forgeNoopEntryFromImageCollection(
    OperationContext* opCtx, const repl::OplogEntry retryableFindAndModifyOplogEntry) {
    invariant(retryableFindAndModifyOplogEntry.getNeedsRetryImage());

    DBDirectClient client(opCtx);
    BSONObj imageObj =
        client.findOne(NamespaceString::kConfigImagesNamespace.ns(),
                       BSON("_id" << retryableFindAndModifyOplogEntry.getSessionId()->toBSON()),
                       nullptr);
    if (imageObj.isEmpty()) {
        return boost::none;
    }

    auto image = repl::ImageEntry::parse(IDLParserErrorContext("image entry"), imageObj);
    if (image.getTxnNumber() != retryableFindAndModifyOplogEntry.getTxnNumber()) {
        // In our snapshot, fetch the current transaction number for a session. If that transaction
        // number doesn't match what's found on the image lookup, it implies that the image is not
        // the correct version for this oplog entry. We will not forge a noop from it.
        return boost::none;
    }

    repl::MutableOplogEntry forgedNoop;
    forgedNoop.setSessionId(image.get_id());
    forgedNoop.setTxnNumber(image.getTxnNumber());
    forgedNoop.setObject(image.getImage());
    forgedNoop.setOpType(repl::OpTypeEnum::kNoop);
    // The wallclock and namespace are not directly available on the txn document when
    // forging the noop image document.
    forgedNoop.setWallClockTime(Date_t::now());
    forgedNoop.setNss(retryableFindAndModifyOplogEntry.getNss());
    forgedNoop.setUuid(retryableFindAndModifyOplogEntry.getUuid());
    // The OpTime is probably the last write time, but the destination will overwrite this
    // anyways. Just set an OpTime to satisfy the IDL constraints for calling `toBSON`.
    repl::OpTimeBase opTimeBase(Timestamp::min());
    opTimeBase.setTerm(-1);
    forgedNoop.setOpTimeBase(opTimeBase);
    forgedNoop.setStatementIds({0});
    forgedNoop.setPrevWriteOpTimeInTransaction(repl::OpTime(Timestamp::min(), -1));
    return repl::OplogEntry::parse(forgedNoop.toBSON()).getValue();
}

boost::optional<repl::OplogEntry> fetchPrePostImageOplog(OperationContext* opCtx,
                                                         repl::OplogEntry* oplog) {
    if (oplog->getNeedsRetryImage()) {
        auto ret = forgeNoopEntryFromImageCollection(opCtx, *oplog);
        if (ret == boost::none) {
            // No pre/post image was found. Defensively strip the `needsRetryImage` value to remove
            // any notion this operation was a retryable findAndModify. If the request is retried on
            // the destination, it will surface an error to the user.
            auto mutableOplog =
                fassert(5676405, repl::MutableOplogEntry::parse(oplog->getEntry().toBSON()));
            mutableOplog.setNeedsRetryImage(boost::none);
            *oplog = repl::OplogEntry(mutableOplog.toBSON());
        }
        return ret;
    }

    auto opTimeToFetch = oplog->getPreImageOpTime();
    if (!opTimeToFetch) {
        opTimeToFetch = oplog->getPostImageOpTime();
    }

    if (!opTimeToFetch) {
        return boost::none;
    }

    auto opTime = opTimeToFetch.value();
    DBDirectClient client(opCtx);
    auto oplogBSON =
        client.findOne(NamespaceString::kRsOplogNamespace.ns(), opTime.asQuery(), nullptr);

    return uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
}

/**
 * Creates an OplogEntry using the given field values
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                long long hash,
                                repl::OpTypeEnum opType,
                                const BSONObj& oField,
                                const boost::optional<BSONObj>& o2Field,
                                const OperationSessionInfo& sessionInfo,
                                Date_t wallClockTime,
                                const std::vector<StmtId>& statementIds) {
    return {
        repl::DurableOplogEntry(opTime,                           // optime
                                hash,                             // hash
                                opType,                           // op type
                                {},                               // namespace
                                boost::none,                      // uuid
                                boost::none,                      // fromMigrate
                                repl::OplogEntry::kOplogVersion,  // version
                                oField,                           // o
                                o2Field,                          // o2
                                sessionInfo,                      // session info
                                boost::none,                      // upsert
                                wallClockTime,                    // wall clock time
                                statementIds,                     // statement ids
                                boost::none,    // optime of previous write within same transaction
                                boost::none,    // pre-image optime
                                boost::none,    // post-image optime
                                boost::none,    // ShardId of resharding recipient
                                boost::none,    // _id
                                boost::none)};  // needsRetryImage
}

/**
 * Creates a special "write history lost" sentinel oplog entry.
 */
repl::OplogEntry makeSentinelOplogEntry(const LogicalSessionId& lsid,
                                        const TxnNumber& txnNumber,
                                        Date_t wallClockTime) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    return makeOplogEntry({},                                        // optime
                          hashGenerator.nextInt64(),                 // hash
                          repl::OpTypeEnum::kNoop,                   // op type
                          {},                                        // o
                          TransactionParticipant::kDeadEndSentinel,  // o2
                          sessionInfo,                               // session info
                          wallClockTime,                             // wall clock time
                          {kIncompleteHistoryStmtId});               // statement id
}

}  // namespace

SessionCatalogMigrationSource::SessionCatalogMigrationSource(OperationContext* opCtx,
                                                             NamespaceString ns,
                                                             ChunkRange chunk,
                                                             KeyPattern shardKey)
    : _ns(std::move(ns)),
      _rollbackIdAtInit(repl::ReplicationProcess::get(opCtx)->getRollbackID()),
      _chunkRange(std::move(chunk)),
      _keyPattern(shardKey) {
    Query query;
    // Sort is not needed for correctness. This is just for making it easier to write deterministic
    // tests.
    query.sort(BSON("_id" << 1));

    DBDirectClient client(opCtx);
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace, query);

    while (cursor->more()) {
        auto nextSession = SessionTxnRecord::parse(
            IDLParserErrorContext("Session migration cloning"), cursor->next());
        if (!nextSession.getLastWriteOpTime().isNull()) {
            _sessionOplogIterators.push_back(
                std::make_unique<SessionOplogIterator>(std::move(nextSession), _rollbackIdAtInit));
        }
    }

    {
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
        writeConflictRetry(
            opCtx,
            "session migration initialization majority commit barrier",
            NamespaceString::kRsOplogNamespace.ns(),
            [&] {
                const auto message = BSON("sessionMigrateCloneStart" << _ns.ns());

                WriteUnitOfWork wuow(opCtx);
                opCtx->getClient()->getServiceContext()->getOpObserver()->onInternalOpMessage(
                    opCtx,
                    _ns,
                    {},
                    {},
                    message,
                    boost::none,
                    boost::none,
                    boost::none,
                    boost::none);
                wuow.commit();
            });
    }

    auto opTimeToWait = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    WriteConcernResult result;
    WriteConcernOptions majority(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, 0);
    uassertStatusOK(waitForWriteConcern(opCtx, opTimeToWait, majority, &result));
}

bool SessionCatalogMigrationSource::hasMoreOplog() {
    if (_hasMoreOplogFromSessionCatalog()) {
        return true;
    }

    stdx::lock_guard<Latch> lk(_newOplogMutex);
    return _hasNewWrites(lk);
}

void SessionCatalogMigrationSource::onCommitCloneStarted() {
    stdx::lock_guard<Latch> _lk(_newOplogMutex);

    _state = State::kCommitStarted;
    if (_newOplogNotification) {
        _newOplogNotification->set(true);
        _newOplogNotification.reset();
    }
}

void SessionCatalogMigrationSource::onCloneCleanup() {
    stdx::lock_guard<Latch> _lk(_newOplogMutex);

    _state = State::kCleanup;
    if (_newOplogNotification) {
        _newOplogNotification->set(true);
        _newOplogNotification.reset();
    }
}

SessionCatalogMigrationSource::OplogResult SessionCatalogMigrationSource::getLastFetchedOplog() {
    {
        stdx::lock_guard<Latch> _lk(_sessionCloneMutex);
        if (_lastFetchedOplog) {
            return OplogResult(_lastFetchedOplog, false);
        }
    }

    {
        stdx::lock_guard<Latch> _lk(_newOplogMutex);
        if (_lastFetchedNewWriteOplogImage) {
            return OplogResult(_lastFetchedNewWriteOplogImage.get(), false);
        }

        return OplogResult(_lastFetchedNewWriteOplog, true);
    }
}

bool SessionCatalogMigrationSource::fetchNextOplog(OperationContext* opCtx) {
    if (_fetchNextOplogFromSessionCatalog(opCtx)) {
        return true;
    }

    return _fetchNextNewWriteOplog(opCtx);
}

std::shared_ptr<Notification<bool>> SessionCatalogMigrationSource::getNotificationForNewOplog() {
    invariant(!_hasMoreOplogFromSessionCatalog());

    stdx::lock_guard<Latch> lk(_newOplogMutex);

    if (_newOplogNotification) {
        return _newOplogNotification;
    }

    auto notification = std::make_shared<Notification<bool>>();
    if (_state == State::kCleanup) {
        notification->set(true);
    }
    // Even if commit has started, we still need to drain the current buffer.
    else if (_hasNewWrites(lk)) {
        notification->set(false);
    } else if (_state == State::kCommitStarted) {
        notification->set(true);
    } else {
        _newOplogNotification = notification;
    }

    return notification;
}

bool SessionCatalogMigrationSource::_handleWriteHistory(WithLock, OperationContext* opCtx) {
    while (_currentOplogIterator) {
        if (auto nextOplog = _currentOplogIterator->getNext(opCtx)) {
            auto nextStmtIds = nextOplog->getStatementIds();

            // Skip the rest of the chain for this session since the ns is unrelated with the
            // current one being migrated. It is ok to not check the rest of the chain because
            // retryable writes doesn't allow touching different namespaces.
            if (nextStmtIds.empty() ||
                (nextStmtIds.front() != kIncompleteHistoryStmtId && nextOplog->getNss() != _ns)) {
                _currentOplogIterator.reset();
                return false;
            }

            if (nextOplog->isCrudOpType()) {
                auto shardKey =
                    _keyPattern.extractShardKeyFromDoc(nextOplog->getObjectContainingDocumentKey());
                if (!_chunkRange.containsKey(shardKey)) {
                    continue;
                }
            }

            auto doc = fetchPrePostImageOplog(opCtx, &(nextOplog.get()));
            if (doc) {
                _lastFetchedOplogBuffer.push_back(*nextOplog);
                _lastFetchedOplog = *doc;
            } else {
                _lastFetchedOplog = *nextOplog;
            }

            return true;
        } else {
            _currentOplogIterator.reset();
        }
    }

    return false;
}

bool SessionCatalogMigrationSource::_hasMoreOplogFromSessionCatalog() {
    stdx::lock_guard<Latch> _lk(_sessionCloneMutex);
    return _lastFetchedOplog || !_lastFetchedOplogBuffer.empty() ||
        !_sessionOplogIterators.empty() || _currentOplogIterator;
}

bool SessionCatalogMigrationSource::_fetchNextOplogFromSessionCatalog(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_sessionCloneMutex);

    if (!_lastFetchedOplogBuffer.empty()) {
        _lastFetchedOplog = _lastFetchedOplogBuffer.back();
        _lastFetchedOplogBuffer.pop_back();
        return true;
    }

    _lastFetchedOplog.reset();

    if (_handleWriteHistory(lk, opCtx)) {
        return true;
    }

    while (!_sessionOplogIterators.empty()) {
        _currentOplogIterator = std::move(_sessionOplogIterators.back());
        _sessionOplogIterators.pop_back();

        if (_handleWriteHistory(lk, opCtx)) {
            return true;
        }
    }

    return false;
}

bool SessionCatalogMigrationSource::_hasNewWrites(WithLock) {
    return _lastFetchedNewWriteOplog || !_newWriteOpTimeList.empty();
}

bool SessionCatalogMigrationSource::_fetchNextNewWriteOplog(OperationContext* opCtx) {
    repl::OpTime nextOpTimeToFetch;
    EntryAtOpTimeType entryAtOpTimeType;

    {
        stdx::lock_guard<Latch> lk(_newOplogMutex);
        if (_lastFetchedNewWriteOplogImage) {
            // When `_lastFetchedNewWriteOplogImage` is set, it means we found an oplog entry with
            // `needsRetryImage`. At this step, we've already returned the image document, but we
            // have yet to return the original oplog entry stored in `_lastFetchedNewWriteOplog`. We
            // will unset this value and return such that the next call to `getLastFetchedOplog`
            // will return `_lastFetchedNewWriteOplog`.
            _lastFetchedNewWriteOplogImage.reset();
            return true;
        }

        if (_newWriteOpTimeList.empty()) {
            _lastFetchedNewWriteOplog.reset();
            return false;
        }

        std::tie(nextOpTimeToFetch, entryAtOpTimeType) = _newWriteOpTimeList.front();
    }

    DBDirectClient client(opCtx);
    const auto& newWriteOplogDoc = client.findOne(
        NamespaceString::kRsOplogNamespace.ns(), nextOpTimeToFetch.asQuery(), nullptr);

    uassert(40620,
            str::stream() << "Unable to fetch oplog entry with opTime: "
                          << nextOpTimeToFetch.toBSON(),
            !newWriteOplogDoc.isEmpty());

    auto newWriteOplogEntry = uassertStatusOK(repl::OplogEntry::parse(newWriteOplogDoc));

    // If this oplog entry corresponds to transaction prepare/commit, replace it with a sentinel
    // entry.
    if (entryAtOpTimeType == EntryAtOpTimeType::kTransaction) {
        newWriteOplogEntry =
            makeSentinelOplogEntry(*newWriteOplogEntry.getSessionId(),
                                   *newWriteOplogEntry.getTxnNumber(),
                                   opCtx->getServiceContext()->getFastClockSource()->now());
    }

    boost::optional<repl::OplogEntry> forgedNoopImage;
    if (newWriteOplogEntry.getNeedsRetryImage()) {
        // Generate the image outside of the mutex. Assign it atomically with the actual oplog
        // entry.
        forgedNoopImage = forgeNoopEntryFromImageCollection(opCtx, newWriteOplogEntry);
        if (forgedNoopImage == boost::none) {
            // No pre/post image was found. Defensively strip the `needsRetryImage` value to remove
            // any notion this operation was a retryable findAndModify. If the request is retried on
            // the destination, it will surface an error to the user.
            auto mutableOplog = fassert(5676404, repl::MutableOplogEntry::parse(newWriteOplogDoc));
            mutableOplog.setNeedsRetryImage(boost::none);
            newWriteOplogEntry = repl::OplogEntry(mutableOplog.toBSON());
        }
    }

    {
        stdx::lock_guard<Latch> lk(_newOplogMutex);
        _lastFetchedNewWriteOplog = newWriteOplogEntry;
        _newWriteOpTimeList.pop_front();

        if (forgedNoopImage) {
            _lastFetchedNewWriteOplogImage = forgedNoopImage;
        }
    }

    return true;
}

void SessionCatalogMigrationSource::notifyNewWriteOpTime(repl::OpTime opTime,
                                                         EntryAtOpTimeType entryAtOpTimeType) {
    stdx::lock_guard<Latch> lk(_newOplogMutex);
    _newWriteOpTimeList.emplace_back(opTime, entryAtOpTimeType);

    if (_newOplogNotification) {
        _newOplogNotification->set(false);
        _newOplogNotification.reset();
    }
}

SessionCatalogMigrationSource::SessionOplogIterator::SessionOplogIterator(
    SessionTxnRecord txnRecord, int expectedRollbackId)
    : _record(std::move(txnRecord)), _initialRollbackId(expectedRollbackId) {
    _writeHistoryIterator =
        std::make_unique<TransactionHistoryIterator>(_record.getLastWriteOpTime());
}

boost::optional<repl::OplogEntry> SessionCatalogMigrationSource::SessionOplogIterator::getNext(
    OperationContext* opCtx) {

    if (!_writeHistoryIterator || !_writeHistoryIterator->hasNext()) {
        return boost::none;
    }

    try {
        uassert(ErrorCodes::IncompleteTransactionHistory,
                str::stream() << "Cannot migrate multi-statement transaction state",
                !_record.getState());

        // Note: during SessionCatalogMigrationSource::init, we inserted a document and wait for it
        // to committed to the majority. In addition, the TransactionHistoryIterator uses OpTime
        // to query for the oplog. This means that if we can successfully fetch the oplog, we are
        // guaranteed that they are majority committed. If we can't fetch the oplog, it can either
        // mean that the oplog has been rolled over or was rolled back.
        return _writeHistoryIterator->next(opCtx);
    } catch (const AssertionException& excep) {
        if (excep.code() == ErrorCodes::IncompleteTransactionHistory) {
            // Note: no need to check if in replicaSet mode because having an iterator implies
            // oplog exists.
            auto rollbackId = repl::ReplicationProcess::get(opCtx)->getRollbackID();

            uassert(40656,
                    str::stream() << "rollback detected, rollbackId was " << _initialRollbackId
                                  << " but is now " << rollbackId,
                    rollbackId == _initialRollbackId);

            // If the rollbackId hasn't changed, and this record corresponds to a retryable write,
            // this means that the oplog has been truncated, so we return a sentinel oplog entry
            // indicating that the history for the retryable write has been lost. We also return
            // this sentinel entry for prepared or committed transaction records, since we don't
            // support retrying entire transactions.
            //
            // Otherwise, skip the record by returning boost::none.
            auto result = [&]() -> boost::optional<repl::OplogEntry> {
                if (!_record.getState() ||
                    _record.getState().get() == DurableTxnStateEnum::kCommitted ||
                    _record.getState().get() == DurableTxnStateEnum::kPrepared) {
                    return makeSentinelOplogEntry(
                        _record.getSessionId(),
                        _record.getTxnNum(),
                        opCtx->getServiceContext()->getFastClockSource()->now());
                } else {
                    return boost::none;
                }
            }();

            // Reset the iterator so that subsequent calls to getNext() will return boost::none.
            _writeHistoryIterator.reset();

            return result;
        }
        throw;
    }
}

}  // namespace mongo
