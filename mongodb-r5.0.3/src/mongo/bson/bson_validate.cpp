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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include <cstring>
#include <vector>

#include "mongo/base/data_view.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

// The values of the kSkipXX styles are used to compute the size, the remaining ones are arbitrary.
// NOTE: The kSkipXX values directly encode the amount of 4-byte words to skip: don't change them!
enum ValidationStyle : uint8_t {
    kSkip0 = 0,          // The element only consists of the type byte and field name.
    kSkip4 = 1,          // There are 4 additional bytes of data, see note above.
    kSkip8 = 2,          // There are 8 additional bytes of data, see note above.
    kSkip12 = 3,         // There are 12 additional bytes of data, see note above.
    kSkip16 = 4,         // There are 16 additional bytes of data, see note above.
    kString = 5,         // An int32 with the string length (including NUL) follows the field name.
    kObjectOrArray = 6,  // The type starts a new nested object or array.
    kSpecial = 7,        // Handled specially: any cases that don't fall into the above.
};

// This table is padded and aligned to 32 bytes for more efficient lookup.
static constexpr ValidationStyle kTypeInfoTable alignas(32)[32] = {
    ValidationStyle::kSpecial,        // \x00 EOO
    ValidationStyle::kSkip8,          // \x01 NumberDouble
    ValidationStyle::kString,         // \x02 String
    ValidationStyle::kObjectOrArray,  // \x03 Object
    ValidationStyle::kObjectOrArray,  // \x04 Array
    ValidationStyle::kSpecial,        // \x05 BinData
    ValidationStyle::kSkip0,          // \x06 Undefined
    ValidationStyle::kSkip12,         // \x07 OID
    ValidationStyle::kSpecial,        // \x08 Bool (requires 0/1 false/true validation)
    ValidationStyle::kSkip8,          // \x09 Date
    ValidationStyle::kSkip0,          // \x0a Null
    ValidationStyle::kSpecial,        // \x0b Regex (two nul-terminated strings)
    ValidationStyle::kSpecial,        // \x0c DBRef
    ValidationStyle::kString,         // \x0d Code
    ValidationStyle::kString,         // \x0e Symbol
    ValidationStyle::kSpecial,        // \x0f CodeWScope
    ValidationStyle::kSkip4,          // \x10 Int
    ValidationStyle::kSkip8,          // \x11 Timestamp
    ValidationStyle::kSkip8,          // \x12 Long
    ValidationStyle::kSkip16,         // \x13 Decimal
};
MONGO_STATIC_ASSERT(sizeof(kTypeInfoTable) == 32);

constexpr ErrorCodes::Error InvalidBSON = ErrorCodes::InvalidBSON;

template <bool precise>
class ValidateBuffer {
public:
    ValidateBuffer(const char* data, uint64_t maxLength) : _data(data), _maxLength(maxLength) {
        if constexpr (precise)
            _frames.resize(BSONDepth::getMaxAllowableDepth() + 1);
    }

    Status validate() noexcept {
        try {
            _currFrame = _frames.begin();
            _currElem = nullptr;
            auto maxFrames = BSONDepth::getMaxAllowableDepth() + 1;  // A flat BSON has one frame.
            uassert(InvalidBSON, "Cannot enforce max nesting depth", _frames.size() <= maxFrames);
            uassert(InvalidBSON, "BSON data has to be at least 5 bytes", _maxLength >= 5);

            // Read the length as signed integer, to ensure we limit it to < 2GB.
            // All other lengths are read as unsigned, which makes for easier bounds checking.
            Cursor cursor = {_data, _data + _maxLength};
            int32_t len = cursor.template read<int32_t>();
            uassert(InvalidBSON, "BSON data has to be at least 5 bytes", len >= 5);
            uassert(InvalidBSON, "Incorrect BSON length", static_cast<size_t>(len) <= _maxLength);
            const char* end = _currFrame->end = _data + len;
            uassert(InvalidBSON, "BSON object not terminated with EOO", end[-1] == 0);
            _validateIterative(Cursor{cursor.ptr, end});
        } catch (const ExceptionForCat<ErrorCategory::ValidationError>& e) {
            return Status(e.code(), str::stream() << e.what() << " " << _context());
        }
        return Status::OK();
    }

private:
    struct Empty {};

