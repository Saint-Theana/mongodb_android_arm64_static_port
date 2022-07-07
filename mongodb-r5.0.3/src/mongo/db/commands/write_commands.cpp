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

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/commands/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/doc_validation_error.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangWriteBeforeWaitingForMigrationDecision);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeCommit);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeWrite);
MONGO_FAIL_POINT_DEFINE(failUnorderedTimeseriesInsert);

void redactTooLongLog(mutablebson::Document* cmdObj, StringData fieldName) {
    namespace mmb = mutablebson;
    mmb::Element root = cmdObj->root();
    mmb::Element field = root.findFirstChildNamed(fieldName);

    // If the cmdObj is too large, it will be a "too big" message given by CachedBSONObj.get()
    if (!field.ok()) {
        return;
    }

    // Redact the log if there are more than one documents or operations.
    if (field.countChildren() > 1) {
        field.setValueInt(field.countChildren()).transitional_ignore();
    }
}

bool shouldSkipOutput(OperationContext* opCtx) {
    const WriteConcernOptions& writeConcern = opCtx->getWriteConcern();
    return writeConcern.wMode.empty() && writeConcern.wNumNodes == 0 &&
        (writeConcern.syncMode == WriteConcernOptions::SyncMode::NONE ||
         writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET);
}

/**
 * Returns true if 'ns' is a time-series collection. That is, this namespace is backed by a
 * time-series buckets collection.
 */
bool isTimeseries(OperationContext* opCtx, const NamespaceString& ns) {
    // If the buckets collection exists now, the time-series insert path will check for the
    // existence of the buckets collection later on with a lock.
    // If this check is concurrent with the creation of a time-series collection and the buckets
    // collection does not yet exist, this check may return false unnecessarily. As a result, an
    // insert attempt into the time-series namespace will either succeed or fail, depending on who
    // wins the race.
    auto bucketsNs = ns.makeTimeseriesBucketsNamespace();
    return CollectionCatalog::get(opCtx)
        ->lookupCollectionByNamespaceForRead(opCtx, bucketsNs)
        .get();
}

// Default for control.version in time-series bucket collection.
const int kTimeseriesControlVersion = 1;

/**
 * Transforms a single time-series insert to an update request on an existing bucket.
 */
write_ops::UpdateOpEntry makeTimeseriesUpdateOpEntry(
    OperationContext* opCtx,
    std::shared_ptr<BucketCatalog::WriteBatch> batch,
    const BSONObj& metadata) {
    BSONObjBuilder updateBuilder;
    {
        if (!batch->min().isEmpty() || !batch->max().isEmpty()) {
            BSONObjBuilder controlBuilder(updateBuilder.subobjStart(
                str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "control"));
            if (!batch->min().isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "min", batch->min());
            }
            if (!batch->max().isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "max", batch->max());
            }
        }
    }
    {
        // doc_diff::kSubDiffSectionFieldPrefix + <field name> => {<index_0>: ..., <index_1>: ...}
        StringDataMap<BSONObjBuilder> dataFieldBuilders;
        auto metadataElem = metadata.firstElement();
        DecimalCounter<uint32_t> count(batch->numPreviouslyCommittedMeasurements());
        for (const auto& doc : batch->measurements()) {
            for (const auto& elem : doc) {
                auto key = elem.fieldNameStringData();
                if (metadataElem && key == metadataElem.fieldNameStringData()) {
                    continue;
                }
                auto& builder = dataFieldBuilders[key];
                builder.appendAs(elem, count);
            }
            ++count;
        }

        // doc_diff::kSubDiffSectionFieldPrefix + <field name>
        BSONObjBuilder dataBuilder(updateBuilder.subobjStart("sdata"));
        BSONObjBuilder newDataFieldsBuilder;
        for (auto& pair : dataFieldBuilders) {
            // Existing 'data' fields with measurements require different treatment from fields
            // not observed before (missing from control.min and control.max).
            if (batch->newFieldNamesToBeInserted().count(pair.first)) {
                newDataFieldsBuilder.append(pair.first, pair.second.obj());
            }
        }
        auto newDataFields = newDataFieldsBuilder.obj();
        if (!newDataFields.isEmpty()) {
            dataBuilder.append(doc_diff::kInsertSectionFieldName, newDataFields);
        }
        for (auto& pair : dataFieldBuilders) {
            // Existing 'data' fields with measurements require different treatment from fields
            // not observed before (missing from control.min and control.max).
            if (!batch->newFieldNamesToBeInserted().count(pair.first)) {
                dataBuilder.append(doc_diff::kSubDiffSectionFieldPrefix + pair.first.toString(),
                                   BSON(doc_diff::kInsertSectionFieldName << pair.second.obj()));
            }
        }
    }
    write_ops::UpdateModification::DiffOptions options;
    options.mustCheckExistenceForInsertOperations =
        static_cast<bool>(repl::tenantMigrationRecipientInfo(opCtx));
    write_ops::UpdateModification u(updateBuilder.obj(), options);
    write_ops::UpdateOpEntry update(BSON("_id" << batch->bucket()->id()), std::move(u));
    invariant(!update.getMulti(), batch->bucket()->id().toString());
    invariant(!update.getUpsert(), batch->bucket()->id().toString());
    return update;
}

/**
 * Returns the document for inserting a new bucket.
 */
