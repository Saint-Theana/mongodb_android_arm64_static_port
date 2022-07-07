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

#include "mongo/rpc/metadata/impersonated_user_metadata.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/server_options.h"

namespace mongo {
namespace rpc {
namespace {

static const OperationContext::Decoration<MaybeImpersonatedUserMetadata> getForOpCtx =
    OperationContext::declareDecoration<MaybeImpersonatedUserMetadata>();
}  // namespace

MaybeImpersonatedUserMetadata getImpersonatedUserMetadata(OperationContext* opCtx) {
    return opCtx ? getForOpCtx(opCtx) : boost::none;
}

void readImpersonatedUserMetadata(const BSONElement& elem, OperationContext* opCtx) {
    // If we have no opCtx, which does appear to happen, don't do anything.
    if (!opCtx) {
        return;
    }

    // Always reset the current impersonation data to boost::none.
    getForOpCtx(opCtx) = boost::none;
    if (elem.type() == Object) {
        IDLParserErrorContext errCtx(kImpersonationMetadataSectionName);
        auto data = ImpersonatedUserMetadata::parse(errCtx, elem.embeddedObject());

        // Set the impersonation data only if there are actually impersonated
        // users/roles.
        if ((!data.getUsers().empty()) || (!data.getRoles().empty())) {
            getForOpCtx(opCtx) = std::move(data);
        }
    }
}

void writeAuthDataToImpersonatedUserMetadata(OperationContext* opCtx, BSONObjBuilder* out) {
    // If we have no opCtx, which does appear to happen, don't do anything.
    if (!opCtx) {
        return;
    }

    // Otherwise construct a metadata section from the list of authenticated users/roles
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    auto userNames = authSession->getImpersonatedUserNames();
    auto roleNames = authSession->getImpersonatedRoleNames();
    if (!userNames.more() && !roleNames.more()) {
        userNames = authSession->getAuthenticatedUserNames();
        roleNames = authSession->getAuthenticatedRoleNames();
    }

    // If there are no users/roles being impersonated just exit
    if (!userNames.more() && !roleNames.more()) {
        return;
    }

    ImpersonatedUserMetadata metadata;
    metadata.setUsers(userNameIteratorToContainer<std::vector<UserName>>(userNames));
    metadata.setRoles(roleNameIteratorToContainer<std::vector<RoleName>>(roleNames));

    BSONObjBuilder section(out->subobjStart(kImpersonationMetadataSectionName));
    metadata.serialize(&section);
}

}  // namespace rpc
}  // namespace mongo