    /**
     * Extra information for each nesting level in the precise validation mode.
     */
    struct PreciseFrameInfo {
        BSONElement elem;  // _id for top frame, unchecked Object, Array or CodeWScope otherwise.
    };

    struct Frame : public std::conditional<precise, PreciseFrameInfo, Empty>::type {
        const char* end;  // Used for checking encoded object/array sizes, not bounds checking.
    };

    using Frames =
        typename std::conditional<precise, std::vector<Frame>, std::array<Frame, 32>>::type;

    struct Cursor {
        void skip(size_t len) {
            uassert(InvalidBSON, "BSON size is larger than buffer size", (ptr += len) < end);
        }

        template <typename T>
        const T read() {
            auto val = ptr;
            skip(sizeof(T));
            return ConstDataView(val).read<LittleEndian<T>>();
        }

        void skipString() {
            auto len = read<uint32_t>();
            skip(len);
            uassert(InvalidBSON, "Not null terminated string", !ptr[-1] && len > 0);
        }

        size_t strlen() const {
            // This is actually by far the hottest code in all of BSON validation.
            dassert(ptr < end);
            size_t len = 0;
            while (ptr[len])
                ++len;
            return len;
        }

        const char* ptr;
        const char* const end;
    };

    const char* _pushFrame(Cursor cursor) {
        uassert(ErrorCodes::Overflow,
                "BSONObj exceeds maximum nested object depth",
                ++_currFrame != _frames.end());

        auto obj = cursor.ptr;
        auto len = cursor.template read<int32_t>();
        uassert(ErrorCodes::InvalidBSON, "Nested BSON object has to be at least 5 bytes", len >= 5);
        _currFrame->end = obj + len;

        if constexpr (precise) {
            auto nameLen = obj - _currElem;
            _currFrame->elem =
                BSONElement(_currElem, nameLen, nameLen + len, BSONElement::CachedSizeTag());
        }
        return cursor.ptr;
    }

    bool _popFrame() {
        if (_currFrame == _frames.begin())
            return false;
        --_currFrame;
        return true;
    }

    static const char* _validateSpecial(Cursor cursor, uint8_t type) {
        switch (type) {
            case BSONType::BinData:
                cursor.skip(cursor.template read<uint32_t>());  // Like String, but...
                cursor.skip(1);  // ...add extra skip for the subtype byte to avoid overflow.
                break;
            case BSONType::Bool:
                if (auto value = cursor.template read<uint8_t>())  // If not 0, must be 1.
                    uassert(InvalidBSON, "BSON bool is neither false nor true", value == 1);
                break;
            case BSONType::RegEx:
                cursor.skip(0);  // Force validation of the ptr after skipping past the field name.
                cursor.skip(cursor.strlen() + 1);  // Skip regular expression cstring.
                cursor.skip(cursor.strlen() + 1);  // Skip options cstring.
                break;
            case BSONType::DBRef:
                cursor.skipString();  // Like String, but...
                cursor.skip(12);      // ...also skip the 12-byte ObjectId.
                break;
            case static_cast<uint8_t>(BSONType::MinKey):  // Need to cast, as MinKey is negative.
            case BSONType::MaxKey:
                cursor.skip(0);  // Force validation of the ptr after skipping past the field name.
                break;
            default:
                uasserted(InvalidBSON, str::stream() << "Unrecognized BSON type " << type);
        }
        return cursor.ptr;
    }

    const char* _pushCodeWithScope(Cursor cursor) {
        cursor.ptr = _pushFrame(cursor);  // Push a dummy frame to check the CodeWScope size.
        cursor.skipString();              // Now skip the BSON UTF8 string containing the code.
        _currElem = cursor.ptr - 1;       // Use the terminating NUL as adummy scope element.
        return _pushFrame(cursor);
    }

