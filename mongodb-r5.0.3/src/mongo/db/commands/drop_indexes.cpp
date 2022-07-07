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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/drop_indexes_gen.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(reIndexCrashAfterDrop);

/**
 * Returns a DropIndexes for dropping indexes on the bucket collection.
 *
 * The 'index' dropIndexes parameter may refer to an index name, or array of names, or "*" for all
 * indexes, or an index spec key (an object). Only the index spec key has to be translated for the
 * bucket collection. The other forms of 'index' can be passed along unmodified.
 *
 * Returns null if 'origCmd' is not for a time-series collection.
 */
std::unique_ptr<DropIndexes> makeTimeseriesDropIndexesCommand(OperationContext* opCtx,
                                                              const DropIndexes& origCmd) {
    const auto& origNs = origCmd.getNamespace();

    auto timeseriesOptions = timeseries::getTimeseriesOptions(opCtx, origNs);

    // Return early with null if we are not working with a time-series collection.
    if (!timeseriesOptions) {
        return {};
    }

    auto ns = origNs.makeTimeseriesBucketsNamespace();

    const auto& origIndex = origCmd.getIndex();
    if (auto keyPtr = stdx::get_if<BSONObj>(&origIndex)) {
        auto bucketsIndexSpecWithStatus =
            timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(*timeseriesOptions, *keyPtr);

        uassert(ErrorCodes::IndexNotFound,
                str::stream() << bucketsIndexSpecWithStatus.getStatus().toString()
                              << " Command request: " << redact(origCmd.toBSON({})),
                bucketsIndexSpecWithStatus.isOK());

        return std::make_unique<DropIndexes>(ns, std::move(bucketsIndexSpecWithStatus.getValue()));
    }

    return std::make_unique<DropIndexes>(ns, origIndex);
}

class CmdDropIndexes : public DropIndexesCmdVersion1Gen<CmdDropIndexes> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }
    std::string help() const override {
        return "drop indexes for a collection";
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return request().getNamespace();
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop index(es) on collection"
                                  << request().getNamespace(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(request().getNamespace(),
                                                            ActionType::dropIndex));
        }
        Reply typedRun(OperationContext* opCtx) final {
            // If the request namespace refers to a time-series collection, transform the user
            // time-series index request to one on the underlying bucket.
            if (auto timeseriesCmd = makeTimeseriesDropIndexesCommand(opCtx, request())) {
                return dropIndexes(opCtx, timeseriesCmd->getNamespace(), timeseriesCmd->getIndex());
            }

            return dropIndexes(opCtx, request().getNamespace(), request().getIndex());
        }
    };
} cmdDropIndexes;

