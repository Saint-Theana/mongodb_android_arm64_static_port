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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <bcrypt.h>
#include <memory>
#include <vector>

#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/platform/shared_library.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace crypto {

namespace {

// RtlNtStatusToDosError function, only available via GetProcAddress
using pRtlNtStatusToDosError = ULONG(WINAPI*)(NTSTATUS Status);

std::string statusWithDescription(NTSTATUS status) {
    auto swLib = SharedLibrary::create("ntdll.dll");
    if (swLib.getStatus().isOK()) {

        auto swFunc =
            swLib.getValue()->getFunctionAs<pRtlNtStatusToDosError>("RtlNtStatusToDosError");
        if (swFunc.isOK()) {

            pRtlNtStatusToDosError RtlNtStatusToDosErrorFunc = swFunc.getValue();
            ULONG errorCode = RtlNtStatusToDosErrorFunc(status);

            if (errorCode != ERROR_MR_MID_NOT_FOUND) {
                return errnoWithDescription(errorCode);
            }
        }
    }

    return str::stream() << "Failed to get error message for NTSTATUS: " << status;
}

struct AlgoInfo {
    BCRYPT_ALG_HANDLE algo;
    DWORD keyBlobSize;
};

/**
 * Initialize crypto algorithms from default system CNG provider.
 */
class BCryptCryptoLoader {
public:
    BCryptCryptoLoader() {
        loadAlgo(_algoAESCBC, BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_CBC);

        auto status =
            ::BCryptOpenAlgorithmProvider(&_random, BCRYPT_RNG_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
        invariant(status == STATUS_SUCCESS);
    }

    ~BCryptCryptoLoader() {
        invariant(BCryptCloseAlgorithmProvider(_algoAESCBC.algo, 0) == STATUS_SUCCESS);
        invariant(BCryptCloseAlgorithmProvider(_random, 0) == STATUS_SUCCESS);
    }

    AlgoInfo& getAlgo(aesMode mode) {
        switch (mode) {
            case aesMode::cbc:
                return _algoAESCBC;
            default:
                MONGO_UNREACHABLE;
        }
    }

    BCRYPT_ALG_HANDLE getRandom() {
        return _random;
    }

private:
    void loadAlgo(AlgoInfo& algo, const wchar_t* name, const wchar_t* chainingMode) {
        NTSTATUS status = BCryptOpenAlgorithmProvider(&algo.algo, name, MS_PRIMITIVE_PROVIDER, 0);
        invariant(status == STATUS_SUCCESS);

        status = BCryptSetProperty(algo.algo,
                                   BCRYPT_CHAINING_MODE,
                                   reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(chainingMode)),
                                   sizeof(wchar_t) * wcslen(chainingMode),
                                   0);
        invariant(status == STATUS_SUCCESS);

        DWORD cbOutput = sizeof(algo.keyBlobSize);
        status = BCryptGetProperty(algo.algo,
                                   BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&algo.keyBlobSize),
                                   cbOutput,
                                   &cbOutput,
                                   0);
        invariant(status == STATUS_SUCCESS);
    }

private:
    AlgoInfo _algoAESCBC;
    BCRYPT_ALG_HANDLE _random;
};

static BCryptCryptoLoader& getBCryptCryptoLoader() {
    static BCryptCryptoLoader loader;
    return loader;
}

/**
 * Base class to support initialize symmetric key buffers and state.
 */
template <typename Parent>
class SymmetricImplWindows : public Parent {
public:
    SymmetricImplWindows(const SymmetricKey& key, aesMode mode, const uint8_t* iv, size_t ivLen)
        : _keyHandle(INVALID_HANDLE_VALUE), _mode(mode) {
        AlgoInfo& algo = getBCryptCryptoLoader().getAlgo(mode);


        // Initialize key storage buffers
        _keyObjectBuf->resize(algo.keyBlobSize);

        SecureVector<unsigned char> keyBlob;
        keyBlob->reserve(sizeof(BCRYPT_KEY_DATA_BLOB_HEADER) + key.getKeySize());

        BCRYPT_KEY_DATA_BLOB_HEADER blobHeader;
        blobHeader.dwMagic = BCRYPT_KEY_DATA_BLOB_MAGIC;
        blobHeader.dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION1;
        blobHeader.cbKeyData = key.getKeySize();

        std::copy(reinterpret_cast<uint8_t*>(&blobHeader),
                  reinterpret_cast<uint8_t*>(&blobHeader) + sizeof(BCRYPT_KEY_DATA_BLOB_HEADER),
                  std::back_inserter(*keyBlob));

        std::copy(key.getKey(), key.getKey() + key.getKeySize(), std::back_inserter(*keyBlob));

        NTSTATUS status = BCryptImportKey(algo.algo,
                                          NULL,
                                          BCRYPT_KEY_DATA_BLOB,
                                          &_keyHandle,
                                          _keyObjectBuf->data(),
                                          _keyObjectBuf->size(),
                                          keyBlob->data(),
                                          keyBlob->size(),
                                          0);
        uassert(ErrorCodes::OperationFailed,
                str::stream() << "ImportKey failed: " << statusWithDescription(status),
                status == STATUS_SUCCESS);

        std::copy(iv, iv + ivLen, std::back_inserter(_iv));
    }