BSONObj makeTimeseriesInsertDocument(std::shared_ptr<BucketCatalog::WriteBatch> batch,
                                     const BSONObj& metadata) {
    auto metadataElem = metadata.firstElement();

    StringDataMap<BSONObjBuilder> dataBuilders;
    DecimalCounter<uint32_t> count;
    for (const auto& doc : batch->measurements()) {
        for (const auto& elem : doc) {
            auto key = elem.fieldNameStringData();
            if (metadataElem && key == metadataElem.fieldNameStringData()) {
                continue;
            }
            dataBuilders[key].appendAs(elem, count);
        }
        ++count;
    }

    BSONObjBuilder builder;
    builder.append("_id", batch->bucket()->id());
    {
        BSONObjBuilder bucketControlBuilder(builder.subobjStart("control"));
        bucketControlBuilder.append("version", kTimeseriesControlVersion);
        bucketControlBuilder.append("min", batch->min());
        bucketControlBuilder.append("max", batch->max());
    }
    if (metadataElem) {
        builder.appendAs(metadataElem, "meta");
    }
    {
        BSONObjBuilder bucketDataBuilder(builder.subobjStart("data"));
        for (auto& dataBuilder : dataBuilders) {
            bucketDataBuilder.append(dataBuilder.first, dataBuilder.second.obj());
        }
    }

    return builder.obj();
}

/**
 * Returns true if the time-series write is retryable.
 */
bool isTimeseriesWriteRetryable(OperationContext* opCtx) {
    if (!opCtx->getTxnNumber()) {
        return false;
    }

    if (opCtx->inMultiDocumentTransaction()) {
        return false;
    }

    return true;
}

void getOpTimeAndElectionId(OperationContext* opCtx,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId) {
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    const auto replMode = replCoord->getReplicationMode();

    *opTime = replMode != repl::ReplicationCoordinator::modeNone
        ? boost::make_optional(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp())
        : boost::none;
    *electionId = replMode == repl::ReplicationCoordinator::modeReplSet
        ? boost::make_optional(replCoord->getElectionId())
        : boost::none;
}

boost::optional<Status> checkFailUnorderedTimeseriesInsertFailPoint(const BSONObj& metadata) {
    if (MONGO_unlikely(failUnorderedTimeseriesInsert.shouldFail([&metadata](const BSONObj& data) {
            BSONElementComparator comp(BSONElementComparator::FieldNamesMode::kIgnore, nullptr);
            return comp.compare(data["metadata"], metadata.firstElement()) == 0;
        }))) {
        return {{ErrorCodes::FailPointEnabled,
                 "Failed unordered time-series insert due to failUnorderedTimeseriesInsert fail "
                 "point"}};
    }
    return boost::none;
}

boost::optional<BSONObj> generateError(OperationContext* opCtx,
                                       const Status& status,
                                       int index,
                                       size_t numErrors) {
    if (status.isOK()) {
        return boost::none;
    }

    auto errorMessage = [numErrors, errorSize = size_t(0)](StringData rawMessage) mutable {
        // Start truncating error messages once both of these limits are exceeded.
        constexpr size_t kErrorSizeTruncationMin = 1024 * 1024;
        constexpr size_t kErrorCountTruncationMin = 2;
        if (errorSize >= kErrorSizeTruncationMin && numErrors >= kErrorCountTruncationMin) {
            return ""_sd;
        }

        errorSize += rawMessage.size();
        return rawMessage;
    };

    BSONSizeTracker errorsSizeTracker;
    BSONObjBuilder error(errorsSizeTracker);
    error.append("index", index);
    if (auto staleInfo = status.extraInfo<StaleConfigInfo>()) {
        error.append("code", int(ErrorCodes::StaleShardVersion));  // Different from exception!
        {
            BSONObjBuilder errInfo(error.subobjStart("errInfo"));
            staleInfo->serialize(&errInfo);
        }
    } else if (auto docValidationError =
                   status.extraInfo<doc_validation_error::DocumentValidationFailureInfo>()) {
        error.append("code", static_cast<int>(ErrorCodes::DocumentValidationFailure));
        error.append("errInfo", docValidationError->getDetails());
    } else if (status.code() == ErrorCodes::TenantMigrationConflict) {
        hangWriteBeforeWaitingForMigrationDecision.pauseWhileSet(opCtx);

        auto migrationStatus =
            tenant_migration_access_blocker::handleTenantMigrationConflict(opCtx, status);

        // Interruption errors encountered during batch execution fail the entire batch, so throw on
        // such errors here for consistency.
        if (ErrorCodes::isInterruption(migrationStatus)) {
            uassertStatusOK(migrationStatus);
        }

        error.append("code", migrationStatus.code());

        // We want to append an empty errmsg for the errors after the first one, so let the
        // code below that appends errmsg do that.
        if (status.reason() != "") {
            error.append("errmsg", errorMessage(migrationStatus.reason()));
        }
    } else {
        error.append("code", int(status.code()));
        if (auto const extraInfo = status.extraInfo()) {
            extraInfo->serialize(&error);
        }
    }

    // Skip appending errmsg if it has already been appended like in the case of
    // TenantMigrationConflict.
    if (!error.hasField("errmsg")) {
        error.append("errmsg", errorMessage(status.reason()));
    }
    return error.obj();
}

template <typename T>
boost::optional<BSONObj> generateError(OperationContext* opCtx,
                                       const StatusWith<T>& result,
                                       int index,
                                       size_t numErrors) {
    return generateError(opCtx, result.getStatus(), index, numErrors);
}

