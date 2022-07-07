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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/ops/parsed_delete.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

ParsedDelete::ParsedDelete(OperationContext* opCtx, const DeleteRequest* request)
    : _opCtx(opCtx), _request(request) {}

Status ParsedDelete::parseRequest() {
    dassert(!_canonicalQuery.get());
    // It is invalid to request that the DeleteStage return the deleted document during a
    // multi-remove.
    invariant(!(_request->getReturnDeleted() && _request->getMulti()));

    // It is invalid to request that a ProjectionStage be applied to the DeleteStage if the
    // DeleteStage would not return the deleted document.
    invariant(_request->getProj().isEmpty() || _request->getReturnDeleted());

    std::unique_ptr<CollatorInterface> collator(nullptr);
    if (!_request->getCollation().isEmpty()) {
        auto statusWithCollator = CollatorFactoryInterface::get(_opCtx->getServiceContext())
                                      ->makeFromBSON(_request->getCollation());

        if (!statusWithCollator.isOK()) {
            return statusWithCollator.getStatus();
        }
        collator = uassertStatusOK(std::move(statusWithCollator));
    }
    _expCtx = make_intrusive<ExpressionContext>(_opCtx,
                                                std::move(collator),
                                                _request->getNsString(),
                                                _request->getLegacyRuntimeConstants(),
                                                _request->getLet());

    if (CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
        return Status::OK();
    }

    return parseQueryToCQ();
}

Status ParsedDelete::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    const ExtensionsCallbackReal extensionsCallback(_opCtx, &_request->getNsString());

    // The projection needs to be applied after the delete operation, so we do not specify a
    // projection during canonicalization.
    auto findCommand = std::make_unique<FindCommandRequest>(_request->getNsString());
    findCommand->setFilter(_request->getQuery().getOwned());
    findCommand->setSort(_request->getSort().getOwned());
    findCommand->setCollation(_request->getCollation().getOwned());
    findCommand->setHint(_request->getHint());

    // Limit should only used for the findAndModify command when a sort is specified. If a sort
    // is requested, we want to use a top-k sort for efficiency reasons, so should pass the
    // limit through. Generally, a delete stage expects to be able to skip documents that were
    // deleted out from under it, but a limit could inhibit that and give an EOF when the delete
    // has not actually deleted a document. This behavior is fine for findAndModify, but should
    // not apply to deletes in general.
    if (!_request->getMulti() && !_request->getSort().isEmpty()) {
        findCommand->setLimit(1);
    }

    // If the delete request has runtime constants or let parameters attached to it, pass them to
    // the FindCommandRequest.
    if (auto& runtimeConstants = _request->getLegacyRuntimeConstants())
        findCommand->setLegacyRuntimeConstants(*runtimeConstants);
    if (auto& letParams = _request->getLet())
        findCommand->setLet(*letParams);

    auto statusWithCQ =
        CanonicalQuery::canonicalize(_opCtx,
                                     std::move(findCommand),
                                     _request->getIsExplain(),
                                     _expCtx,
                                     extensionsCallback,
                                     MatchExpressionParser::kAllowAllSpecialFeatures);

    if (statusWithCQ.isOK()) {
        _canonicalQuery = std::move(statusWithCQ.getValue());
    }

    return statusWithCQ.getStatus();
}

const DeleteRequest* ParsedDelete::getRequest() const {
    return _request;
}

PlanYieldPolicy::YieldPolicy ParsedDelete::yieldPolicy() const {
    return _request->getGod() ? PlanYieldPolicy::YieldPolicy::NO_YIELD : _request->getYieldPolicy();
}

bool ParsedDelete::hasParsedQuery() const {
    return _canonicalQuery.get() != nullptr;
}

std::unique_ptr<CanonicalQuery> ParsedDelete::releaseParsedQuery() {
    invariant(_canonicalQuery.get() != nullptr);
    return std::move(_canonicalQuery);
}

}  // namespace mongo