    void _maybePopCodeWithScope(Cursor cursor) {
        if constexpr (precise) {
            // When ending the scope of a CodeWScope, pop the extra dummy frame and check its size.
            if (_currFrame != _frames.begin() && (_currFrame - 1)->elem.type() == CodeWScope) {
                invariant(_popFrame());
                uassert(InvalidBSON, "incorrect BSON length", cursor.ptr == _currFrame->end);
            }
        }
    }

    const char* _validateElem(Cursor cursor, uint8_t type) {
        if (MONGO_unlikely(type > JSTypeMax))
            return _validateSpecial(cursor, type);

        auto style = kTypeInfoTable[type];
        if (MONGO_likely(style <= kSkip16))
            cursor.skip(style * 4);
        else if (MONGO_likely(style == kString))
            cursor.skipString();
        else if (MONGO_likely(style == kObjectOrArray))
            cursor.ptr = _pushFrame(cursor);
        else if (MONGO_unlikely(precise && type == CodeWScope))
            cursor.ptr = _pushCodeWithScope(cursor);
        else
            cursor.ptr = _validateSpecial(cursor, type);

        return cursor.ptr;
    }

    MONGO_COMPILER_NOINLINE void _validateIterative(Cursor cursor) {
        do {
            // Use the fact that the EOO byte is 0, just like the end of string, so checking for EOO
            // is same as finding len == 0. The cursor cannot point past EOO, so the strlen is safe.
            uassert(InvalidBSON, "BSON size is larger than buffer size", cursor.ptr < cursor.end);
            while (size_t len = cursor.strlen()) {
                uint8_t type = *cursor.ptr;
                _currElem = cursor.ptr;
                cursor.ptr += len + 1;
                cursor.ptr = _validateElem(cursor, type);

                if constexpr (precise) {
                    // See if the _id field was just validated. If so, set the global scope element.
                    if (_currFrame == _frames.begin() && StringData(_currElem + 1) == "_id"_sd)
                        _currFrame->elem = BSONElement(_currElem);  // This is fully validated now.
                }
                dassert(cursor.ptr <= cursor.end);
            }

            // Got the EOO byte: skip it and compare its location with the expected frame end.
            uassert(InvalidBSON, "incorrect BSON length", ++cursor.ptr == _currFrame->end);
            _maybePopCodeWithScope(cursor);
        } while (_popFrame());  // Finished when there are no frames left.
    }

    /**
     * Returns a string qualifying the context in which an exception occurred. Example return is
     * "in element with field name 'foo.bar' in object with _id: 1".
     */
    std::string _context() {
        str::stream ctx;
        ctx << "in element with field name '";
        if constexpr (precise) {
            std::for_each(_frames.begin() + 1,
                          _currFrame + (_currFrame != _frames.end()),
                          [&](auto& frame) { ctx << frame.elem.fieldName() << "."; });
        }
        ctx << (_currElem ? _currElem + 1 : "?") << "'";

        if constexpr (precise) {
            auto _id = _frames.begin()->elem;
            ctx << " in object with " << (_id ? BSONElement(_id).toString() : "unknown _id");
        }
        return str::escape(ctx);
    }

    const char* const _data;  // The data buffer to check.
    const size_t _maxLength;  // The size of the data buffer. The BSON object may be smaller.
    const char* _currElem = nullptr;  // Element to validate: only the name is known to be good.
    typename Frames::iterator _currFrame;  // Frame currently being validated.
    Frames _frames;  // Has end pointers to check and the containing element for precise mode.
};
}  // namespace

Status validateBSON(const char* originalBuffer, uint64_t maxLength) noexcept {
    // First try validating using the fast but less precise version. That version will return
    // a not-OK status for objects with CodeWScope or nesting exceeding 32 levels. These cases and
    // actual failures will rerun the precise version that gives a detailed error context.
    if (MONGO_likely(ValidateBuffer<false>(originalBuffer, maxLength).validate().isOK()))
        return Status::OK();

    return ValidateBuffer<true>(originalBuffer, maxLength).validate();
}
}  // namespace mongo
