/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/logv2/log.h"

namespace mongo {
class OplogStonesServerStatusSection : public ServerStatusSection {
public:
    OplogStonesServerStatusSection() : ServerStatusSection("oplogTruncation") {}
    /**
     * <ServerStatusSection>
     */
    bool includeByDefault() const override {
        return true;
    }

    /**
     * <ServerStatusSection>
     */
    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;
        if (!opCtx->getServiceContext()->getStorageEngine()->supportsOplogStones()) {
            return builder.obj();
        }

        Lock::GlobalLock lk(
            opCtx, LockMode::MODE_IS, Date_t::now(), Lock::InterruptBehavior::kLeaveUnlocked);
        if (!lk.isLocked()) {
            LOGV2_DEBUG(4822100, 2, "Failed to retrieve oplogTruncation statistics");
            return BSONObj();
        }

        AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
        const auto& oplog = oplogRead.getCollection();
        if (oplog) {
            const auto localDb =
                DatabaseHolder::get(opCtx)->getDb(opCtx, NamespaceString::kLocalDb);
            invariant(localDb);
            AutoStatsTracker statsTracker(
                opCtx,
                NamespaceString::kRsOplogNamespace,
                Top::LockType::ReadLocked,
                AutoStatsTracker::LogMode::kUpdateTop,
                CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(NamespaceString::kLocalDb));
            oplog->getRecordStore()->getOplogTruncateStats(builder);
        }
        return builder.obj();
    }


} oplogStonesStats;
}  // namespace mongo
