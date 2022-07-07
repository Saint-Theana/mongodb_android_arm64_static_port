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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace timeseries {

// These are hard-coded constants in the bucket schema.
static constexpr StringData kBucketIdFieldName = "_id"_sd;
static constexpr StringData kBucketDataFieldName = "data"_sd;
static constexpr StringData kBucketMetaFieldName = "meta"_sd;
static constexpr StringData kBucketControlFieldName = "control"_sd;
static constexpr StringData kControlMaxFieldNamePrefix = "control.max."_sd;
static constexpr StringData kControlMinFieldNamePrefix = "control.min."_sd;

// These are hard-coded field names in create collection for time-series collections.
static constexpr StringData kTimeFieldName = "timeField"_sd;
static constexpr StringData kMetaFieldName = "metaField"_sd;

// These are hard-coded field names in index specs.
static constexpr StringData kKeyFieldName = "key"_sd;

static const StringDataSet kAllowedCollectionCreationOptions{
    CreateCommand::kStorageEngineFieldName,
    CreateCommand::kIndexOptionDefaultsFieldName,
    CreateCommand::kCollationFieldName,
    CreateCommand::kTimeseriesFieldName,
    CreateCommand::kExpireAfterSecondsFieldName};

}  // namespace timeseries
}  // namespace mongo
