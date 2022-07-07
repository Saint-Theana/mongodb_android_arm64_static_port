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

/**
 * Connect to a Mongo database as a database, from C++.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_cursor.h"

#include <memory>

#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/object_check.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/exit.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {
Message assembleCommandRequest(DBClientBase* cli,
                               StringData database,
                               int legacyQueryOptions,
                               BSONObj legacyQuery) {
    auto request = rpc::upconvertRequest(database, std::move(legacyQuery), legacyQueryOptions);

    if (cli->getRequestMetadataWriter()) {
        BSONObjBuilder bodyBob(std::move(request.body));
        auto opCtx = (haveClient() ? cc().getOperationContext() : nullptr);
        uassertStatusOK(cli->getRequestMetadataWriter()(opCtx, &bodyBob));
        request.body = bodyBob.obj();
    }

    return rpc::messageFromOpMsgRequest(
        cli->getClientRPCProtocols(), cli->getServerRPCProtocols(), std::move(request));
}

}  // namespace

int DBClientCursor::nextBatchSize() {
    if (nToReturn == 0)
        return batchSize;

    if (batchSize == 0)
        return nToReturn;

    return batchSize < nToReturn ? batchSize : nToReturn;
}

Message DBClientCursor::_assembleInit() {
    if (cursorId) {
        return _assembleGetMore();
    }

    // If we haven't gotten a cursorId yet, we need to issue a new query or command.
    if (_isCommand) {
        // HACK:
        // Unfortunately, this code is used by the shell to run commands,
        // so we need to allow the shell to send invalid options so that we can
        // test that the server rejects them. Thus, to allow generating commands with
        // invalid options, we validate them here, and fall back to generating an OP_QUERY
        // through assembleQueryRequest if the options are invalid.
        bool hasValidNToReturnForCommand = (nToReturn == 1 || nToReturn == -1);
        bool hasValidFlagsForCommand = !(opts & mongo::QueryOption_Exhaust);
        bool hasInvalidMaxTimeMs = query.hasField("$maxTimeMS");

        if (hasValidNToReturnForCommand && hasValidFlagsForCommand && !hasInvalidMaxTimeMs) {
            return assembleCommandRequest(_client, ns.db(), opts, query);
        }
    } else if (_useFindCommand) {
        // The caller supplies a 'query' object which may have $-prefixed directives in the format
        // expected for a legacy OP_QUERY. Therefore, we use the legacy parsing code supplied by
        // query_request_helper. When actually issuing the request to the remote node, we will
        // assemble a find command.
        bool explain = false;
        auto findCommand =
            query_request_helper::fromLegacyQuery(_nsOrUuid,
                                                  query,
                                                  fieldsToReturn ? *fieldsToReturn : BSONObj(),
                                                  nToSkip,
                                                  nextBatchSize(),
                                                  opts,
                                                  &explain);
        if (findCommand.isOK() && !explain) {
            if (query.getBoolField("$readOnce")) {
                // Legacy queries don't handle readOnce.
                findCommand.getValue()->setReadOnce(true);
            }
            if (query.getBoolField(FindCommandRequest::kRequestResumeTokenFieldName)) {
                // Legacy queries don't handle requestResumeToken.
                findCommand.getValue()->setRequestResumeToken(true);
            }
            if (query.hasField(FindCommandRequest::kResumeAfterFieldName)) {
                // Legacy queries don't handle resumeAfter.
                findCommand.getValue()->setResumeAfter(
                    query.getObjectField(FindCommandRequest::kResumeAfterFieldName));
            }
            if (auto replTerm = query[FindCommandRequest::kTermFieldName]) {
                // Legacy queries don't handle term.
                findCommand.getValue()->setTerm(replTerm.numberLong());
            }
            // Legacy queries don't handle readConcern.
            // We prioritize the readConcern parsed from the query object over '_readConcernObj'.
            if (auto readConcern = query[repl::ReadConcernArgs::kReadConcernFieldName]) {
                findCommand.getValue()->setReadConcern(readConcern.Obj());
            } else if (_readConcernObj) {
                findCommand.getValue()->setReadConcern(_readConcernObj);
            }
            BSONObj cmd = findCommand.getValue()->toBSON(BSONObj());

            if (auto readPref = query["$readPreference"]) {
                // FindCommandRequest doesn't handle $readPreference.
                cmd = BSONObjBuilder(std::move(cmd)).append(readPref).obj();
            }
            return assembleCommandRequest(_client, ns.db(), opts, std::move(cmd));
        }
        // else use legacy OP_QUERY request.
        // Legacy OP_QUERY request does not support UUIDs.
        if (_nsOrUuid.uuid()) {
            // If there was a problem building the query request, report that.
            uassertStatusOK(findCommand.getStatus());
            // Otherwise it must have been explain.
            uasserted(50937, "Query by UUID is not supported for explain queries.");
        }
    }

    _useFindCommand = false;  // Make sure we handle the reply correctly.
    Message toSend;
    assembleQueryRequest(ns.ns(), query, nextBatchSize(), nToSkip, fieldsToReturn, opts, toSend);
    return toSend;
}

Message DBClientCursor::_assembleGetMore() {
    invariant(cursorId);
    if (_useFindCommand) {
        std::int64_t batchSize = nextBatchSize();
        auto gmr = GetMoreRequest(ns,
                                  cursorId,
                                  boost::make_optional(batchSize != 0, batchSize),
                                  boost::make_optional(tailableAwaitData(),
                                                       _awaitDataTimeout),  // awaitDataTimeout
                                  _term,
                                  _lastKnownCommittedOpTime);
        auto msg = assembleCommandRequest(_client, ns.db(), opts, gmr.toBSON());
        // Set the exhaust flag if needed.
        if (opts & QueryOption_Exhaust && msg.operation() == dbMsg) {
            OpMsg::setFlag(&msg, OpMsg::kExhaustSupported);
        }
        return msg;
    } else {
        // Assemble a legacy getMore request.
        return makeGetMoreMessage(ns.ns(), cursorId, nextBatchSize(), opts);
    }
}

bool DBClientCursor::init() {
    invariant(!_connectionHasPendingReplies);
    Message toSend = _assembleInit();
    verify(_client);
    Message reply;
    try {
        _client->call(toSend, reply, true, &_originalHost);
    } catch (const DBException&) {
        // log msg temp?
        LOGV2(20127, "DBClientCursor::init call() failed");
        // We always want to throw on network exceptions.
        throw;
    }
    if (reply.empty()) {
        // log msg temp?
        LOGV2(20128, "DBClientCursor::init message from call() was empty");
        return false;
    }
    dataReceived(reply);
    return true;
}

void DBClientCursor::initLazy(bool isRetry) {
    massert(15875,
            "DBClientCursor::initLazy called on a client that doesn't support lazy",
            _client->lazySupported());
    Message toSend = _assembleInit();
    _client->say(toSend, isRetry, &_originalHost);
    _lastRequestId = toSend.header().getId();
    _connectionHasPendingReplies = true;
}

bool DBClientCursor::initLazyFinish(bool& retry) {
    invariant(_connectionHasPendingReplies);
    Message reply;
    Status recvStatus = _client->recv(reply, _lastRequestId);
    _connectionHasPendingReplies = false;

    // If we get a bad response, return false
    if (!recvStatus.isOK() || reply.empty()) {
        if (!recvStatus.isOK())
            LOGV2(20129,
                  "DBClientCursor::init lazy say() failed: {error}",
                  "DBClientCursor::init lazy say() failed",
                  "error"_attr = redact(recvStatus));
        if (reply.empty())
            LOGV2(20130, "DBClientCursor::init message from say() was empty");

        _client->checkResponse({}, true, &retry, &_lazyHost);

        return false;
    }

    dataReceived(reply, retry, _lazyHost);

    return !retry;
}

void DBClientCursor::requestMore() {
    // For exhaust queries, once the stream has been initiated we get data blasted to us
    // from the remote server, without a need to send any more 'getMore' requests.
    const auto isExhaust = opts & QueryOption_Exhaust;
    if (isExhaust && (!_useFindCommand || _connectionHasPendingReplies)) {
        return exhaustReceiveMore();
    }

    invariant(!_connectionHasPendingReplies);
    verify(cursorId && batch.pos == batch.objs.size());

    if (haveLimit) {
        nToReturn -= batch.objs.size();
        verify(nToReturn > 0);
    }

    auto doRequestMore = [&] {
        Message toSend = _assembleGetMore();
        Message response;
        _client->call(toSend, response);
        dataReceived(response);
    };
    if (_client)
        return doRequestMore();

    invariant(_scopedHost.size());
    DBClientBase::withConnection_do_not_use(_scopedHost, [&](DBClientBase* conn) {
        ON_BLOCK_EXIT([&, origClient = _client] { _client = origClient; });
        _client = conn;
        doRequestMore();
    });
}

/**
 * With QueryOption_Exhaust, the server just blasts data at us. The end of a stream is marked with a
 * cursor id of 0.
 */