/**
 * Contains hooks that are used by 'populateReply' method.
 */
struct PopulateReplyHooks {
    // Called for each 'SingleWriteResult' processed by 'populateReply' method.
    std::function<void(const SingleWriteResult&, int)> singleWriteResultHandler;

    // Called after all 'SingleWriteResult' processing is completed by 'populateReply' method.
    // This is called as the last method.
    std::function<void()> postProcessHandler;
};

/**
 * Method to populate a write command reply message. It takes 'result' parameter as an input
 * source and populate the fields of 'cmdReply'.
 */
template <typename CommandReplyType>
void populateReply(OperationContext* opCtx,
                   bool continueOnError,
                   size_t opsInBatch,
                   write_ops_exec::WriteResult result,
                   CommandReplyType* cmdReply,
                   boost::optional<PopulateReplyHooks> hooks = boost::none) {

    invariant(cmdReply);

    if (shouldSkipOutput(opCtx))
        return;

    if (continueOnError) {
        invariant(!result.results.empty());
        const auto& lastResult = result.results.back();

        if (lastResult == ErrorCodes::StaleDbVersion ||
            ErrorCodes::isStaleShardVersionError(lastResult.getStatus()) ||
            ErrorCodes::isTenantMigrationError(lastResult.getStatus())) {
            // For ordered:false commands we need to duplicate these error results for all ops
            // after we stopped. See handleError() in write_ops_exec.cpp for more info.
            //
            // Omit the reason from the duplicate unordered responses so it doesn't consume BSON
            // object space
            result.results.resize(opsInBatch, lastResult.getStatus().withReason(""));
        }
    }

    long long nVal = 0;
    std::vector<BSONObj> errors;

    for (size_t i = 0; i < result.results.size(); i++) {
        if (auto error = generateError(opCtx, result.results[i], i, errors.size())) {
            errors.push_back(*error);
            continue;
        }

        const auto& opResult = result.results[i].getValue();
        nVal += opResult.getN();  // Always there.

        // Handle custom processing of each result.
        if (hooks && hooks->singleWriteResultHandler)
            hooks->singleWriteResultHandler(opResult, i);
    }

    auto& replyBase = cmdReply->getWriteCommandReplyBase();
    replyBase.setN(nVal);

    if (!errors.empty()) {
        replyBase.setWriteErrors(errors);
    }

    // writeConcernError field is handled by command processor.

    {
        // Undocumented repl fields that mongos depends on.
        auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
        const auto replMode = replCoord->getReplicationMode();
        if (replMode != repl::ReplicationCoordinator::modeNone) {
            replyBase.setOpTime(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());

            if (replMode == repl::ReplicationCoordinator::modeReplSet) {
                replyBase.setElectionId(replCoord->getElectionId());
            }
        }
    }

    // Call the called-defined post processing handler.
    if (hooks && hooks->postProcessHandler)
        hooks->postProcessHandler();
}

void transactionChecks(OperationContext* opCtx, const NamespaceString& ns) {
    if (!opCtx->inMultiDocumentTransaction())
        return;
    uassert(50791,
            str::stream() << "Cannot write to system collection " << ns.toString()
                          << " within a transaction.",
            !ns.isSystem() || ns.isPrivilegeCollection());
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    uassert(50790,
            str::stream() << "Cannot write to unreplicated collection " << ns.toString()
                          << " within a transaction.",
            !replCoord->isOplogDisabledFor(opCtx, ns));
}

