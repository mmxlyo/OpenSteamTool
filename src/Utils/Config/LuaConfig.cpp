#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/SteamMetadata/StatsClient.h"
#include "Utils/Tickets/AppTicket.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <lua.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
    struct lua_State;
}

namespace LuaConfig{
    static std::shared_mutex g_configSharedMutex;
    static std::mutex g_luaStateMutex;
    static lua_State* g_lua_state = nullptr;
    static std::atomic<bool> g_hasManifestCodeFunc{false};
    static std::atomic<bool> g_hasManifestCodeFuncEx{false};
    static std::unordered_map<AppId_t, std::string> DepotKeySet{};
    static std::unordered_map<AppId_t, uint64_t> AccessTokenSet{};
    static std::unordered_set<AppId_t> PinnedApps{};
    static std::unordered_map<uint64_t, ManifestOverride> ManifestOverrides{};
    static std::unordered_map<AppId_t, uint64_t> StatSteamIdSet{};
    static std::unordered_set<AppId_t> OwnedAppIdSet{};
    // Process exe name (lowercase) → appid; populated by addprocess() in Lua config.
    static std::unordered_map<std::string, AppId_t> ProcessNameAppIdMap{};
    // App IDs that should bypass ProtectionScan and be treated as Denuvo games.
    static std::unordered_set<AppId_t> ForcedDenuvoSet{};
    // App IDs that should bypass ProtectionScan and be treated as non-Denuvo games.
    static std::unordered_set<AppId_t> NoDenuvoSet{};
    // On-demand eticket mint endpoint, set via seteticketurl() in Lua config.
    // Empty = disabled (EticketClient falls back to credential-store ticket).
    static std::string EticketUrl{};

    // Per-file tracking: which depots each .lua file contributed.
    static std::string g_currentFile;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileDepots;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileAppTickets;
    static std::unordered_map<AppId_t, uint32_t> g_appTicketRefCount;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileETickets;
    static std::unordered_map<AppId_t, uint32_t> g_eTicketRefCount;
    static std::unordered_map<std::string, std::unordered_map<uint64_t, ManifestOverride>> g_fileManifestOverrides;
    static std::unordered_map<std::string, std::unordered_map<AppId_t, uint64_t>> g_fileTokens;
    static std::unordered_map<std::string, std::unordered_map<std::string, AppId_t>> g_fileProcesses;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileForcedDenuvo;
    static std::unordered_map<AppId_t, uint32_t> g_forcedDenuvoRefCount;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileNoDenuvo;
    static std::unordered_map<AppId_t, uint32_t> g_noDenuvoRefCount;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_filePinnedApps;
    static std::unordered_map<AppId_t, uint32_t> g_pinnedAppsRefCount;
    static std::unordered_map<std::string, std::unordered_map<AppId_t, uint64_t>> g_fileStats;
    static std::unordered_map<std::string, std::string> g_fileEticketUrl;
    static std::unordered_map<std::string, uint64_t> g_fileParseSequence;
    static uint64_t g_nextFileParseSequence = 0;
    // Reference count: how many files provide each depot.
    static std::unordered_map<AppId_t, uint32_t> g_depotRefCount;
    // mtime (unix epoch seconds) of each parsed .lua file, captured at ParseFile entry.
    static std::unordered_map<std::string, uint32_t> g_fileMtime;
    // Per-appId purchase time: max(mtime) across every file that currently contributes it.
    // Simple variant: never lowered on UnloadFile unless the refcount drops to zero.
    static std::unordered_map<AppId_t, uint32_t> g_purchaseTime;
    // Depot IDs removed by UnloadFile / added by ParseFile, consumed by NotifyLicenseChanged.
    static std::vector<AppId_t> g_pendingRemovals;
    static std::vector<AppId_t> g_pendingAdditions;
    constexpr uint64_t kDefaultStatSteamId = 76561198028121353ULL;
    static std::recursive_mutex g_manifestSyncMutex;

    // Case-insensitive function registry: lowercase name → C function
    static std::unordered_map<std::string, lua_CFunction> g_func_registry;

    static bool ParseUInt64Decimal(const char* text, uint64_t* out) {
        if (!text || !out) return false;
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(text);
        if (!parsed) return false;
        *out = *parsed;
        return true;
    }

    static uint8 ParseHexByte(std::string_view text) {
        return OSTPlatform::Numbers::ParseHexUInt8(text).value_or(0);
    }

    static std::vector<uint8_t> ParseHexStringToBytes(const char* hex, size_t hexLen) {
        std::vector<uint8_t> binary;
        if (!hex || hexLen == 0) return binary;
        binary.reserve((hexLen + 1) / 2);
        for (size_t i = 0; i < hexLen; i += 2) {
            char byteStr[3] = {
                hex[i],
                i + 1 < hexLen ? hex[i + 1] : '0',
                '\0'
            };
            binary.push_back(ParseHexByte(byteStr));
        }
        return binary;
    }

    static void SetActiveManifestOverride(uint64_t depotId, const ManifestOverride& override) {
        ManifestOverrides[depotId] = override;
    }

    static void ClearActiveManifestOverride(uint64_t depotId) {
        ManifestOverrides.erase(depotId);
    }

    static void RebuildManifestOverride(uint64_t depotId) {
        const ManifestOverride* best = nullptr;
        uint64_t bestSeq = 0;

        for (const auto& [file, overrides] : g_fileManifestOverrides) {
            auto overrideIt = overrides.find(depotId);
            if (overrideIt == overrides.end()) continue;

            auto seqIt = g_fileParseSequence.find(file);
            if (seqIt == g_fileParseSequence.end()) continue;

            if (!best || seqIt->second > bestSeq) {
                best = &overrideIt->second;
                bestSeq = seqIt->second;
            }
        }

        if (best) {
            SetActiveManifestOverride(depotId, *best);
        } else {
            ClearActiveManifestOverride(depotId);
        }
    }

    static void RebuildAccessToken(AppId_t appId) {
        const uint64_t* best = nullptr;
        uint64_t bestSeq = 0;

        for (const auto& [file, tokens] : g_fileTokens) {
            auto it = tokens.find(appId);
            if (it == tokens.end()) continue;

            auto seqIt = g_fileParseSequence.find(file);
            if (seqIt == g_fileParseSequence.end()) continue;

            if (!best || seqIt->second > bestSeq) {
                best = &it->second;
                bestSeq = seqIt->second;
            }
        }

        if (best) {
            AccessTokenSet[appId] = *best;
        } else {
            AccessTokenSet.erase(appId);
        }
    }

