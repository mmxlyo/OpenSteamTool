#include "AppInfoParser.h"
#include "RaiiGuards.h"
#include "Utils.h"

#include <cstring>

namespace OST::ExtractTickets {

std::unordered_map<uint32_t, uint64_t> ParseAppInfoTokens(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>* targetAppIds) {
    std::unordered_map<uint32_t, uint64_t> tokens;
    if (steamPath.empty() || (targetAppIds && targetAppIds->empty())) {
        return tokens;
    }

    const std::string appinfoPath = JoinPath(steamPath, "appcache\\appinfo.vdf");
    ScopedHandle hFile{CreateFileA(
        appinfoPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    )};

    if (!hFile.IsValid()) {
        return tokens;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 16) {
        return tokens;
    }

    if (fileSize.QuadPart > 1024ULL * 1024ULL * 1024ULL) {
        return tokens;
    }

    ScopedHandle hMapping{CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!hMapping.IsValid()) {
        return tokens;
    }

    ScopedFileMappingView mappedView{MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0)};
    if (!mappedView) {
        return tokens;
    }

    const auto* data = mappedView.As<uint8_t>();
    const size_t totalBytes = static_cast<size_t>(fileSize.QuadPart);

    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(uint32_t));

    // Valid appinfo.vdf magic format: 0x075644xx (version >= 38 has AccessToken at offset +16)
    if ((magic & 0xFFFFFF00) != 0x07564400) {
        return tokens;
    }

    const uint8_t version = static_cast<uint8_t>(magic & 0xFF);
    if (version < 38) {
        return tokens;
    }

    size_t offset = 8;
    size_t appsEnd = totalBytes;

    // Version 41+ (0x29+) includes a 64-bit string table offset at file offset 8.
    // Apps section ends at stringTableOffset.
    if (version >= 41) {
        if (totalBytes >= 16) {
            uint64_t stringTableOffset = 0;
            std::memcpy(&stringTableOffset, data + 8, sizeof(uint64_t));
            if (stringTableOffset >= 16 && stringTableOffset <= totalBytes) {
                appsEnd = static_cast<size_t>(stringTableOffset);
            }
            offset = 16;
        }
    }

    std::unordered_set<uint32_t> matchedTargetAppIds;
    while (offset + 8 <= appsEnd) {
        uint32_t entryAppId = 0;
        uint32_t entrySize = 0;
        std::memcpy(&entryAppId, data + offset, sizeof(uint32_t));
        std::memcpy(&entrySize, data + offset + 4, sizeof(uint32_t));

        if (entryAppId == 0) {
            break; // 0 marks end of apps list
        }

        // Each app entry header after size contains at least 60 bytes:
        // InfoState(4) + LastUpdated(4) + AccessToken(8) + SHA1_text(20) + ChangeNumber(4) + SHA1_bin(20) = 60.
        // Prevent overflow and ensure entry stays strictly within appsEnd.
        if (entrySize < 60 || entrySize > appsEnd - (offset + 8)) {
            break;
        }

        // If filtering by specific AppIDs, only extract when matched
        if (!targetAppIds || targetAppIds->contains(entryAppId)) {
            if (targetAppIds) {
                matchedTargetAppIds.insert(entryAppId);
            }
            // AccessToken is at entry offset +16
            uint64_t accessToken = 0;
            std::memcpy(&accessToken, data + offset + 16, sizeof(uint64_t));
            if (accessToken != 0) {
                tokens[entryAppId] = accessToken;
            }
            if (targetAppIds && matchedTargetAppIds.size() >= targetAppIds->size()) {
                break; // All unique target apps found; early exit to avoid scanning remaining thousands of apps
            }
        }

        offset += 8 + entrySize;
    }

    return tokens;
}

} // namespace OST::ExtractTickets
