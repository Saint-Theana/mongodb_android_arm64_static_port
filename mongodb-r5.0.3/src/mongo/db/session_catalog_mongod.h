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

#include "mongo/db/session_catalog.h"

namespace mongo {

class SessionsCollection;

class MongoDSessionCatalog {
public:
    /**
     * Invoked when the node enters the primary state. Ensures that the transactions collection is
     * created. Throws on severe exceptions due to which it is not safe to continue the step-up
     * process.
     */
    static void onStepUp(OperationContext* opCtx);

    /**
     * Fetches the UUID of the transaction table, or an empty optional if the collection does not
     * exist or has no UUID. Acquires a lock on the collection.
     *
     * Required for rollback via refetch.
     */
    static boost::optional<UUID> getTransactionTableUUID(OperationContext* opCtx);

    /**
     * Callback to be invoked in response to insert/update/delete of 'config.transactions' in order
     * to notify the session catalog that the on-disk contents are out of sync with the in-memory
     * state. The 'singleSessionDoc' must contain the _id of the session which was updated.
     */
    static void observeDirectWriteToConfigTransactions(OperationContext* opCtx,
                                                       BSONObj singleSessionDoc);

    /**
     * Callback to be invoked when the contents of 'config.transactions' are out of sync with that
     * in the in-memory catalog, such as when rollback happens or drop of 'config.transactions'.
     */
    static void invalidateAllSessions(OperationContext* opCtx);

    /**
     * Locates session entries from the in-memory catalog and in 'config.transactions' which have
     * not been referenced before 'possiblyExpired' and deletes them.
     *
     * Returns the number of sessions, which were reaped from the persisted store on disk.
     */
    static int reapSessionsOlderThan(OperationContext* opCtx,
                                     SessionsCollection& sessionsCollection,
                                     Date_t possiblyExpired);
};

/**
 * Scoped object, which checks out the session specified in the passed operation context and stores
 * it for later access by the command. The session is installed at construction time and is removed
 * at destruction.
 */
class MongoDOperationContextSession {
public:
    MongoDOperationContextSession(OperationContext* opCtx);
    ~MongoDOperationContextSession();

    /**
     * This method takes an operation context with a checked-out session and allows it to be
     * temporarily or permanently checked back in, in order to allow other operations to use it.
     *
     * May only be called if the session has actually been checked out previously.
     */
    static void checkIn(OperationContext* opCtx);

    /**
     * May only be called if the session is not checked out already.
     */
    static void checkOut(OperationContext* opCtx);

private:
    OperationContextSession _operationContextSession;
};

/**
 * Similar to MongoDOperationContextSession, but marks the TransactionParticipant as valid without
 * refreshing from disk and starts a new transaction unconditionally.
 *
 * NOTE: Only used by the replication oplog application logic on secondaries in order to replay
 * prepared transactions.
 */
class MongoDOperationContextSessionWithoutRefresh {
public:
    MongoDOperationContextSessionWithoutRefresh(OperationContext* opCtx);
    ~MongoDOperationContextSessionWithoutRefresh();

private:
    OperationContextSession _operationContextSession;
    OperationContext* const _opCtx;
};

/**
 * Similar to MongoDOperationContextSession, but marks the TransactionParticipant as valid without
 * loading the retryable write oplog history.  If the last operation was a multi-document
 * transaction, is equivalent to MongoDOperationContextSession.
 *
 * NOTE: Should only be used when reading the oplog history is not possible.
 */
class MongoDOperationContextSessionWithoutOplogRead {
public:
    MongoDOperationContextSessionWithoutOplogRead(OperationContext* opCtx);
    ~MongoDOperationContextSessionWithoutOplogRead();

private:
    OperationContextSession _operationContextSession;
    OperationContext* const _opCtx;
};

}  // namespace mongo
