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

#include "mongo/dbtests/mock/mock_dbclient_connection.h"

#include "mongo/client/dbclient_mockcursor.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/time_support.h"

using mongo::BSONObj;

using std::string;
using std::vector;

namespace mongo {
MockDBClientConnection::MockDBClientConnection(MockRemoteDBServer* remoteServer, bool autoReconnect)
    : DBClientConnection(autoReconnect),
      _remoteServer(remoteServer),
      _sockCreationTime(mongo::curTimeMicros64()) {
    invariant(remoteServer);
    _remoteServerInstanceID = remoteServer->getInstanceID();
    _setServerRPCProtocols(rpc::supports::kAll);
    _callIter = _mockCallResponses.begin();
    _recvIter = _mockRecvResponses.begin();
}

MockDBClientConnection::~MockDBClientConnection() {}

bool MockDBClientConnection::connect(const char* hostName,
                                     StringData applicationName,
                                     std::string& errmsg) {
    _serverAddress = _remoteServer->getServerHostAndPort();
    if (_remoteServer->isRunning()) {
        _remoteServerInstanceID = _remoteServer->getInstanceID();
        _setServerRPCProtocols(rpc::supports::kAll);
        return true;
    }

    _failed.store(true);
    errmsg.assign("cannot connect to " + _remoteServer->getServerAddress());
    return false;
}

std::pair<rpc::UniqueReply, DBClientBase*> MockDBClientConnection::runCommandWithTarget(
    OpMsgRequest request) {

    checkConnection();

    try {
        _lastCursorMessage = boost::none;
        auto reply = _remoteServer->runCommand(_remoteServerInstanceID, request);
        auto status = getStatusFromCommandResult(reply->getCommandReply());
        // The real DBClientBase always throws HostUnreachable on network error, so we do the
        // same here.
        uassert(ErrorCodes::HostUnreachable,
                str::stream() << "network error while attempting to run "
                              << "command '" << request.getCommandName() << "' " << status,
                !ErrorCodes::isNetworkError(status));
        auto cursorRes = CursorResponse::parseFromBSON(reply->getCommandReply());
        if (cursorRes.isOK() && cursorRes.getValue().getCursorId() != 0) {
            _lastCursorMessage = request;
        }
        return {std::move(reply), this};
    } catch (const mongo::DBException&) {
        _failed.store(true);
        throw;
    }
}  // namespace mongo


std::unique_ptr<mongo::DBClientCursor> MockDBClientConnection::query(
    const NamespaceStringOrUUID& nsOrUuid,
    mongo::Query query,
    int nToReturn,
    int nToSkip,
    const BSONObj* fieldsToReturn,
    int queryOptions,
    int batchSize,
    boost::optional<BSONObj> readConcernObj) {
    checkConnection();

    try {
        mongo::BSONArray result(_remoteServer->query(_remoteServerInstanceID,
                                                     nsOrUuid,
                                                     query,
                                                     nToReturn,
                                                     nToSkip,
                                                     fieldsToReturn,
                                                     queryOptions,
                                                     batchSize,
                                                     readConcernObj));

        BSONArray resultsInCursor;

        // A simple mock implementation of a resumable query, where we skip the first 'n' fields
        // where 'n' is given by the mock resume token.
        auto nToSkip = 0;
        auto queryBson = fromjson(query.toString());
        if (queryBson.hasField("$_resumeAfter")) {
            if (queryBson["$_resumeAfter"].Obj().hasField("n")) {
                nToSkip = queryBson["$_resumeAfter"]["n"].numberInt();
            }
        }

        bool provideResumeToken = false;
        if (queryBson.hasField("$_requestResumeToken")) {
            provideResumeToken = true;
        }

        // Resume query.
        if (nToSkip != 0) {
            BSONObjIterator iter(result);
            BSONArrayBuilder builder;
            auto numExamined = 0;

            while (iter.more()) {
                numExamined++;

                if (numExamined < nToSkip + 1) {
                    iter.next();
                    continue;
                }

                builder.append(iter.next().Obj());
            }
            resultsInCursor = BSONArray(builder.obj());
        } else {
            // Yield all results instead (default).
            resultsInCursor = BSONArray(result.copy());
        }

        std::unique_ptr<mongo::DBClientCursor> cursor;
        cursor.reset(new DBClientMockCursor(
            this, BSONArray(resultsInCursor), provideResumeToken, batchSize));
        return cursor;
    } catch (const mongo::DBException&) {
        _failed.store(true);
        throw;
    }

    std::unique_ptr<mongo::DBClientCursor> nullPtr;
    return nullPtr;
}

mongo::ConnectionString::ConnectionType MockDBClientConnection::type() const {
    return mongo::ConnectionString::ConnectionType::kCustom;
}

unsigned long long MockDBClientConnection::query(
    std::function<void(mongo::DBClientCursorBatchIterator&)> f,
    const NamespaceStringOrUUID& nsOrUuid,
    mongo::Query query,
    const mongo::BSONObj* fieldsToReturn,
    int queryOptions,
    int batchSize,
    boost::optional<BSONObj> readConcernObj) {
    return DBClientBase::query(
        f, nsOrUuid, query, fieldsToReturn, queryOptions, batchSize, readConcernObj);
}

uint64_t MockDBClientConnection::getSockCreationMicroSec() const {
    return _sockCreationTime;
}

void MockDBClientConnection::insert(const string& ns,
                                    BSONObj obj,
                                    int flags,
                                    boost::optional<BSONObj> writeConcernObj) {
    _remoteServer->insert(ns, obj, flags);
}

void MockDBClientConnection::insert(const string& ns,
                                    const vector<BSONObj>& objList,
                                    int flags,
                                    boost::optional<BSONObj> writeConcernObj) {
    for (vector<BSONObj>::const_iterator iter = objList.begin(); iter != objList.end(); ++iter) {
        insert(ns, *iter, flags);
    }
}

void MockDBClientConnection::remove(const string& ns,
                                    Query query,
                                    int flags,
                                    boost::optional<BSONObj> writeConcernObj) {
    _remoteServer->remove(ns, query, flags);
}

void MockDBClientConnection::killCursor(const NamespaceString& ns, long long cursorID) {
    // It is not worth the bother of killing the cursor in the mock.
}

bool MockDBClientConnection::call(mongo::Message& toSend,
                                  mongo::Message& response,
                                  bool assertOk,
                                  string* actualServer) {
    // Here we check for a getMore command, and if it is that, we respond with the next
    // reply message from the previous command that returned a cursor response.
    // This allows us to mock commands with implicit cursors (e.g. listCollections).
    // It is not used for query() calls.
    if (_lastCursorMessage && !toSend.empty() && toSend.operation() == dbMsg) {
        // This might be a getMore.
        OpMsg parsedMsg;
        try {
            parsedMsg = OpMsg::parse(toSend);
        } catch (...) {
            // Any exceptions in parsing fall through to unsupported case.
        }
        if (!parsedMsg.body.isEmpty() &&
            parsedMsg.body.firstElement().fieldName() == "getMore"_sd) {
            auto reply = runCommandWithTarget(*_lastCursorMessage).first;
            response = reply.releaseMessage();
            return true;
        }
    }

    auto killSessionOnDisconnect = makeGuard([this] { shutdown(); });

    stdx::unique_lock lk(_netMutex);
    checkConnection();
    if (!isStillConnected() || !_remoteServer->isRunning()) {
        uasserted(ErrorCodes::SocketException, "Broken pipe in call");
    }

    _lastSentMessage = toSend;
    _mockCallResponsesCV.wait(lk, [&] {
        _blockedOnNetwork = (_callIter == _mockCallResponses.end());
        return !_blockedOnNetwork || !isStillConnected() || !_remoteServer->isRunning();
    });

    uassert(ErrorCodes::HostUnreachable,
            "Socket was shut down while in call",
            isStillConnected() && _remoteServer->isRunning());

    killSessionOnDisconnect.dismiss();

    const auto& swResponse = *_callIter;
    _callIter++;
    response = uassertStatusOK(swResponse);
    return true;
}

Status MockDBClientConnection::recv(mongo::Message& m, int lastRequestId) {
    auto killSessionOnDisconnect = makeGuard([this] { shutdown(); });

    stdx::unique_lock lk(_netMutex);
    if (!isStillConnected() || !_remoteServer->isRunning()) {
        return Status(ErrorCodes::SocketException, "Broken pipe in recv");
    }

    _mockRecvResponsesCV.wait(lk, [&] {
        _blockedOnNetwork = (_recvIter == _mockRecvResponses.end());
        return !_blockedOnNetwork || !isStillConnected() || !_remoteServer->isRunning();
    });

    if (!isStillConnected() || !_remoteServer->isRunning()) {
        return Status(ErrorCodes::HostUnreachable, "Socket was shut down while in recv");
    }

    killSessionOnDisconnect.dismiss();

    const auto& swResponse = *_recvIter;
    _recvIter++;
    m = uassertStatusOK(swResponse);
    return Status::OK();
}

void MockDBClientConnection::shutdown() {
    stdx::lock_guard lk(_netMutex);
    DBClientConnection::shutdown();
    _mockCallResponsesCV.notify_all();
    _mockRecvResponsesCV.notify_all();
}

void MockDBClientConnection::shutdownAndDisallowReconnect() {
    stdx::lock_guard lk(_netMutex);
    DBClientConnection::shutdownAndDisallowReconnect();
    _mockCallResponsesCV.notify_all();
    _mockRecvResponsesCV.notify_all();
}

void MockDBClientConnection::setCallResponses(Responses responses) {
    stdx::lock_guard lk(_netMutex);
    _mockCallResponses = std::move(responses);
    _callIter = _mockCallResponses.begin();
    if (_blockedOnNetwork && !_mockCallResponses.empty()) {
        _blockedOnNetwork = false;
        _mockCallResponsesCV.notify_all();
    }
}

void MockDBClientConnection::setRecvResponses(Responses responses) {
    stdx::lock_guard lk(_netMutex);
    _mockRecvResponses = std::move(responses);
    _recvIter = _mockRecvResponses.begin();
    if (_blockedOnNetwork && !_mockRecvResponses.empty()) {
        _blockedOnNetwork = false;
        _mockRecvResponsesCV.notify_all();
    }
}

void MockDBClientConnection::say(mongo::Message& toSend, bool isRetry, string* actualServer) {
    invariant(false);  // unimplemented
}

bool MockDBClientConnection::lazySupported() const {
    invariant(false);  // unimplemented
    return false;
}

void MockDBClientConnection::checkConnection() {
    if (_failed.load()) {
        uassert(ErrorCodes::SocketException, toString(), autoReconnect);
        uassert(ErrorCodes::HostUnreachable,
                "cannot connect to " + _remoteServer->getServerAddress(),
                _remoteServer->isRunning());
        _remoteServerInstanceID = _remoteServer->getInstanceID();
        _failed.store(false);
    }
}
}  // namespace mongo
