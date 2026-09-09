#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "steam.h"

namespace {

struct DepotKeyInfo {
    uint32_t depotId{0};
    std::string hexKey;           // 64 hex characters (32 bytes AES key)
    std::string manifestId;       // optional manifest id
    std::string manifestFilePath; // optional full path to cached .manifest file
};

bool IsDecimal(std::string_view value) {
    if (value.empty()) return false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

std::optional<uint32_t> ParseAppId(const std::string& value) {
    if (!IsDecimal(value)) return std::nullopt;

    unsigned long parsed{0};
    try {
        size_t consumed{0};
        parsed = std::stoul(value, &consumed, 10);
        if (consumed != value.size() || parsed > 0xFFFFFFFFul) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }

    return static_cast<uint32_t>(parsed);
}

std::optional<uint32_t> ReadAppIdFromConsole() {
    std::cout << "AppID: ";

    std::string input;
    if (!std::getline(std::cin, input)) return std::nullopt;
    return ParseAppId(input);
}

std::optional<std::string> QueryRegistryString(HKEY root, const char* subKey, const char* valueName) {
    HKEY key{nullptr};
    if (RegOpenKeyExA(root, subKey, 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD valueType{0};
    DWORD valueSize{0};
    LSTATUS status{RegQueryValueExA(key, valueName, nullptr, &valueType, nullptr, &valueSize)};
    if (status != ERROR_SUCCESS || valueType != REG_SZ || valueSize == 0) {
        RegCloseKey(key);
        return std::nullopt;
    }

    std::string value(valueSize, '\0');
    status = RegQueryValueExA(
        key,
        valueName,
        nullptr,
        nullptr,
        reinterpret_cast<LPBYTE>(value.data()),
        &valueSize);
    RegCloseKey(key);

    if (status != ERROR_SUCCESS) return std::nullopt;
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

std::optional<std::string> FindSteamInstallPath() {
    constexpr const char* kSteamKey{"Software\\Valve\\Steam"};

    if (auto path{QueryRegistryString(HKEY_CURRENT_USER, kSteamKey, "SteamPath")}) {
        std::cout << "Found SteamPath in HKEY_CURRENT_USER: " << *path << "\n";
        return path;
    }

    return std::nullopt;
}

std::string JoinPath(std::string base, std::string_view name) {
    for (char& ch : base) {
        if (ch == '/') ch = '\\';
    }
    if (!base.empty() && base.back() != '\\') base += '\\';
    base += name;
    return base;
}

std::string NormalizeDir(std::string dir) {
    for (char& ch : dir) {
        if (ch == '/') ch = '\\';
    }
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();
    return dir;
}

std::optional<std::vector<uint8_t>> HexStringToBytes(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = hexVal(hex[i]);
        int lo = hexVal(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return bytes;
}

std::vector<std::string> FindSteamLibraryFolders(const std::string& steamPath) {
    std::vector<std::string> libraries;
    libraries.push_back(NormalizeDir(steamPath));

    const std::string libraryVdfPath = JoinPath(steamPath, "steamapps\\libraryfolders.vdf");
    std::ifstream file(libraryVdfPath);
    if (!file) return libraries;

    std::string line;
    while (std::getline(file, line)) {
        size_t pos = line.find("\"path\"");
        if (pos != std::string::npos) {
            size_t start = line.find('"', pos + 6);
            if (start != std::string::npos) {
                size_t end = line.find('"', start + 1);
                if (end != std::string::npos) {
                    std::string lib = line.substr(start + 1, end - start - 1);
                    std::string unescaped;
                    for (size_t i = 0; i < lib.size(); ++i) {
                        if (lib[i] == '\\' && i + 1 < lib.size() && lib[i + 1] == '\\') {
                            unescaped += '\\';
                            ++i;
                        } else {
                            unescaped += lib[i];
                        }
                    }
                    unescaped = NormalizeDir(unescaped);
                    if (!unescaped.empty()) {
                        bool exists = false;
                        for (const auto& existing : libraries) {
                            if (_stricmp(existing.c_str(), unescaped.c_str()) == 0) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) libraries.push_back(unescaped);
                    }
                }
            }
        }
    }
    return libraries;
}

std::vector<std::string> GetDepotcacheDirs(const std::string& steamPath) {
    std::vector<std::string> dirs;
    if (!steamPath.empty()) {
        dirs.push_back(JoinPath(steamPath, "depotcache"));
    }

    auto libraries = FindSteamLibraryFolders(steamPath);
    for (const auto& lib : libraries) {
        std::string dc1 = JoinPath(lib, "depotcache");
        std::string dc2 = JoinPath(lib, "steamapps\\depotcache");
        bool exist1 = false, exist2 = false;
        for (const auto& d : dirs) {
            if (_stricmp(d.c_str(), dc1.c_str()) == 0) exist1 = true;
            if (_stricmp(d.c_str(), dc2.c_str()) == 0) exist2 = true;
        }
        if (!exist1) dirs.push_back(dc1);
        if (!exist2) dirs.push_back(dc2);
    }
    return dirs;
}

std::string FindDepotManifestFile(const std::vector<std::string>& depotcacheDirs,
                                  uint32_t depotId,
                                  std::string& inOutManifestId) {
    // 1. If inOutManifestId is known, check if <depotId>_<manifestId>.manifest exists directly
    if (!inOutManifestId.empty()) {
        std::string expectedName = std::to_string(depotId) + "_" + inOutManifestId + ".manifest";
        for (const auto& dc : depotcacheDirs) {
            std::string fullPath = JoinPath(dc, expectedName);
            DWORD attr = GetFileAttributesA(fullPath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return fullPath;
            }
        }
    }

    // 2. Search for <depotId>_*.manifest in depotcache dirs, choosing the latest modified file
    std::string bestPath;
    FILETIME bestTime{};
    std::string bestManifestId;

    for (const auto& dc : depotcacheDirs) {
        std::string pattern = JoinPath(dc, std::to_string(depotId) + "_*.manifest");
        WIN32_FIND_DATAA fd{};
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (bestPath.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
                        bestTime = fd.ftLastWriteTime;
                        bestPath = JoinPath(dc, fd.cFileName);

                        // Extract manifest GID from filename: <depotId>_<manifestId>.manifest
                        std::string fname = fd.cFileName;
                        size_t under = fname.find('_');
                        size_t dot = fname.rfind('.');
                        if (under != std::string::npos && dot != std::string::npos && dot > under + 1) {
                            bestManifestId = fname.substr(under + 1, dot - under - 1);
                        }
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    if (!bestPath.empty()) {
        if (inOutManifestId.empty()) {
            inOutManifestId = bestManifestId;
        }
        return bestPath;
    }

    return "";
}

void ParseAcfDepots(const std::string& acfPath,
                    std::unordered_map<uint32_t, std::string>& outDepots,
                    std::unordered_set<uint32_t>& outDlcIds) {
    std::ifstream file(acfPath);
    if (!file) return;

    std::string line;
    bool inInstalledDepots = false;
    uint32_t currentDepotId = 0;
    int braceDepth = 0;
    int depotsDepth = -1;

    while (std::getline(file, line)) {
        for (char c : line) {
            if (c == '{') {
                braceDepth++;
            } else if (c == '}') {
                if (braceDepth == depotsDepth) {
                    inInstalledDepots = false;
                    depotsDepth = -1;
                }
                braceDepth--;
            }
        }

        if (!inInstalledDepots) {
            if (line.find("\"InstalledDepots\"") != std::string::npos) {
                inInstalledDepots = true;
                depotsDepth = braceDepth;
            }
            continue;
        }

        std::vector<std::string> tokens;
        size_t pos = 0;
        while ((pos = line.find('"', pos)) != std::string::npos) {
            size_t endPos = line.find('"', pos + 1);
            if (endPos == std::string::npos) break;
            tokens.push_back(line.substr(pos + 1, endPos - pos - 1));
            pos = endPos + 1;
        }

        if (tokens.size() == 1 && IsDecimal(tokens[0])) {
            auto parsed = ParseAppId(tokens[0]);
            if (parsed) {
                currentDepotId = *parsed;
                if (outDepots.find(currentDepotId) == outDepots.end()) {
                    outDepots[currentDepotId] = "";
                }
            }
        } else if (tokens.size() >= 2 && currentDepotId != 0) {
            if (tokens[0] == "manifest") {
                outDepots[currentDepotId] = tokens[1];
            } else if (tokens[0] == "dlcappid") {
                if (auto dlc = ParseAppId(tokens[1])) {
                    outDlcIds.insert(*dlc);
                }
            }
        }
    }
}

std::unordered_map<uint32_t, std::string> ParseConfigVdfDepotKeys(const std::string& steamPath) {
    std::unordered_map<uint32_t, std::string> depotKeys;
    const std::string configPath = JoinPath(steamPath, "config\\config.vdf");
    std::ifstream file(configPath);
    if (!file) return depotKeys;

    std::string line;
    uint32_t currentDepotId = 0;
    bool inDepots = false;
    int braceDepth = 0;
    int depotsDepth = -1;

    while (std::getline(file, line)) {
        if (size_t comment = line.find("//"); comment != std::string::npos) {
            line.erase(comment);
        }

        for (char c : line) {
            if (c == '{') {
                braceDepth++;
            } else if (c == '}') {
                if (braceDepth == depotsDepth) {
                    inDepots = false;
                    depotsDepth = -1;
                }
                braceDepth--;
            }
        }

        if (!inDepots) {
            if (line.find("\"depots\"") != std::string::npos) {
                inDepots = true;
                depotsDepth = braceDepth;
            }
        }

        std::vector<std::string> tokens;
        size_t pos = 0;
        while ((pos = line.find('"', pos)) != std::string::npos) {
            size_t endPos = line.find('"', pos + 1);
            if (endPos == std::string::npos) break;
            tokens.push_back(line.substr(pos + 1, endPos - pos - 1));
            pos = endPos + 1;
        }

        if (tokens.size() == 1 && IsDecimal(tokens[0])) {
            auto parsed = ParseAppId(tokens[0]);
            if (parsed) {
                currentDepotId = *parsed;
            }
        } else if (tokens.size() >= 2) {
            if (tokens[0] == "DecryptionKey" && tokens[1].size() == 64 && currentDepotId != 0) {
                depotKeys[currentDepotId] = tokens[1];
            }
        }
    }

    if (depotKeys.empty()) {
        file.clear();
        file.seekg(0, std::ios::beg);
        std::string fullText((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        size_t offset = 0;
        while ((offset = fullText.find("\"DecryptionKey\"", offset)) != std::string::npos) {
            size_t keyStart = fullText.find('"', offset + 15);
            if (keyStart != std::string::npos) {
                size_t keyEnd = fullText.find('"', keyStart + 1);
                if (keyEnd != std::string::npos && (keyEnd - keyStart - 1) == 64) {
                    std::string key = fullText.substr(keyStart + 1, 64);
                    size_t searchBack = offset;
                    while (searchBack > 0 && fullText[searchBack] != '{') searchBack--;
                    size_t q2 = fullText.rfind('"', searchBack);
                    if (q2 != std::string::npos && q2 > 0) {
                        size_t q1 = fullText.rfind('"', q2 - 1);
                        if (q1 != std::string::npos) {
                            std::string candidateId = fullText.substr(q1 + 1, q2 - q1 - 1);
                            if (IsDecimal(candidateId)) {
                                if (auto dId = ParseAppId(candidateId)) {
                                    depotKeys[*dId] = key;
                                }
                            }
                        }
                    }
                }
            }
            offset += 15;
        }
    }

    return depotKeys;
}

std::vector<DepotKeyInfo> ExtractDepotDecryptionKeys(
    const std::string& steamPath,
    uint32_t appId,
    ISteamClient* client,
    HSteamPipe pipe,
    HSteamUser user) {

    std::unordered_map<uint32_t, std::string> knownDepotManifests;
    std::unordered_set<uint32_t> knownDlcIds;

    knownDepotManifests[appId] = "";

    if (client && pipe && user) {
        auto* apps = reinterpret_cast<ISteamApps*>(
            client->GetISteamGenericInterface(user, pipe, kSteamAppsInterfaceVersion));
        if (apps) {
            DepotId_t depots[128]{};
            uint32_t count = apps->GetInstalledDepots(appId, depots, 128);
            for (uint32_t i = 0; i < count; ++i) {
                if (depots[i] != 0 && knownDepotManifests.find(depots[i]) == knownDepotManifests.end()) {
                    knownDepotManifests[depots[i]] = "";
                }
            }

            int dlcCount = apps->GetDLCCount();
            for (int i = 0; i < dlcCount; ++i) {
                AppId_t dlcId{0};
                bool available{false};
                char dlcName[256]{};
                if (apps->BGetDLCDataByIndex(i, &dlcId, &available, dlcName, static_cast<int>(sizeof(dlcName))) && dlcId != 0) {
                    knownDlcIds.insert(dlcId);
                    DepotId_t dlcDepots[64]{};
                    uint32_t dlcDepotCount = apps->GetInstalledDepots(dlcId, dlcDepots, 64);
                    for (uint32_t j = 0; j < dlcDepotCount; ++j) {
                        if (dlcDepots[j] != 0 && knownDepotManifests.find(dlcDepots[j]) == knownDepotManifests.end()) {
                            knownDepotManifests[dlcDepots[j]] = "";
                        }
                    }
                }
            }
        }
    }

    auto libraries = FindSteamLibraryFolders(steamPath);
    for (const auto& lib : libraries) {
        std::string acf = JoinPath(lib, ("steamapps\\appmanifest_" + std::to_string(appId) + ".acf").c_str());
        ParseAcfDepots(acf, knownDepotManifests, knownDlcIds);
    }

    for (uint32_t dlcId : knownDlcIds) {
        for (const auto& lib : libraries) {
            std::string acf = JoinPath(lib, ("steamapps\\appmanifest_" + std::to_string(dlcId) + ".acf").c_str());
            ParseAcfDepots(acf, knownDepotManifests, knownDlcIds);
        }
    }

    auto allDepotKeys = ParseConfigVdfDepotKeys(steamPath);

    std::vector<DepotKeyInfo> result;
    std::unordered_set<uint32_t> addedDepots;

    for (const auto& [dId, manifest] : knownDepotManifests) {
        auto it = allDepotKeys.find(dId);
        if (it != allDepotKeys.end() && !it->second.empty()) {
            result.push_back({dId, it->second, manifest, ""});
            addedDepots.insert(dId);
        }
    }

    for (uint32_t dlcId : knownDlcIds) {
        if (addedDepots.find(dlcId) == addedDepots.end()) {
            auto it = allDepotKeys.find(dlcId);
            if (it != allDepotKeys.end() && !it->second.empty()) {
                result.push_back({dlcId, it->second, "", ""});
                addedDepots.insert(dlcId);
            }
        }
    }

    for (const auto& [dId, key] : allDepotKeys) {
        if (addedDepots.find(dId) == addedDepots.end()) {
            if (dId >= appId && dId <= appId + 50) {
                std::string manifest = "";
                auto it = knownDepotManifests.find(dId);
                if (it != knownDepotManifests.end()) manifest = it->second;
                result.push_back({dId, key, manifest, ""});
                addedDepots.insert(dId);
            }
        }
    }

    // Also check known depots that might not have keys in config.vdf,
    // so any cached manifest files can still be discovered and extracted.
    for (const auto& [dId, manifest] : knownDepotManifests) {
        if (addedDepots.find(dId) == addedDepots.end()) {
            result.push_back({dId, "", manifest, ""});
            addedDepots.insert(dId);
        }
    }

    // Search for cached .manifest files across all depotcache directories
    auto depotcacheDirs = GetDepotcacheDirs(steamPath);
    for (auto& dk : result) {
        dk.manifestFilePath = FindDepotManifestFile(depotcacheDirs, dk.depotId, dk.manifestId);
    }

    // Remove entries that have no key, no manifest file, and no manifest ID
    result.erase(
        std::remove_if(result.begin(), result.end(), [](const DepotKeyInfo& dk) {
            return dk.hexKey.empty() && dk.manifestFilePath.empty() && dk.manifestId.empty();
        }),
        result.end()
    );

    std::sort(result.begin(), result.end(), [](const DepotKeyInfo& a, const DepotKeyInfo& b) {
        return a.depotId < b.depotId;
    });

    return result;
}

HMODULE LoadSteamClient64(std::string& loadedPath) {
    auto steamPath{FindSteamInstallPath()};
    if (!steamPath) {
        std::cerr << "Failed to find Steam install path in registry.\n";
        return nullptr;
    }

    const std::string steamDir{NormalizeDir(*steamPath)};
    loadedPath = JoinPath(*steamPath, "steamclient64.dll");

    // steamclient64.dll pulls in tier0_s64.dll / vstdlib_s64.dll from the Steam
    // directory. Add that directory to the search path and load with
    // LOAD_WITH_ALTERED_SEARCH_PATH so those dependencies resolve; otherwise the
    // load fails with ERROR_MOD_NOT_FOUND (126).
    SetDllDirectoryA(steamDir.c_str());
    HMODULE module{LoadLibraryExA(loadedPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)};
    if (!module) {
        std::cerr << "Failed to load " << loadedPath << " (GetLastError=" << GetLastError() << ").\n";
        return nullptr;
    }

    return module;
}

ISteamClient* CreateSteamClient(HMODULE module) {
    auto createInterface{reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"))};
    if (!createInterface) {
        std::cerr << "steamclient64.dll has no CreateInterface export.\n";
        return nullptr;
    }

    int returnCode{0};
    auto* client{reinterpret_cast<ISteamClient*>(createInterface(kSteamClientInterfaceVersion, &returnCode))};
    if (!client) {
        std::cerr << "CreateInterface(" << kSteamClientInterfaceVersion
                  << ") failed (returnCode=" << returnCode << ").\n";
        return nullptr;
    }
    return client;
}

// Open a pipe and attach to the already-running global user
bool OpenSession(ISteamClient* client, HSteamPipe& pipe, HSteamUser& user) {
    pipe = client->CreateSteamPipe();
    if (!pipe) {
        std::cerr << "CreateSteamPipe failed. Is Steam running?\n";
        return false;
    }

    user = client->ConnectToGlobalUser(pipe);
    if (!user) {
        std::cerr << "ConnectToGlobalUser failed. Is a user logged in?\n";
        client->BReleaseSteamPipe(pipe);
        pipe = 0;
        return false;
    }

    return true;
}

// App ownership ticket: ISteamAppTicket hands back the raw signed buffer plus
// offsets into it. nAppID is explicit, so this works for any owned app.
std::optional<std::vector<uint8_t>> ExtractAppOwnershipTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId) {
    auto* appTicket{reinterpret_cast<ISteamAppTicket*>(
        client->GetISteamGenericInterface(user, pipe, kSteamAppTicketInterfaceVersion))};
    if (!appTicket) {
        std::cerr << "GetISteamGenericInterface(" << kSteamAppTicketInterfaceVersion
                  << ") returned null.\n";
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(2048);
    uint32_t appIdOffset{0};
    uint32_t steamIdOffset{0};
    uint32_t signatureOffset{0};
    uint32_t signatureSize{0};
    const uint32_t written{appTicket->GetAppOwnershipTicketData(
        appId,
        buffer.data(),
        static_cast<uint32_t>(buffer.size()),
        &appIdOffset,
        &steamIdOffset,
        &signatureOffset,
        &signatureSize)};

    if (written == 0 || written > buffer.size()) {
        std::cerr << "GetAppOwnershipTicketData returned no ticket for AppID " << appId
                  << " (own the app and have it cached locally?).\n";
        return std::nullopt;
    }

    buffer.resize(written);
    std::cout << "Ownership ticket " << written << " bytes"
              << " (appIdOffset=" << appIdOffset
              << " steamIdOffset=" << steamIdOffset
              << " signatureOffset=" << signatureOffset
              << " signatureSize=" << signatureSize << ")\n";
    return buffer;
}

// Encrypted app ticket: asynchronous request whose result arrives as
// EncryptedAppTicketResponse_t. We have no callback dispatcher, so we poll
// ISteamUtils::IsAPICallCompleted and then read the result + ticket.
// See https://partner.steamgames.com/doc/api/ISteamUser#RequestEncryptedAppTicket
std::optional<std::vector<uint8_t>> ExtractEncryptedAppTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId) {
    auto* utils{client->GetISteamUtils(pipe, kSteamUtilsInterfaceVersion)};
    auto* steamUser{client->GetISteamUser(user, pipe, kSteamUserInterfaceVersion)};
    if (!utils || !steamUser) {
        std::cerr << "GetISteamUtils/GetISteamUser returned null.\n";
        return std::nullopt;
    }

    const SteamAPICall_t hCall{steamUser->RequestEncryptedAppTicket(nullptr, 0)};
    if (!hCall) {
        std::cerr << "RequestEncryptedAppTicket failed to start for AppID " << appId << ".\n";
        return std::nullopt;
    }

    // Bounded poll so a wedged client can never hang the tool.
    constexpr int kMaxWaitMs{15000};
    constexpr int kStepMs{50};
    bool failed{false};
    int waited{0};
    while (!utils->IsAPICallCompleted(hCall, &failed)) {
        if (waited >= kMaxWaitMs) {
            std::cerr << "Timed out waiting for EncryptedAppTicketResponse_t.\n";
            return std::nullopt;
        }
        Sleep(kStepMs);
        waited += kStepMs;
    }

    EncryptedAppTicketResponse_t response{};
    const bool gotResult{utils->GetAPICallResult(
        hCall,
        &response,
        sizeof(response),
        EncryptedAppTicketResponse_t::k_iCallback,
        &failed)};
    if (!gotResult || failed) {
        std::cerr << "GetAPICallResult failed for EncryptedAppTicketResponse_t.\n";
        return std::nullopt;
    }
    if (response.m_eResult != k_EResultOK) {
        std::cerr << "RequestEncryptedAppTicket returned EResult "
                  << static_cast<int>(response.m_eResult) << ".\n";
        return std::nullopt;
    }

    // Pass a null buffer first to learn the size, then fetch.
    uint32_t cbTicket{0};
    steamUser->GetEncryptedAppTicket(nullptr, 0, &cbTicket);
    if (cbTicket == 0) {
        std::cerr << "Encrypted app ticket is empty.\n";
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(cbTicket);
    if (!steamUser->GetEncryptedAppTicket(buffer.data(), static_cast<int>(buffer.size()), &cbTicket)) {
        std::cerr << "GetEncryptedAppTicket failed.\n";
        return std::nullopt;
    }

    buffer.resize(cbTicket);
    std::cout << "Encrypted ticket " << cbTicket << " bytes\n";
    return buffer;
}

// Dump the raw ticket bytes as a classic hex view: offset, 16 hex bytes, ASCII.
void PrintHex(const char* label, const std::vector<uint8_t>& data) {
    std::cout << label << " (" << data.size() << " bytes):\n";

    constexpr size_t kBytesPerRow{16};
    static const char kHex[]{"0123456789abcdef"};

    for (size_t row{0}; row < data.size(); row += kBytesPerRow) {
        // Offset column.
        std::string line;
        for (int shift{12}; shift >= 0; shift -= 4) {
            line += kHex[(row >> shift) & 0xF];
        }
        line += "  ";

        // Hex column.
        std::string ascii;
        for (size_t col{0}; col < kBytesPerRow; ++col) {
            if (row + col < data.size()) {
                const uint8_t byte{data[row + col]};
                line += kHex[byte >> 4];
                line += kHex[byte & 0xF];
                line += ' ';
                ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
            } else {
                line += "   ";
            }
            if (col == 7) line += ' ';
        }

        std::cout << line << " " << ascii << "\n";
    }
}

std::string ToHexString(const std::vector<uint8_t>& data) {
    static const char kHex[]{"0123456789abcdef"};
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0xF];
    }
    return out;
}

bool WriteBinaryFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        std::cerr << "Failed to create " << path << ".\n";
        return false;
    }
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!output) {
        std::cerr << "Failed to write " << path << ".\n";
        return false;
    }
    return true;
}

// Build the plain-text summary line for one ticket. Present -> the hex string,
// absent -> "null".
std::string TicketLine(const char* name, const std::optional<std::vector<uint8_t>>& ticket) {
    if (!ticket) return std::string{name} + ":null\n";
    return std::string{name} + "(" + std::to_string(ticket->size()) + "bytes):"
           + ToHexString(*ticket) + "\n";
}

// Everything lands in a single <appid> folder: the raw binary tickets,
// raw depot keys, plus a plain-text summary and ready-to-use .lua script.
bool WriteOutputs(uint32_t appId,
                  const std::optional<std::vector<uint8_t>>& ownership,
                  const std::optional<std::vector<uint8_t>>& encrypted,
                  const std::vector<DepotKeyInfo>& depotKeys) {
    const std::string dir{std::to_string(appId)};
    if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::cerr << "Failed to create directory " << dir
                  << " (GetLastError=" << GetLastError() << ").\n";
        return false;
    }

    bool ok{true};
    if (ownership) ok = WriteBinaryFile(JoinPath(dir, "appticket.bin"), *ownership) && ok;
    if (encrypted) ok = WriteBinaryFile(JoinPath(dir, "eticket.bin"), *encrypted) && ok;

    // Write binary depot key files (.key)
    for (const auto& dk : depotKeys) {
        if (dk.hexKey.empty()) continue;
        auto keyBytes = HexStringToBytes(dk.hexKey);
        if (keyBytes) {
            ok = WriteBinaryFile(JoinPath(dir, "depot_" + std::to_string(dk.depotId) + ".key"), *keyBytes) && ok;
        }
    }

    // Copy manifest files (.manifest) if found in depotcache
    std::vector<std::string> copiedManifests;
    for (const auto& dk : depotKeys) {
        if (!dk.manifestFilePath.empty()) {
            size_t slash = dk.manifestFilePath.find_last_of("\\/");
            std::string fname = (slash != std::string::npos) ? dk.manifestFilePath.substr(slash + 1) : dk.manifestFilePath;
            std::string dest = JoinPath(dir, fname);
            if (CopyFileA(dk.manifestFilePath.c_str(), dest.c_str(), FALSE)) {
                copiedManifests.push_back(fname);
            } else {
                std::cerr << "[WARN] Failed to copy manifest " << fname << " (GetLastError=" << GetLastError() << ").\n";
            }
        }
    }

    // Build tickets.txt summary
    std::string text = "appid:" + std::to_string(appId) + "\n";
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty()) {
            text += "depotkey(" + std::to_string(dk.depotId) + "):" + dk.hexKey + "\n";
        }
    }
    for (const auto& dk : depotKeys) {
        if (!dk.manifestId.empty()) {
            text += "manifest(" + std::to_string(dk.depotId) + "):" + dk.manifestId + "\n";
        }
    }
    text += TicketLine("appticket", ownership);
    text += TicketLine("eticket", encrypted);

    const std::string textPath{JoinPath(dir, "tickets.txt")};
    std::ofstream summary{textPath, std::ios::trunc};
    if (!summary || !(summary << text)) {
        std::cerr << "Failed to write " << textPath << ".\n";
        return false;
    }

    // Generate ready-to-use Lua script
    std::string luaText;
    luaText += "-- Auto-generated by extract_tickets for AppID: " + std::to_string(appId) + "\n";

    bool appIdHasKey = false;
    for (const auto& dk : depotKeys) {
        if (dk.depotId == appId && !dk.hexKey.empty()) {
            appIdHasKey = true;
            break;
        }
    }

    if (!appIdHasKey) {
        luaText += "addappid(" + std::to_string(appId) + ")\n";
    }

    // Write depot decryption keys
    bool hasKeys = false;
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty()) {
            hasKeys = true;
            break;
        }
    }
    if (hasKeys) {
        luaText += "\n-- Depot Decryption Keys\n";
        for (const auto& dk : depotKeys) {
            if (!dk.hexKey.empty()) {
                luaText += "addappid(" + std::to_string(dk.depotId) + ", 1, \"" + dk.hexKey + "\")\n";
            }
        }
    }

    // Write manifest IDs to pin manifests
    bool hasManifests = false;
    for (const auto& dk : depotKeys) {
        if (!dk.manifestId.empty()) {
            hasManifests = true;
            break;
        }
    }
    if (hasManifests) {
        luaText += "\n-- Manifest IDs (pinned)\n";
        for (const auto& dk : depotKeys) {
            if (!dk.manifestId.empty()) {
                luaText += "setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\")\n";
            }
        }
    }

    luaText += "\n";
    if (ownership) {
        luaText += "-- App Ownership Ticket (AppTicket)\n";
        luaText += "setAppTicket(" + std::to_string(appId) + ", \"" + ToHexString(*ownership) + "\")\n\n";
    }

    if (encrypted) {
        luaText += "-- Encrypted App Ticket (ETicket)\n";
        luaText += "setETicket(" + std::to_string(appId) + ", \"" + ToHexString(*encrypted) + "\")\n\n";
    }

    const std::string luaPath{JoinPath(dir, std::to_string(appId) + ".lua")};
    std::ofstream luaFile{luaPath, std::ios::trunc};
    if (!luaFile || !(luaFile << luaText)) {
        std::cerr << "Failed to write " << luaPath << ".\n";
        ok = false;
    }

    std::cout << "Wrote " << dir << "\\ (" << std::to_string(appId) << ".lua, tickets.txt";
    if (ownership) std::cout << ", appticket.bin";
    if (encrypted) std::cout << ", eticket.bin";
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty()) std::cout << ", depot_" << dk.depotId << ".key";
    }
    for (const auto& mName : copiedManifests) {
        std::cout << ", " << mName;
    }
    std::cout << ")\n";

    size_t keyCount = 0;
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty()) keyCount++;
    }
    if (keyCount > 0) {
        std::cout << "[INFO] Extracted " << keyCount << " depot decryption key(s):\n";
        for (const auto& dk : depotKeys) {
            if (!dk.hexKey.empty()) {
                std::cout << "       Depot " << dk.depotId << ": " << dk.hexKey << "\n";
            }
        }
    } else {
        std::cout << "[INFO] No cached depot decryption keys found in config.vdf for AppID " << appId << ".\n";
        std::cout << "[TIP] If this game requires depot keys, start installing/updating it once in Steam to cache them, then run extract_tickets again.\n";
    }

    if (!copiedManifests.empty()) {
        std::cout << "[INFO] Extracted " << copiedManifests.size() << " depot manifest file(s) (.manifest):\n";
        for (const auto& mName : copiedManifests) {
            std::cout << "       " << mName << "\n";
        }
    } else {
        std::cout << "[INFO] No cached .manifest files found in depotcache for AppID " << appId << ".\n";
    }

    std::cout << "[INFO] Ready-to-use Lua script saved to: " << luaPath << "\n";
    return ok;
}