class CmdReIndex : public ErrmsgCommandDeprecated {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        // Even though reIndex is a standalone-only command, this will return that the command is
        // allowed on secondaries so that it will fail with a more useful error message to the user
        // rather than with a NotWritablePrimary error.
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "re-index a collection (can only be run on a standalone mongod)";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::reIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    CmdReIndex() : ErrmsgCommandDeprecated("reIndex") {}

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& jsobj,
                   std::string& errmsg,
                   BSONObjBuilder& result) {

        const NamespaceString toReIndexNss =
            CommandHelpers::parseNsCollectionRequired(dbname, jsobj);

        LOGV2(20457, "CMD: reIndex {namespace}", "CMD reIndex", "namespace"_attr = toReIndexNss);

        if (repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
            repl::ReplicationCoordinator::modeNone) {
            uasserted(
                ErrorCodes::IllegalOperation,
                str::stream()
                    << "reIndex is only allowed on a standalone mongod instance. Cannot reIndex '"
                    << toReIndexNss << "' while replication is active");
        }

        AutoGetCollection autoColl(opCtx, toReIndexNss, MODE_X);
        if (!autoColl) {
            auto db = autoColl.getDb();
            if (db && ViewCatalog::get(db)->lookup(opCtx, toReIndexNss.ns()))
                uasserted(ErrorCodes::CommandNotSupportedOnView, "can't re-index a view");
            else
                uasserted(ErrorCodes::NamespaceNotFound, "collection does not exist");
        }

        CollectionWriter collection(autoColl);
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
            collection->uuid());

        // This is necessary to set up CurOp and update the Top stats.
        OldClientContext ctx(opCtx, toReIndexNss.ns());

        const auto defaultIndexVersion = IndexDescriptor::getDefaultIndexVersion();

        std::vector<BSONObj> all;
        {
            std::vector<std::string> indexNames;
            writeConflictRetry(opCtx, "listIndexes", toReIndexNss.ns(), [&] {
                indexNames.clear();
                collection->getAllIndexes(&indexNames);
            });

            all.reserve(indexNames.size());

            for (size_t i = 0; i < indexNames.size(); i++) {
                const std::string& name = indexNames[i];
                BSONObj spec = writeConflictRetry(opCtx, "getIndexSpec", toReIndexNss.ns(), [&] {
                    return collection->getIndexSpec(name);
                });

                {
                    BSONObjBuilder bob;

                    for (auto&& indexSpecElem : spec) {
                        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
                        if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
                            // We create a new index specification with the 'v' field set as
                            // 'defaultIndexVersion'.
                            bob.append(IndexDescriptor::kIndexVersionFieldName,
                                       static_cast<int>(defaultIndexVersion));
                        } else {
                            bob.append(indexSpecElem);
                        }
                    }

                    all.push_back(bob.obj());
                }

                const BSONObj key = spec.getObjectField("key");
                const Status keyStatus =
                    index_key_validate::validateKeyPattern(key, defaultIndexVersion);
                if (!keyStatus.isOK()) {
                    errmsg = str::stream()
                        << "Cannot rebuild index " << spec << ": " << keyStatus.reason()
                        << " For more info see http://dochub.mongodb.org/core/index-validation";
                    return false;
                }
            }
        }

        result.appendNumber("nIndexesWas", static_cast<long long>(all.size()));

        std::unique_ptr<MultiIndexBlock> indexer = std::make_unique<MultiIndexBlock>();
        indexer->setIndexBuildMethod(IndexBuildMethod::kForeground);
        StatusWith<std::vector<BSONObj>> swIndexesToRebuild(ErrorCodes::UnknownError,
                                                            "Uninitialized");
        writeConflictRetry(opCtx, "dropAllIndexes", toReIndexNss.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            collection.getWritableCollection()->getIndexCatalog()->dropAllIndexes(
                opCtx, collection.getWritableCollection(), true);

            swIndexesToRebuild =
                indexer->init(opCtx, collection, all, MultiIndexBlock::kNoopOnInitFn);
            uassertStatusOK(swIndexesToRebuild.getStatus());
            wunit.commit();
        });

        // The 'indexer' can throw, so ensure build cleanup occurs.
        auto abortOnExit = makeGuard([&] {
            indexer->abortIndexBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
        });

        if (MONGO_unlikely(reIndexCrashAfterDrop.shouldFail())) {
            LOGV2(20458, "Exiting because 'reIndexCrashAfterDrop' fail point was set");
            quickExit(EXIT_ABRUPT);
        }

        // The following function performs its own WriteConflict handling, so don't wrap it in a
        // writeConflictRetry loop.
        uassertStatusOK(indexer->insertAllDocumentsInCollection(opCtx, collection.get()));

        uassertStatusOK(indexer->checkConstraints(opCtx, collection.get()));

        writeConflictRetry(opCtx, "commitReIndex", toReIndexNss.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            uassertStatusOK(indexer->commit(opCtx,
                                            collection.getWritableCollection(),
                                            MultiIndexBlock::kNoopOnCreateEachFn,
                                            MultiIndexBlock::kNoopOnCommitFn));
            wunit.commit();
        });
        abortOnExit.dismiss();

        result.append("nIndexes", static_cast<int>(swIndexesToRebuild.getValue().size()));
        result.append("indexes", swIndexesToRebuild.getValue());

        return true;
    }
} cmdReIndex;

}  // namespace
}  // namespace mongo
