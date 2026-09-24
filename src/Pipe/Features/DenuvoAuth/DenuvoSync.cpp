#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"
#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Tickets/AppTicket.h"
#include "OSTPlatform/include/Encoding.h"
#include "Steam/Structs.h"
#include "Steam/Enums.h"
#include "Utils/Config/Config.h"
#include "dllmain.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace PipeManager::DenuvoAuth {

    bool HasCmdLineArg(const char* cmdLine, const char* arg) {
        if (!cmdLine || !arg) return false;
        const size_t argLen = strlen(arg);
        if (argLen == 0) return false;

        for (const char* p = cmdLine; *p; ++p) {
            // Fast token boundary check: token must begin at string start or after whitespace/quotes
            if (p == cmdLine || *(p - 1) == ' ' || *(p - 1) == '\t' || *(p - 1) == '"' || *(p - 1) == '\'') {
                if (_strnicmp(p, arg, argLen) == 0) {
                    const char next = p[argLen];
                    if (next == '\0' || next == ' ' || next == '\t' || next == '"' || next == '\'' || next == '=') {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool HasNoDenuvoArg(const char* cmdLine) {
        return cmdLine && (HasCmdLineArg(cmdLine, "-nodenuvo") ||
                           HasCmdLineArg(cmdLine, "-no-denuvo") ||
                           HasCmdLineArg(cmdLine, "-no_denuvo"));
    }

    bool HasForcedDenuvoArg(const char* cmdLine) {
        return cmdLine && (HasCmdLineArg(cmdLine, "-forcedenuvo") ||
                           HasCmdLineArg(cmdLine, "-force-denuvo") ||
                           HasCmdLineArg(cmdLine, "-force_denuvo"));
    }

namespace {

    std::mutex g_dPlusMutex;
    std::unordered_set<AppId_t> g_dPlusLaunches;
    std::mutex g_syncFileMutex;

    std::string_view TrimWhitespace(std::string_view str) noexcept {
        while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
            str.remove_prefix(1);
        }
        while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
            str.remove_suffix(1);
        }
        return str;
    }

    std::vector<std::string> TokenizeQuoted(std::string_view line) {
        std::vector<std::string> tokens;
        tokens.reserve(4);

        size_t pos = 0;
        while (pos < line.size()) {
            pos = line.find('"', pos);
            if (pos == std::string_view::npos) break;

            std::string token;
            token.reserve(32);
            size_t i = pos + 1;
            bool closed = false;

            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    if (line[i + 1] == '"') {
                        token.push_back('"');
                        i += 2;
                        continue;
                    }
                    if (line[i + 1] == '\\') {
                        token.push_back('\\');
                        i += 2;
                        continue;
                    }
                }
                if (line[i] == '"') {
                    closed = true;
                    pos = i + 1;
                    break;
                }
                token.push_back(line[i]);
                ++i;
            }

            if (!closed) break;
            tokens.push_back(std::move(token));
        }

        return tokens;
    }

    std::filesystem::path FindAcfPath(AppId_t appId, const std::string& exePath) {
        std::error_code ec;

        // Clean exePath from enclosing quotes and whitespace
        std::string cleanExe = exePath;
        while (!cleanExe.empty() && (cleanExe.front() == '"' || cleanExe.front() == '\'' || std::isspace(static_cast<unsigned char>(cleanExe.front())))) cleanExe.erase(0, 1);
        while (!cleanExe.empty() && (cleanExe.back() == '"' || cleanExe.back() == '\'' || std::isspace(static_cast<unsigned char>(cleanExe.back())))) cleanExe.pop_back();

        // Strategy 1: Walk up from exePath directly to find steamapps folder
        if (!cleanExe.empty()) {
            for (auto p = OSTPlatform::Encoding::PathFromUtf8(cleanExe).parent_path();
                 !p.empty() && p != p.parent_path();
                 p = p.parent_path()) {
                const auto& fn = p.filename().native();
                if (_wcsicmp(fn.c_str(), L"steamapps") == 0) {
                    auto candidate = p / ("appmanifest_" + std::to_string(appId) + ".acf");
                    if (std::filesystem::exists(candidate, ec) && !ec) {
                        LOG_INFO("DenuvoSync: located ACF from exePath: {}", candidate.string());
                        return candidate;
                    }
                }
            }
        }

        // Strategy 2: Check primary Steam install path
        std::string steamPath = SteamInstallPath;
        if (!steamPath.empty()) {
            auto primaryAcf = std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(steamPath)) / "steamapps" / ("appmanifest_" + std::to_string(appId) + ".acf");
            if (std::filesystem::exists(primaryAcf, ec) && !ec) {
                LOG_INFO("DenuvoSync: located ACF from primary Steam path: {}", primaryAcf.string());
                return primaryAcf;
            }

            // Strategy 3: Parse libraryfolders.vdf
            auto libVdf = std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(steamPath)) / "steamapps" / "libraryfolders.vdf";
            std::ifstream file(libVdf);
            if (file.is_open()) {
                std::string line;
                while (std::getline(file, line)) {
                    std::string_view trimmed = TrimWhitespace(line);
                    if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("#")) continue;

                    auto tokens = TokenizeQuoted(trimmed);
                    if (tokens.size() >= 2) {
                        std::string key = tokens[0];
                        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (key == "path") {
                            auto libAcf = std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(tokens[1])) / "steamapps" / ("appmanifest_" + std::to_string(appId) + ".acf");
                            if (std::filesystem::exists(libAcf, ec) && !ec) {
                                LOG_INFO("DenuvoSync: located ACF from libraryfolders.vdf: {}", libAcf.string());
                                return libAcf;
                            }
                        }
                    }
                }
            }
        }

        LOG_WARN("DenuvoSync: failed to find appmanifest_{}.acf across all library paths", appId);
        return {};
    }

    struct AcfData {
        std::map<uint32_t, std::string> installedDepots; // depotId -> manifestGid
        std::map<uint32_t, uint64_t> depotSizes;
    };

    AcfData ParseAcf(const std::filesystem::path& acfPath) {
        AcfData data;
        std::ifstream file(acfPath);
        if (!file.is_open()) return data;

        std::string line;
        std::vector<std::string> stack;
        std::string pendingSection;

        while (std::getline(file, line)) {
            std::string_view trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("#")) continue;

            auto tokens = TokenizeQuoted(trimmed);

            if (tokens.size() == 1) {
                std::string s = tokens[0];
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                pendingSection = std::move(s);
            }

            for (char ch : trimmed) {
                if (ch == '{') {
                    if (!pendingSection.empty()) {
                        stack.push_back(std::move(pendingSection));
                        pendingSection.clear();
                    } else {
                        stack.push_back("");
                    }
                }
            }

            if (tokens.size() >= 2) {
                std::string key = tokens[0];
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const std::string& val = tokens[1];

                // Case A: stack = [..., "installeddepots", "<depotId>"]
                if (stack.size() >= 2 && stack[stack.size() - 2] == "installeddepots") {
                    const std::string& depotSec = stack.back();
                    uint32_t dId = 0;
                    auto [p, ec] = std::from_chars(depotSec.data(), depotSec.data() + depotSec.size(), dId);
                    if (ec == std::errc{} && dId != 0) {
                        if (key == "manifest") {
                            data.installedDepots[dId] = val;
                        } else if (key == "size") {
                            uint64_t sz = 0;
                            std::from_chars(val.data(), val.data() + val.size(), sz);
                            data.depotSizes[dId] = sz;
                        }
                    }
                }
                // Case B: stack = [..., "mounteddepots"] -> "<depotId>" "<manifestGid>"
                else if (!stack.empty() && stack.back() == "mounteddepots") {
                    uint32_t dId = 0;
                    auto [p, ec] = std::from_chars(tokens[0].data(), tokens[0].data() + tokens[0].size(), dId);
                    if (ec == std::errc{} && dId != 0 && !val.empty()) {
                        data.installedDepots.try_emplace(dId, val);
                    }
                }
            }

            for (char ch : trimmed) {
                if (ch == '}') {
                    if (!stack.empty()) {
                        stack.pop_back();
                    }
                }
            }
        }

        return data;
    }

    std::map<uint32_t, std::string> ParseConfigVdfKeys(const std::string& steamPath) {
        std::map<uint32_t, std::string> keys;
        if (steamPath.empty()) return keys;

        auto configVdf = std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(steamPath)) / "config" / "config.vdf";
        std::ifstream file(configVdf);
        if (!file.is_open()) return keys;

        std::string line;
        std::vector<std::string> stack;
        std::string pendingSection;

        while (std::getline(file, line)) {
            std::string_view trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("#")) continue;

            auto tokens = TokenizeQuoted(trimmed);

            if (tokens.size() == 1) {
                std::string s = tokens[0];
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                pendingSection = std::move(s);
            }

            for (char ch : trimmed) {
                if (ch == '{') {
                    if (!pendingSection.empty()) {
                        stack.push_back(std::move(pendingSection));
                        pendingSection.clear();
                    } else {
                        stack.push_back("");
                    }
                }
            }

            if (tokens.size() >= 2) {
                std::string key = tokens[0];
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (key == "decryptionkey" && tokens[1].size() == 64 && stack.size() >= 2) {
                    if (stack[stack.size() - 2] == "depots") {
                        const std::string& depotSec = stack.back();
                        uint32_t dId = 0;
                        auto [p, ec] = std::from_chars(depotSec.data(), depotSec.data() + depotSec.size(), dId);
                        if (ec == std::errc{} && dId != 0) {
                            keys[dId] = tokens[1];
                        }
                    }
                }
            }

            for (char ch : trimmed) {
                if (ch == '}') {
                    if (!stack.empty()) {
                        stack.pop_back();
                    }
                }
            }
        }

        return keys;
    }

    bool AtomicWriteLines(const std::filesystem::path& targetPath, const std::vector<std::string>& lines) {
        std::error_code ec;
        auto parent = targetPath.parent_path();
        std::filesystem::create_directories(parent, ec);

        static std::atomic<uint64_t> s_seq{0};
        const auto seq = s_seq.fetch_add(1, std::memory_order_relaxed);
        const auto tid = GetCurrentThreadId();
        const std::filesystem::path tempPath = targetPath.wstring() + L".tmp." + std::to_wstring(tid) + L"." + std::to_wstring(seq);

        {
            std::ofstream out(tempPath, std::ios::trunc);
            if (!out.is_open()) {
                LOG_ERROR("DenuvoSync: failed to open temp file for writing: {}", tempPath.string());
                return false;
            }
            for (const auto& line : lines) {
                out << line << "\n";
            }
            out.flush();
            if (!out.good()) {
                out.close();
                std::filesystem::remove(tempPath, ec);
                return false;
            }
        }

        bool moved = false;
        DWORD lastError = 0;
        for (int attempt = 0; attempt < 5; ++attempt) {
            SetFileAttributesW(targetPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                moved = true;
                break;
            }
            lastError = GetLastError();
            Sleep(10);
        }

        if (!moved) {
            LOG_ERROR("DenuvoSync: MoveFileExW failed to replace {} (error={})", targetPath.string(), lastError);
            std::filesystem::remove(tempPath, ec);
            return false;
        }

        return true;
    }

    void CopyManifestMirrors(AppId_t appId, const std::filesystem::path& targetDir, const AcfData& acfData, const std::filesystem::path& acfPath) {
        std::error_code ec;
        std::filesystem::create_directories(targetDir, ec);

        std::vector<std::filesystem::path> searchDirs;
        auto addSearchDir = [&](const std::filesystem::path& dir) {
            if (dir.empty()) return;
            for (const auto& existing : searchDirs) {
                if (std::filesystem::equivalent(dir, existing, ec) || (!ec && dir == existing)) return;
                ec.clear();
            }
            if (std::filesystem::is_directory(dir, ec) && !ec) {
                searchDirs.push_back(dir);
            }
        };

        if (!acfPath.empty()) {
            addSearchDir(acfPath.parent_path() / "depotcache");
            if (acfPath.parent_path() != acfPath.parent_path().parent_path()) {
                addSearchDir(acfPath.parent_path().parent_path() / "depotcache");
            }
        }

        if (SteamInstallPath[0] != '\0') {
            addSearchDir(std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath)) / "depotcache");
        }

        for (const auto& [depotId, gid] : acfData.installedDepots) {
            if (gid.empty() || gid == "0") continue;
            std::string manifestFileName = std::to_string(depotId) + "_" + gid + ".manifest";

            for (const auto& sDir : searchDirs) {
                auto srcFile = sDir / manifestFileName;
                if (std::filesystem::exists(srcFile, ec) && !ec) {
                    auto destFile = targetDir / manifestFileName;
                    if (!std::filesystem::exists(destFile, ec)) {
                        if (CopyFileW(srcFile.c_str(), destFile.c_str(), FALSE)) {
                            LOG_INFO("DenuvoSync: mirrored manifest {} -> {}", srcFile.string(), destFile.string());
                        } else {
                            LOG_WARN("DenuvoSync: failed to mirror manifest {} -> {} (error={})",
                                     srcFile.string(), destFile.string(), GetLastError());
                        }
                    }
                    break;
                }
            }
        }
    }

} // namespace