class CmdInsert final : public write_ops::InsertCmdVersion1Gen<CmdInsert> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "documents");
    }

    std::string help() const final {
        return "insert documents";
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest) {
            InsertOp::validate(request());
        }

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        write_ops::InsertCommandReply typedRun(OperationContext* opCtx) final try {
            transactionChecks(opCtx, ns());
            write_ops::InsertCommandReply insertReply;

            if (isTimeseries(opCtx, ns())) {
                // Re-throw parsing exceptions to be consistent with CmdInsert::Invocation's
                // constructor.
                try {
                    _performTimeseriesWrites(opCtx, &insertReply);
                } catch (DBException& ex) {
                    ex.addContext(str::stream() << "time-series insert failed: " << ns().ns());
                    throw;
                }

                return insertReply;
            }
            auto reply = write_ops_exec::performInserts(opCtx, request());

            populateReply(opCtx,
                          !request().getWriteCommandRequestBase().getOrdered(),
                          request().getDocuments().size(),
                          std::move(reply),
                          &insertReply);

            return insertReply;
        } catch (const DBException& ex) {
            LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
            throw;
        }

    private:
        using TimeseriesBatches =
            std::vector<std::pair<std::shared_ptr<BucketCatalog::WriteBatch>, size_t>>;
        using TimeseriesStmtIds = stdx::unordered_map<BucketCatalog::Bucket*, std::vector<StmtId>>;

        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auth::checkAuthForInsertCommand(AuthorizationSession::get(opCtx->getClient()),
                                            request().getBypassDocumentValidation(),
                                            request());
        } catch (const DBException& ex) {
            LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
            throw;
        }

        BucketCatalog::CombineWithInsertsFromOtherClients
        _canCombineTimeseriesInsertWithOtherClients(OperationContext* opCtx) const {
            return isTimeseriesWriteRetryable(opCtx) || request().getOrdered()
                ? BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow
                : BucketCatalog::CombineWithInsertsFromOtherClients::kAllow;
        }

        StatusWith<SingleWriteResult> _getTimeseriesSingleWriteResult(
            const write_ops_exec::WriteResult& reply) const {
            invariant(reply.results.size() == 1,
                      str::stream() << "Unexpected number of results (" << reply.results.size()
                                    << ") for insert on time-series collection " << ns());

            return reply.results[0];
        }

        write_ops::WriteCommandRequestBase _makeTimeseriesWriteOpBase(
            std::vector<StmtId>&& stmtIds) const {
            write_ops::WriteCommandRequestBase base;

            // The schema validation configured in the bucket collection is intended for direct
            // operations by end users and is not applicable here.
            base.setBypassDocumentValidation(true);

            if (!stmtIds.empty()) {
                base.setStmtIds(std::move(stmtIds));
            }

            return base;
        }

        write_ops::InsertCommandRequest _makeTimeseriesInsertOp(
            std::shared_ptr<BucketCatalog::WriteBatch> batch,
            const BSONObj& metadata,
            std::vector<StmtId>&& stmtIds) const {
            write_ops::InsertCommandRequest op{ns().makeTimeseriesBucketsNamespace(),
                                               {makeTimeseriesInsertDocument(batch, metadata)}};
            op.setWriteCommandRequestBase(_makeTimeseriesWriteOpBase(std::move(stmtIds)));
            return op;
        }

        write_ops::UpdateCommandRequest _makeTimeseriesUpdateOp(
            OperationContext* opCtx,
            std::shared_ptr<BucketCatalog::WriteBatch> batch,
            const BSONObj& metadata,
            std::vector<StmtId>&& stmtIds) const {
            write_ops::UpdateCommandRequest op(
                ns().makeTimeseriesBucketsNamespace(),
                {makeTimeseriesUpdateOpEntry(opCtx, batch, metadata)});
            op.setWriteCommandRequestBase(_makeTimeseriesWriteOpBase(std::move(stmtIds)));
            return op;
        }

        StatusWith<SingleWriteResult> _performTimeseriesInsert(
            OperationContext* opCtx,
            std::shared_ptr<BucketCatalog::WriteBatch> batch,
            const BSONObj& metadata,
            std::vector<StmtId>&& stmtIds) const {
            if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
                return *status;
            }

            return _getTimeseriesSingleWriteResult(write_ops_exec::performInserts(
                opCtx,
                _makeTimeseriesInsertOp(batch, metadata, std::move(stmtIds)),
                OperationSource::kTimeseries));
        }

        StatusWith<SingleWriteResult> _performTimeseriesUpdate(
            OperationContext* opCtx,
            std::shared_ptr<BucketCatalog::WriteBatch> batch,
            const BSONObj& metadata,
            std::vector<StmtId>&& stmtIds) const {
            if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
                return *status;
            }

            return _getTimeseriesSingleWriteResult(write_ops_exec::performUpdates(
                opCtx,
                _makeTimeseriesUpdateOp(opCtx, batch, metadata, std::move(stmtIds)),
                OperationSource::kTimeseries));
        }

        void _commitTimeseriesBucket(OperationContext* opCtx,
                                     std::shared_ptr<BucketCatalog::WriteBatch> batch,
                                     size_t start,
                                     size_t index,
                                     std::vector<StmtId>&& stmtIds,
                                     std::vector<BSONObj>* errors,
                                     boost::optional<repl::OpTime>* opTime,
                                     boost::optional<OID>* electionId,
                                     std::vector<size_t>* docsToRetry) const {
            auto& bucketCatalog = BucketCatalog::get(opCtx);

            auto metadata = bucketCatalog.getMetadata(batch->bucket());
            bool prepared = bucketCatalog.prepareCommit(batch);
            if (!prepared) {
                invariant(batch->finished());
                invariant(batch->getResult().getStatus() == ErrorCodes::TimeseriesBucketCleared,
                          str::stream()
                              << "Got unexpected error (" << batch->getResult().getStatus()
                              << ") preparing time-series bucket to be committed for " << ns()
                              << ": " << redact(request().toBSON({})));
                docsToRetry->push_back(index);
                return;
            }
            // Now that the batch is prepared, make sure we clean up if we throw.
            auto batchGuard = makeGuard([&] { bucketCatalog.abort(batch); });

            hangTimeseriesInsertBeforeWrite.pauseWhileSet();

            const auto docId = batch->bucket()->id();
            const bool performInsert = batch->numPreviouslyCommittedMeasurements() == 0;
            if (performInsert) {
                auto result = _performTimeseriesInsert(opCtx, batch, metadata, std::move(stmtIds));

                if (auto error = generateError(opCtx, result, start + index, errors->size())) {
                    errors->push_back(*error);
                    bucketCatalog.abort(batch, result.getStatus());
                    batchGuard.dismiss();
                    return;
                }

                invariant(result.getValue().getN() == 1,
                          str::stream() << "Expected 1 insertion of document with _id '" << docId
                                        << "', but found " << result.getValue().getN() << ".");
            } else {
                auto result = _performTimeseriesUpdate(opCtx, batch, metadata, std::move(stmtIds));

                if (auto error = generateError(opCtx, result, start + index, errors->size())) {
                    errors->push_back(*error);
                    bucketCatalog.abort(batch, result.getStatus());
                    batchGuard.dismiss();
                    return;
                }

                invariant(result.getValue().getNModified() == 1,
                          str::stream()
                              << "Expected 1 update of document with _id '" << docId
                              << "', but found " << result.getValue().getNModified() << ".");
            }

            getOpTimeAndElectionId(opCtx, opTime, electionId);

            bucketCatalog.finish(batch, BucketCatalog::CommitInfo{*opTime, *electionId});
            batchGuard.dismiss();
        }

        bool _commitTimeseriesBucketsAtomically(OperationContext* opCtx,
                                                TimeseriesBatches* batches,
                                                TimeseriesStmtIds&& stmtIds,
                                                std::vector<BSONObj>* errors,
                                                boost::optional<repl::OpTime>* opTime,
                                                boost::optional<OID>* electionId) const {
            auto& bucketCatalog = BucketCatalog::get(opCtx);

            std::vector<std::reference_wrapper<std::shared_ptr<BucketCatalog::WriteBatch>>>
                batchesToCommit;

            for (auto& [batch, _] : *batches) {
                if (batch->claimCommitRights()) {
                    batchesToCommit.push_back(batch);
                }
            }

            if (batchesToCommit.empty()) {
                return true;
            }

            // Sort by bucket so that preparing the commit for each batch cannot deadlock.
            std::sort(batchesToCommit.begin(), batchesToCommit.end(), [](auto left, auto right) {
                return left.get()->bucket() < right.get()->bucket();
            });

            std::vector<write_ops::InsertCommandRequest> insertOps;
            std::vector<write_ops::UpdateCommandRequest> updateOps;

            for (auto batch : batchesToCommit) {
                auto metadata = bucketCatalog.getMetadata(batch.get()->bucket());
                if (!bucketCatalog.prepareCommit(batch)) {
                    for (auto batchToAbort : batchesToCommit) {
                        bucketCatalog.abort(batchToAbort);
                    }
                    return false;
                }

                if (batch.get()->numPreviouslyCommittedMeasurements() == 0) {
                    insertOps.push_back(_makeTimeseriesInsertOp(
                        batch, metadata, std::move(stmtIds[batch.get()->bucket()])));
                } else {
                    updateOps.push_back(_makeTimeseriesUpdateOp(
                        opCtx, batch, metadata, std::move(stmtIds[batch.get()->bucket()])));
                }
            }

            hangTimeseriesInsertBeforeWrite.pauseWhileSet();

            auto result =
                write_ops_exec::performAtomicTimeseriesWrites(opCtx, insertOps, updateOps);
            if (!result.isOK()) {
                for (auto batch : batchesToCommit) {
                    bucketCatalog.abort(batch, result);
                }
                return false;
            }

            getOpTimeAndElectionId(opCtx, opTime, electionId);

            for (auto batch : batchesToCommit) {
                bucketCatalog.finish(batch, BucketCatalog::CommitInfo{*opTime, *electionId});
                batch.get().reset();
            }

            return true;
        }

        std::tuple<TimeseriesBatches, TimeseriesStmtIds, size_t> _insertIntoBucketCatalog(
            OperationContext* opCtx,
            size_t start,
            size_t numDocs,
            const std::vector<size_t>& indices,
            std::vector<BSONObj>* errors,
            bool* containsRetry) const {
            auto& bucketCatalog = BucketCatalog::get(opCtx);

            auto bucketsNs = ns().makeTimeseriesBucketsNamespace();
            // Holding this shared pointer to the collection guarantees that the collator is not
            // invalidated.
            auto bucketsColl =
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(opCtx, bucketsNs);
            uassert(ErrorCodes::NamespaceNotFound,
                    "Could not find time-series buckets collection for write",
                    bucketsColl);
            uassert(ErrorCodes::InvalidOptions,
                    "Time-series buckets collection is missing time-series options",
                    bucketsColl->getTimeseriesOptions());

            TimeseriesBatches batches;
            TimeseriesStmtIds stmtIds;

            auto insert = [&](size_t index) {
                invariant(start + index < request().getDocuments().size());

                auto stmtId = request().getStmtIds()
                    ? request().getStmtIds()->at(start + index)
                    : request().getStmtId().value_or(0) + start + index;

                if (isTimeseriesWriteRetryable(opCtx) &&
                    TransactionParticipant::get(opCtx).checkStatementExecutedNoOplogEntryFetch(
                        stmtId)) {
                    RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                    *containsRetry = true;
                    return true;
                }

                auto result =
                    bucketCatalog.insert(opCtx,
                                         ns(),
                                         bucketsColl->getDefaultCollator(),
                                         *bucketsColl->getTimeseriesOptions(),
                                         request().getDocuments()[start + index],
                                         _canCombineTimeseriesInsertWithOtherClients(opCtx));

                if (auto error = generateError(opCtx, result, start + index, errors->size())) {
                    errors->push_back(*error);
                    return false;
                } else {
                    const auto& batch = result.getValue();
                    batches.emplace_back(batch, index);
                    if (isTimeseriesWriteRetryable(opCtx)) {
                        stmtIds[batch->bucket()].push_back(stmtId);
                    }
                }

                return true;
            };

            if (!indices.empty()) {
                std::for_each(indices.begin(), indices.end(), insert);
            } else {
                for (size_t i = 0; i < numDocs; i++) {
                    if (!insert(i) && request().getOrdered()) {
                        return {std::move(batches), std::move(stmtIds), i};
                    }
                }
            }

            return {std::move(batches), std::move(stmtIds), request().getDocuments().size()};
        }

        void _getTimeseriesBatchResults(OperationContext* opCtx,
                                        const TimeseriesBatches& batches,
                                        size_t start,
                                        std::vector<BSONObj>* errors,
                                        boost::optional<repl::OpTime>* opTime,
                                        boost::optional<OID>* electionId,
                                        std::vector<size_t>* docsToRetry = nullptr) const {
            for (const auto& [batch, index] : batches) {
                if (!batch) {
                    continue;
                }

                auto swCommitInfo = batch->getResult();
                if (swCommitInfo.getStatus() == ErrorCodes::TimeseriesBucketCleared) {
                    docsToRetry->push_back(index);
                    continue;
                }
                if (auto error = generateError(
                        opCtx, swCommitInfo.getStatus(), start + index, errors->size())) {
                    errors->push_back(*error);
                    continue;
                }

                const auto& commitInfo = swCommitInfo.getValue();
                if (commitInfo.opTime) {
                    *opTime = std::max(opTime->value_or(repl::OpTime()), *commitInfo.opTime);
                }
                if (commitInfo.electionId) {
                    *electionId = std::max(electionId->value_or(OID()), *commitInfo.electionId);
                }
            }
        }

        bool _performOrderedTimeseriesWritesAtomically(OperationContext* opCtx,
                                                       std::vector<BSONObj>* errors,
                                                       boost::optional<repl::OpTime>* opTime,
                                                       boost::optional<OID>* electionId,
                                                       bool* containsRetry) const {
            auto [batches, stmtIds, numInserted] = _insertIntoBucketCatalog(
                opCtx, 0, request().getDocuments().size(), {}, errors, containsRetry);

            hangTimeseriesInsertBeforeCommit.pauseWhileSet();

            if (!_commitTimeseriesBucketsAtomically(
                    opCtx, &batches, std::move(stmtIds), errors, opTime, electionId)) {
                return false;
            }

            _getTimeseriesBatchResults(opCtx, batches, 0, errors, opTime, electionId);

            return true;
        }

        /**
         * Returns the number of documents that were inserted.
         */
        size_t _performOrderedTimeseriesWrites(OperationContext* opCtx,
                                               std::vector<BSONObj>* errors,
                                               boost::optional<repl::OpTime>* opTime,
                                               boost::optional<OID>* electionId,
                                               bool* containsRetry) const {
            if (_performOrderedTimeseriesWritesAtomically(
                    opCtx, errors, opTime, electionId, containsRetry)) {
                return request().getDocuments().size();
            }

            for (size_t i = 0; i < request().getDocuments().size(); ++i) {
                _performUnorderedTimeseriesWritesWithRetries(
                    opCtx, i, 1, errors, opTime, electionId, containsRetry);
                if (!errors->empty()) {
                    return i;
                }
            }

            return request().getDocuments().size();
        }

        /**
         * Writes to the underlying system.buckets collection. Returns the indices, of the batch
         * which were attempted in an update operation, but found no bucket to update. These indices
         * can be passed as the 'indices' parameter in a subsequent call to this function, in order
         * to to be retried.
         */
        std::vector<size_t> _performUnorderedTimeseriesWrites(OperationContext* opCtx,
                                                              size_t start,
                                                              size_t numDocs,
                                                              const std::vector<size_t>& indices,
                                                              std::vector<BSONObj>* errors,
                                                              boost::optional<repl::OpTime>* opTime,
                                                              boost::optional<OID>* electionId,
                                                              bool* containsRetry) const {
            auto [batches, bucketStmtIds, _] =
                _insertIntoBucketCatalog(opCtx, start, numDocs, indices, errors, containsRetry);

            hangTimeseriesInsertBeforeCommit.pauseWhileSet();

            std::vector<size_t> docsToRetry;

            for (auto& [batch, index] : batches) {
                if (batch->claimCommitRights()) {
                    auto stmtIds = isTimeseriesWriteRetryable(opCtx)
                        ? std::move(bucketStmtIds[batch->bucket()])
                        : std::vector<StmtId>{};

                    _commitTimeseriesBucket(opCtx,
                                            batch,
                                            start,
                                            index,
                                            std::move(stmtIds),
                                            errors,
                                            opTime,
                                            electionId,
                                            &docsToRetry);
                    batch.reset();
                }
            }

            _getTimeseriesBatchResults(opCtx, batches, 0, errors, opTime, electionId, &docsToRetry);

            return docsToRetry;
        }

        void _performUnorderedTimeseriesWritesWithRetries(OperationContext* opCtx,
                                                          size_t start,
                                                          size_t numDocs,
                                                          std::vector<BSONObj>* errors,
                                                          boost::optional<repl::OpTime>* opTime,
                                                          boost::optional<OID>* electionId,
                                                          bool* containsRetry) const {
            std::vector<size_t> docsToRetry;
            do {
                docsToRetry = _performUnorderedTimeseriesWrites(
                    opCtx, start, numDocs, docsToRetry, errors, opTime, electionId, containsRetry);
            } while (!docsToRetry.empty());
        }

        void _performTimeseriesWrites(OperationContext* opCtx,
                                      write_ops::InsertCommandReply* insertReply) const {
            auto& curOp = *CurOp::get(opCtx);
            ON_BLOCK_EXIT([&] {
                // This is the only part of finishCurOp we need to do for inserts because they reuse
                // the top-level curOp. The rest is handled by the top-level entrypoint.
                curOp.done();
                Top::get(opCtx->getServiceContext())
                    .record(opCtx,
                            request().getNamespace().ns(),
                            LogicalOp::opInsert,
                            Top::LockType::WriteLocked,
                            durationCount<Microseconds>(curOp.elapsedTimeExcludingPauses()),
                            curOp.isCommand(),
                            curOp.getReadWriteType());
            });

            uassert(
                ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot insert into a time-series collection in a multi-document "
                                 "transaction: "
                              << ns(),
                !opCtx->inMultiDocumentTransaction());

            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                curOp.setNS_inlock(ns().ns());
                curOp.setLogicalOp_inlock(LogicalOp::opInsert);
                curOp.ensureStarted();
                curOp.debug().additiveMetrics.ninserted = 0;
            }

            std::vector<BSONObj> errors;
            boost::optional<repl::OpTime> opTime;
            boost::optional<OID> electionId;
            bool containsRetry = false;

            auto& baseReply = insertReply->getWriteCommandReplyBase();

            if (request().getOrdered()) {
                baseReply.setN(_performOrderedTimeseriesWrites(
                    opCtx, &errors, &opTime, &electionId, &containsRetry));
            } else {
                _performUnorderedTimeseriesWritesWithRetries(opCtx,
                                                             0,
                                                             request().getDocuments().size(),
                                                             &errors,
                                                             &opTime,
                                                             &electionId,
                                                             &containsRetry);
                baseReply.setN(request().getDocuments().size() - errors.size());
            }

            if (!errors.empty()) {
                baseReply.setWriteErrors(errors);
            }
            if (opTime) {
                baseReply.setOpTime(*opTime);
            }
            if (electionId) {
                baseReply.setElectionId(*electionId);
            }
            if (containsRetry) {
                RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
            }

            curOp.debug().additiveMetrics.ninserted = baseReply.getN();
        }
    };
} cmdInsert;