    ~SymmetricImplWindows() {
        if (_keyHandle != INVALID_HANDLE_VALUE) {
            BCryptDestroyKey(_keyHandle);
        }
    }

    Status addAuthenticatedData(const uint8_t* in, size_t inLen) final {
        fassert(51127, inLen == 0);
        return Status::OK();
    }

protected:
    const aesMode _mode;

    // Buffers for key data
    BCRYPT_KEY_HANDLE _keyHandle;

    SecureVector<unsigned char> _keyObjectBuf;

    // Buffer for CBC data
    std::vector<unsigned char> _iv;
};

/**
 * Like other symmetric encryptors, this class encrypts block-by-block with update and then only
 * pads once finalize is called. However, the Windows's BCrypt implementation does not natively
 * implement this functionality (see SERVER-47733), and will either require block aligned inputs or
 * will attempt to pad every input. This class bulks together inputs in a local buffer which is
 * flushed to BCrypt whenever a full block is accumulated via update invocations. Data provided to
 * update may be encrypted immediately, on a subsequent call to update, or on the call to finalize.
 */
class SymmetricEncryptorWindows : public SymmetricImplWindows<SymmetricEncryptor> {
public:
    using SymmetricImplWindows::SymmetricImplWindows;

    SymmetricEncryptorWindows(const SymmetricKey& key,
                              aesMode mode,
                              const uint8_t* iv,
                              size_t ivLen)
        : _blockData(_blockBuffer->data(), _blockBuffer->size()),
          _blockCursor(_blockData),
          SymmetricImplWindows<SymmetricEncryptor>(key, mode, iv, ivLen) {}

    StatusWith<size_t> update(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) final {
        ULONG blockBufferEncryptLen = 0;
        ULONG inputEncryptLen = 0;
        ConstDataRange inData(in, inLen);
        ConstDataRangeCursor inCursor(inData);

        // If we have an incomplete block, we need to fill it before encrypting.
        // If the total amount of input bytes will not fill the blockBuffer, just add it all to
        // the buffer.
        if (inLen < _blockCursor.length()) {
            _blockCursor.writeAndAdvance(inCursor);
            return 0;
        } else if (_blockCursor.length() < _blockData.length() && _blockCursor.length() > 0) {
            // Entering this code path means that we had data left over from the last time update
            // was called. What we do below is fill the buffer with new input data until it is full.
            // We then encrypt that buffer. We skip this step when the buffer is empty.
            uint8_t bytesToFill = _blockCursor.length();
            ConstDataRange bytesToFillRange(inCursor.data(), bytesToFill);
            _blockCursor.writeAndAdvance(bytesToFillRange);
            inCursor.advance(bytesToFill);
            // We now encrypt the full buffer.
            NTSTATUS status = BCryptEncrypt(_keyHandle,
                                            const_cast<PUCHAR>(_blockBuffer->data()),
                                            _blockBuffer->size(),
                                            NULL,
                                            _iv.data(),
                                            _iv.size(),
                                            out,
                                            outLen,
                                            &blockBufferEncryptLen,
                                            0);
            if (status != STATUS_SUCCESS) {
                return Status{ErrorCodes::OperationFailed,
                              str::stream() << "Encrypt failed: " << statusWithDescription(status)};
            }
            _blockCursor = DataRangeCursor(_blockData);
        }

        // we will attempt to encrypt as much of the remaining data as we can (i.e. the largest
        // available size that is a multiple of the block length)
        size_t remainingBytes = inCursor.length();
        ULONG bytesToEncrypt = remainingBytes - (remainingBytes % aesBlockSize);

        NTSTATUS status = BCryptEncrypt(_keyHandle,
                                        const_cast<PUCHAR>(inCursor.data<UCHAR>()),
                                        bytesToEncrypt,
                                        NULL,
                                        _iv.data(),
                                        _iv.size(),
                                        out + blockBufferEncryptLen,
                                        outLen - blockBufferEncryptLen,
                                        &inputEncryptLen,
                                        0);

        if (status != STATUS_SUCCESS) {
            return Status{ErrorCodes::OperationFailed,
                          str::stream() << "Encrypt failed: " << statusWithDescription(status)};
        }

        inCursor.advance(bytesToEncrypt);

        // we now have to store what is left of the input in the block buffer
        _blockCursor.writeAndAdvance(inCursor);

        return static_cast<size_t>(blockBufferEncryptLen + inputEncryptLen);
    }

