/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/remote_command_targeter_rs.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/string_map.h"

namespace mongo {

class TenantMigrationDonorService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "TenantMigrationDonorService"_sd;

    explicit TenantMigrationDonorService(ServiceContext* const serviceContext)
        : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}
    ~TenantMigrationDonorService() = default;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kTenantMigrationDonorsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        ThreadPool::Limits limits;
        limits.maxThreads = repl::maxTenantMigrationDonorServiceThreadPoolSize;
        return limits;
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<TenantMigrationDonorService::Instance>(
            _serviceContext, this, initialState);
    }

    /**
     * Sends an abort to all tenant migration instances on this donor.
     */
    void abortAllMigrations(OperationContext* opCtx);

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        struct DurableState {
            TenantMigrationDonorStateEnum state;
            boost::optional<Status> abortReason;
        };

        explicit Instance(ServiceContext* const serviceContext,
                          const TenantMigrationDonorService* donorService,
                          const BSONObj& initialState);

        ~Instance();

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancellationToken& token) noexcept override;

        void interrupt(Status status) override;

        /**
         * Report TenantMigrationDonorService Instances in currentOp().
         */
        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

        /**
         * To be called on the instance returned by PrimaryOnlyService::getOrCreate. Returns an
         * error if the options this Instance was created with are incompatible with the options
         * given in 'stateDoc'.
         */
        Status checkIfOptionsConflict(const TenantMigrationDonorDocument& stateDoc);

        /**
         * Returns the latest durable migration state.
         */
        DurableState getDurableState(OperationContext* opCtx);

        /**
         * Returns a Future that will be resolved when all work associated with this Instance has
         * completed running.
         */
        SharedSemiFuture<void> getCompletionFuture() const {
            return _completionPromise.getFuture();
        }

        /**
         * Returns a Future that will be resolved when an abort or commit decision has been reached.
         */
        SharedSemiFuture<void> getDecisionFuture() const {
            return _decisionPromise.getFuture();
        }

        /**
         * Kicks off work for the donorAbortMigration command.
         */
        void onReceiveDonorAbortMigration();

        /**
         * Kicks off the work for the donorForgetMigration command.
         */
        void onReceiveDonorForgetMigration();

        StringData getTenantId() const {
            return _stateDoc.getTenantId();
        }

        StringData getRecipientConnectionString() const {
            return _stateDoc.getRecipientConnectionString();
        }

    private:
        const NamespaceString _stateDocumentsNS = NamespaceString::kTenantMigrationDonorsNamespace;

        ExecutorFuture<void> _enterAbortingIndexBuildsState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
            const CancellationToken& token);

        void _abortIndexBuilds(const CancellationToken& token);

        /**
         * Fetches all key documents from the recipient's admin.system.keys collection, stores
         * them in config.external_validation_keys, and refreshes the keys cache.
         */
        ExecutorFuture<void> _fetchAndStoreRecipientClusterTimeKeyDocs(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const CancellationToken& token);

        ExecutorFuture<void> _enterDataSyncState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
            const CancellationToken& token);

        ExecutorFuture<void> _waitForRecipientToBecomeConsistentAndEnterBlockingState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const CancellationToken& token);

        ExecutorFuture<void> _waitForRecipientToReachBlockTimestampAndEnterCommittedState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const CancellationToken& token);

        ExecutorFuture<void> _handleErrorOrEnterAbortedState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
            const CancellationToken& token,
            const CancellationToken& abortToken,
            Status status);

        ExecutorFuture<void> _waitForForgetMigrationThenMarkMigrationGarbageCollectable(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const CancellationToken& token);

        /**
         * Makes a task executor for executing commands against the recipient. If the server
         * parameter 'tenantMigrationDisableX509Auth' is false, configures the executor to use the
         * migration certificate to establish an SSL connection to the recipient.
         */
        std::shared_ptr<executor::ThreadPoolTaskExecutor> _makeRecipientCmdExecutor();

        /**
         * Inserts the state document to _stateDocumentsNS and returns the opTime for the insert
         * oplog entry.
         */
        ExecutorFuture<repl::OpTime> _insertStateDoc(
            std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token);

        /**
         * Updates the state document to have the given state. Then, persists the updated document
         * by reserving an oplog slot beforehand and using its timestamp as the blockTimestamp or
         * commitOrAbortTimestamp depending on the state. Returns the opTime for the update oplog
         * entry.
         */
        ExecutorFuture<repl::OpTime> _updateStateDoc(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            const TenantMigrationDonorStateEnum nextState,
            const CancellationToken& token);

        /**
         * Sets the "expireAt" time for the state document to be garbage collected, and returns the
         * the opTime for the write.
         */
        ExecutorFuture<repl::OpTime> _markStateDocAsGarbageCollectable(
            std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token);

        /**
         * Waits for given opTime to be majority committed.
         */
        ExecutorFuture<void> _waitForMajorityWriteConcern(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            repl::OpTime opTime,
            const CancellationToken& token);

        /**
         * Sends the given command to the recipient replica set.
         */
        ExecutorFuture<void> _sendCommandToRecipient(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const BSONObj& cmdObj,
            const CancellationToken& token);

        /**
         * Sends the recipientSyncData command to the recipient replica set.
         */
        ExecutorFuture<void> _sendRecipientSyncDataCommand(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const CancellationToken& token);

        /**
         * Sends the recipientForgetMigration command to the recipient replica set.
         */
        ExecutorFuture<void> _sendRecipientForgetMigrationCommand(
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
            const CancellationToken& token);

        ThreadPool::Limits _getRecipientCmdThreadPoolLimits() const {
            ThreadPool::Limits recipientCmdThreadPoolLimits;
            recipientCmdThreadPoolLimits.maxThreads = 1;
            return recipientCmdThreadPoolLimits;
        }

        /*
         * Initializes _abortMigrationSource and returns a token from it. The source will be
         * immediately canceled if an abort has already been requested.
         */
        CancellationToken _initAbortMigrationSource(const CancellationToken& token);

        ServiceContext* const _serviceContext;
        const TenantMigrationDonorService* const _donorService;

        TenantMigrationDonorDocument _stateDoc;
        const std::string _instanceName;
        const MongoURI _recipientUri;

        // This data is provided in the initial state doc and never changes.  We keep copies to
        // avoid having to obtain the mutex to access them.
        const std::string _tenantId;
        const std::string _recipientConnectionString;
        const ReadPreferenceSetting _readPreference;
        const UUID _migrationUuid;
        const boost::optional<TenantMigrationPEMPayload> _donorCertificateForRecipient;
        const boost::optional<TenantMigrationPEMPayload> _recipientCertificateForDonor;

        // TODO (SERVER-54085): Remove server parameter tenantMigrationDisableX509Auth.
        const transport::ConnectSSLMode _sslMode;

        // Task executor used for executing commands against the recipient.
        std::shared_ptr<executor::TaskExecutor> _recipientCmdExecutor;

        // Weak pointer to the Fetcher used for fetching admin.system.keys documents from the
        // recipient. It is only not null when the instance is actively fetching the documents.
        std::weak_ptr<Fetcher> _recipientKeysFetcher;

        boost::optional<Status> _abortReason;

        // Protects the durable state, state document, abort requested boolean, and the promises
        // below.
        mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationDonorService::_mutex");

        // The latest majority-committed migration state.
        DurableState _durableState;

        // Promise that is resolved when the donor has majority-committed the write to insert the
        // donor state doc for the migration.
        SharedPromise<void> _initialDonorStateDurablePromise;

        // Promise that is resolved when the donor receives the donorForgetMigration command.
        SharedPromise<void> _receiveDonorForgetMigrationPromise;

        // Promise that is resolved when the chain of work kicked off by run() has completed.
        SharedPromise<void> _completionPromise;

        // Promise that is resolved when the donor has majority-committed the write to commit or
        // abort.
        SharedPromise<void> _decisionPromise;

        // Set to true when a request to cancel the migration has been processed, e.g. after
        // executing the donorAbortMigration command.
        bool _abortRequested{false};

        // Used for logical interrupts that require aborting the migration but not unconditionally
        // interrupting the instance, e.g. receiving donorAbortMigration. Initialized in
        // _initAbortMigrationSource().
        boost::optional<CancellationSource> _abortMigrationSource;
    };

private:
    ExecutorFuture<void> createStateDocumentTTLIndex(
        std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token);

    ExecutorFuture<void> createExternalKeysTTLIndex(
        std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token);

    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override;

    ServiceContext* const _serviceContext;
};
}  // namespace mongo