void OnSpawnProcess(AppId_t appId, const char* pExePath, const char* cmdLine) {
    if (appId == 0 || appId == k_uAppIdInvalid) return;

    const bool hasDPlus = cmdLine && HasCmdLineArg(cmdLine, "-d+");
    const bool hasNoDenuvo = HasNoDenuvoArg(cmdLine);
    const bool hasForcedDenuvo = HasForcedDenuvoArg(cmdLine);
    const bool hasLua = LuaConfig::HasDepot(appId, false);

    LuaConfig::SetCmdLineNoDenuvo(appId, hasNoDenuvo);
    LuaConfig::SetCmdLineForcedDenuvo(appId, hasForcedDenuvo);

    if (hasNoDenuvo) {
        LOG_INFO("DenuvoSync: -nodenuvo active for appId={} — skipping Denuvo sync and manifest locking", appId);
        ClearDPlusLaunch(appId);
        return;
    }

    if (hasForcedDenuvo) {
        LOG_INFO("DenuvoSync: -forcedenuvo active for appId={}", appId);
    }

    // 🛑 ABSOLUTE ZERO-OPERATION INVARIANT:
    // If there is no Lua file configured and neither -d+ nor -forcedenuvo is passed,
    // OST executes ABSOLUTELY ZERO operations. Pure vanilla pass-through!
    if (!hasLua && !hasDPlus && !hasForcedDenuvo) {
        ClearDPlusLaunch(appId);
        return;
    }

    if (hasDPlus) {
        std::lock_guard lock(g_dPlusMutex);
        g_dPlusLaunches.insert(appId);
        LOG_INFO("DenuvoSync: recorded -d+ launch option for appId={}", appId);
    } else {
        ClearDPlusLaunch(appId);
    }

    // Only genuine owners, -d+, or -forcedenuvo execute sync or package generation
    if (LuaConfig::IsOwned(appId) || hasDPlus || hasForcedDenuvo) {
        std::string_view exeSv = pExePath ? pExePath : "";
        exeSv = TrimWhitespace(exeSv);
        while (!exeSv.empty() && (exeSv.front() == '"' || exeSv.front() == '\'')) {
            exeSv.remove_prefix(1);
        }
        while (!exeSv.empty() && (exeSv.back() == '"' || exeSv.back() == '\'')) {
            exeSv.remove_suffix(1);
        }
        try {
            SyncOrGenerate(appId, std::string(exeSv), hasDPlus);
        } catch (const std::exception& ex) {
            LOG_ERROR("DenuvoSync: exception in SyncOrGenerate for appId={}: {}", appId, ex.what());
        } catch (...) {
            LOG_ERROR("DenuvoSync: unknown exception in SyncOrGenerate for appId={}", appId);
        }
    }
}