    static void RebuildProcess(const std::string& procName) {
        const AppId_t* best = nullptr;
        uint64_t bestSeq = 0;

        for (const auto& [file, procs] : g_fileProcesses) {
            auto it = procs.find(procName);
            if (it == procs.end()) continue;

            auto seqIt = g_fileParseSequence.find(file);
            if (seqIt == g_fileParseSequence.end()) continue;

            if (!best || seqIt->second > bestSeq) {
                best = &it->second;
                bestSeq = seqIt->second;
            }
        }

        if (best) {
            ProcessNameAppIdMap[procName] = *best;
        } else {
            ProcessNameAppIdMap.erase(procName);
        }
    }

    static void RebuildStatSteamId(AppId_t appId) {
        const uint64_t* best = nullptr;
        uint64_t bestSeq = 0;

        for (const auto& [file, stats] : g_fileStats) {
            auto it = stats.find(appId);
            if (it == stats.end()) continue;

            auto seqIt = g_fileParseSequence.find(file);
            if (seqIt == g_fileParseSequence.end()) continue;

            if (!best || seqIt->second > bestSeq) {
                best = &it->second;
                bestSeq = seqIt->second;
            }
        }

        if (best) {
            StatSteamIdSet[appId] = *best;
        } else {
            StatSteamIdSet.erase(appId);
        }
    }

    static void RebuildEticketUrl() {
        const std::string* best = nullptr;
        uint64_t bestSeq = 0;

        for (const auto& [file, url] : g_fileEticketUrl) {
            if (url.empty()) continue;

            auto seqIt = g_fileParseSequence.find(file);
            if (seqIt == g_fileParseSequence.end()) continue;

            if (!best || seqIt->second > bestSeq) {
                best = &url;
                bestSeq = seqIt->second;
            }
        }

        if (best) {
            EticketUrl = *best;
        } else {
            EticketUrl.clear();
        }
    }

    // ── Lua HTTP helpers ──────────────────────────────────────────
    //   http_get(url [, headers]) → body, status_code
    //     headers: optional table, e.g. {["Accept"]="application/json"}
    //
    //   http_post(url, post_body [, headers]) → body, status_code
    //     post_body: string payload
    // ────────────────────────────────────────────────────────────────

