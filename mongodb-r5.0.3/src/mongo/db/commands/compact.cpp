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

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_compact.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/views/view_catalog.h"

namespace mongo {

using std::string;
using std::stringstream;

class CompactCmd : public ErrmsgCommandDeprecated {
public:
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::compact);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    std::string help() const override {
        return "compact collection\n"
               "warning: this operation locks the database and is slow. you can cancel with "
               "killOp()\n"
               "{ compact : <collection_name>, [force:<bool>] }\n"
               "  force - allows to run on a replica set primary\n";
    }
    CompactCmd() : ErrmsgCommandDeprecated("compact") {}

    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& db,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        NamespaceString nss = CommandHelpers::parseNsCollectionRequired(db, cmdObj);

        repl::ReplicationCoordinator* replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (replCoord->getMemberState().primary() && !cmdObj["force"].trueValue()) {
            errmsg =
                "will not run compact on an active replica set primary as this is a slow blocking "
                "operation. use force:true to force";
            return false;
        }

        if (nss.isSystem()) {
            // Items in system.* cannot be moved as there might be pointers to them.
            errmsg = "can't compact a system namespace";
            return false;
        }

        StatusWith<int64_t> status = compactCollection(opCtx, nss);
        uassertStatusOK(status.getStatus());

        int64_t bytesFreed = status.getValue();
        if (bytesFreed < 0) {
            // When compacting a collection that is actively being written to, it is possible that
            // the collection is larger at the completion of compaction than when it started.
            bytesFreed = 0;
        }

        result.appendNumber("bytesFreed", static_cast<long long>(bytesFreed));

        return true;
    }
};
static CompactCmd compactCmd;
}  // namespace mongo
