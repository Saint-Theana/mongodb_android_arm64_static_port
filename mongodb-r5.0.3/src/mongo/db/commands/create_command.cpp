/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

constexpr auto kCreateCommandHelp =
    "explicitly creates a collection or view\n"
    "{\n"
    "  create: <string: collection or view name> [,\n"
    "  capped: <bool: capped collection>,\n"
    "  autoIndexId: <bool: automatic creation of _id index>,\n"
    "  idIndex: <document: _id index specification>,\n"
    "  size: <int: size in bytes of the capped collection>,\n"
    "  max: <int: max number of documents in the capped collection>,\n"
    "  storageEngine: <document: storage engine configuration>,\n"
    "  validator: <document: validation rules>,\n"
    "  validationLevel: <string: validation level>,\n"
    "  validationAction: <string: validation action>,\n"
    "  indexOptionDefaults: <document: default configuration for indexes>,\n"
    "  viewOn: <string: name of source collection or view>,\n"
    "  pipeline: <array<object>: aggregation pipeline stage>,\n"
    "  collation: <document: default collation for the collection or view>,\n"
    "  writeConcern: <document: write concern expression for the operation>]\n"
    "}"_sd;

class CmdCreate final : public CreateCmdVersion1Gen<CmdCreate> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    std::string help() const final {
        return kCreateCommandHelp.toString();
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassertStatusOK(auth::checkAuthForCreate(
                AuthorizationSession::get(opCtx->getClient()), request(), false));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        CreateCommandReply typedRun(OperationContext* opCtx) final {
            auto cmd = request();

            CreateCommandReply reply;
            if (cmd.getAutoIndexId()) {
#define DEPR_23800 "The autoIndexId option is deprecated and will be removed in a future release"
                LOGV2_WARNING(23800, DEPR_23800);
                reply.setNote(StringData(DEPR_23800));
#undef DEPR_23800
            }

            // Ensure that the 'size' field is present if 'capped' is set to true.
            if (cmd.getCapped()) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "the 'size' field is required when 'capped' is true",
                        cmd.getSize());
            }

            // If the 'size' or 'max' fields are present, then 'capped' must be set to true.
            if (cmd.getSize() || cmd.getMax()) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream()
                            << "the 'capped' field needs to be true when either the 'size'"
                            << " or 'max' fields are present",
                        cmd.getCapped());
            }

            // The 'temp' field is only allowed to be used internally and isn't available to
            // clients.
            if (cmd.getTemp()) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "the 'temp' field is an invalid option",
                        opCtx->getClient()->isInDirectClient() ||
                            (opCtx->getClient()->session()->getTags() &
                             transport::Session::kInternalClient));
            }

            if (cmd.getPipeline()) {
                uassert(ErrorCodes::InvalidOptions,
                        "'pipeline' requires 'viewOn' to also be specified",
                        cmd.getViewOn());
            }

            if (auto timeseries = cmd.getTimeseries()) {
                uassert(ErrorCodes::InvalidOptions,
                        "Time-series collection is not enabled",
                        feature_flags::gTimeseriesCollection.isEnabled(
                            serverGlobalParams.featureCompatibility));

                for (auto&& option : cmd.toBSON({})) {
                    auto fieldName = option.fieldNameStringData();

                    if (fieldName == CreateCommand::kCommandName) {
                        continue;
                    }

                    // The 'capped' option defaults to false. We accept it unless it is set to true.
                    if (fieldName == CreateCommand::kCappedFieldName && !option.Bool()) {
                        continue;
                    }

                    // The 'timeseries' option may be passed with a 'validator' or 'clusteredIndex'
                    // if a buckets collection is being restored. We assume the caller knows what
                    // they are doing.
                    if ((fieldName == CreateCommand::kValidatorFieldName ||
                         fieldName == CreateCommand::kClusteredIndexFieldName) &&
                        cmd.getNamespace().isTimeseriesBucketsCollection()) {
                        continue;
                    }

                    uassert(ErrorCodes::InvalidOptions,
                            str::stream()
                                << cmd.getNamespace() << ": 'timeseries' is not allowed with '"
                                << fieldName << "'",
                            timeseries::kAllowedCollectionCreationOptions.contains(fieldName));
                }

                auto hasDot = [](StringData field) -> bool {
                    return field.find('.') != std::string::npos;
                };
                auto mustBeTopLevel = [&cmd](StringData field) -> std::string {
                    return str::stream()
                        << cmd.getNamespace() << ": '" << field << "' must be a top-level field "
                        << "and not contain a '.'";
                };
                uassert(ErrorCodes::InvalidOptions,
                        mustBeTopLevel("timeField"),
                        !hasDot(timeseries->getTimeField()));

                if (auto metaField = timeseries->getMetaField()) {
                    uassert(ErrorCodes::InvalidOptions,
                            "'metaField' cannot be \"_id\"",
                            *metaField != "_id");
                    uassert(ErrorCodes::InvalidOptions,
                            "'metaField' cannot be the same as 'timeField'",
                            *metaField != timeseries->getTimeField());
                    uassert(ErrorCodes::InvalidOptions,
                            mustBeTopLevel("metaField"),
                            !hasDot(*metaField));
                }
            }

            if (cmd.getExpireAfterSeconds()) {
                uassert(ErrorCodes::InvalidOptions,
                        "'expireAfterSeconds' is only supported on time-series collections",
                        cmd.getTimeseries() ||
                            (cmd.getClusteredIndex() &&
                             cmd.getNamespace().isTimeseriesBucketsCollection()));
            }

            // Validate _id index spec and fill in missing fields.
            if (cmd.getIdIndex()) {
                auto idIndexSpec = *cmd.getIdIndex();

                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'idIndex' is not allowed with 'viewOn': " << idIndexSpec,
                        !cmd.getViewOn());

                uassert(ErrorCodes::InvalidOptions,
                        str::stream()
                            << "'idIndex' is not allowed with 'autoIndexId': " << idIndexSpec,
                        !cmd.getAutoIndexId());

                // Perform index spec validation.
                idIndexSpec =
                    uassertStatusOK(index_key_validate::validateIndexSpec(opCtx, idIndexSpec));
                uassertStatusOK(index_key_validate::validateIdIndexSpec(idIndexSpec));

                // Validate or fill in _id index collation.
                std::unique_ptr<CollatorInterface> defaultCollator;
                if (cmd.getCollation()) {
                    auto collatorStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(cmd.getCollation()->toBSON());
                    uassertStatusOK(collatorStatus.getStatus());
                    defaultCollator = std::move(collatorStatus.getValue());
                }

                idIndexSpec = uassertStatusOK(index_key_validate::validateIndexSpecCollation(
                    opCtx, idIndexSpec, defaultCollator.get()));

                std::unique_ptr<CollatorInterface> idIndexCollator;
                if (auto collationElem = idIndexSpec["collation"]) {
                    auto collatorStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(collationElem.Obj());
                    // validateIndexSpecCollation() should have checked that the _id index collation
                    // spec is valid.
                    invariant(collatorStatus.isOK());
                    idIndexCollator = std::move(collatorStatus.getValue());
                }
                if (!CollatorInterface::collatorsMatch(defaultCollator.get(),
                                                       idIndexCollator.get())) {
                    uasserted(ErrorCodes::BadValue,
                              "'idIndex' must have the same collation as the collection.");
                }

                cmd.setIdIndex(idIndexSpec);
            }

            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);
            uassertStatusOK(createCollection(opCtx, cmd.getNamespace(), cmd));
            return reply;
        }
    };
} cmdCreate;

}  // namespace
}  // namespace mongo