    // Serialise a Lua {key=val, ...} table at stack index idx into a
    // wstring of "Key: Val\r\n" lines. Returns L"" if no table.
    static std::wstring LuaHeadersToWstr(lua_State* L, int idx) {
        std::wstring headers;
        if (idx < 1 || !lua_istable(L, idx)) return headers;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            std::string key(lua_tostring(L, -2));
            std::string val(lua_tostring(L, -1));
            headers += std::wstring(key.begin(), key.end())
                    + L": " + std::wstring(val.begin(), val.end()) + L"\r\n";
            lua_pop(L, 1);
        }
        return headers;
    }

    static int lua_http_get(lua_State* L) {
        // http_get(url [, headers]) → body, status_code
        auto hdrs = LuaHeadersToWstr(L, lua_gettop(L) >= 2 ? 2 : -1);
        auto r = OSTPlatform::Http::Execute(L"GET", luaL_checkstring(L, 1),
                                            nullptr, 0, hdrs.empty() ? nullptr : hdrs.c_str());
        if (r.ok) {
            lua_pushstring(L, r.body.c_str());
            lua_pushinteger(L, r.status);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "HTTP request failed");
        }
        return 2;
    }

    static int lua_http_post(lua_State* L) {
        // http_post(url, post_body [, headers]) → body, status_code
        size_t bodyLen = 0;
        const char* body = luaL_checklstring(L, 2, &bodyLen);
        auto hdrs = LuaHeadersToWstr(L, lua_gettop(L) >= 3 ? 3 : -1);
        auto r = OSTPlatform::Http::Execute(L"POST", luaL_checkstring(L, 1),
                                            body, static_cast<uint32_t>(bodyLen),
                                            hdrs.empty() ? nullptr : hdrs.c_str());
        if (r.ok) {
            lua_pushstring(L, r.body.c_str());
            lua_pushinteger(L, r.status);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "HTTP request failed");
        }
        return 2;
    }

    // ── Case-insensitive global function lookup ─────────────────
    // __index metamethod on _G: when a global name isn't found,
    // lower-case the name and look it up in g_func_registry.
    static int case_insensitive_global_index(lua_State* L) {
        const char* name = lua_tostring(L, 2);
        if (!name) {
            lua_pushnil(L);
            return 1;
        }
        std::string lower;
        for (const char* p = name; *p; ++p) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        }
        auto it = g_func_registry.find(lower);
        if (it != g_func_registry.end()) {
            lua_pushcfunction(L, it->second);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // Register a C function both in _G (lowercase name) and in the
    // case-insensitive lookup table so any case variant resolves.
    static void register_func(lua_State* L, const char* lowercase_name, lua_CFunction fn) {
        g_func_registry[lowercase_name] = fn;
        lua_pushcfunction(L, fn);
        lua_setglobal(L, lowercase_name);
    }

    // ── Lua: addappid / addtoken / pinApp / setManifestid ────────
    static int lua_addappid(lua_State* L) {
        // addappid(integer, integer, string)
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as app/depot id.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t DepotId = (uint32_t)value;
        // Read the optional third argument as a key.
        std::string Key = "";
        if (argc > 2) {
            if (!lua_isstring(L, 3))
                return luaL_error(L, "");
            const char* key = lua_tostring(L, 3);
            // Keep only keys with exactly 64 characters.
            if (strlen(key) == 64) {
                Key = std::string(key);
            }
        }
        // Non-empty keys have priority over existing empty keys.
        if (!Key.empty() || !DepotKeySet.count(DepotId)) {
            DepotKeySet[DepotId] = Key;
        }

        if (!g_currentFile.empty()) {
            if (g_fileDepots[g_currentFile].insert(DepotId).second) {
                if (++g_depotRefCount[DepotId] == 1)
                    g_pendingAdditions.push_back(DepotId);
                
                // Update the appId's purchase time with the current file's mtime,
                // keeping the maximum across every contributing file.
                auto mtIt = g_fileMtime.find(g_currentFile);
                if (mtIt != g_fileMtime.end()) {
                    uint32_t mt = mtIt->second;
                    auto& slot = g_purchaseTime[DepotId];
                    if (mt > slot) slot = mt;
                }
            }
        }

        return 0;
    }

    static int lua_addtoken(lua_State* L) {
        // addtoken(integer, string(uint64_t))
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as app/depot id.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;
        // Read the second argument as a token.
        if (argc > 1) {
            if (!lua_isstring(L, 2))
                return luaL_error(L, "");
            const char* token = lua_tostring(L, 2);
            uint64_t parsedToken = 0;
            if (!ParseUInt64Decimal(token, &parsedToken)) {
                return luaL_error(L, "");
            }
            if (!g_currentFile.empty()) {
                g_fileTokens[g_currentFile][AppId] = parsedToken;
                RebuildAccessToken(AppId);
            } else {
                AccessTokenSet[AppId] = parsedToken;
            }
        }

        return 0;
    }

    static int lua_addprocess(lua_State* L) {
        // addprocess(appid, "ExeName.exe")
        // Maps a process exe name to an appid so OST can identify games
        // that launch without exporting SteamAppId env vars.
        int argc = lua_gettop(L);
        if (argc < 2 || !lua_isinteger(L, 1) || !lua_isstring(L, 2))
            return luaL_error(L, "addprocess requires (appid: integer, exename: string)");
        lua_Integer value = lua_tointeger(L, 1);
        if (value <= 0 || value > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "addprocess: appid out of range");
        std::string name(lua_tostring(L, 2));
        for (char& ch : name)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        AppId_t appId = static_cast<AppId_t>(value);
        if (!g_currentFile.empty()) {
            g_fileProcesses[g_currentFile][name] = appId;
            RebuildProcess(name);
        } else {
            ProcessNameAppIdMap[name] = appId;
        }
        return 0;
    }

    static int lua_forcedenuvo(lua_State* L) {
        // forcedenuvo(appid) — bypass ProtectionScan for games where the heuristic fails.
        if (lua_gettop(L) < 1 || !lua_isinteger(L, 1))
            return luaL_error(L, "forcedenuvo requires (appid: integer)");
        lua_Integer value = lua_tointeger(L, 1);
        if (value <= 0 || value > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "forcedenuvo: appid out of range");
        AppId_t appId = static_cast<AppId_t>(value);
        if (!g_currentFile.empty()) {
            if (g_fileForcedDenuvo[g_currentFile].insert(appId).second) {
                if (++g_forcedDenuvoRefCount[appId] == 1) {
                    ForcedDenuvoSet.insert(appId);
                }
            }
        } else {
            ForcedDenuvoSet.insert(appId);
        }
        return 0;
    }

    static int lua_nodenuvo(lua_State* L) {
        // nodenuvo(appid) — explicitly mark as non-Denuvo, bypassing ProtectionScan.
        if (lua_gettop(L) < 1 || !lua_isinteger(L, 1))
            return luaL_error(L, "nodenuvo requires (appid: integer)");
        lua_Integer value = lua_tointeger(L, 1);
        if (value <= 0 || value > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "nodenuvo: appid out of range");
        AppId_t appId = static_cast<AppId_t>(value);
        if (!g_currentFile.empty()) {
            if (g_fileNoDenuvo[g_currentFile].insert(appId).second) {
                if (++g_noDenuvoRefCount[appId] == 1) {
                    NoDenuvoSet.insert(appId);
                }
            }
        } else {
            NoDenuvoSet.insert(appId);
        }
        return 0;
    }

    static int lua_seteticketurl(lua_State* L) {
        // seteticketurl("http://your-backend/eticket")
        // Endpoint that mints fresh nonce-bound encrypted app tickets for
        // strict Denuvo titles. Set to "" (or omit the call) to disable.
        if (lua_gettop(L) < 1 || !lua_isstring(L, 1))
            return luaL_error(L, "seteticketurl requires (url: string)");
        std::string url(lua_tostring(L, 1));
        if (!g_currentFile.empty()) {
            g_fileEticketUrl[g_currentFile] = url;
            RebuildEticketUrl();
        } else {
            EticketUrl = url;
        }
        return 0;
    }

    static int lua_pinApp(lua_State* L) {
        // pinApp(integer)
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as appid.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;

        if (!g_currentFile.empty()) {
            if (g_filePinnedApps[g_currentFile].insert(AppId).second) {
                if (++g_pinnedAppsRefCount[AppId] == 1) {
                    PinnedApps.insert(AppId);
                }
            }
        } else {
            PinnedApps.insert(AppId);
        }

        return 0;
    }

    static int lua_setManifestid(lua_State* L)
    {
        // setManifestid(depotId, gid_string [, size])
        // size is always forced to 0 to prevent incorrect size from breaking Steam.
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setManifestid: need depotId, gid");

        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setManifestid: depotId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setManifestid: gid must be decimal string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setManifestid: depotId out of range");

        uint64_t depotId = (uint64_t)(uint32_t)val;
        const char* gidStr = lua_tostring(L, 2);

        uint64_t gid = 0;
        if (!ParseUInt64Decimal(gidStr, &gid))
            return luaL_error(L, "setManifestid: gid must be all digits");

        ManifestOverride override{ gid, 0 };
        if (!g_currentFile.empty()) {
            g_fileManifestOverrides[g_currentFile][depotId] = override;
            RebuildManifestOverride(depotId);
        } else {
            SetActiveManifestOverride(depotId, override);
        }
        return 0;
    }

    // ── Lua: setAppTicket / setETicket ──────────────────────────
    static int lua_setAppticket(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setAppTicket: need appId and hex string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setAppTicket: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setAppTicket: ticket must be hex string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setAppTicket: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        size_t hexLen;
        const char* hex = lua_tolstring(L, 2, &hexLen);
        const auto binary = ParseHexStringToBytes(hex, hexLen);

        if (!AppTicket::WriteAppOwnershipTicket(appId, binary))
            return luaL_error(L, "setAppTicket: failed to write credential store");

        if (!g_currentFile.empty()) {
            if (g_fileAppTickets[g_currentFile].insert(appId).second) {
                ++g_appTicketRefCount[appId];
            }
        }

        return 0;
    }

    static int lua_setEticket(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setETicket: need appId and hex string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setETicket: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setETicket: ticket must be hex string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setETicket: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        size_t hexLen;
        const char* hex = lua_tolstring(L, 2, &hexLen);
        const auto binary = ParseHexStringToBytes(hex, hexLen);

        if (!AppTicket::WriteEncryptedTicket(appId, binary))
            return luaL_error(L, "setETicket: failed to write credential store");

        if (!g_currentFile.empty()) {
            if (g_fileETickets[g_currentFile].insert(appId).second) {
                ++g_eTicketRefCount[appId];
            }
        }

        return 0;
    }

    
    // ── Lua: setStat ────────────────────────────────────────────
    static int lua_setStat(lua_State* L) {
        // setStat(appid, "steamid")
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setStat: need appId and steamId string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setStat: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setStat: steamId must be string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setStat: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        const char* sidStr = lua_tostring(L, 2);
        uint64_t steamId = 0;
        if (!ParseUInt64Decimal(sidStr, &steamId))
            return luaL_error(L, "setStat: steamId must be all digits");

        if (!g_currentFile.empty()) {
            g_fileStats[g_currentFile][appId] = steamId;
            RebuildStatSteamId(appId);
        } else {
            StatSteamIdSet[appId] = steamId;
        }
        return 0;
    }

    // ── init / cleanup ───────────────────────────────────────────
    static bool Initialize() {
        std::lock_guard lock(g_luaStateMutex);
        if (g_lua_state)
            return true;
        g_lua_state = luaL_newstate();
        if (!g_lua_state)
            return false;
        // Load standard Lua libraries.
        luaL_openlibs(g_lua_state);

        // Set up case-insensitive global lookup via __index on _G's metatable.
        lua_getglobal(g_lua_state, "_G");
        if (!lua_getmetatable(g_lua_state, -1)) {
            lua_newtable(g_lua_state);
        }
        lua_pushcfunction(g_lua_state, case_insensitive_global_index);
        lua_setfield(g_lua_state, -2, "__index");
        lua_setmetatable(g_lua_state, -2);
        lua_pop(g_lua_state, 1);  // pop _G

        // Register custom helper functions for scripts.
        // All functions use register_func() so any case variant works
        // (e.g. setAppTICKET, addAppId, SETManifestid, etc.).
        register_func(g_lua_state, "addappid", lua_addappid);
        register_func(g_lua_state, "addtoken", lua_addtoken);
        register_func(g_lua_state, "addprocess", lua_addprocess);
        register_func(g_lua_state, "forcedenuvo", lua_forcedenuvo);
        register_func(g_lua_state, "nodenuvo", lua_nodenuvo);
        register_func(g_lua_state, "disallowdenuvo", lua_nodenuvo);
        register_func(g_lua_state, "seteticketurl", lua_seteticketurl);
        // we don't need it?
        // register_func(g_lua_state, "pinapp", lua_pinApp);
        register_func(g_lua_state, "setmanifestid", lua_setManifestid);
        register_func(g_lua_state, "http_get", lua_http_get);
        register_func(g_lua_state, "http_post", lua_http_post);
        register_func(g_lua_state, "setappticket", lua_setAppticket);
        register_func(g_lua_state, "seteticket", lua_setEticket);
        register_func(g_lua_state, "setstat", lua_setStat);
        return true;
    }

    static void Cleanup() {
        std::lock_guard lock(g_luaStateMutex);
        if (g_lua_state) {
            lua_close(g_lua_state);
            g_lua_state = nullptr;
        }
        g_hasManifestCodeFunc = false;
        g_hasManifestCodeFuncEx = false;
    }

    // ── public query API ─────────────────────────────────────────
    AppId_t GetAppIdForProcess(const std::string& imageName) {
        std::string lower(imageName);
        for (char& ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::shared_lock lock(g_configSharedMutex);
        const auto it = ProcessNameAppIdMap.find(lower);
        return it != ProcessNameAppIdMap.end() ? it->second : k_uAppIdInvalid;
    }

    bool IsForcedDenuvo(AppId_t appId) {
        std::shared_lock lock(g_configSharedMutex);
        return ForcedDenuvoSet.count(appId) > 0;
    }

    bool IsNoDenuvo(AppId_t appId) {
        std::shared_lock lock(g_configSharedMutex);
        return NoDenuvoSet.count(appId) > 0;
    }

    std::string GetEticketUrl() {
        std::shared_lock lock(g_configSharedMutex);
        return EticketUrl;
    }

    bool HasDepot(AppId_t DepotId, bool excludeOwned) {
        std::shared_lock lock(g_configSharedMutex);
        return DepotKeySet.count(DepotId) && (!excludeOwned || !OwnedAppIdSet.count(DepotId));
    }

    bool IsOwned(AppId_t AppId) {
        std::shared_lock lock(g_configSharedMutex);
        return OwnedAppIdSet.count(AppId) > 0;
    }

    void MarkOwned(AppId_t AppId) {
        std::unique_lock lock(g_configSharedMutex);
        if (OwnedAppIdSet.insert(AppId).second) {
            LOG_PACKAGE_INFO("Marking app {} as owned", AppId);
        }
    }

    std::vector<AppId_t> GetAllDepotIds() {
        std::shared_lock lock(g_configSharedMutex);
        std::vector<AppId_t> DepotIds;
        DepotIds.reserve(DepotKeySet.size());
        for (const auto& pair : DepotKeySet) {
            DepotIds.push_back(pair.first);
        }
        return DepotIds;
    }

    std::vector<uint8> GetDecryptionKey(AppId_t DepotId) {
        std::string keyCopy;
        {
            std::shared_lock lock(g_configSharedMutex);
            auto it = DepotKeySet.find(DepotId);
            if (it != DepotKeySet.end()) {
                keyCopy = it->second;
            }
        }
        if (!keyCopy.empty()) {
            return ParseHexStringToBytes(keyCopy.data(), keyCopy.size());
        }
        return {};
    }

    uint64_t GetAccessToken(AppId_t AppId) {
        std::shared_lock lock(g_configSharedMutex);
        auto it = AccessTokenSet.find(AppId);
        return it != AccessTokenSet.end() ? it->second : 0;
    }

    bool pinApp(AppId_t AppId) {
        std::shared_lock lock(g_configSharedMutex);
        return PinnedApps.count(AppId) > 0;
    }

    uint64_t GetStatSteamId(AppId_t AppId) {
        {
            std::shared_lock lock(g_configSharedMutex);
            auto it = StatSteamIdSet.find(AppId);
            if (it != StatSteamIdSet.end())
                return it->second;
        }
        uint64_t apiSteamId = 0;
        if (StatsClient::FetchStatSteamId(AppId, &apiSteamId))
            return apiSteamId;
        return kDefaultStatSteamId;
    }

    uint32_t GetPurchaseTime(AppId_t AppId) {
        std::shared_lock lock(g_configSharedMutex);
        auto it = g_purchaseTime.find(AppId);
        return it != g_purchaseTime.end() ? it->second : 0;
    }

    std::unordered_map<uint64_t, ManifestOverride> GetManifestOverrides() {
        std::shared_lock lock(g_configSharedMutex);
        return ManifestOverrides;
    }

    bool HasManifestCodeFunc() {
        return g_hasManifestCodeFunc.load(std::memory_order_relaxed);
    }

    bool CallManifestFetchCode(uint64_t gid, uint64_t* outCode) {
        if (!g_hasManifestCodeFunc.load(std::memory_order_relaxed))
            return false;

        std::lock_guard lock(g_luaStateMutex);
        if (!g_lua_state)
            return false;

        lua_getglobal(g_lua_state, "fetch_manifest_code");
        lua_pushinteger(g_lua_state, static_cast<lua_Integer>(gid));

        if (lua_pcall(g_lua_state, 1, 1, 0) != LUA_OK) {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) error: {}", gid,
                             lua_tostring(g_lua_state, -1));
            lua_pop(g_lua_state, 1);
            return false;
        }

        // nil → fallback to config url (normal, not an error)
        if (lua_isnil(g_lua_state, -1)) {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) returned nil", gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        // The function must return a digit string (uint64 as decimal).
        // tonumber() loses precision for values > 2^53; string is safe.
        if (lua_isinteger(g_lua_state, -1)) {
            *outCode = static_cast<uint64_t>(lua_tointeger(g_lua_state, -1));
        } else if (lua_isstring(g_lua_state, -1)) {
            const char* s = lua_tostring(g_lua_state, -1);
            uint64_t parsed = 0;
            if (!ParseUInt64Decimal(s, &parsed)) {
                LOG_MANIFEST_WARN("fetch_manifest_code({}) returned invalid numeric string '{}'",
                                 gid, s);
                lua_pop(g_lua_state, 1);
                return false;
            }
            *outCode = parsed;
        } else {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) unexpected type (expected digit-string)",
                             gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        LOG_MANIFEST_INFO("fetch_manifest_code({}) = {}", gid, *outCode);
        lua_pop(g_lua_state, 1);
        return true;
    }

    bool HasManifestCodeFuncEx() {
        return g_hasManifestCodeFuncEx.load(std::memory_order_relaxed);
    }

    bool CallManifestFetchCodeEx(uint64_t app_id, uint64_t depot_id, uint64_t gid, uint64_t* outCode) {
        if (!g_hasManifestCodeFuncEx.load(std::memory_order_relaxed))
            return false;

        std::lock_guard lock(g_luaStateMutex);
        if (!g_lua_state)
            return false;

        lua_getglobal(g_lua_state, "fetch_manifest_code_ex");
        lua_pushinteger(g_lua_state, static_cast<lua_Integer>(app_id));
        lua_pushinteger(g_lua_state, static_cast<lua_Integer>(depot_id));
        lua_pushinteger(g_lua_state, static_cast<lua_Integer>(gid));

        if (lua_pcall(g_lua_state, 3, 1, 0) != LUA_OK) {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) error: {}", app_id, depot_id, gid,
                             lua_tostring(g_lua_state, -1));
            lua_pop(g_lua_state, 1);
            return false;
        }

        if (lua_isnil(g_lua_state, -1)) {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) returned nil", app_id, depot_id, gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        if (lua_isinteger(g_lua_state, -1)) {
            *outCode = static_cast<uint64_t>(lua_tointeger(g_lua_state, -1));
        } else if (lua_isstring(g_lua_state, -1)) {
            const char* s = lua_tostring(g_lua_state, -1);
            uint64_t parsed = 0;
            if (!ParseUInt64Decimal(s, &parsed)) {
                LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) returned invalid numeric string '{}'",
                                 app_id, depot_id, gid, s);
                lua_pop(g_lua_state, 1);
                return false;
            }
            *outCode = parsed;
        } else {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) unexpected type (expected digit-string)",
                             app_id, depot_id, gid);
            lua_pop(g_lua_state, 1);
            return false;
        }

        LOG_MANIFEST_INFO("fetch_manifest_code_ex({}, {}, {}) = {}", app_id, depot_id, gid, *outCode);
        lua_pop(g_lua_state, 1);
        return true;
    }

    // ── per-file unload ────────────────────────────────────────
    // Design note: AppTicket and ETicket are handled purely in memory.
    // - On file reload/re-parse (ParseFile) or unload: when ticket refcount reaches 0,
    //   in-memory tickets are cleared immediately (commenting out a ticket removes it naturally).
    // - On explicit file deletion (LuaFileWatcher delete event): `isPermanentRemoval` is true,
    //   triggering physical deletion of the credentials directory (including SteamID.txt)
    //   once all referencing lua files and depots drop to 0.
    static void UnloadFileLocked(const std::string& rawFilePath, bool isPermanentRemoval) {
        std::string filePath = OSTPlatform::Encoding::PathToUtf8(
            OSTPlatform::Encoding::PathFromUtf8(rawFilePath).lexically_normal());
        auto depotsIt = g_fileDepots.find(filePath);
        auto manifestIt = g_fileManifestOverrides.find(filePath);
        auto appTicketIt = g_fileAppTickets.find(filePath);
        auto eTicketIt = g_fileETickets.find(filePath);
        auto tokenIt = g_fileTokens.find(filePath);
        auto procIt = g_fileProcesses.find(filePath);
        auto forcedIt = g_fileForcedDenuvo.find(filePath);
        auto noDenuvoIt = g_fileNoDenuvo.find(filePath);
        auto pinnedIt = g_filePinnedApps.find(filePath);
        auto statIt = g_fileStats.find(filePath);
        auto eticketUrlIt = g_fileEticketUrl.find(filePath);

        if (depotsIt == g_fileDepots.end() && manifestIt == g_fileManifestOverrides.end() &&
            appTicketIt == g_fileAppTickets.end() && eTicketIt == g_fileETickets.end() &&
            tokenIt == g_fileTokens.end() &&
            procIt == g_fileProcesses.end() && forcedIt == g_fileForcedDenuvo.end() &&
            noDenuvoIt == g_fileNoDenuvo.end() && pinnedIt == g_filePinnedApps.end() &&
            statIt == g_fileStats.end() && eticketUrlIt == g_fileEticketUrl.end()) {
            g_fileParseSequence.erase(filePath);
            g_fileMtime.erase(filePath);
            return;
        }

        if (depotsIt != g_fileDepots.end()) {
            for (AppId_t id : depotsIt->second) {
                LOG_PACKAGE_DEBUG("UnloadFile:Ref count for AppId {} is {}", id, g_depotRefCount[id]);
                if (--g_depotRefCount[id] == 0) {
                    g_depotRefCount.erase(id);
                    DepotKeySet.erase(id);
                    g_purchaseTime.erase(id);
                    g_pendingRemovals.push_back(id);
                    if (isPermanentRemoval && !g_appTicketRefCount.contains(id) && !g_eTicketRefCount.contains(id)) {
                        AppTicket::RemoveCredentials(id);
                    }
                }
            }

            LOG_PACKAGE_INFO("UnloadFile: removed {} depots from {}", depotsIt->second.size(), filePath);
            g_fileDepots.erase(depotsIt);
        }

        if (appTicketIt != g_fileAppTickets.end()) {
            for (AppId_t id : appTicketIt->second) {
                auto refIt = g_appTicketRefCount.find(id);
                if (refIt != g_appTicketRefCount.end()) {
                    if (--refIt->second == 0) {
                        g_appTicketRefCount.erase(refIt);
                        AppTicket::RemoveAppOwnershipTicket(id);
                        if (isPermanentRemoval && !g_depotRefCount.contains(id) && !g_eTicketRefCount.contains(id)) {
                            AppTicket::RemoveCredentials(id);
                        }
                    }
                }
            }
            g_fileAppTickets.erase(appTicketIt);
        }

        if (eTicketIt != g_fileETickets.end()) {
            for (AppId_t id : eTicketIt->second) {
                auto refIt = g_eTicketRefCount.find(id);
                if (refIt != g_eTicketRefCount.end()) {
                    if (--refIt->second == 0) {
                        g_eTicketRefCount.erase(refIt);
                        AppTicket::RemoveEncryptedTicket(id);
                        if (isPermanentRemoval && !g_depotRefCount.contains(id) && !g_appTicketRefCount.contains(id)) {
                            AppTicket::RemoveCredentials(id);
                        }
                    }
                }
            }
            g_fileETickets.erase(eTicketIt);
        }

        if (manifestIt != g_fileManifestOverrides.end()) {
            std::vector<uint64_t> affectedDepots;
            affectedDepots.reserve(manifestIt->second.size());
            for (const auto& [depotId, _] : manifestIt->second) {
                affectedDepots.push_back(depotId);
            }

            g_fileManifestOverrides.erase(manifestIt);
            for (uint64_t depotId : affectedDepots) {
                RebuildManifestOverride(depotId);
            }

            LOG_MANIFEST_INFO("UnloadFile: removed {} manifest override(s) from {}", affectedDepots.size(), filePath);
        }

        if (tokenIt != g_fileTokens.end()) {
            std::vector<AppId_t> affectedTokens;
            affectedTokens.reserve(tokenIt->second.size());
            for (const auto& [appId, _] : tokenIt->second) {
                affectedTokens.push_back(appId);
            }
            g_fileTokens.erase(tokenIt);
            for (AppId_t appId : affectedTokens) {
                RebuildAccessToken(appId);
            }
        }

        if (procIt != g_fileProcesses.end()) {
            std::vector<std::string> affectedProcs;
            affectedProcs.reserve(procIt->second.size());
            for (const auto& [procName, _] : procIt->second) {
                affectedProcs.push_back(procName);
            }
            g_fileProcesses.erase(procIt);
            for (const auto& procName : affectedProcs) {
                RebuildProcess(procName);
            }
        }

        if (forcedIt != g_fileForcedDenuvo.end()) {
            for (AppId_t appId : forcedIt->second) {
                auto refIt = g_forcedDenuvoRefCount.find(appId);
                if (refIt != g_forcedDenuvoRefCount.end()) {
                    if (--refIt->second == 0) {
                        g_forcedDenuvoRefCount.erase(refIt);
                        ForcedDenuvoSet.erase(appId);
                    }
                }
            }
            g_fileForcedDenuvo.erase(forcedIt);
        }

        if (noDenuvoIt != g_fileNoDenuvo.end()) {
            for (AppId_t appId : noDenuvoIt->second) {
                auto refIt = g_noDenuvoRefCount.find(appId);
                if (refIt != g_noDenuvoRefCount.end()) {
                    if (--refIt->second == 0) {
                        g_noDenuvoRefCount.erase(refIt);
                        NoDenuvoSet.erase(appId);
                    }
                }
            }
            g_fileNoDenuvo.erase(noDenuvoIt);
        }

        if (pinnedIt != g_filePinnedApps.end()) {
            for (AppId_t appId : pinnedIt->second) {
                auto refIt = g_pinnedAppsRefCount.find(appId);
                if (refIt != g_pinnedAppsRefCount.end()) {
                    if (--refIt->second == 0) {
                        g_pinnedAppsRefCount.erase(refIt);
                        PinnedApps.erase(appId);
                    }
                }
            }
            g_filePinnedApps.erase(pinnedIt);
        }

        if (statIt != g_fileStats.end()) {
            std::vector<AppId_t> affectedStats;
            affectedStats.reserve(statIt->second.size());
            for (const auto& [appId, _] : statIt->second) {
                affectedStats.push_back(appId);
            }
            g_fileStats.erase(statIt);
            for (AppId_t appId : affectedStats) {
                RebuildStatSteamId(appId);
            }
        }

        if (eticketUrlIt != g_fileEticketUrl.end()) {
            g_fileEticketUrl.erase(eticketUrlIt);
            RebuildEticketUrl();
        }

        g_fileParseSequence.erase(filePath);
        g_fileMtime.erase(filePath);
    }

    void UnloadFile(const std::string& rawFilePath, bool isPermanentRemoval) {
        std::unique_lock lock(g_configSharedMutex);
        UnloadFileLocked(rawFilePath, isPermanentRemoval);
    }

    static bool StartsWithCaseInsensitive(std::string_view str, std::string_view prefix) {
        if (prefix.empty()) return true;
        if (str.empty()) return false;
        std::wstring wideStr = OSTPlatform::Encoding::Utf8ToWide(str);
        std::wstring widePrefix = OSTPlatform::Encoding::Utf8ToWide(prefix);
        if (wideStr.size() < widePrefix.size()) return false;
        return _wcsnicmp(wideStr.c_str(), widePrefix.c_str(), widePrefix.size()) == 0;
    }

    uint32_t UnloadDirectory(const std::string& rawDirPath) {
        std::unique_lock lock(g_configSharedMutex);
        std::string dirPath = OSTPlatform::Encoding::PathToUtf8(
            OSTPlatform::Encoding::PathFromUtf8(rawDirPath).lexically_normal());
        if (dirPath.empty()) return 0;
        if (dirPath.back() != '\\' && dirPath.back() != '/') {
            dirPath += '\\';
        }

        std::vector<std::string> toUnload;
        for (const auto& [filePath, _] : g_fileDepots) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileManifestOverrides) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileAppTickets) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileETickets) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileTokens) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileProcesses) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileForcedDenuvo) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileNoDenuvo) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_filePinnedApps) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileStats) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileEticketUrl) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }
        for (const auto& [filePath, _] : g_fileParseSequence) {
            if (StartsWithCaseInsensitive(filePath, dirPath)) {
                toUnload.push_back(filePath);
            }
        }

        std::sort(toUnload.begin(), toUnload.end());
        toUnload.erase(std::unique(toUnload.begin(), toUnload.end()), toUnload.end());

        for (const auto& filePath : toUnload) {
            LOG_PACKAGE_INFO("UnloadDirectory: unloading file '{}' from removed dir '{}'", filePath, rawDirPath);
            UnloadFileLocked(filePath, true);
        }
        return static_cast<uint32_t>(toUnload.size());
    }

    std::vector<AppId_t> TakePendingRemovals() {
        std::unique_lock lock(g_configSharedMutex);
        std::vector<AppId_t> result;
        result.swap(g_pendingRemovals);
        return result;
    }

    std::vector<AppId_t> TakePendingAdditions() {
        std::unique_lock lock(g_configSharedMutex);
        std::vector<AppId_t> result;
        result.swap(g_pendingAdditions);
        return result;
    }

    std::string GetSteamDepotcacheDir() {
        if (SteamInstallPath[0] == '\0') {
            return {};
        }
        return OSTPlatform::Encoding::PathToUtf8(
            (OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / "depotcache").lexically_normal());
    }

    uint32_t SyncManifests(const std::string& directory, const std::string& targetDepotcacheDir) {
        std::lock_guard<std::recursive_mutex> lock(g_manifestSyncMutex);

        std::string depotcache = !targetDepotcacheDir.empty() ? targetDepotcacheDir : GetSteamDepotcacheDir();
        if (depotcache.empty()) {
            LOG_MANIFEST_WARN("SyncManifests: Steam depotcache directory could not be resolved");
            return 0;
        }

        std::error_code ec;
        auto dirPath = OSTPlatform::Encoding::PathFromUtf8(directory);
        auto depotcachePath = OSTPlatform::Encoding::PathFromUtf8(depotcache);

        if (!std::filesystem::exists(dirPath, ec) || !std::filesystem::is_directory(dirPath, ec))
            return 0;

        if (!std::filesystem::exists(depotcachePath, ec)) {
            std::filesystem::create_directories(depotcachePath, ec);
            if (ec) {
                LOG_MANIFEST_WARN("SyncManifests: failed to create depotcache dir '{}' ({})",
                                  OSTPlatform::Encoding::PathToUtf8(depotcachePath), ec.message());
                return 0;
            }
        }

        uint32_t copiedCount = 0;
        uint32_t skippedCount = 0;

        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     dirPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;

                // Skip zero-byte/incomplete source manifests
                if (entry.file_size(ec) == 0) continue;

                std::string ext = OSTPlatform::Encoding::PathToUtf8(entry.path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                });
                if (ext != ".manifest") continue;

                std::filesystem::path destPath = depotcachePath / entry.path().filename();

                // Skip if destination already exists and is non-empty
                if (std::filesystem::exists(destPath, ec) && std::filesystem::file_size(destPath, ec) > 0) {
                    if (std::filesystem::equivalent(entry.path(), destPath, ec)) {
                        continue;
                    }
                    LOG_MANIFEST_DEBUG("SyncManifests: skipped existing manifest '{}'",
                                       OSTPlatform::Encoding::PathToUtf8(destPath.filename()));
                    ++skippedCount;
                    continue;
                }

                // Retry on temporary file sharing locks (e.g. while being extracted)
                bool copied = false;
                constexpr int kMaxRetries = 3;
                for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
                    ec.clear();
                    if (std::filesystem::copy_file(entry.path(), destPath, std::filesystem::copy_options::overwrite_existing, ec)) {
                        copied = true;
                        break;
                    }
                    if (attempt < kMaxRetries &&
                        (ec.value() == ERROR_SHARING_VIOLATION || ec.value() == ERROR_ACCESS_DENIED)) {
                        Sleep(50);
                    }
                }

                if (copied) {
                    LOG_MANIFEST_INFO("SyncManifests: copied manifest '{}' -> '{}'",
                                      OSTPlatform::Encoding::PathToUtf8(entry.path().filename()),
                                      OSTPlatform::Encoding::PathToUtf8(destPath));
                    ++copiedCount;
                } else {
                    LOG_MANIFEST_WARN("SyncManifests: failed to copy manifest '{}' -> '{}' ({})",
                                      OSTPlatform::Encoding::PathToUtf8(entry.path().filename()),
                                      OSTPlatform::Encoding::PathToUtf8(destPath), ec.message());
                }
            }
        } catch (const std::exception& ex) {
            LOG_MANIFEST_WARN("SyncManifests exception: {}", ex.what());
        }

        if (copiedCount > 0 || skippedCount > 0) {
            LOG_MANIFEST_INFO("SyncManifests: {} manifest(s) copied, {} duplicate(s) skipped from '{}' to '{}'",
                              copiedCount, skippedCount, directory, depotcache);
        }

        return copiedCount;
    }

    bool CopyManifestToDepotcache(const std::string& manifestFilePath, const std::string& targetDepotcacheDir) {
        std::lock_guard<std::recursive_mutex> lock(g_manifestSyncMutex);

        std::string depotcache = !targetDepotcacheDir.empty() ? targetDepotcacheDir : GetSteamDepotcacheDir();
        if (depotcache.empty()) return false;

        std::error_code ec;
        std::filesystem::path src = OSTPlatform::Encoding::PathFromUtf8(manifestFilePath);
        if (!std::filesystem::exists(src, ec) || !std::filesystem::is_regular_file(src, ec))
            return false;

        // Skip zero-byte/incomplete source manifests
        if (std::filesystem::file_size(src, ec) == 0) return false;

        std::string ext = OSTPlatform::Encoding::PathToUtf8(src.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        if (ext != ".manifest") return false;

        std::filesystem::path depotcachePath = OSTPlatform::Encoding::PathFromUtf8(depotcache);
        if (!std::filesystem::exists(depotcachePath, ec)) {
            std::filesystem::create_directories(depotcachePath, ec);
            if (ec) return false;
        }

        std::filesystem::path dest = depotcachePath / src.filename();

        // Skip if destination already exists and is non-empty
        if (std::filesystem::exists(dest, ec) && std::filesystem::file_size(dest, ec) > 0) {
            if (std::filesystem::equivalent(src, dest, ec)) {
                return false;
            }
            LOG_MANIFEST_DEBUG("CopyManifestToDepotcache: skipped existing manifest '{}'",
                               OSTPlatform::Encoding::PathToUtf8(dest.filename()));
            return false;
        }

        // Retry on temporary file sharing locks
        constexpr int kMaxRetries = 3;
        for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
            ec.clear();
            if (std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec)) {
                LOG_MANIFEST_INFO("CopyManifestToDepotcache: copied manifest '{}' -> '{}'",
                                  OSTPlatform::Encoding::PathToUtf8(src.filename()),
                                  OSTPlatform::Encoding::PathToUtf8(dest));
                return true;
            }
            if (attempt < kMaxRetries &&
                (ec.value() == ERROR_SHARING_VIOLATION || ec.value() == ERROR_ACCESS_DENIED)) {
                Sleep(50);
            }
        }

        LOG_MANIFEST_WARN("CopyManifestToDepotcache: failed to copy manifest '{}' ({})",
                          OSTPlatform::Encoding::PathToUtf8(src.filename()), ec.message());
        return false;
    }

    static std::vector<std::string> CollectLuaFiles(const std::string& directory) {
        std::vector<std::string> files;

        std::error_code ec;
        std::filesystem::path dirPath = OSTPlatform::Encoding::PathFromUtf8(directory);
        if (!std::filesystem::exists(dirPath, ec) || !std::filesystem::is_directory(dirPath, ec))
            return files;

        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     dirPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;

                std::string ext = OSTPlatform::Encoding::PathToUtf8(entry.path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                });
                if (ext != ".lua") continue;

                files.push_back(OSTPlatform::Encoding::PathToUtf8(entry.path().lexically_normal()));
            }
        } catch (const std::exception& ex) {
            LOG_PACKAGE_WARN("CollectLuaFiles exception: {}", ex.what());
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    // ── single-file parser ──────────────────────────────────────
    void ParseFile(const std::string& rawFilePath) {
        if (!Initialize()) return;

        std::filesystem::path path = OSTPlatform::Encoding::PathFromUtf8(rawFilePath).lexically_normal();
        std::string filePath = OSTPlatform::Encoding::PathToUtf8(path);
        std::string filenameUtf8 = OSTPlatform::Encoding::PathToUtf8(path.filename());

        std::ifstream file(path);
        if (!file) {
            LOG_WARN("ParseFile: failed to open {}", filenameUtf8);
            return;
        }

        // Capture the file's last-modified time (unix epoch, seconds) so
        // lua_addappid can stamp it onto every appId this file contributes.
        // Portable conversion that does not require C++20 clock_cast.
        uint32_t mtime = 0;
        {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(path, ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
                mtime = static_cast<uint32_t>(std::chrono::system_clock::to_time_t(sctp));
            }
        }

        std::unique_lock configLock(g_configSharedMutex);
        std::lock_guard luaLock(g_luaStateMutex);

        // Remove old entries from this file before re-parsing.
        UnloadFileLocked(filePath, false);
        g_currentFile = filePath;
        g_fileParseSequence[filePath] = ++g_nextFileParseSequence;
        g_fileMtime[filePath] = mtime;

        try {
            std::string chunk, line;
            int lineNo = 0;
            while (std::getline(file, line)) {
                ++lineNo;
                // Strip UTF-8 BOM if present on the first line
                if (lineNo == 1 && line.size() >= 3 &&
                    static_cast<unsigned char>(line[0]) == 0xEF &&
                    static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF) {
                    line.erase(0, 3);
                }
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!chunk.empty()) chunk += '\n';
                chunk += line;

                lua_settop(g_lua_state, 0);
                int rc = luaL_loadstring(g_lua_state, chunk.c_str());
                if (rc == LUA_OK) {
                    if (lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
                        const char* err = lua_tostring(g_lua_state, -1);
                        LOG_WARN("{}:{}: {}", filenameUtf8, lineNo,
                                 err ? err : "unknown");
                    }
                    chunk.clear();
                } else if (rc == LUA_ERRSYNTAX) {
                    const char* err = lua_tostring(g_lua_state, -1);
                    if (err && strstr(err, "<eof>") != nullptr) {
                        // Incomplete multi-line statement: keep accumulating
                        lua_pop(g_lua_state, 1);
                    } else {
                        // Genuine syntax error on this statement: log and discard bad chunk
                        // so following valid statements in the file are not dropped.
                        LOG_WARN("{}:{}: {}", filenameUtf8, lineNo, err ? err : "syntax error");
                        lua_pop(g_lua_state, 1);
                        chunk.clear();
                    }
                } else {
                    const char* err = lua_tostring(g_lua_state, -1);
                    LOG_WARN("{}:{}: {}", filenameUtf8, lineNo, err ? err : "unknown");
                    lua_pop(g_lua_state, 1);
                    chunk.clear();
                }
            }
            if (!chunk.empty()) {
                LOG_WARN("{}: incomplete statement at end of file", filenameUtf8);
            }
        } catch (const std::exception& ex) {
            LOG_WARN("ParseFile exception in {}: {}", filenameUtf8, ex.what());
        }

        // Check for manifest code functions after parsing.
        bool hasCode = false;
        lua_getglobal(g_lua_state, "fetch_manifest_code");
        if (lua_isfunction(g_lua_state, -1)) {
            hasCode = true;
            LOG_INFO("manifest.lua: fetch_manifest_code found");
        }
        lua_pop(g_lua_state, 1);
        g_hasManifestCodeFunc.store(hasCode, std::memory_order_relaxed);

        bool hasCodeEx = false;
        lua_getglobal(g_lua_state, "fetch_manifest_code_ex");
        if (lua_isfunction(g_lua_state, -1)) {
            hasCodeEx = true;
            LOG_INFO("manifest.lua: fetch_manifest_code_ex found");
        }
        lua_pop(g_lua_state, 1);
        g_hasManifestCodeFuncEx.store(hasCodeEx, std::memory_order_relaxed);

        g_currentFile.clear();
    }

    // ── directory scanner ────────────────────────────────────────
    void ParseDirectory(const std::string& directory) {
        if (!Initialize()) return;

        SyncManifests(directory);

        for (const auto& filePath : CollectLuaFiles(directory)) {
            ParseFile(filePath);
        }

        // Initial parse — discard pending additions so NotifyLicenseChanged
        // only sees changes that happen after startup.
        std::unique_lock lock(g_configSharedMutex);
        g_pendingAdditions.clear();
    }

    void ReloadDirectories(const std::vector<std::string>& directories, bool clearPendingAdditions) {
        if (!Initialize()) return;

        for (const auto& directory : directories) {
            SyncManifests(directory);
        }

        std::unordered_set<std::string> activeFiles;
        std::vector<std::string> orderedFiles;

        for (const auto& directory : directories) {
            for (const auto& filePath : CollectLuaFiles(directory)) {
                if (activeFiles.insert(filePath).second) {
                    orderedFiles.push_back(filePath);
                }
            }
        }

        std::vector<std::string> trackedFiles;
        {
            std::shared_lock lock(g_configSharedMutex);
            std::unordered_set<std::string> trackedSet;
            auto rememberTracked = [&](const std::string& filePath) {
                if (trackedSet.insert(filePath).second) {
                    trackedFiles.push_back(filePath);
                }
            };

            for (const auto& [filePath, _] : g_fileDepots) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileManifestOverrides) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileAppTickets) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileETickets) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileTokens) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileProcesses) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileForcedDenuvo) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileNoDenuvo) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_filePinnedApps) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileStats) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileEticketUrl) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileParseSequence) {
                rememberTracked(filePath);
            }
        }

        for (const auto& filePath : trackedFiles) {
            if (!activeFiles.contains(filePath)) {
                UnloadFile(filePath, true);
            }
        }

        for (const auto& filePath : orderedFiles) {
            ParseFile(filePath);
        }

        if (clearPendingAdditions) {
            std::unique_lock lock(g_configSharedMutex);
            g_pendingAdditions.clear();
        }
    }

}
