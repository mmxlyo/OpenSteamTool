#include "include/Hash.h"

#include "Handles.h"
#include "include/Encoding.h"
#include "include/Log.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <vector>

namespace OSTPlatform::Hash {

namespace {

uint32_t StatusCode(NTSTATUS status) {
    return static_cast<uint32_t>(status);
}

} // namespace

std::string Sha256OfFile(const std::filesystem::path& path) {
    Windows::UniqueFileHandle hFile(CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!hFile) {
        OSTP_LOG_WARN("Sha256OfFile: CreateFileW failed for '{}' (error={})",
                      Encoding::PathToUtf8(path), GetLastError());
        return {};
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        OSTP_LOG_WARN("Sha256OfFile: BCryptOpenAlgorithmProvider failed (status=0x{:08X})",
                      StatusCode(status));
        return {};
    }
    Windows::ScopeExit algGuard([&] { BCryptCloseAlgorithmProvider(hAlg, 0); });

    BCRYPT_HASH_HANDLE hHash = nullptr;
    Windows::ScopeExit hashGuard([&] { if (hHash) BCryptDestroyHash(hHash); });

    DWORD cbData = 0;
    DWORD hashObjSize = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjSize),
                               sizeof(DWORD), &cbData, 0);
    if (!BCRYPT_SUCCESS(status) || hashObjSize == 0) {
        OSTP_LOG_WARN("Sha256OfFile: BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed (status=0x{:08X}, size={})",
                      StatusCode(status), hashObjSize);
        return {};
    }
    std::vector<uint8_t> hashObj(hashObjSize);

    DWORD hashSize = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize),
                               sizeof(DWORD), &cbData, 0);
    if (!BCRYPT_SUCCESS(status) || hashSize == 0) {
        OSTP_LOG_WARN("Sha256OfFile: BCryptGetProperty(BCRYPT_HASH_LENGTH) failed (status=0x{:08X}, size={})",
                      StatusCode(status), hashSize);
        return {};
    }
    std::vector<uint8_t> hashBuf(hashSize);

    status = BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        OSTP_LOG_WARN("Sha256OfFile: BCryptCreateHash failed (status=0x{:08X})", StatusCode(status));
        return {};
    }

    constexpr DWORD kChunk = 65536;
    std::vector<uint8_t> buf(kChunk);
    for (;;) {
        DWORD bytesRead = 0;
        // A read failure must not be mistaken for EOF: hashing partial content
        // would yield a valid-looking but wrong digest.
        if (!ReadFile(hFile.get(), buf.data(), kChunk, &bytesRead, nullptr)) {
            OSTP_LOG_WARN("Sha256OfFile: ReadFile failed for '{}' (error={})",
                          Encoding::PathToUtf8(path), GetLastError());
            return {};
        }
        if (bytesRead == 0) break;
        status = BCryptHashData(hHash, buf.data(), bytesRead, 0);
        if (!BCRYPT_SUCCESS(status)) {
            OSTP_LOG_WARN("Sha256OfFile: BCryptHashData failed (status=0x{:08X})", StatusCode(status));
            return {};
        }
    }

    status = BCryptFinishHash(hHash, hashBuf.data(), hashSize, 0);
    if (!BCRYPT_SUCCESS(status)) {
        OSTP_LOG_WARN("Sha256OfFile: BCryptFinishHash failed (status=0x{:08X})", StatusCode(status));
        return {};
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(hashSize * 2);
    for (uint8_t b : hashBuf) {
        result += kHex[b >> 4];
        result += kHex[b & 0xF];
    }
    return result;
}

} // namespace OSTPlatform::Hash