void DBClientCursor::exhaustReceiveMore() {
    verify(cursorId);
    verify(batch.pos == batch.objs.size());
    uassert(40675, "Cannot have limit for exhaust query", !haveLimit);
    Message response;
    verify(_client);
    uassertStatusOK(
        _client->recv(response, _lastRequestId).withContext("recv failed while exhausting cursor"));
    dataReceived(response);
}

BSONObj DBClientCursor::commandDataReceived(const Message& reply) {
    int op = reply.operation();
    invariant(op == opReply || op == dbMsg);

    // Check if the reply indicates that it is part of an exhaust stream.
    const auto isExhaust = OpMsg::isFlagSet(reply, OpMsg::kMoreToCome);
    _connectionHasPendingReplies = isExhaust;
    if (isExhaust) {
        _lastRequestId = reply.header().getId();
    }

    auto commandReply = _client->parseCommandReplyMessage(_client->getServerAddress(), reply);
    auto commandStatus = getStatusFromCommandResult(commandReply->getCommandReply());

    if (commandStatus == ErrorCodes::StaleConfig) {
        uassertStatusOK(
            commandStatus.withContext("stale config in DBClientCursor::dataReceived()"));
    } else if (!commandStatus.isOK()) {
        wasError = true;
    }

    return commandReply->getCommandReply().getOwned();
}

