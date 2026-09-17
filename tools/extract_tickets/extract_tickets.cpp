#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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
    uint32_t dlcId{0};            // 0 for base game, or DLC AppID if associated with a DLC
};

struct DlcInfo {
    uint32_t dlcId{0};
    std::string name;
    bool isOwned{false};
};

// RAII wrapper for Windows HANDLE
struct ScopedHandle {
    HANDLE handle{INVALID_HANDLE_VALUE};
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) noexcept : handle(h) {}
    ~ScopedHandle() noexcept {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                CloseHandle(handle);
            }
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    operator HANDLE() const noexcept { return handle; }
    bool IsValid() const noexcept { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
};

// RAII wrapper for MapViewOfFile memory pointer
struct ScopedFileMappingView {
    const void* address{nullptr};
    ScopedFileMappingView() = default;
    explicit ScopedFileMappingView(const void* addr) noexcept : address(addr) {}
    ~ScopedFileMappingView() noexcept {
        if (address) {
            UnmapViewOfFile(address);
        }
    }
    ScopedFileMappingView(const ScopedFileMappingView&) = delete;
    ScopedFileMappingView& operator=(const ScopedFileMappingView&) = delete;
    ScopedFileMappingView(ScopedFileMappingView&& other) noexcept : address(other.address) {
        other.address = nullptr;
    }
    ScopedFileMappingView& operator=(ScopedFileMappingView&& other) noexcept {
        if (this != &other) {
            if (address) {
                UnmapViewOfFile(address);
            }
            address = other.address;
            other.address = nullptr;
        }
        return *this;
    }
    template<typename T>
    const T* As() const noexcept { return static_cast<const T*>(address); }
    explicit operator bool() const noexcept { return address != nullptr; }
};

// RAII guard for Steam client session, loaded module, and process environment
struct SteamSessionGuard {
    ISteamClient* client{nullptr};
    HSteamPipe pipe{0};
    HSteamUser user{0};
    HMODULE module{nullptr};

    SteamSessionGuard() = default;
    SteamSessionGuard(ISteamClient* c, HSteamPipe p, HSteamUser u, HMODULE m) noexcept
        : client(c), pipe(p), user(u), module(m) {}

    ~SteamSessionGuard() noexcept {
        Reset();
    }

    SteamSessionGuard(const SteamSessionGuard&) = delete;
    SteamSessionGuard& operator=(const SteamSessionGuard&) = delete;

    SteamSessionGuard(SteamSessionGuard&& other) noexcept
        : client(other.client), pipe(other.pipe), user(other.user), module(other.module) {
        other.client = nullptr;
        other.pipe = 0;
        other.user = 0;
        other.module = nullptr;
    }

    SteamSessionGuard& operator=(SteamSessionGuard&& other) noexcept {
        if (this != &other) {
            Reset();
            client = other.client;
            pipe = other.pipe;
            user = other.user;
            module = other.module;
            other.client = nullptr;
            other.pipe = 0;
            other.user = 0;
            other.module = nullptr;
        }
        return *this;
    }

    void Reset() noexcept {
        if (client) {
            if (pipe && user) {
                client->ReleaseUser(pipe, user);
                user = 0;
            }
            if (pipe) {
                client->BReleaseSteamPipe(pipe);
                pipe = 0;
            }
            client = nullptr;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
            SetDllDirectoryA(nullptr);
        }
        // Clear spoofed Steam environment variables
        SetEnvironmentVariableA("SteamAppId", nullptr);
        SetEnvironmentVariableA("SteamGameId", nullptr);
        SetEnvironmentVariableA("SteamOverlayGameId", nullptr);
    }
};


// RAII wrapper for Windows FindFirstFile/FindNextFile HANDLE
struct ScopedFindHandle {
    HANDLE handle{INVALID_HANDLE_VALUE};
    ScopedFindHandle() = default;
    explicit ScopedFindHandle(HANDLE h) noexcept : handle(h) {}
    ~ScopedFindHandle() noexcept {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            FindClose(handle);
        }
    }
    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;
    ScopedFindHandle(ScopedFindHandle&& other) noexcept : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }
    ScopedFindHandle& operator=(ScopedFindHandle&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                FindClose(handle);
            }
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    operator HANDLE() const noexcept { return handle; }
    bool IsValid() const noexcept { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
};

// RAII wrapper for Windows HKEY
struct ScopedHKey {
    HKEY key{nullptr};
    ScopedHKey() = default;
    explicit ScopedHKey(HKEY k) noexcept : key(k) {}
    ~ScopedHKey() noexcept {
        if (key != nullptr) {
            RegCloseKey(key);
        }
    }
    ScopedHKey(const ScopedHKey&) = delete;
    ScopedHKey& operator=(const ScopedHKey&) = delete;
    ScopedHKey(ScopedHKey&& other) noexcept : key(other.key) {
        other.key = nullptr;
    }
    ScopedHKey& operator=(ScopedHKey&& other) noexcept {
        if (this != &other) {
            if (key != nullptr) {
                RegCloseKey(key);
            }
            key = other.key;
            other.key = nullptr;
        }
        return *this;
    }
    operator HKEY() const noexcept { return key; }
    bool IsValid() const noexcept { return key != nullptr; }
};

std::string SanitizeComment(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return out;
}

std::vector<std::string> TokenizeQuoted(std::string_view line) {
    std::vector<std::string> tokens;
    tokens.reserve(4);
    size_t pos = 0;
    while ((pos = line.find('"', pos)) != std::string_view::npos) {
        size_t endPos = line.find('"', pos + 1);
        if (endPos == std::string_view::npos) break;
        tokens.emplace_back(line.substr(pos + 1, endPos - pos - 1));
        pos = endPos + 1;
    }
    return tokens;
}

bool IsDecimal(std::string_view value) {
    if (value.empty()) return false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

bool IsValidManifestId(std::string_view value) {
    if (value.empty()) return false;
    bool hasNonZero = false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        if (ch != '0') hasNonZero = true;
    }
    return hasNonZero;
}