void WaitForExit() {
    std::cout << "\nPress Enter to exit...";
    std::string dummy;
    std::getline(std::cin, dummy);
}

#if defined(_WIN64)
int Run(int argc, char** argv) {
    std::optional<uint32_t> appId;
    if (argc >= 2) {
        appId = ParseAppId(argv[1]);
        if (!appId) {
            std::cerr << "Invalid AppID: " << argv[1] << "\n";
            return 1;
        }
    } else {
        appId = ReadAppIdFromConsole();
        if (!appId) {
            std::cerr << "Invalid AppID.\n";
            return 1;
        }
    }

    // Run in the target app's context so GetAppID and RequestEncryptedAppTicket
    // resolve to this AppID. Must be set before steamclient64.dll initializes.
    const std::string appIdStr{std::to_string(*appId)};
    SetEnvironmentVariableA("SteamAppId", appIdStr.c_str());
    SetEnvironmentVariableA("SteamGameId", appIdStr.c_str());

    std::string steamClientPath;
    HMODULE steamClient{LoadSteamClient64(steamClientPath)};
    if (!steamClient) return 1;

    std::cout << "Loaded " << steamClientPath << "\n";

    ISteamClient* client{CreateSteamClient(steamClient)};
    if (!client) {
        FreeLibrary(steamClient);
        return 1;
    }

    HSteamPipe pipe{0};
    HSteamUser user{0};
    if (!OpenSession(client, pipe, user)) {
        FreeLibrary(steamClient);
        return 1;
    }

    if (auto* utils{client->GetISteamUtils(pipe, kSteamUtilsInterfaceVersion)}) {
        std::cout << "ConnectedUniverse=" << static_cast<int>(utils->GetConnectedUniverse())
                  << " ClientAppID=" << utils->GetAppID() << "\n";
    }

    auto ownership{ExtractAppOwnershipTicket(client, pipe, user, *appId)};
    if (ownership) PrintHex("Ownership ticket", *ownership);

    auto encrypted{ExtractEncryptedAppTicket(client, pipe, user, *appId)};
    if (encrypted) PrintHex("Encrypted ticket", *encrypted);

    auto steamPathOpt{FindSteamInstallPath()};
    std::string steamPath = steamPathOpt ? *steamPathOpt : "";
    std::vector<DepotKeyInfo> depotKeys;
    if (!steamPath.empty()) {
        depotKeys = ExtractDepotDecryptionKeys(steamPath, *appId, client, pipe, user);
    }

    const bool ok{WriteOutputs(*appId, ownership, encrypted, depotKeys)};

    client->BReleaseSteamPipe(pipe);
    FreeLibrary(steamClient);
    return ok ? 0 : 1;
}
#endif

} // namespace

int main(int argc, char** argv) {
#if !defined(_WIN64)
    std::cerr << "extract_tickets must be built as a 64-bit Windows executable.\n";
    WaitForExit();
    return 1;
#else
    const int rc{Run(argc, argv)};
    WaitForExit();
    return rc;
#endif
}
