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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(returnResponseOkForRecipientSyncDataCmd);

class RecipientSyncDataCmd : public TypedCommand<RecipientSyncDataCmd> {
public:
    using Request = RecipientSyncData;
    using Response = RecipientSyncDataResponse;

    std::set<StringData> sensitiveFieldNames() const final {
        return {Request::kRecipientCertificateForDonorFieldName};
    }

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "recipientSyncData command not enabled",
                    repl::feature_flags::gTenantMigrations.isEnabled(
                        serverGlobalParams.featureCompatibility));

            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are not available in sharded clusters",
                    serverGlobalParams.clusterRole == ClusterRole::None);

            // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
            uassert(
                5356101,
                "recipientSyncData not available while upgrading or downgrading the recipient FCV",
                !serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());

            const auto& cmd = request();

            TenantMigrationRecipientDocument stateDoc(cmd.getMigrationId(),
                                                      cmd.getDonorConnectionString().toString(),
                                                      cmd.getTenantId().toString(),
                                                      cmd.getStartMigrationDonorTimestamp(),
                                                      cmd.getReadPreference());

            if (!repl::tenantMigrationDisableX509Auth) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'" << Request::kRecipientCertificateForDonorFieldName
                                      << "' is a required field",
                        cmd.getRecipientCertificateForDonor());
                stateDoc.setRecipientCertificateForDonor(cmd.getRecipientCertificateForDonor());
            }

            const auto stateDocBson = stateDoc.toBSON();

            if (MONGO_unlikely(returnResponseOkForRecipientSyncDataCmd.shouldFail())) {
                LOGV2(4879608,
                      "'returnResponseOkForRecipientSyncDataCmd' failpoint enabled.",
                      "tenantMigrationRecipientInstance"_attr = stateDoc.toBSON());
                return Response(repl::OpTime());
            }

            auto recipientService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(repl::TenantMigrationRecipientService::
                                              kTenantMigrationRecipientServiceName);
            auto recipientInstance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
                opCtx, recipientService, stateDocBson);

            // Ensure that the options (e.g. tenantId, recipientConnectionString, or readPreference)
            // received by this migration match the options it was created with. If there is a
            // conflict, it means there exists a migration with the same migrationId, but different
            // options.
            uassertStatusOK(recipientInstance->checkIfOptionsConflict(stateDoc));

            auto returnAfterReachingDonorTs = cmd.getReturnAfterReachingDonorTimestamp();

            try {
                if (!returnAfterReachingDonorTs) {
                    return Response(
                        recipientInstance->waitUntilMigrationReachesConsistentState(opCtx));
                }

                return Response(
                    recipientInstance->waitUntilMigrationReachesReturnAfterReachingTimestamp(
                        opCtx, *returnAfterReachingDonorTs));
            } catch (ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
                // A conflict may arise when inserting the recipientInstance's  state document.
                // Since the conflict occurred at the insert stage, that means this instance's
                // tenantId conflicts with an existing instance's tenantId. Therefore, remove the
                // instance that was just created.
                // The status from this exception will be passed to the instance interrupt() method.
                recipientService->releaseInstance(stateDocBson["_id"].wrap(), ex.toStatus());
                throw;
            }
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const {
        return "Internal replica set command; instructs the recipient to sync data as part of a "
               "tenant migration.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

} recipientSyncDataCmd;

class RecipientForgetMigrationCmd : public TypedCommand<RecipientForgetMigrationCmd> {
public:
    using Request = RecipientForgetMigration;

    std::set<StringData> sensitiveFieldNames() const final {
        return {Request::kRecipientCertificateForDonorFieldName};
    }

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "recipientForgetMigration command not enabled",
                    repl::feature_flags::gTenantMigrations.isEnabled(
                        serverGlobalParams.featureCompatibility));

            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are not available in sharded clusters",
                    serverGlobalParams.clusterRole == ClusterRole::None);

            const auto& cmd = request();

            opCtx->setAlwaysInterruptAtStepDownOrUp();
            auto recipientService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(repl::TenantMigrationRecipientService::
                                              kTenantMigrationRecipientServiceName);

            // We may not have a document if recipientForgetMigration is received before
            // recipientSyncData. But even if that's the case, we still need to create an instance
            // and persist a state document that's marked garbage collectable (which is done by the
            // main chain).
            const Timestamp kUnusedStartMigrationTimestamp(1, 1);
            TenantMigrationRecipientDocument stateDoc(cmd.getMigrationId(),
                                                      cmd.getDonorConnectionString().toString(),
                                                      cmd.getTenantId().toString(),
                                                      kUnusedStartMigrationTimestamp,
                                                      cmd.getReadPreference());
            if (!repl::tenantMigrationDisableX509Auth) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'" << Request::kRecipientCertificateForDonorFieldName
                                      << "' is a required field",
                        cmd.getRecipientCertificateForDonor());
                stateDoc.setRecipientCertificateForDonor(cmd.getRecipientCertificateForDonor());
            }

            // Set the state to 'kDone' so that we don't create a recipient access blocker
            // unnecessarily if this recipientForgetMigration command is received before a
            // recipientSyncData command or after the state doc is garbage collected.
            stateDoc.setState(TenantMigrationRecipientStateEnum::kDone);

            auto recipientInstance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
                opCtx, recipientService, stateDoc.toBSON());

            // Instruct the instance run() function to mark this migration garbage collectable.
            recipientInstance->onReceiveRecipientForgetMigration(opCtx);
            recipientInstance->getCompletionFuture().get(opCtx);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const {
        return "Interrupts tenant migration data sync and marks that the recipient's durable state "
               "machine may be garbage collected.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

} recipientForgetMigrationCmd;

}  // namespace
}  // namespace mongo
