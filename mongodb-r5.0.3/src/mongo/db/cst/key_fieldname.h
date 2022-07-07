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

#pragma once

#include "mongo/platform/basic.h"

#include "mongo/db/query/util/named_enum.h"

namespace mongo {

#define KEYFIELDNAMES(ENUMIFY)   \
    ENUMIFY(abs)                 \
    ENUMIFY(acos)                \
    ENUMIFY(acosh)               \
    ENUMIFY(add)                 \
    ENUMIFY(allElementsTrue)     \
    ENUMIFY(andExpr)             \
    ENUMIFY(anyElementTrue)      \
    ENUMIFY(asin)                \
    ENUMIFY(asinh)               \
    ENUMIFY(atan)                \
    ENUMIFY(arrayElemAt)         \
    ENUMIFY(arrayToObject)       \
    ENUMIFY(asArg)               \
    ENUMIFY(atan2)               \
    ENUMIFY(atanh)               \
    ENUMIFY(caseSensitive)       \
    ENUMIFY(ceil)                \
    ENUMIFY(charsArg)            \
    ENUMIFY(cmp)                 \
    ENUMIFY(collArg)             \
    ENUMIFY(commentExpr)         \
    ENUMIFY(concat)              \
    ENUMIFY(concatArrays)        \
    ENUMIFY(condArg)             \
    ENUMIFY(constExpr)           \
    ENUMIFY(convert)             \
    ENUMIFY(cos)                 \
    ENUMIFY(cosh)                \
    ENUMIFY(dateArg)             \
    ENUMIFY(dateFromParts)       \
    ENUMIFY(dateFromString)      \
    ENUMIFY(dateStringArg)       \
    ENUMIFY(dateToParts)         \
    ENUMIFY(dateToString)        \
    ENUMIFY(dayArg)              \
    ENUMIFY(dayOfMonth)          \
    ENUMIFY(dayOfWeek)           \
    ENUMIFY(dayOfYear)           \
    ENUMIFY(degreesToRadians)    \
    ENUMIFY(diacriticSensitive)  \
    ENUMIFY(divide)              \
    ENUMIFY(elemMatch)           \
    ENUMIFY(eq)                  \
    ENUMIFY(existsExpr)          \
    ENUMIFY(exponent)            \
    ENUMIFY(expr)                \
    ENUMIFY(findArg)             \
    ENUMIFY(filter)              \
    ENUMIFY(first)               \
    ENUMIFY(floor)               \
    ENUMIFY(formatArg)           \
    ENUMIFY(gt)                  \
    ENUMIFY(gte)                 \
    ENUMIFY(hour)                \
    ENUMIFY(hourArg)             \
    ENUMIFY(id)                  \
    ENUMIFY(in)                  \
    ENUMIFY(indexOfArray)        \
    ENUMIFY(indexOfBytes)        \
    ENUMIFY(indexOfCP)           \
    ENUMIFY(inhibitOptimization) \
    ENUMIFY(inputArg)            \
    ENUMIFY(isArray)             \
    ENUMIFY(iso8601Arg)          \
    ENUMIFY(isoDayOfWeek)        \
    ENUMIFY(isoDayOfWeekArg)     \
    ENUMIFY(isoWeek)             \
    ENUMIFY(isoWeekArg)          \
    ENUMIFY(isoWeekYear)         \
    ENUMIFY(isoWeekYearArg)      \
    ENUMIFY(language)            \
    ENUMIFY(limit)               \
    ENUMIFY(literal)             \
    ENUMIFY(ln)                  \
    ENUMIFY(log)                 \
    ENUMIFY(logten)              \
    ENUMIFY(lt)                  \
    ENUMIFY(lte)                 \
    ENUMIFY(ltrim)               \
    ENUMIFY(match)               \
    ENUMIFY(matchMod)            \
    ENUMIFY(meta)                \
    ENUMIFY(millisecond)         \
    ENUMIFY(millisecondArg)      \
    ENUMIFY(minute)              \
    ENUMIFY(minuteArg)           \
    ENUMIFY(mod)                 \
    ENUMIFY(month)               \
    ENUMIFY(monthArg)            \
    ENUMIFY(multiply)            \
    ENUMIFY(ne)                  \
    ENUMIFY(norExpr)             \
    ENUMIFY(notExpr)             \
    ENUMIFY(onErrorArg)          \
    ENUMIFY(onNullArg)           \
    ENUMIFY(optionsArg)          \
    ENUMIFY(orExpr)              \
    ENUMIFY(pipelineArg)         \
    ENUMIFY(pow)                 \
    ENUMIFY(projectExclusion)    \
    ENUMIFY(projectInclusion)    \
    ENUMIFY(radiansToDegrees)    \
    ENUMIFY(regexArg)            \
    ENUMIFY(regexFind)           \
    ENUMIFY(regexFindAll)        \
    ENUMIFY(regexMatch)          \
    ENUMIFY(replaceAll)          \
    ENUMIFY(replacementArg)      \
    ENUMIFY(replaceOne)          \
    ENUMIFY(round)               \
    ENUMIFY(rtrim)               \
    ENUMIFY(sample)              \
    ENUMIFY(search)              \
    ENUMIFY(second)              \
    ENUMIFY(secondArg)           \
    ENUMIFY(setDifference)       \
    ENUMIFY(setEquals)           \
    ENUMIFY(setIntersection)     \
    ENUMIFY(setIsSubset)         \
    ENUMIFY(setUnion)            \
    ENUMIFY(sin)                 \
    ENUMIFY(sinh)                \
    ENUMIFY(sizeArg)             \
    ENUMIFY(skip)                \
    ENUMIFY(slice)               \
    ENUMIFY(split)               \
    ENUMIFY(sqrt)                \
    ENUMIFY(strcasecmp)          \
    ENUMIFY(strLenBytes)         \
    ENUMIFY(strLenCP)            \
    ENUMIFY(substr)              \
    ENUMIFY(substrBytes)         \
    ENUMIFY(substrCP)            \
    ENUMIFY(subtract)            \
    ENUMIFY(tan)                 \
    ENUMIFY(tanh)                \
    ENUMIFY(text)                \
    ENUMIFY(timezoneArg)         \
    ENUMIFY(toArg)               \
    ENUMIFY(toBool)              \
    ENUMIFY(toDate)              \
    ENUMIFY(toDecimal)           \
    ENUMIFY(toDouble)            \
    ENUMIFY(toInt)               \
    ENUMIFY(toLong)              \
    ENUMIFY(toLower)             \
    ENUMIFY(toObjectId)          \
    ENUMIFY(toString)            \
    ENUMIFY(toUpper)             \
    ENUMIFY(trim)                \
    ENUMIFY(trunc)               \
    ENUMIFY(type)                \
    ENUMIFY(unionWith)           \
    ENUMIFY(week)                \
    ENUMIFY(where)               \
    ENUMIFY(year)                \
    ENUMIFY(yearArg)

QUERY_UTIL_NAMED_ENUM_DEFINE(KeyFieldname, KEYFIELDNAMES)
#undef KEYFIELDNAMES
}  // namespace mongo