class CmdUpdate final : public write_ops::UpdateCmdVersion1Gen<CmdUpdate> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "updates");
    }

    std::string help() const final {
        return "update documents";
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest), _commandObj(opMsgRequest.body) {
            UpdateOp::validate(request());

            invariant(_commandObj.isOwned());

            // Extend the lifetime of `updates` to allow asynchronous mirroring.
            if (auto seq = opMsgRequest.getSequence("updates"_sd); seq && !seq->objs.empty()) {
                // Current design ignores contents of `updates` array except for the first entry.
                // Assuming identical collation for all elements in `updates`, future design could
                // use the disjunction primitive (i.e, `$or`) to compile all queries into a single
                // filter. Such a design also requires a sound way of combining hints.
                invariant(seq->objs.front().isOwned());
                _updateOpObj = seq->objs.front();
            }
        }

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        bool getBypass() const {
            return request().getBypassDocumentValidation();
        }

        bool supportsReadMirroring() const override {
            return true;
        }

        void appendMirrorableRequest(BSONObjBuilder* bob) const override {
            auto extractQueryDetails = [](const BSONObj& update, BSONObjBuilder* bob) -> void {
                // "filter", "hint", and "collation" fields are optional.
                if (update.isEmpty())
                    return;

                // The constructor verifies the following.
                invariant(update.isOwned());

                if (update.hasField("q"))
                    bob->append("filter", update["q"].Obj());
                if (update.hasField("hint") && !update["hint"].Obj().isEmpty())
                    bob->append("hint", update["hint"].Obj());
                if (update.hasField("collation") && !update["collation"].Obj().isEmpty())
                    bob->append("collation", update["collation"].Obj());
            };

            invariant(!_commandObj.isEmpty());

            bob->append("find", _commandObj["update"].String());
            extractQueryDetails(_updateOpObj, bob);
            bob->append("batchSize", 1);
            bob->append("singleBatch", true);
        }

        write_ops::UpdateCommandReply typedRun(OperationContext* opCtx) final try {
            transactionChecks(opCtx, ns());

            write_ops::UpdateCommandReply updateReply;
            long long nModified = 0;

            // Tracks the upserted information. The memory of this variable gets moved in the
            // 'postProcessHandler' and should not be accessed afterwards.
            std::vector<write_ops::Upserted> upsertedInfoVec;

            auto reply = write_ops_exec::performUpdates(opCtx, request());

            // Handler to process each 'SingleWriteResult'.
            auto singleWriteHandler = [&](const SingleWriteResult& opResult, int index) {
                nModified += opResult.getNModified();
                BSONSizeTracker upsertInfoSizeTracker;

                if (auto idElement = opResult.getUpsertedId().firstElement())
                    upsertedInfoVec.emplace_back(write_ops::Upserted(index, idElement));
            };

            // Handler to do the post-processing.
            auto postProcessHandler = [&]() {
                updateReply.setNModified(nModified);
                if (!upsertedInfoVec.empty())
                    updateReply.setUpserted(std::move(upsertedInfoVec));
            };

            populateReply(opCtx,
                          !request().getWriteCommandRequestBase().getOrdered(),
                          request().getUpdates().size(),
                          std::move(reply),
                          &updateReply,
                          PopulateReplyHooks{singleWriteHandler, postProcessHandler});

            // Collect metrics.
            for (auto&& update : request().getUpdates()) {
                // If this was a pipeline style update, record that pipeline-style was used and
                // which stages were being used.
                auto& updateMod = update.getU();
                if (updateMod.type() == write_ops::UpdateModification::Type::kPipeline) {
                    AggregateCommandRequest aggCmd(request().getNamespace(),
                                                   updateMod.getUpdatePipeline());
                    LiteParsedPipeline pipeline(aggCmd);
                    pipeline.tickGlobalStageCounters();
                    CmdUpdate::updateMetrics.incrementExecutedWithAggregationPipeline();
                }

                // If this command had arrayFilters option, record that it was used.
                if (update.getArrayFilters()) {
                    CmdUpdate::updateMetrics.incrementExecutedWithArrayFilters();
                }
            }

            return updateReply;
        } catch (const DBException& ex) {
            LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
            throw;
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auth::checkAuthForUpdateCommand(AuthorizationSession::get(opCtx->getClient()),
                                            request().getBypassDocumentValidation(),
                                            request());
        } catch (const DBException& ex) {
            LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
            throw;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    request().getUpdates().size() == 1);

            UpdateRequest updateRequest(request().getUpdates()[0]);
            updateRequest.setNamespaceString(request().getNamespace());
            updateRequest.setLegacyRuntimeConstants(request().getLegacyRuntimeConstants().value_or(
                Variables::generateRuntimeConstants(opCtx)));
            updateRequest.setLetParameters(request().getLet());
            updateRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            updateRequest.setExplain(verbosity);

            const ExtensionsCallbackReal extensionsCallback(opCtx,
                                                            &updateRequest.getNamespaceString());
            ParsedUpdate parsedUpdate(opCtx, &updateRequest, extensionsCallback);
            uassertStatusOK(parsedUpdate.parseRequest());

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, request().getNamespace(), MODE_IX);

            auto exec = uassertStatusOK(getExecutorUpdate(&CurOp::get(opCtx)->debug(),
                                                          &collection.getCollection(),
                                                          &parsedUpdate,
                                                          verbosity));
            auto bodyBuilder = result->getBodyBuilder();
            Explain::explainStages(exec.get(),
                                   collection.getCollection(),
                                   verbosity,
                                   BSONObj(),
                                   _commandObj,
                                   &bodyBuilder);
        }

        BSONObj _commandObj;

        // Holds a shared pointer to the first entry in `updates` array.
        BSONObj _updateOpObj;
    };

    // Update related command execution metrics.
    static UpdateMetrics updateMetrics;
} cmdUpdate;