void DBClientCursor::dataReceived(const Message& reply, bool& retry, string& host) {
    batch.objs.clear();
    batch.pos = 0;

    // If this is a reply to our initial command request.
    if (_isCommand && cursorId == 0) {
        batch.objs.push_back(commandDataReceived(reply));
        return;
    }

    if (_useFindCommand) {
        const auto replyObj = commandDataReceived(reply);
        cursorId = 0;  // Don't try to kill cursor if we get back an error.
        auto cr = uassertStatusOK(CursorResponse::parseFromBSON(replyObj));
        cursorId = cr.getCursorId();
        uassert(50935,
                "Received a getMore response with a cursor id of 0 and the moreToCome flag set.",
                !(_connectionHasPendingReplies && cursorId == 0));

        ns = cr.getNSS();  // Unlike OP_REPLY, find command can change the ns to use for getMores.
        // Store the resume token, if we got one.
        _postBatchResumeToken = cr.getPostBatchResumeToken();
        batch.objs = cr.releaseBatch();

        if (replyObj.hasField(LogicalTime::kOperationTimeFieldName)) {
            _operationTime = LogicalTime::fromOperationTime(replyObj).asTimestamp();
        }
        return;
    }

    QueryResult::View qr = reply.singleData().view2ptr();
    resultFlags = qr.getResultFlags();

    if (resultFlags & ResultFlag_ErrSet) {
        wasError = true;
    }

    if (resultFlags & ResultFlag_CursorNotFound) {
        // cursor id no longer valid at the server.
        invariant(qr.getCursorId() == 0);

        // 0 indicates no longer valid (dead).
        cursorId = 0;

        uasserted(ErrorCodes::CursorNotFound,
                  str::stream() << "cursor id " << cursorId << " didn't exist on server.");
    }

    if (cursorId == 0 || !(opts & QueryOption_CursorTailable)) {
        // only set initially: we don't want to kill it on end of data
        // if it's a tailable cursor
        cursorId = qr.getCursorId();
    }

    if (opts & QueryOption_Exhaust) {
        // With exhaust mode, each reply after the first claims to be a reply to the previous one
        // rather than the initial request.
        _connectionHasPendingReplies = (cursorId != 0);
        _lastRequestId = reply.header().getId();
    }

    batch.objs.reserve(qr.getNReturned());

    BufReader data(qr.data(), qr.dataLen());
    while (static_cast<int>(batch.objs.size()) < qr.getNReturned()) {
        if (serverGlobalParams.objcheck) {
            batch.objs.push_back(data.read<Validated<BSONObj>>());
        } else {
            batch.objs.push_back(data.read<BSONObj>());
        }
        batch.objs.back().shareOwnershipWith(reply.sharedBuffer());
    }
    uassert(ErrorCodes::InvalidBSON,
            "Got invalid reply from external server while reading from cursor",
            data.atEof());

    _client->checkResponse(batch.objs, false, &retry, &host);  // watches for "not primary"

    tassert(5262101,
            "Deprecated ShardConfigStale flag encountered in query result",
            !(resultFlags & ResultFlag_ShardConfigStaleDeprecated));

    /* this assert would fire the way we currently work:
        verify( nReturned || cursorId == 0 );
    */
}

