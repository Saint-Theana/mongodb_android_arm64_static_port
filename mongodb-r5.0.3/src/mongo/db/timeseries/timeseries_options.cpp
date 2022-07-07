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

#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/timeseries_options.h"

#include "mongo/db/catalog/collection_catalog.h"

namespace mongo {

namespace timeseries {

namespace {

BSONObj wrapInArrayIf(bool doWrap, BSONObj&& obj) {
    if (doWrap) {
        return (::mongo::BSONArrayBuilder() << std::move(obj)).arr();
    }
    return std::move(obj);
}


bool isValidTimeseriesGranularityTransition(BucketGranularityEnum current,
                                            BucketGranularityEnum target) {
    bool validTransition = true;
    if (current == target) {
        return validTransition;
    }

    switch (current) {
        case BucketGranularityEnum::Seconds: {
            // Both minutes and hours are allowed.
            break;
        }
        case BucketGranularityEnum::Minutes: {
            if (target != BucketGranularityEnum::Hours) {
                validTransition = false;
            }
            break;
        }
        case BucketGranularityEnum::Hours: {
            validTransition = false;
            break;
        }
    }

    return validTransition;
}

}  // namespace

boost::optional<TimeseriesOptions> getTimeseriesOptions(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    auto bucketsNs = nss.makeTimeseriesBucketsNamespace();
    auto bucketsColl =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(opCtx, bucketsNs);
    if (!bucketsColl) {
        return boost::none;
    }
    return bucketsColl->getTimeseriesOptions();
}

int getMaxSpanSecondsFromGranularity(BucketGranularityEnum granularity) {
    switch (granularity) {
        case BucketGranularityEnum::Seconds:
            // 3600 seconds in an hour
            return 60 * 60;
        case BucketGranularityEnum::Minutes:
            // 1440 minutes in a day
            return 60 * 60 * 24;
        case BucketGranularityEnum::Hours:
            // 720 hours in an average month. Note that this only affects internal bucketing and
            // query optimizations, but users should not depend on or be aware of this estimation.
            return 60 * 60 * 24 * 30;
    }
    MONGO_UNREACHABLE;
}

int getBucketRoundingSecondsFromGranularity(BucketGranularityEnum granularity) {
    switch (granularity) {
        case BucketGranularityEnum::Seconds:
            // Round down to nearest minute.
            return 60;
        case BucketGranularityEnum::Minutes:
            // Round down to nearest hour.
            return 60 * 60;
        case BucketGranularityEnum::Hours:
            // Round down to hearest day.
            return 60 * 60 * 24;
    }
    MONGO_UNREACHABLE;
}

StatusWith<std::pair<TimeseriesOptions, bool>> applyTimeseriesOptionsModifications(
    const TimeseriesOptions& currentOptions, const BSONObj& mod) {
    TimeseriesOptions newOptions = currentOptions;
    bool changed = false;

    if (mod.hasField("granularity")) {
        BSONElement granularityElem = mod.getField("granularity");
        BucketGranularityEnum target = BucketGranularity_parse(
            IDLParserErrorContext("BucketGranularity"), granularityElem.valueStringData());
        if (target != currentOptions.getGranularity()) {
            if (!isValidTimeseriesGranularityTransition(currentOptions.getGranularity(), target)) {
                return Status{ErrorCodes::InvalidOptions,
                              "Invalid transition for timeseries.granularity. Can only transition "
                              "from 'seconds' to 'minutes' or 'minutes' to 'hours'."};
            }
            newOptions.setGranularity(target);
            newOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(target));
            changed = true;
        }
    }

    return std::make_pair(newOptions, changed);
}

BSONObj generateViewPipeline(const TimeseriesOptions& options, bool asArray) {
    if (options.getMetaField()) {
        return wrapInArrayIf(
            asArray,
            BSON("$_internalUnpackBucket" << BSON(
                     "timeField" << options.getTimeField() << "metaField" << *options.getMetaField()
                                 << "bucketMaxSpanSeconds" << *options.getBucketMaxSpanSeconds()
                                 << "exclude" << BSONArray())));
    }
    return wrapInArrayIf(
        asArray,
        BSON("$_internalUnpackBucket" << BSON(
                 "timeField" << options.getTimeField() << "bucketMaxSpanSeconds"
                             << *options.getBucketMaxSpanSeconds() << "exclude" << BSONArray())));
}

}  // namespace timeseries
}  // namespace mongo