UpdateMetrics CmdUpdate::updateMetrics{"update"};

class CmdDelete final : public write_ops::DeleteCmdVersion1Gen<CmdDelete> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    void snipForLogging(mutablebson::Document* cmdObj) const final {
        redactTooLongLog(cmdObj, "deletes");
    }

    std::string help() const final {
        return "delete documents";
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest), _commandObj(opMsgRequest.body) {
            DeleteOp::validate(request());
        }

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        write_ops::DeleteCommandReply typedRun(OperationContext* opCtx) final try {
            transactionChecks(opCtx, ns());

            write_ops::DeleteCommandReply deleteReply;

            auto reply = write_ops_exec::performDeletes(opCtx, request());
            populateReply(opCtx,
                          !request().getWriteCommandRequestBase().getOrdered(),
                          request().getDeletes().size(),
                          std::move(reply),
                          &deleteReply);

            return deleteReply;
        } catch (const DBException& ex) {
            LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
            throw;
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auth::checkAuthForDeleteCommand(AuthorizationSession::get(opCtx->getClient()),
                                            request().getBypassDocumentValidation(),
                                            request());
        } catch (const DBException& ex) {
            LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
            throw;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            uassert(ErrorCodes::InvalidLength,
                    "explained write batches must be of size 1",
                    request().getDeletes().size() == 1);

            auto deleteRequest = DeleteRequest{};
            deleteRequest.setNsString(request().getNamespace());
            deleteRequest.setLegacyRuntimeConstants(request().getLegacyRuntimeConstants().value_or(
                Variables::generateRuntimeConstants(opCtx)));
            deleteRequest.setLet(request().getLet());
            deleteRequest.setQuery(request().getDeletes()[0].getQ());
            deleteRequest.setCollation(write_ops::collationOf(request().getDeletes()[0]));
            deleteRequest.setMulti(request().getDeletes()[0].getMulti());
            deleteRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            deleteRequest.setHint(request().getDeletes()[0].getHint());
            deleteRequest.setIsExplain(true);

            ParsedDelete parsedDelete(opCtx, &deleteRequest);
            uassertStatusOK(parsedDelete.parseRequest());

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetCollection collection(opCtx, request().getNamespace(), MODE_IX);

            // Explain the plan tree.
            auto exec = uassertStatusOK(getExecutorDelete(&CurOp::get(opCtx)->debug(),
                                                          &collection.getCollection(),
                                                          &parsedDelete,
                                                          verbosity));
            auto bodyBuilder = result->getBodyBuilder();
            Explain::explainStages(exec.get(),
                                   collection.getCollection(),
                                   verbosity,
                                   BSONObj(),
                                   _commandObj,
                                   &bodyBuilder);
        }

        const BSONObj& _commandObj;
    };
} cmdDelete;

}  // namespace
}  // namespace mongo
