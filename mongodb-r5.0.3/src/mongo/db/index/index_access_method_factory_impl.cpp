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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/index_access_method_factory_impl.h"

#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/logv2/log.h"

namespace mongo {

std::unique_ptr<IndexAccessMethod> IndexAccessMethodFactoryImpl::make(
    IndexCatalogEntry* entry, std::unique_ptr<SortedDataInterface> sortedDataInterface) {
    auto desc = entry->descriptor();
    const std::string& type = desc->getAccessMethodName();
    if ("" == type)
        return std::make_unique<BtreeAccessMethod>(entry, std::move(sortedDataInterface));
    else if (IndexNames::HASHED == type)
        return std::make_unique<HashAccessMethod>(entry, std::move(sortedDataInterface));
    else if (IndexNames::GEO_2DSPHERE == type)
        return std::make_unique<S2AccessMethod>(entry, std::move(sortedDataInterface));
    else if (IndexNames::TEXT == type)
        return std::make_unique<FTSAccessMethod>(entry, std::move(sortedDataInterface));
    else if (IndexNames::GEO_HAYSTACK == type)
        return std::make_unique<HaystackAccessMethod>(entry, std::move(sortedDataInterface));
    else if (IndexNames::GEO_2D == type)
        return std::make_unique<TwoDAccessMethod>(entry, std::move(sortedDataInterface));
    else if (IndexNames::WILDCARD == type)
        return std::make_unique<WildcardAccessMethod>(entry, std::move(sortedDataInterface));
    LOGV2(20688,
          "Can't find index for keyPattern {keyPattern}",
          "Can't find index for keyPattern",
          "keyPattern"_attr = desc->keyPattern());
    fassertFailed(31021);
}

}  // namespace mongo