bool IsHex64(std::string_view value) {
    if (value.size() != 64) return false;
    for (char ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

std::optional<uint32_t> ParseAppId(std::string_view value) {
    if (value.empty()) return std::nullopt;
    uint32_t parsed{0};
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size() || parsed == 0) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<uint32_t> ReadAppIdFromConsole() {
    std::cout << "AppID: ";

    std::string input;
    if (!std::getline(std::cin, input)) return std::nullopt;
    return ParseAppId(input);
}

std::string JoinPath(std::string_view base, std::string_view name) {
    std::string result;
    result.reserve(base.size() + 1 + name.size());
    result.append(base);
    std::replace(result.begin(), result.end(), '/', '\\');
    if (!result.empty() && result.back() != '\\') {
        result.push_back('\\');
    }
    const size_t nameStart = result.size();
    result.append(name);
    std::replace(result.begin() + nameStart, result.end(), '/', '\\');
    return result;
}

std::string NormalizeDir(std::string dir) {
    std::replace(dir.begin(), dir.end(), '/', '\\');
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();
    return dir;
}

std::optional<std::string> QueryRegistryString(HKEY root, const char* subKey, const char* valueName) {
    HKEY rawKey{nullptr};
    LSTATUS status = RegOpenKeyExA(root, subKey, 0, KEY_READ | KEY_WOW64_32KEY, &rawKey);
    if (status != ERROR_SUCCESS) {
        status = RegOpenKeyExA(root, subKey, 0, KEY_READ, &rawKey);
    }
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    ScopedHKey key{rawKey};

    DWORD valueType{0};
    DWORD valueSize{0};
    status = RegQueryValueExA(key, valueName, nullptr, &valueType, nullptr, &valueSize);
    if (status != ERROR_SUCCESS || (valueType != REG_SZ && valueType != REG_EXPAND_SZ) || valueSize == 0) {
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

    if (status != ERROR_SUCCESS) return std::nullopt;
    value.resize(valueSize);
    while (!value.empty() && (value.back() == '\0' || value.back() == ' ')) value.pop_back();

    if (valueType == REG_EXPAND_SZ) {
        char expanded[MAX_PATH * 2]{0};
        DWORD expLen = ExpandEnvironmentStringsA(value.c_str(), expanded, static_cast<DWORD>(sizeof(expanded)));
        if (expLen > 0 && expLen < sizeof(expanded)) {
            value = expanded;
        }
    }

    return value;
}

std::optional<std::string> FindSteamInstallPath() {
    constexpr const char* kSteamKey{"Software\\Valve\\Steam"};

    if (auto path{QueryRegistryString(HKEY_CURRENT_USER, kSteamKey, "SteamPath")}) {
        std::string norm = NormalizeDir(*path);
        std::cout << "Found SteamPath in HKEY_CURRENT_USER: " << norm << "\n";
        return norm;
    }

    if (auto path{QueryRegistryString(HKEY_LOCAL_MACHINE, kSteamKey, "InstallPath")}) {
        std::string norm = NormalizeDir(*path);
        std::cout << "Found InstallPath in HKEY_LOCAL_MACHINE: " << norm << "\n";
        return norm;
    }

    const char* defaultPaths[] = {
        "C:\\Program Files (x86)\\Steam",
        "C:\\Program Files\\Steam"
    };
    for (const char* p : defaultPaths) {
        std::string checkExe = JoinPath(p, "steam.exe");
        DWORD attr = GetFileAttributesA(checkExe.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            std::cout << "Found Steam install path at default location: " << p << "\n";
            return NormalizeDir(p);
        }
    }

    return std::nullopt;
}


std::vector<std::string> FindSteamLibraryFolders(const std::string& steamPath) {
    std::vector<std::string> libraries;
    if (!steamPath.empty()) {
        libraries.push_back(NormalizeDir(steamPath));
    }

    const std::string libraryVdfPath = JoinPath(steamPath, "steamapps\\libraryfolders.vdf");
    std::ifstream file(libraryVdfPath);
    if (!file) return libraries;

    auto addLibrary = [&](std::string lib) {
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
        if (!unescaped.empty() && std::none_of(libraries.begin(), libraries.end(), [&](const auto& existing) {
            return _stricmp(existing.c_str(), unescaped.c_str()) == 0;
        })) {
            libraries.push_back(std::move(unescaped));
        }
    };

    std::string line;
    while (std::getline(file, line)) {
        if (size_t comment = line.find("//"); comment != std::string::npos) {
            line.erase(comment);
        }

        auto tokens = TokenizeQuoted(line);

        if (tokens.size() >= 2) {
            if (_stricmp(tokens[0].c_str(), "path") == 0) {
                addLibrary(tokens[1]);
            } else if (IsDecimal(tokens[0])) {
                if (tokens[1].find(':') != std::string::npos ||
                    tokens[1].find('\\') != std::string::npos ||
                    tokens[1].find('/') != std::string::npos) {
                    addLibrary(tokens[1]);
                }
            }
        }
    }
    return libraries;
}

std::vector<std::string> GetDepotcacheDirs(const std::string& steamPath, const std::vector<std::string>& libraries) {
    std::vector<std::string> dirs;
    auto addDir = [&](std::string d) {
        if (!d.empty() && std::none_of(dirs.begin(), dirs.end(), [&](const auto& existing) {
            return _stricmp(existing.c_str(), d.c_str()) == 0;
        })) {
            dirs.push_back(std::move(d));
        }
    };

    if (!steamPath.empty()) {
        addDir(JoinPath(steamPath, "depotcache"));
    }

    for (const auto& lib : libraries) {
        addDir(JoinPath(lib, "depotcache"));
        addDir(JoinPath(lib, "steamapps\\depotcache"));
    }
    return dirs;
}

std::vector<std::string> GetDepotcacheDirs(const std::string& steamPath) {
    return GetDepotcacheDirs(steamPath, FindSteamLibraryFolders(steamPath));
}

std::string FindDepotManifestFile(const std::vector<std::string>& depotcacheDirs,
                                  uint32_t depotId,
                                  std::string& inOutManifestId) {
    // 1. If inOutManifestId is known and valid decimal GID, check if <depotId>_<manifestId>.manifest exists directly
    if (IsValidManifestId(inOutManifestId)) {
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
        ScopedFindHandle hFind{FindFirstFileA(pattern.c_str(), &fd)};
        if (hFind.IsValid()) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::string_view fname{fd.cFileName};
                    size_t under = fname.find('_');
                    size_t dot = fname.rfind('.');
                    if (under != std::string_view::npos && dot != std::string_view::npos && dot > under + 1) {
                        std::string_view candidate = fname.substr(under + 1, dot - under - 1);
                        if (IsValidManifestId(candidate)) {
                            if (bestPath.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
                                bestTime = fd.ftLastWriteTime;
                                bestPath = JoinPath(dc, fname);
                                bestManifestId = std::string(candidate);
                            }
                        }
                    }
                }
            } while (FindNextFileA(hFind, &fd));
        }
    }

    if (!bestPath.empty()) {
        if (!bestManifestId.empty()) {
            inOutManifestId = bestManifestId;
        }
        return bestPath;
    }

    return "";
}