    StatusWith<size_t> finalize(uint8_t* out, size_t outLen) final {
        // if there is any data left over in the block buffer, we will encrypt it with padding
        ULONG len = 0;
        NTSTATUS status = BCryptEncrypt(_keyHandle,
                                        const_cast<PUCHAR>(_blockBuffer->data()),
                                        _blockBuffer->size() - _blockCursor.length(),
                                        NULL,
                                        _iv.data(),
                                        _iv.size(),
                                        out,
                                        outLen,
                                        &len,
                                        BCRYPT_BLOCK_PADDING);

        if (status != STATUS_SUCCESS) {
            return Status{ErrorCodes::OperationFailed,
                          str::stream() << "Encrypt failed: " << statusWithDescription(status)};
        }

        // we will now start a new block
        _blockCursor = DataRangeCursor(_blockData);

        return static_cast<size_t>(len);
    }

    StatusWith<size_t> finalizeTag(uint8_t* out, size_t outLen) final {
        // Not a tagged cipher mode, write nothing.
        return 0;
    }

private:
    // buffer to store a single block of data, to be encrypted by update when filled, or by finalize
    // with padding. 16 is the block length for AES.
    SecureAllocatorDefaultDomain::SecureHandle<std::array<uint8_t, aesBlockSize>> _blockBuffer;
    DataRange _blockData;
    DataRangeCursor _blockCursor;
};

class SymmetricDecryptorWindows : public SymmetricImplWindows<SymmetricDecryptor> {
public:
    using SymmetricImplWindows::SymmetricImplWindows;

    StatusWith<size_t> update(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) final {
        ULONG len = 0;

        NTSTATUS status = BCryptDecrypt(_keyHandle,
                                        const_cast<PUCHAR>(in),
                                        inLen,
                                        NULL,
                                        _iv.data(),
                                        _iv.size(),
                                        out,
                                        outLen,
                                        &len,
                                        BCRYPT_BLOCK_PADDING);

        if (status != STATUS_SUCCESS) {
            return Status{ErrorCodes::OperationFailed,
                          str::stream() << "Decrypt failed: " << statusWithDescription(status)};
        }

        return static_cast<size_t>(len);
    }

    StatusWith<size_t> finalize(uint8_t* out, size_t outLen) final {
        return 0;
    }

    Status updateTag(const uint8_t* tag, size_t tagLen) final {
        return Status::OK();
    }
};

}  // namespace

std::set<std::string> getSupportedSymmetricAlgorithms() {
    return {aes256CBCName};
}

Status engineRandBytes(uint8_t* buffer, size_t len) {
    NTSTATUS status = BCryptGenRandom(getBCryptCryptoLoader().getRandom(), buffer, len, 0);
    if (status == STATUS_SUCCESS) {
        return Status::OK();
    }

    return {ErrorCodes::UnknownError,
            str::stream() << "Unable to acquire random bytes from BCrypt: "
                          << statusWithDescription(status)};
}

StatusWith<std::unique_ptr<SymmetricEncryptor>> SymmetricEncryptor::create(const SymmetricKey& key,
                                                                           aesMode mode,
                                                                           const uint8_t* iv,
                                                                           size_t ivLen) {
    if (mode != aesMode::cbc) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "Native crypto on this platform only supports AES256-CBC");
    }

    try {
        std::unique_ptr<SymmetricEncryptor> encryptor =
            std::make_unique<SymmetricEncryptorWindows>(key, mode, iv, ivLen);
        return std::move(encryptor);
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

StatusWith<std::unique_ptr<SymmetricDecryptor>> SymmetricDecryptor::create(const SymmetricKey& key,
                                                                           aesMode mode,
                                                                           const uint8_t* iv,
                                                                           size_t ivLen) {
    if (mode != aesMode::cbc) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "Native crypto on this platform only supports AES256-CBC");
    }

    try {
        std::unique_ptr<SymmetricDecryptor> decryptor =
            std::make_unique<SymmetricDecryptorWindows>(key, mode, iv, ivLen);
        return std::move(decryptor);
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

}  // namespace crypto
}  // namespace mongo
