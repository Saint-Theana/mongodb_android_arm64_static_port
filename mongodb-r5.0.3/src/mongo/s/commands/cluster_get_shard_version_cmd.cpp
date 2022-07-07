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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class GetShardVersion : public BasicCommand {
public:
    GetShardVersion() : BasicCommand("getShardVersion", "getshardversion") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        BSONElement first = cmdObj.firstElement();
        uassert(ErrorCodes::BadValue,
                str::stream() << "namespace has invalid type " << typeName(first.type()),
                first.canonicalType() == canonicalizeBSONType(mongo::String));
        const NamespaceString nss(first.valueStringData());
        return nss.ns();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));

        const auto catalogCache = Grid::get(opCtx)->catalogCache();

        if (nss.coll().empty()) {
            // Return the database's information.
            auto cachedDbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.ns()));
            result.append("primaryShard", cachedDbInfo.primaryId().toString());
            result.append("shardingEnabled", cachedDbInfo.shardingEnabled());
            result.append("version", cachedDbInfo.databaseVersion().toBSON());
        } else {
            // Return the collection's information.
            const auto cm = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
            uassert(ErrorCodes::NamespaceNotSharded,
                    str::stream() << "Collection " << nss.ns() << " is not sharded.",
                    cm.isSharded());
            cm.getVersion().appendLegacyWithField(&result, "version");

            if (cmdObj["fullMetadata"].trueValue()) {
                BSONArrayBuilder chunksArrBuilder;
                bool exceedsSizeLimit = false;

                LOGV2(22753,
                      "Routing info requested by getShardVersion: {routingInfo}",
                      "Routing info requested by getShardVersion",
                      "routingInfo"_attr = redact(cm.toString()));

                cm.forEachChunk([&](const auto& chunk) {
                    if (!exceedsSizeLimit) {
                        BSONArrayBuilder chunkBB(chunksArrBuilder.subarrayStart());
                        chunkBB.append(chunk.getMin());
                        chunkBB.append(chunk.getMax());
                        chunkBB.done();
                        if (chunksArrBuilder.len() + result.len() > BSONObjMaxUserSize) {
                            exceedsSizeLimit = true;
                        }
                    }

                    return true;
                });

                if (!exceedsSizeLimit) {
                    result.append("chunks", chunksArrBuilder.arr());
                }
            }
        }

        return true;
    }

} getShardVersionCmd;

}  // namespace
}  // namespace mongo