void ParseAcfDepots(const std::string& acfPath,
                    std::unordered_map<uint32_t, std::string>& outDepots,
                    std::unordered_set<uint32_t>& outDlcIds,
                    std::unordered_map<uint32_t, uint32_t>& outDepotToDlc) {
    std::ifstream file(acfPath);
    if (!file) return;

    std::string line;
    bool inInstalledDepots = false;
    bool inMountedDepots = false;
    bool pendingInstalledDepots = false;
    bool pendingMountedDepots = false;
    uint32_t currentDepotId = 0;
    int braceDepth = 0;
    int installedDepth = -1;
    int mountedDepth = -1;

    while (std::getline(file, line)) {
        if (size_t comment = line.find("//"); comment != std::string::npos) {
            line.erase(comment);
        }

        auto tokens = TokenizeQuoted(line);

        if (!inInstalledDepots && !inMountedDepots) {
            for (const auto& tok : tokens) {
                if (_stricmp(tok.c_str(), "InstalledDepots") == 0) {
                    pendingInstalledDepots = true;
                    break;
                } else if (_stricmp(tok.c_str(), "MountedDepots") == 0) {
                    pendingMountedDepots = true;
                    break;
                }
            }
        }

        for (char c : line) {
            if (c == '{') {
                braceDepth++;
                if (pendingInstalledDepots) {
                    inInstalledDepots = true;
                    installedDepth = braceDepth;
                    pendingInstalledDepots = false;
                } else if (pendingMountedDepots) {
                    inMountedDepots = true;
                    mountedDepth = braceDepth;
                    pendingMountedDepots = false;
                }
            } else if (c == '}') {
                if (inInstalledDepots && braceDepth == installedDepth) {
                    inInstalledDepots = false;
                    installedDepth = -1;
                    currentDepotId = 0;
                }
                if (inMountedDepots && braceDepth == mountedDepth) {
                    inMountedDepots = false;
                    mountedDepth = -1;
                }
                braceDepth--;
            }
        }

        if (inInstalledDepots) {
            if (tokens.size() == 1) {
                if (auto parsed = ParseAppId(tokens[0])) {
                    currentDepotId = *parsed;
                    outDepots.try_emplace(currentDepotId, "");
                    outDepotToDlc.try_emplace(currentDepotId, 0);
                }
            } else if (tokens.size() >= 2 && currentDepotId != 0) {
                if (_stricmp(tokens[0].c_str(), "manifest") == 0) {
                    outDepots[currentDepotId] = tokens[1];
                } else if (_stricmp(tokens[0].c_str(), "dlcappid") == 0) {
                    if (auto dlc = ParseAppId(tokens[1])) {
                        outDlcIds.insert(*dlc);
                        outDepotToDlc[currentDepotId] = *dlc;
                        outDepotToDlc[*dlc] = *dlc;
                        outDepots.try_emplace(*dlc, "");
                    }
                }
            }
        } else if (inMountedDepots) {
            if (tokens.size() >= 2 && IsValidManifestId(tokens[1])) {
                if (auto dId = ParseAppId(tokens[0])) {
                    if (outDepots[*dId].empty()) {
                        outDepots[*dId] = tokens[1];
                    }
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
    bool pendingDepots = false;
    int braceDepth = 0;
    int depotsDepth = -1;

    while (std::getline(file, line)) {
        if (size_t comment = line.find("//"); comment != std::string::npos) {
            line.erase(comment);
        }

        auto tokens = TokenizeQuoted(line);

        if (!inDepots) {
            for (const auto& tok : tokens) {
                if (_stricmp(tok.c_str(), "depots") == 0) {
                    pendingDepots = true;
                    break;
                }
            }
        }

        for (char c : line) {
            if (c == '{') {
                braceDepth++;
                if (pendingDepots) {
                    inDepots = true;
                    depotsDepth = braceDepth;
                    pendingDepots = false;
                }
            } else if (c == '}') {
                if (inDepots && braceDepth == depotsDepth) {
                    inDepots = false;
                    depotsDepth = -1;
                    currentDepotId = 0;
                }
                braceDepth--;
            }
        }

        if (!inDepots) continue;

        if (tokens.size() == 1) {
            if (auto parsed = ParseAppId(tokens[0])) {
                currentDepotId = *parsed;
            }
        } else if (tokens.size() >= 2) {
            if (_stricmp(tokens[0].c_str(), "DecryptionKey") == 0 && IsHex64(tokens[1]) && currentDepotId != 0) {
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
                    if (IsHex64(key)) {
                        size_t searchBack = offset;
                        while (searchBack > 0 && fullText[searchBack] != '{') searchBack--;
                        size_t q2 = fullText.rfind('"', searchBack);
                        if (q2 != std::string::npos && q2 > 0) {
                            size_t q1 = fullText.rfind('"', q2 - 1);
                            if (q1 != std::string::npos) {
                                std::string candidateId = fullText.substr(q1 + 1, q2 - q1 - 1);
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
    HSteamUser user,
    std::vector<DlcInfo>& outDlcs) {

    std::unordered_map<uint32_t, std::string> knownDepotManifests;
    std::unordered_set<uint32_t> knownDlcIds;
    std::unordered_map<uint32_t, uint32_t> depotToDlc;
    std::map<uint32_t, DlcInfo> dlcMap;

    knownDepotManifests[appId] = "";
    depotToDlc[appId] = 0;

    if (client && pipe && user) {
        auto* apps = reinterpret_cast<ISteamApps*>(
            client->GetISteamGenericInterface(user, pipe, kSteamAppsInterfaceVersion));
        auto* steamUser = client->GetISteamUser(user, pipe, kSteamUserInterfaceVersion);
        uint64 steamId{0};
        if (steamUser) {
            steamUser->GetSteamID(&steamId);
        }

        if (apps) {
            DepotId_t depots[128]{};
            uint32_t count = apps->GetInstalledDepots(appId, depots, static_cast<uint32_t>(std::size(depots)));
            uint32_t safeCount = std::min(count, static_cast<uint32_t>(std::size(depots)));
            for (uint32_t i = 0; i < safeCount; ++i) {
                if (depots[i] != 0) {
                    knownDepotManifests.try_emplace(depots[i], "");
                    depotToDlc.try_emplace(depots[i], 0);
                }
            }

            int dlcCount = apps->GetDLCCount();
            for (int i = 0; i < dlcCount; ++i) {
                AppId_t dlcId{0};
                bool available{false};
                char dlcName[256]{};
                if (apps->BGetDLCDataByIndex(i, &dlcId, &available, dlcName, static_cast<int>(sizeof(dlcName))) && dlcId != 0) {
                    bool isSubscribed = apps->BIsSubscribedApp(dlcId);
                    bool isInstalled = apps->BIsDlcInstalled(dlcId);
                    bool hasLicense = (steamUser && steamId != 0) ? (steamUser->UserHasLicenseForApp(steamId, dlcId) == 0) : false;
                    // Note: 'available' in BGetDLCDataByIndex indicates whether the DLC is published on Steam store,
                    // NOT whether the current account owns it. Only include DLCs actually owned, subscribed, or installed.
                    bool owned = isSubscribed || hasLicense || isInstalled;

                    if (owned && dlcId != appId) {
                        DlcInfo& d = dlcMap[dlcId];
                        d.dlcId = dlcId;
                        if (d.name.empty() && dlcName[0] != '\0') {
                            d.name = dlcName;
                        }
                        d.isOwned = true;
                        knownDlcIds.insert(dlcId);
                        depotToDlc[dlcId] = dlcId;

                        // Ensure DLC itself is checked for depot manifests
                        knownDepotManifests.try_emplace(dlcId, "");

                        // Also query any installed depots for this DLC
                        DepotId_t dlcDepots[64]{};
                        uint32_t dlcDepotCount = apps->GetInstalledDepots(dlcId, dlcDepots, static_cast<uint32_t>(std::size(dlcDepots)));
                        uint32_t safeDlcCount = std::min(dlcDepotCount, static_cast<uint32_t>(std::size(dlcDepots)));
                        for (uint32_t j = 0; j < safeDlcCount; ++j) {
                            if (dlcDepots[j] != 0) {
                                knownDepotManifests.try_emplace(dlcDepots[j], "");
                                depotToDlc[dlcDepots[j]] = dlcId;
                            }
                        }
                    }
                }
            }
        }
    }

    std::vector<std::string> libraries;
    if (!steamPath.empty()) {
        libraries = FindSteamLibraryFolders(steamPath);
        for (const auto& lib : libraries) {
            std::string acf = JoinPath(lib, "steamapps\\appmanifest_" + std::to_string(appId) + ".acf");
            ParseAcfDepots(acf, knownDepotManifests, knownDlcIds, depotToDlc);
        }

        std::vector<uint32_t> dlcQueue(knownDlcIds.begin(), knownDlcIds.end());
        std::unordered_set<uint32_t> visitedDlcs;
        while (!dlcQueue.empty()) {
            uint32_t dlcId = dlcQueue.back();
            dlcQueue.pop_back();
            if (!visitedDlcs.insert(dlcId).second) continue;

            for (const auto& lib : libraries) {
                std::string acf = JoinPath(lib, "steamapps\\appmanifest_" + std::to_string(dlcId) + ".acf");
                std::unordered_set<uint32_t> newlyFoundDlcs;
                ParseAcfDepots(acf, knownDepotManifests, newlyFoundDlcs, depotToDlc);
                for (uint32_t newDlc : newlyFoundDlcs) {
                    if (knownDlcIds.insert(newDlc).second) {
                        dlcQueue.push_back(newDlc);
                    }
                }
            }
        }
    }

    // Any DLC found via ACF installed depots is also confirmed owned
    for (uint32_t dlcId : knownDlcIds) {
        if (dlcId != appId) {
            DlcInfo& d = dlcMap[dlcId];
            d.dlcId = dlcId;
            d.isOwned = true;
            knownDepotManifests.try_emplace(dlcId, "");
        }
    }

    outDlcs.clear();
    for (const auto& [id, info] : dlcMap) {
        if (info.isOwned) {
            outDlcs.push_back(info);
        }
    }

    auto allDepotKeys = !steamPath.empty() ? ParseConfigVdfDepotKeys(steamPath) : std::unordered_map<uint32_t, std::string>{};

    auto getDlcIdForDepot = [&](uint32_t dId) -> uint32_t {
        auto it = depotToDlc.find(dId);
        if (it != depotToDlc.end()) return it->second;
        for (uint32_t dlcId : knownDlcIds) {
            if (dId == dlcId || (dlcId > 0 && dId >= dlcId && dId <= dlcId + 20)) {
                return dlcId;
            }
        }
        return 0; // base game
    };

    std::vector<DepotKeyInfo> result;
    std::unordered_set<uint32_t> addedDepots;

    for (const auto& [dId, manifest] : knownDepotManifests) {
        auto it = allDepotKeys.find(dId);
        if (it != allDepotKeys.end() && !it->second.empty()) {
            result.push_back({dId, it->second, manifest, "", getDlcIdForDepot(dId)});
            addedDepots.insert(dId);
        }
    }

    for (uint32_t dlcId : knownDlcIds) {
        if (!addedDepots.contains(dlcId)) {
            auto it = allDepotKeys.find(dlcId);
            if (it != allDepotKeys.end() && !it->second.empty()) {
                result.push_back({dlcId, it->second, "", "", dlcId});
                addedDepots.insert(dlcId);
            }
        }
    }

    for (const auto& [dId, key] : allDepotKeys) {
        if (!addedDepots.contains(dId)) {
            bool inRange = (dId >= appId && dId <= appId + 50);
            uint32_t matchedDlcId = 0;
            if (!inRange) {
                for (uint32_t dlcId : knownDlcIds) {
                    if (dlcId > 0 && dId >= dlcId && dId <= dlcId + 20) {
                        inRange = true;
                        matchedDlcId = dlcId;
                        break;
                    }
                }
            }
            if (inRange) {
                std::string manifest = "";
                auto it = knownDepotManifests.find(dId);
                if (it != knownDepotManifests.end()) manifest = it->second;
                result.push_back({dId, key, manifest, "", matchedDlcId ? matchedDlcId : getDlcIdForDepot(dId)});
                addedDepots.insert(dId);
            }
        }
    }

    // Also check known depots that might not have keys in config.vdf,
    // so any cached manifest files can still be discovered and extracted.
    for (const auto& [dId, manifest] : knownDepotManifests) {
        if (!addedDepots.contains(dId)) {
            result.push_back({dId, "", manifest, "", getDlcIdForDepot(dId)});
            addedDepots.insert(dId);
        }
    }

    // Search for cached .manifest files across all depotcache directories
    if (!steamPath.empty()) {
        auto depotcacheDirs = GetDepotcacheDirs(steamPath, libraries);
        for (auto& dk : result) {
            dk.manifestFilePath = FindDepotManifestFile(depotcacheDirs, dk.depotId, dk.manifestId);
        }
    }

    // Remove entries that have no key, no manifest file, and no valid manifest ID
    std::erase_if(result, [](const DepotKeyInfo& dk) {
        return dk.hexKey.empty() && dk.manifestFilePath.empty() && !IsValidManifestId(dk.manifestId);
    });

    std::sort(result.begin(), result.end(), [](const DepotKeyInfo& a, const DepotKeyInfo& b) {
        return a.depotId < b.depotId;
    });

    return result;
}

HMODULE LoadSteamClient64(const std::string& steamPath, std::string& loadedPath) {
    if (steamPath.empty()) {
        std::cerr << "[WARN] 未在注册表中找到 Steam 安装路径 / Failed to find Steam install path in registry.\n";
        return nullptr;
    }

    const std::string steamDir{NormalizeDir(steamPath)};
    loadedPath = JoinPath(steamPath, "steamclient64.dll");

    // steamclient64.dll pulls in tier0_s64.dll / vstdlib_s64.dll from the Steam
    // directory. Add that directory to the search path and load with
    // LOAD_WITH_ALTERED_SEARCH_PATH so those dependencies resolve; otherwise the
    // load fails with ERROR_MOD_NOT_FOUND (126).
    SetDllDirectoryA(steamDir.c_str());
    HMODULE module{LoadLibraryExA(loadedPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)};
    if (!module) {
        std::cerr << "[WARN] 加载 steamclient64.dll 失败 / Failed to load " << loadedPath << " (GetLastError=" << GetLastError() << ").\n";
        return nullptr;
    }

    return module;
}

ISteamClient* CreateSteamClient(HMODULE module) {
    if (!module) return nullptr;
    auto createInterface{reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"))};
    if (!createInterface) {
        std::cerr << "[WARN] steamclient64.dll 缺少 CreateInterface 导出 / steamclient64.dll has no CreateInterface export.\n";
        return nullptr;
    }

    int returnCode{0};
    auto* client{reinterpret_cast<ISteamClient*>(createInterface(kSteamClientInterfaceVersion, &returnCode))};
    if (!client) {
        std::cerr << "[WARN] CreateInterface(" << kSteamClientInterfaceVersion
                  << ") 失败 / failed (returnCode=" << returnCode << ").\n";
        return nullptr;
    }
    return client;
}

// Open a pipe and attach to the already-running global user
bool OpenSession(ISteamClient* client, HSteamPipe& pipe, HSteamUser& user) {
    if (!client) return false;
    pipe = client->CreateSteamPipe();
    if (!pipe) {
        return false;
    }

    user = client->ConnectToGlobalUser(pipe);
    if (!user) {
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
        std::cerr << "[WARN] GetISteamGenericInterface(" << kSteamAppTicketInterfaceVersion
                  << ") 返回空 / returned null.\n";
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(2048);
    uint32_t appIdOffset{0};
    uint32_t steamIdOffset{0};
    uint32_t signatureOffset{0};
    uint32_t signatureSize{0};
    uint32_t written{appTicket->GetAppOwnershipTicketData(
        appId,
        buffer.data(),
        static_cast<uint32_t>(buffer.size()),
        &appIdOffset,
        &steamIdOffset,
        &signatureOffset,
        &signatureSize)};

    if (written > buffer.size()) {
        buffer.resize(written);
        const uint32_t written2{appTicket->GetAppOwnershipTicketData(
            appId,
            buffer.data(),
            static_cast<uint32_t>(buffer.size()),
            &appIdOffset,
            &steamIdOffset,
            &signatureOffset,
            &signatureSize)};
        if (written2 == 0 || written2 > buffer.size()) {
            std::cerr << "[INFO] 未能获取 AppID " << appId << " 的所有权票据 (账号可能未拥有或本地未缓存) / "
                      << "GetAppOwnershipTicketData returned no ticket for AppID " << appId
                      << " (account may not own the app or not cached locally).\n";
            return std::nullopt;
        }
        written = written2;
    } else if (written == 0) {
        std::cerr << "[INFO] 未能获取 AppID " << appId << " 的所有权票据 (账号可能未拥有或本地未缓存) / "
                  << "GetAppOwnershipTicketData returned no ticket for AppID " << appId
                  << " (account may not own the app or not cached locally).\n";
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
        std::cerr << "[WARN] GetISteamUtils/GetISteamUser 返回空 / returned null.\n";
        return std::nullopt;
    }

    const SteamAPICall_t hCall{steamUser->RequestEncryptedAppTicket(nullptr, 0)};
    if (!hCall) {
        std::cerr << "[WARN] 请求 EncryptedAppTicket 启动失败 / RequestEncryptedAppTicket failed to start for AppID " << appId << ".\n";
        return std::nullopt;
    }

    // Bounded poll so a wedged client can never hang the tool.
    constexpr int kMaxWaitMs{15000};
    constexpr int kStepMs{50};
    bool failed{false};
    int waited{0};
    while (!utils->IsAPICallCompleted(hCall, &failed)) {
        if (waited >= kMaxWaitMs) {
            std::cerr << "[WARN] 等待 EncryptedAppTicket 超时 / Timed out waiting for EncryptedAppTicketResponse_t.\n";
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
        int failureReason = utils->GetAPICallFailureReason(hCall);
        std::cerr << "[WARN] 获取 EncryptedAppTicket 结果失败 / GetAPICallResult failed for EncryptedAppTicketResponse_t (failureReason="
                  << failureReason << ").\n";
        return std::nullopt;
    }
    if (response.m_eResult != k_EResultOK) {
        std::cerr << "[WARN] 请求 EncryptedAppTicket 返回状态码 / RequestEncryptedAppTicket returned EResult "
                  << static_cast<int>(response.m_eResult);
        if (response.m_eResult == k_EResultAccessDenied) {
            std::cerr << " (AccessDenied: 当前登录账号未拥有该游戏或无权获取其凭据 / Account does not own this app or lacks permission)";
        }
        std::cerr << ".\n";
        return std::nullopt;
    }

    // Pass a null buffer first to learn the size, then fetch.
    uint32_t cbTicket{0};
    steamUser->GetEncryptedAppTicket(nullptr, 0, &cbTicket);
    if (cbTicket == 0) {
        std::cerr << "[WARN] 加密票据为空 / Encrypted app ticket is empty.\n";
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(cbTicket);
    if (!steamUser->GetEncryptedAppTicket(buffer.data(), static_cast<int>(buffer.size()), &cbTicket)) {
        std::cerr << "[WARN] 获取 EncryptedAppTicket 数据失败 / GetEncryptedAppTicket failed.\n";
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
    if (!ticket || ticket->empty()) return std::string{name} + ":null\n";
    return std::string{name} + "(" + std::to_string(ticket->size()) + "bytes):"
           + ToHexString(*ticket) + "\n";
}

// Parses <steamPath>/appcache/appinfo.vdf to extract non-zero PICS AccessTokens.
// If targetAppIds is provided, only extracts tokens for AppIDs present in that set.
// Returns a map of AppID -> AccessToken.
std::unordered_map<uint32_t, uint64_t> ParseAppInfoTokens(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>* targetAppIds = nullptr) {
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

// Everything lands in a single <appid> folder: the raw binary tickets (if available),
// copied depot manifests, plus a plain-text summary and ready-to-use .lua script.
bool WriteOutputs(uint32_t appId,
                  const std::optional<std::vector<uint8_t>>& ownership,
                  const std::optional<std::vector<uint8_t>>& encrypted,
                  const std::vector<DepotKeyInfo>& depotKeys,
                  const std::vector<DlcInfo>& dlcs,
                  const std::unordered_map<uint32_t, uint64_t>& appTokens) {
    const std::string dir{std::to_string(appId)};
    if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::cerr << "Failed to create directory " << dir
                  << " (GetLastError=" << GetLastError() << ").\n";
        return false;
    }

    bool ok{true};
    if (ownership && !ownership->empty()) ok = WriteBinaryFile(JoinPath(dir, "appticket.bin"), *ownership) && ok;
    if (encrypted && !encrypted->empty()) ok = WriteBinaryFile(JoinPath(dir, "eticket.bin"), *encrypted) && ok;

    // Copy manifest files (.manifest) if found in depotcache
    std::vector<std::string> copiedManifests;
    copiedManifests.reserve(depotKeys.size());
    for (const auto& dk : depotKeys) {
        if (!dk.manifestFilePath.empty()) {
            std::string_view pathView{dk.manifestFilePath};
            size_t slash = pathView.find_last_of("\\/");
            std::string_view fname = (slash != std::string_view::npos) ? pathView.substr(slash + 1) : pathView;
            if (std::ranges::find(copiedManifests, fname) == copiedManifests.end()) {
                std::string dest = JoinPath(dir, fname);
                if (CopyFileA(dk.manifestFilePath.c_str(), dest.c_str(), FALSE)) {
                    copiedManifests.emplace_back(fname);
                } else {
                    std::cerr << "[WARN] Failed to copy manifest " << fname << " (GetLastError=" << GetLastError() << ").\n";
                }
            }
        }
    }

    uint64_t baseToken = 0;
    auto baseTokIt = appTokens.find(appId);
    if (baseTokIt != appTokens.end()) {
        baseToken = baseTokIt->second;
    }

    std::map<uint32_t, uint64_t> relevantTokens;
    if (baseToken != 0) {
        relevantTokens[appId] = baseToken;
    }
    for (const auto& dlc : dlcs) {
        auto it = appTokens.find(dlc.dlcId);
        if (it != appTokens.end() && it->second != 0) {
            relevantTokens[dlc.dlcId] = it->second;
        }
    }

    // Build tickets.txt summary
    std::string text;
    text.reserve(2048);
    text += "appid:" + std::to_string(appId) + "\n";
    for (const auto& dlc : dlcs) {
        text += "dlc(" + std::to_string(dlc.dlcId) + ")";
        if (!dlc.name.empty()) {
            text += ":" + SanitizeComment(dlc.name);
        }
        text += "\n";
    }
    for (const auto& dk : depotKeys) {
        if (!dk.hexKey.empty()) {
            text += "depotkey(" + std::to_string(dk.depotId) + "):" + dk.hexKey + "\n";
        }
    }
    for (const auto& dk : depotKeys) {
        if (IsValidManifestId(dk.manifestId)) {
            text += "manifest(" + std::to_string(dk.depotId) + "):" + dk.manifestId + "\n";
        }
    }
    if (baseToken != 0) {
        text += "token(" + std::to_string(appId) + "):" + std::to_string(baseToken) + "\n";
    } else {
        text += "token(" + std::to_string(appId) + "):null\n";
    }
    for (const auto& [tId, tVal] : relevantTokens) {
        if (tId != appId) {
            text += "token(" + std::to_string(tId) + "):" + std::to_string(tVal) + "\n";
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
    luaText.reserve(4096);
    luaText += "-- Auto-generated by extract_tickets for AppID: " + std::to_string(appId) + "\n\n";

    std::unordered_set<uint32_t> ownedDlcIdSet;
    for (const auto& dlc : dlcs) {
        ownedDlcIdSet.insert(dlc.dlcId);
    }

    // 1. Base Game Unlocks (AppID and base game depots)
    luaText += "-- Base Game\n";
    const DepotKeyInfo* baseAppDk = nullptr;
    for (const auto& dk : depotKeys) {
        if (dk.depotId == appId) {
            baseAppDk = &dk;
            break;
        }
    }

    if (baseAppDk && !baseAppDk->hexKey.empty()) {
        luaText += "addappid(" + std::to_string(appId) + ", 1, \"" + baseAppDk->hexKey + "\")\n";
    } else {
        luaText += "addappid(" + std::to_string(appId) + ")\n";
    }

    if (baseToken != 0) {
        luaText += "addtoken(" + std::to_string(appId) + ", \"" + std::to_string(baseToken) + "\")\n";
    }

    for (const auto& dk : depotKeys) {
        if (dk.depotId != appId && (dk.dlcId == 0 || !ownedDlcIdSet.contains(dk.dlcId))) {
            if (!dk.hexKey.empty()) {
                luaText += "addappid(" + std::to_string(dk.depotId) + ", 1, \"" + dk.hexKey + "\")\n";
            }
        }
    }

    // 2. Base Game Manifests (placed right under Base Game)
    bool hasBaseManifests = false;
    if (baseAppDk && IsValidManifestId(baseAppDk->manifestId)) {
        hasBaseManifests = true;
    }
    if (!hasBaseManifests) {
        for (const auto& dk : depotKeys) {
            if (dk.depotId != appId && (dk.dlcId == 0 || !ownedDlcIdSet.contains(dk.dlcId))) {
                if (IsValidManifestId(dk.manifestId)) {
                    hasBaseManifests = true;
                    break;
                }
            }
        }
    }

    if (hasBaseManifests) {
        luaText += "\n-- Base Game Manifests\n";
        if (baseAppDk && IsValidManifestId(baseAppDk->manifestId)) {
            luaText += "setManifestid(" + std::to_string(appId) + ", \"" + baseAppDk->manifestId + "\")\n";
        }
        for (const auto& dk : depotKeys) {
            if (dk.depotId != appId && (dk.dlcId == 0 || !ownedDlcIdSet.contains(dk.dlcId))) {
                if (IsValidManifestId(dk.manifestId)) {
                    luaText += "setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\")\n";
                }
            }
        }
    }

    // 3. Owned DLCs Unlocks
    if (!dlcs.empty()) {
        luaText += "\n-- Owned DLCs\n";
        for (const auto& dlc : dlcs) {
            const DepotKeyInfo* dlcDk = nullptr;
            for (const auto& dk : depotKeys) {
                if (dk.depotId == dlc.dlcId) {
                    dlcDk = &dk;
                    break;
                }
            }

            if (dlcDk && !dlcDk->hexKey.empty()) {
                luaText += "addappid(" + std::to_string(dlc.dlcId) + ", 1, \"" + dlcDk->hexKey + "\")";
            } else {
                luaText += "addappid(" + std::to_string(dlc.dlcId) + ")";
            }

            if (!dlc.name.empty()) {
                luaText += " -- " + SanitizeComment(dlc.name);
            }
            luaText += "\n";

            auto dlcTokIt = appTokens.find(dlc.dlcId);
            if (dlcTokIt != appTokens.end() && dlcTokIt->second != 0) {
                luaText += "addtoken(" + std::to_string(dlc.dlcId) + ", \"" + std::to_string(dlcTokIt->second) + "\")\n";
            }

            // Any subdepots of this DLC with keys
            for (const auto& dk : depotKeys) {
                if (dk.dlcId == dlc.dlcId && dk.depotId != dlc.dlcId) {
                    if (!dk.hexKey.empty()) {
                        luaText += "addappid(" + std::to_string(dk.depotId) + ", 1, \"" + dk.hexKey + "\")\n";
                    }
                }
            }
        }

        // 4. DLC Manifests (placed right under DLC unlocks)
        bool hasDlcManifests = false;
        for (const auto& dlc : dlcs) {
            for (const auto& dk : depotKeys) {
                if ((dk.depotId == dlc.dlcId || dk.dlcId == dlc.dlcId) && IsValidManifestId(dk.manifestId)) {
                    hasDlcManifests = true;
                    break;
                }
            }
            if (hasDlcManifests) break;
        }

        if (hasDlcManifests) {
            luaText += "\n-- DLC Manifests\n";
            for (const auto& dlc : dlcs) {
                // DLC itself manifest
                for (const auto& dk : depotKeys) {
                    if (dk.depotId == dlc.dlcId && IsValidManifestId(dk.manifestId)) {
                        luaText += "setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\")";
                        if (!dlc.name.empty()) {
                            luaText += " -- " + SanitizeComment(dlc.name);
                        }
                        luaText += "\n";
                        break;
                    }
                }
                // Any subdepots of this DLC with manifests
                for (const auto& dk : depotKeys) {
                    if (dk.dlcId == dlc.dlcId && dk.depotId != dlc.dlcId && IsValidManifestId(dk.manifestId)) {
                        luaText += "setManifestid(" + std::to_string(dk.depotId) + ", \"" + dk.manifestId + "\")\n";
                    }
                }
            }
        }
    }

    // 5. App Tickets (at the very end of the file)
    luaText += "\n";
    if (ownership && !ownership->empty()) {
        luaText += "-- App Ownership Ticket (AppTicket)\n";
        luaText += "setAppTicket(" + std::to_string(appId) + ", \"" + ToHexString(*ownership) + "\")\n\n";
    } else {
        luaText += "-- App Ownership Ticket (AppTicket)\n";
        luaText += "-- setAppTicket(" + std::to_string(appId) + ", \"null\")\n\n";
    }

    if (encrypted && !encrypted->empty()) {
        luaText += "-- Encrypted App Ticket (ETicket)\n";
        luaText += "setETicket(" + std::to_string(appId) + ", \"" + ToHexString(*encrypted) + "\")\n";
    } else {
        luaText += "-- Encrypted App Ticket (ETicket)\n";
        luaText += "-- setETicket(" + std::to_string(appId) + ", \"null\")\n";
    }

    const std::string luaPath{JoinPath(dir, std::to_string(appId) + ".lua")};
    std::ofstream luaFile{luaPath, std::ios::trunc};
    if (!luaFile || !(luaFile << luaText)) {
        std::cerr << "Failed to write " << luaPath << ".\n";
        ok = false;
    }

    std::cout << "Wrote " << dir << "\\ (" << std::to_string(appId) << ".lua, tickets.txt";
    if (ownership && !ownership->empty()) std::cout << ", appticket.bin";
    if (encrypted && !encrypted->empty()) std::cout << ", eticket.bin";
    for (const auto& mName : copiedManifests) {
        std::cout << ", " << mName;
    }
    std::cout << ")\n";

    if (!dlcs.empty()) {
        std::cout << "[INFO] 已提取 " << dlcs.size() << " 个拥有的 DLC / Extracted " << dlcs.size() << " owned DLC(s):\n";
        for (const auto& dlc : dlcs) {
            std::cout << "       DLC " << dlc.dlcId;
            if (!dlc.name.empty()) {
                std::cout << ": " << dlc.name;
            }
            std::cout << "\n";
        }
    } else {
        std::cout << "[INFO] 未检测到该游戏拥有的 DLC / No owned DLCs found for AppID " << appId << ".\n";
    }

    const size_t keyCount = std::count_if(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& dk) {
        return !dk.hexKey.empty();
    });
    if (keyCount > 0) {
        std::cout << "[INFO] 已提取 " << keyCount << " 个 Depot 解密密钥 / Extracted " << keyCount << " depot decryption key(s):\n";
        for (const auto& dk : depotKeys) {
            if (!dk.hexKey.empty()) {
                std::cout << "       Depot " << dk.depotId << ": " << dk.hexKey << "\n";
            }
        }
    } else {
        std::cout << "[INFO] 未在 config.vdf 中找到缓存的 Depot 解密密钥 / No cached depot decryption keys found in config.vdf for AppID " << appId << ".\n";
        std::cout << "[TIP] 若该游戏需要 Depot 密钥，请在 Steam 中启动一次安装/更新以生成缓存，然后重新运行提取工具。\n"
                  << "      If this game requires depot keys, start installing/updating it once in Steam to cache them, then run extract_tickets again.\n";
    }

    if (!copiedManifests.empty()) {
        std::cout << "[INFO] 已提取 " << copiedManifests.size() << " 个清单文件 (.manifest) / Extracted " << copiedManifests.size() << " depot manifest file(s) (.manifest):\n";
        for (const auto& mName : copiedManifests) {
            std::cout << "       " << mName << "\n";
        }
    } else {
        std::cout << "[INFO] 未在 depotcache 中找到缓存的清单文件 (.manifest) / No cached .manifest files found in depotcache for AppID " << appId << ".\n";
    }

    if (!relevantTokens.empty()) {
        std::cout << "[INFO] 已提取 " << relevantTokens.size() << " 个访问令牌 (AccessToken) / Extracted "
                  << relevantTokens.size() << " access token(s):\n";
        for (const auto& [tId, tVal] : relevantTokens) {
            std::cout << "       AppID " << tId << ": " << tVal << "\n";
        }
    } else {
        std::cout << "[INFO] 未在 appinfo.vdf 中找到非零访问令牌 (该游戏可能无需 Access Token) / "
                  << "No non-zero access token found in appinfo.vdf for AppID " << appId
                  << " (this game may not require an access token).\n";
    }

    std::cout << "[INFO] 配置文件已生成 / Ready-to-use Lua script saved to: " << luaPath << "\n";
    return ok;
}

void WaitForExit() {
    std::cout << "\n按回车键退出... / Press Enter to exit...";
    std::string dummy;
    std::getline(std::cin, dummy);
}

#if defined(_WIN64)
int Run(int argc, char** argv) {
    std::optional<uint32_t> appId;
    if (argc >= 2) {
        appId = ParseAppId(argv[1]);
        if (!appId) {
            std::cerr << "[ERROR] 无效的 AppID / Invalid AppID: " << argv[1] << "\n";
            return 1;
        }
    } else {
        appId = ReadAppIdFromConsole();
        if (!appId) {
            std::cerr << "[ERROR] 无效的 AppID / Invalid AppID.\n";
            return 1;
        }
    }

    // Run in the target app's context so GetAppID and RequestEncryptedAppTicket
    // resolve to this AppID. Must be set before steamclient64.dll initializes.
    const std::string appIdStr{std::to_string(*appId)};
    SetEnvironmentVariableA("SteamAppId", appIdStr.c_str());
    SetEnvironmentVariableA("SteamGameId", appIdStr.c_str());
    SetEnvironmentVariableA("SteamOverlayGameId", appIdStr.c_str());

    auto steamPathOpt{FindSteamInstallPath()};
    std::string steamPath = steamPathOpt ? *steamPathOpt : "";

    std::string steamClientPath;
    HMODULE steamClient{LoadSteamClient64(steamPath, steamClientPath)};
    ISteamClient* client{steamClient ? CreateSteamClient(steamClient) : nullptr};

    HSteamPipe pipe{0};
    HSteamUser user{0};
    const bool sessionOpened = (client != nullptr) && OpenSession(client, pipe, user);

    SteamSessionGuard sessionGuard{sessionOpened ? client : nullptr, pipe, sessionOpened ? user : 0, steamClient};

    if (!sessionOpened) {
        std::cout << "[WARN] Steam 未运行或未登录，已自动切换为【离线降级模式】。\n"
                  << "       Steam is not running or not logged in; switched to [Offline Degradation Mode].\n"
                  << "[INFO] 跳过在线凭证与授权：AppTicket、ETicket 及实时 DLC 状态将不可用。\n"
                  << "       Skipped live credentials: AppTicket, ETicket, and live DLC query are unavailable.\n"
                  << "[INFO] 继续扫描本地磁盘：正在提取本地缓存的 Depot 密钥、ACF 配置、清单文件与访问令牌 (Token)...\n"
                  << "       Continuing local scan: extracting cached depot keys, ACF configs, manifest files, and access tokens (Token)...\n\n";
    } else {
        std::cout << "Loaded " << steamClientPath << "\n";
        if (auto* utils{client->GetISteamUtils(pipe, kSteamUtilsInterfaceVersion)}) {
            std::cout << "ConnectedUniverse=" << static_cast<int>(utils->GetConnectedUniverse())
                      << " ClientAppID=" << utils->GetAppID() << "\n";
        }
    }

    std::optional<std::vector<uint8_t>> ownership;
    std::optional<std::vector<uint8_t>> encrypted;
    if (sessionOpened) {
        ownership = ExtractAppOwnershipTicket(client, pipe, user, *appId);
        if (ownership) PrintHex("Ownership ticket", *ownership);

        encrypted = ExtractEncryptedAppTicket(client, pipe, user, *appId);
        if (encrypted) PrintHex("Encrypted ticket", *encrypted);
    }

    std::vector<DlcInfo> dlcs;
    std::vector<DepotKeyInfo> depotKeys = ExtractDepotDecryptionKeys(
        steamPath, *appId, sessionOpened ? client : nullptr, pipe, user, dlcs);

    // 在线会话已完成全部在线提取工作，立即主动释放 Steam 用户会话与管道并清空环境变量，
    // 使 Steam 客户端无需等待后续本地文件解析或用户按键即可瞬间恢复正常空闲状态。
    sessionGuard.Reset();

    std::unordered_map<uint32_t, uint64_t> appTokens;
    if (!steamPath.empty()) {
        std::unordered_set<uint32_t> targetAppIds;
        targetAppIds.insert(*appId);
        for (const auto& dlc : dlcs) {
            targetAppIds.insert(dlc.dlcId);
        }
        appTokens = ParseAppInfoTokens(steamPath, &targetAppIds);
    }

    const bool ok{WriteOutputs(*appId, ownership, encrypted, depotKeys, dlcs, appTokens)};
    return ok ? 0 : 1;
}
#endif

// RAII guard for Windows console code page restoration
struct ConsoleCodePageGuard {
    UINT oldOutputCP{0};
    UINT oldCP{0};

    ConsoleCodePageGuard() {
        oldOutputCP = GetConsoleOutputCP();
        oldCP = GetConsoleCP();
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    ~ConsoleCodePageGuard() {
        SetConsoleOutputCP(oldOutputCP);
        SetConsoleCP(oldCP);
    }
};

} // namespace

int main(int argc, char** argv) {
#if !defined(_WIN64)
    std::cerr << "[ERROR] extract_tickets 必须编译为 64 位 Windows 程序。\n"
              << "        extract_tickets must be built as a 64-bit Windows executable.\n";
    WaitForExit();
    return 1;
#else
    ConsoleCodePageGuard cpGuard;
    const int rc{Run(argc, argv)};
    WaitForExit();
    return rc;
#endif
}
