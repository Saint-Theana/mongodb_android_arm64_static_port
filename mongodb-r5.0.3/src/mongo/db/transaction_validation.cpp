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

#include "mongo/db/transaction_validation.h"

#include <fmt/format.h>

#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

using namespace fmt::literals;

namespace {

const StringMap<int> retryableWriteCommands = {{"delete", 1},
                                               {"findandmodify", 1},
                                               {"findAndModify", 1},
                                               {"insert", 1},
                                               {"update", 1},
                                               {"_recvChunkStart", 1},
                                               {"_configsvrRemoveChunks", 1},
                                               {"_configsvrRemoveTags", 1},
                                               {"_shardsvrCreateCollectionParticipant", 1},
                                               {"_shardsvrDropCollectionParticipant", 1},
                                               {"_shardsvrRenameCollectionParticipant", 1},
                                               {"_shardsvrRenameCollectionParticipantUnblock", 1},
                                               {"_configsvrRenameCollectionMetadata", 1}};

// Commands that can be sent with session info but should not check out a session.
const StringMap<int> skipSessionCheckoutList = {
    {"coordinateCommitTransaction", 1}, {"_recvChunkStart", 1}, {"replSetStepDown", 1}};

const StringMap<int> transactionCommands = {{"commitTransaction", 1},
                                            {"coordinateCommitTransaction", 1},
                                            {"abortTransaction", 1},
                                            {"prepareTransaction", 1}};

bool isRetryableWriteCommand(StringData cmdName) {
    return retryableWriteCommands.find(cmdName) != retryableWriteCommands.cend();
}

}  // namespace

bool isTransactionCommand(StringData cmdName) {
    return transactionCommands.find(cmdName) != transactionCommands.cend();
}

void validateWriteConcernForTransaction(const WriteConcernOptions& wcResult, StringData cmdName) {
    uassert(ErrorCodes::InvalidOptions,
            "writeConcern is not allowed within a multi-statement transaction",
            wcResult.usedDefaultConstructedWC || isTransactionCommand(cmdName));
}

bool isReadConcernLevelAllowedInTransaction(repl::ReadConcernLevel readConcernLevel) {
    return readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern;
}

bool shouldCommandSkipSessionCheckout(StringData cmdName) {
    return skipSessionCheckoutList.find(cmdName) != skipSessionCheckoutList.cend();
}

void validateSessionOptions(const OperationSessionInfoFromClient& sessionOptions,
                            StringData cmdName,
                            const NamespaceString& nss,
                            bool allowTransactionsOnConfigDatabase) {
    if (sessionOptions.getAutocommit()) {
        CommandHelpers::canUseTransactions(nss, cmdName, allowTransactionsOnConfigDatabase);
    }

    if (!sessionOptions.getAutocommit() && sessionOptions.getTxnNumber()) {
        uassert(ErrorCodes::NotARetryableWriteCommand,
                "txnNumber may only be provided for multi-document transactions and retryable "
                "write commands. autocommit:false was not provided, and {} is not a retryable "
                "write command."_format(cmdName),
                isRetryableWriteCommand(cmdName));
    }

    if (sessionOptions.getStartTransaction()) {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot run killCursors as the first operation in a multi-document transaction.",
                cmdName != "killCursors");

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot start a transaction with a prepare",
                cmdName != PrepareTransaction::kCommandName);

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot start a transaction with a commit",
                cmdName != CommitTransaction::kCommandName);

        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot start a transaction with an abort",
                cmdName != AbortTransaction::kCommandName);
    }
}

}  // namespace mongo