bool IsDPlusLaunch(AppId_t appId) {
    if (appId == 0) return false;
    std::lock_guard lock(g_dPlusMutex);
    return g_dPlusLaunches.count(appId) > 0;
}

void ClearDPlusLaunch(AppId_t appId) {
    if (appId == 0) return;
    std::lock_guard lock(g_dPlusMutex);
    g_dPlusLaunches.erase(appId);
}

bool SyncOrGenerate(AppId_t appId, const std::string& exePath, bool isDPlus) {
    if (appId == 0 || appId == k_uAppIdInvalid) return false;

    // Gatekeeper: if nodenuvo is configured, completely skip
    if (LuaConfig::IsNoDenuvo(appId)) {
        LOG_INFO("DenuvoSync: nodenuvo set for appId={} — skipping Denuvo sync", appId);
        return false;
    }

    const bool hasLua = LuaConfig::HasDepot(appId, false);
    if (!hasLua && !isDPlus) {
        return false;
    }

    // Gatekeeper 2: Denuvo detection
    // "只要是识别到Denuvo，除非配置了noDenuvo"
    bool isDenuvo = isDPlus || LuaConfig::IsForcedDenuvo(appId);
    if (!isDenuvo && !exePath.empty()) {
        isDenuvo = IsDenuvoPath(OSTPlatform::Encoding::PathFromUtf8(exePath));
    }

    if (!isDenuvo) {
        LOG_INFO("DenuvoSync: appId={} is not Denuvo — skipping Denuvo sync", appId);
        return false;
    }

    // Determine Lua paths
    std::filesystem::path luaBaseDir;
    if (LuaDir[0] != '\0') {
        luaBaseDir = OSTPlatform::Encoding::PathFromUtf8(LuaDir);
    } else if (SteamInstallPath[0] != '\0') {
        luaBaseDir = std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath)) / "config" / "lua";
    }
    if (luaBaseDir.empty()) {
        LOG_WARN("DenuvoSync: Lua directory undetermined");
        return false;
    }

    const auto appLuaDir = luaBaseDir / std::to_string(appId);
    const auto luaFilePath = appLuaDir / (std::to_string(appId) + ".lua");

    // Locate ACF file
    auto acfPath = FindAcfPath(appId, exePath);
    if (acfPath.empty()) {
        LOG_WARN("DenuvoSync: cannot find ACF for appId={}", appId);
        return false;
    }

    AcfData acfData = ParseAcf(acfPath);
    LOG_INFO("DenuvoSync: parsed ACF depots: count={}", acfData.installedDepots.size());
    if (acfData.installedDepots.empty()) {
        LOG_WARN("DenuvoSync: no installed depots found in {}", acfPath.string());
        return false;
    }

    auto depotKeys = ParseConfigVdfKeys(SteamInstallPath);
    LOG_INFO("DenuvoSync: parsed config.vdf keys: count={}", depotKeys.size());

    std::error_code ec;
    const bool shouldLockManifest = Config::GetDenuvoLockManifest();
    {
        std::lock_guard fileLock(g_syncFileMutex);
        const bool luaFileExists = std::filesystem::exists(luaFilePath, ec) && !ec;

        if (!luaFileExists) {
            // ── Case 1: First-time generation (-d+) ──────────────────────────────
            LOG_INFO("DenuvoSync: generating new <AppId>.lua for appId={}", appId);
            std::vector<std::string> lines;
            lines.reserve(32);
            lines.push_back("-- Auto-generated by OpenSteamTool (-d+) for AppID: " + std::to_string(appId));
            lines.push_back("");
            lines.push_back("-- Base Game");
            lines.push_back("addappid(" + std::to_string(appId) + ")");

            for (const auto& [depotId, gid] : acfData.installedDepots) {
                if (depotId != appId) {
                    auto itKey = depotKeys.find(depotId);
                    if (itKey != depotKeys.end() && !itKey->second.empty()) {
                        lines.push_back("addappid(" + std::to_string(depotId) + ", 1, \"" + itKey->second + "\")");
                    } else {
                        lines.push_back("addappid(" + std::to_string(depotId) + ")");
                    }
                }
            }

            lines.push_back("");
            lines.push_back("-- Locked Manifests (Prevent Auto-Update)");
            for (const auto& [depotId, gid] : acfData.installedDepots) {
                if (!gid.empty() && gid != "0") {
                    if (shouldLockManifest) {
                        lines.push_back("setManifestid(" + std::to_string(depotId) + ", \"" + gid + "\")");
                    } else {
                        lines.push_back("-- setManifestid(" + std::to_string(depotId) + ", \"" + gid + "\")");
                    }
                }
            }

            if (!AtomicWriteLines(luaFilePath, lines)) {
                LOG_ERROR("DenuvoSync: failed to write {}", luaFilePath.string());
                return false;
            }
        } else {
            // ── Case 2: In-place update / re-locking for existing Lua ────────────
            LOG_INFO("DenuvoSync: checking and updating existing <AppId>.lua for appId={}", appId);
            std::ifstream in(luaFilePath);
            if (!in.is_open()) {
                LOG_ERROR("DenuvoSync: failed to read {}", luaFilePath.string());
                return false;
            }

            std::vector<std::string> lines;
            std::string line;
            while (std::getline(in, line)) {
                lines.push_back(line);
            }
            in.close();

            std::unordered_set<uint32_t> handledDepots;
            int lastManifestLineIdx = -1;

            for (size_t i = 0; i < lines.size(); ++i) {
                std::string_view sv = TrimWhitespace(lines[i]);
                if (sv.empty()) continue;

                std::string lower(sv);
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                // Check for setManifestid line
                size_t mPos = lower.find("setmanifestid");
                if (mPos != std::string::npos) {
                    lastManifestLineIdx = static_cast<int>(i);
                    // Extract depotId: setManifestid( <depotId> ,
                    size_t openParen = lower.find('(', mPos);
                    size_t comma = lower.find(',', openParen);
                    if (openParen != std::string::npos && comma != std::string::npos && comma > openParen) {
                        std::string_view dStr = TrimWhitespace(std::string_view(lower).substr(openParen + 1, comma - openParen - 1));
                        uint32_t dId = 0;
                        auto [p, ec2] = std::from_chars(dStr.data(), dStr.data() + dStr.size(), dId);
                        if (ec2 == std::errc{} && dId != 0) {
                            auto itDepot = acfData.installedDepots.find(dId);
                            if (itDepot != acfData.installedDepots.end() && !itDepot->second.empty() && itDepot->second != "0") {
                                if (shouldLockManifest) {
                                    lines[i] = "setManifestid(" + std::to_string(dId) + ", \"" + itDepot->second + "\")";
                                } else {
                                    lines[i] = "-- setManifestid(" + std::to_string(dId) + ", \"" + itDepot->second + "\")";
                                }
                                handledDepots.insert(dId);
                            }
                        }
                    }
                }
            }

            // Enforce full coverage: insert any missing installed depot manifests
            std::vector<std::string> missingManifestLines;
            for (const auto& [depotId, gid] : acfData.installedDepots) {
                if (!gid.empty() && gid != "0" && !handledDepots.contains(depotId)) {
                    if (shouldLockManifest) {
                        missingManifestLines.push_back("setManifestid(" + std::to_string(depotId) + ", \"" + gid + "\")");
                    } else {
                        missingManifestLines.push_back("-- setManifestid(" + std::to_string(depotId) + ", \"" + gid + "\")");
                    }
                    LOG_INFO("DenuvoSync: re-inserting missing manifest depot={} gid={} (locked={})",
                             depotId, gid, shouldLockManifest);
                }
            }

            if (!missingManifestLines.empty()) {
                size_t insertPos = lastManifestLineIdx >= 0 ? static_cast<size_t>(lastManifestLineIdx + 1) : lines.size();
                lines.insert(lines.begin() + insertPos, missingManifestLines.begin(), missingManifestLines.end());
            }

            if (!AtomicWriteLines(luaFilePath, lines)) {
                LOG_ERROR("DenuvoSync: failed to update {}", luaFilePath.string());
                return false;
            }
        }
    }

    // Immediately load the written/updated Lua configuration into memory
    LuaConfig::ParseFile(OSTPlatform::Encoding::PathToUtf8(luaFilePath));

    if (shouldLockManifest) {
        // Mirror .manifest files into <AppId>/ folder and sync to Steam depotcache
        CopyManifestMirrors(appId, appLuaDir, acfData, acfPath);
        LuaConfig::SyncManifests(OSTPlatform::Encoding::PathToUtf8(appLuaDir));
    }

    // Write SteamID.txt for offline ticket impersonation (NEVER WRITE .bin FILES!)
    if (auto activeId = GetCurrentActiveSteamId(); activeId && *activeId != 0) {
        if (AppTicket::WriteSteamID(appId, *activeId)) {
            LOG_INFO("DenuvoSync: persisted SteamID.txt for appId={} steamid={}", appId, *activeId);
        }
    }

    LOG_INFO("DenuvoSync: SyncOrGenerate completed successfully for appId={}", appId);
    return true;
}

} // namespace PipeManager::DenuvoAuth