/** If true, safe to call next().  Requests more from server if necessary. */
bool DBClientCursor::more() {
    if (!_putBack.empty())
        return true;

    if (haveLimit && static_cast<int>(batch.pos) >= nToReturn)
        return false;

    if (batch.pos < batch.objs.size())
        return true;

    if (cursorId == 0)
        return false;

    requestMore();
    return batch.pos < batch.objs.size();
}

BSONObj DBClientCursor::next() {
    if (!_putBack.empty()) {
        BSONObj ret = _putBack.top();
        _putBack.pop();
        return ret;
    }

    uassert(
        13422, "DBClientCursor next() called but more() is false", batch.pos < batch.objs.size());

    /* todo would be good to make data null at end of batch for safety */
    return std::move(batch.objs[batch.pos++]);
}

BSONObj DBClientCursor::nextSafe() {
    BSONObj o = next();

    // Only convert legacy errors ($err) to exceptions. Otherwise, just return the response and the
    // caller will interpret it as a command error.
    if (wasError && strcmp(o.firstElementFieldName(), "$err") == 0) {
        uassertStatusOK(getStatusFromCommandResult(o));
    }

    return o;
}

void DBClientCursor::peek(vector<BSONObj>& v, int atMost) {
    auto end = atMost >= static_cast<int>(batch.objs.size() - batch.pos)
        ? batch.objs.end()
        : batch.objs.begin() + batch.pos + atMost;
    v.insert(v.end(), batch.objs.begin() + batch.pos, end);
}

BSONObj DBClientCursor::peekFirst() {
    vector<BSONObj> v;
    peek(v, 1);

    if (v.size() > 0)
        return v[0];
    else
        return BSONObj();
}

bool DBClientCursor::peekError(BSONObj* error) {
    if (!wasError)
        return false;

    vector<BSONObj> v;
    peek(v, 1);

    verify(v.size() == 1);
    // We check both the legacy error format, and the new error format. hasErrField checks for
    // $err, and getStatusFromCommandResult checks for modern errors of the form '{ok: 0.0, code:
    // <...>, errmsg: ...}'.
    verify(hasErrField(v[0]) || !getStatusFromCommandResult(v[0]).isOK());

    if (error)
        *error = v[0].getOwned();
    return true;
}

void DBClientCursor::attach(AScopedConnection* conn) {
    verify(_scopedHost.size() == 0);
    verify(conn);
    verify(conn->get());

    if (conn->get()->type() == ConnectionString::ConnectionType::kReplicaSet) {
        if (_lazyHost.size() > 0)
            _scopedHost = _lazyHost;
        else if (_client)
            _scopedHost = _client->getServerAddress();
        else
            massert(14821,
                    "No client or lazy client specified, cannot store multi-host connection.",
                    false);
    } else {
        _scopedHost = conn->getHost();
    }

    conn->done();
    _client = nullptr;
    _lazyHost = "";
}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const NamespaceStringOrUUID& nsOrUuid,
                               const BSONObj& query,
                               int nToReturn,
                               int nToSkip,
                               const BSONObj* fieldsToReturn,
                               int queryOptions,
                               int batchSize,
                               boost::optional<BSONObj> readConcernObj)
    : DBClientCursor(client,
                     nsOrUuid,
                     query,
                     0,  // cursorId
                     nToReturn,
                     nToSkip,
                     fieldsToReturn,
                     queryOptions,
                     batchSize,
                     {},
                     readConcernObj,
                     boost::none) {}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const NamespaceStringOrUUID& nsOrUuid,
                               long long cursorId,
                               int nToReturn,
                               int queryOptions,
                               std::vector<BSONObj> initialBatch,
                               boost::optional<Timestamp> operationTime,
                               boost::optional<BSONObj> postBatchResumeToken)
    : DBClientCursor(client,
                     nsOrUuid,
                     BSONObj(),  // query
                     cursorId,
                     nToReturn,
                     0,        // nToSkip
                     nullptr,  // fieldsToReturn
                     queryOptions,
                     0,
                     std::move(initialBatch),  // batchSize
                     boost::none,
                     operationTime,
                     postBatchResumeToken) {}

DBClientCursor::DBClientCursor(DBClientBase* client,
                               const NamespaceStringOrUUID& nsOrUuid,
                               const BSONObj& query,
                               long long cursorId,
                               int nToReturn,
                               int nToSkip,
                               const BSONObj* fieldsToReturn,
                               int queryOptions,
                               int batchSize,
                               std::vector<BSONObj> initialBatch,
                               boost::optional<BSONObj> readConcernObj,
                               boost::optional<Timestamp> operationTime,
                               boost::optional<BSONObj> postBatchResumeToken)
    : batch{std::move(initialBatch)},
      _client(client),
      _originalHost(_client->getServerAddress()),
      _nsOrUuid(nsOrUuid),
      ns(nsOrUuid.nss() ? *nsOrUuid.nss() : NamespaceString(nsOrUuid.dbname())),
      _isCommand(ns.isCommand()),
      query(query),
      nToReturn(nToReturn),
      haveLimit(nToReturn > 0 && !(queryOptions & QueryOption_CursorTailable)),
      nToSkip(nToSkip),
      fieldsToReturn(fieldsToReturn),
      opts(queryOptions & ~QueryOptionLocal_forceOpQuery),
      batchSize(batchSize == 1 ? 2 : batchSize),
      resultFlags(0),
      cursorId(cursorId),
      _ownCursor(true),
      wasError(false),
      _readConcernObj(readConcernObj),
      _operationTime(operationTime),
      _postBatchResumeToken(postBatchResumeToken) {
    if (queryOptions & QueryOptionLocal_forceOpQuery) {
        // Legacy OP_QUERY does not support UUIDs.
        invariant(!_nsOrUuid.uuid());
        _useFindCommand = false;
    }
}

/* static */
StatusWith<std::unique_ptr<DBClientCursor>> DBClientCursor::fromAggregationRequest(
    DBClientBase* client, AggregateCommandRequest aggRequest, bool secondaryOk, bool useExhaust) {
    BSONObj ret;
    try {
        if (!client->runCommand(aggRequest.getNamespace().db().toString(),
                                aggregation_request_helper::serializeToCommandObj(aggRequest),
                                ret,
                                secondaryOk ? QueryOption_SecondaryOk : 0)) {
            return getStatusFromCommandResult(ret);
        }
    } catch (...) {
        return exceptionToStatus();
    }
    long long cursorId = ret["cursor"].Obj()["id"].Long();
    std::vector<BSONObj> firstBatch;
    for (BSONElement elem : ret["cursor"].Obj()["firstBatch"].Array()) {
        firstBatch.emplace_back(elem.Obj().getOwned());
    }
    boost::optional<BSONObj> postBatchResumeToken;
    if (auto postBatchResumeTokenElem = ret["cursor"].Obj()["postBatchResumeToken"];
        postBatchResumeTokenElem.type() == BSONType::Object) {
        postBatchResumeToken = postBatchResumeTokenElem.Obj().getOwned();
    } else if (ret["cursor"].Obj().hasField("postBatchResumeToken")) {
        return Status(ErrorCodes::Error(5761702),
                      "Expected field 'postbatchResumeToken' to be of object type");
    }

    boost::optional<Timestamp> operationTime = boost::none;
    if (ret.hasField(LogicalTime::kOperationTimeFieldName)) {
        operationTime = LogicalTime::fromOperationTime(ret).asTimestamp();
    }

    return {std::make_unique<DBClientCursor>(client,
                                             aggRequest.getNamespace(),
                                             cursorId,
                                             0,
                                             useExhaust ? QueryOption_Exhaust : 0,
                                             firstBatch,
                                             operationTime,
                                             postBatchResumeToken)};
}

DBClientCursor::~DBClientCursor() {
    kill();
}

void DBClientCursor::kill() {
    DESTRUCTOR_GUARD({
        if (cursorId && _ownCursor && !globalInShutdownDeprecated()) {
            auto killCursor = [&](auto&& conn) {
                if (_useFindCommand) {
                    conn->killCursor(ns, cursorId);
                } else {
                    auto toSend = makeKillCursorsMessage(cursorId);
                    conn->say(toSend);
                }
            };

            // We only need to kill the cursor if there aren't pending replies. Pending replies
            // indicates that this is an exhaust cursor, so the connection must be closed and the
            // cursor will automatically be cleaned up by the upstream server.
            if (_client && !_connectionHasPendingReplies) {
                killCursor(_client);
            }
        }
    });

    // Mark this cursor as dead since we can't do any getMores.
    cursorId = 0;
}


}  // namespace mongo
