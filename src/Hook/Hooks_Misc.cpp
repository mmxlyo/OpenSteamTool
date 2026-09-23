#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "dllmain.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {
    // ── Resolve-only functions ─────────────────────────────────────
    RESOLVE_FUNC(CUtlBufferEnsureCapacity, void*, CUtlBuffer* pCUtlBuffer, uint32 newCapacity);

    // ── VEH-captured functions (one-shot int3) ───────────────────────────────
    // On int3 hit, ctx->Rcx is stored to the named output variable.
    CAPTURE_THIS_FUNC(GetAppIDForCurrentPipe, AppId_t,      g_steamEngine,    void*);
    CAPTURE_THIS_FUNC(GetAppDataFromAppInfo,  int64,        g_pCAppInfoCache, void*, AppId_t, const char*, uint8*, int32);

    // Assumes one game at a time.  Set by SpawnProcess VEH when -onlinefix
    // is detected; cleared when a non-onlinefix game launches.
    std::atomic<AppId_t> g_OnlineFixRealAppId{0};
    // True once the game starts SteamNetworkingSockets P2P (see GetAppID handler).
    std::atomic<bool>    g_NetworkingSocketsActive{false};
    // If true, suppress flipping GetAppID to 480.
    // By default in -onlinefix, we keep real AppID so games can read their saves,
    // access user data, and authenticate properly.
    // Flipped to false only if user explicitly passes -p2pflip.
    std::atomic<bool>    g_SuppressAppIdFlip{true};
    std::mutex           g_GameNameMutex;
    std::unordered_map<AppId_t, std::string> g_GameNameCache;


    static bool HasCmdLineArg(const char* cmdLine, const char* arg) {
        if (!cmdLine || !arg) return false;
        const size_t argLen = strlen(arg);
        for (const char* p = cmdLine; *p; ++p) {
            if (_strnicmp(p, arg, argLen) == 0) {
                if (p == cmdLine || *(p - 1) == ' ' || *(p - 1) == '\t' || *(p - 1) == '"') {
                    const char next = p[argLen];
                    if (next == '\0' || next == ' ' || next == '\t' || next == '"' || next == '=') {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // ── SpawnProcess interception ────────────────────────────────────────────
    // CUser_SpawnProcess(pCUser, pExePath, pCommandLine, pWorkingDir,
    //                    pGameID, ...)
    // arg1=pCUser, arg2=pExePath, arg3=pCommandLine, arg4=pWorkingDir
    // arg5=pGameID (CGameID*; low 24 bits = AppId)
    static void OnSpawnProcessHit(OSTPlatform::Trap::Context& ctx, const VehCommon::Int3Site& /*site*/) {
        CGameID* pGameID = VehCommon::GetArg<CGameID*>(ctx, 5);
        if (!pGameID) return;
        AppId_t appId = static_cast<AppId_t>(pGameID->AppID(true));
        const char* cmdLine = VehCommon::GetArg<const char*>(ctx, 3);

        if (cmdLine && HasCmdLineArg(cmdLine, "-onlinefix"))
        {
            // By default, keep real AppID so games can find their save files and authenticate.
            // Only flip GetAppID to 480 if user explicitly passes -p2pflip.
            const bool explicitFlip = HasCmdLineArg(cmdLine, "-p2pflip");
            const bool suppress = !explicitFlip;
            g_OnlineFixRealAppId.store(appId, std::memory_order_release);
            g_NetworkingSocketsActive.store(false, std::memory_order_release);
            g_SuppressAppIdFlip.store(suppress, std::memory_order_release);
            pGameID->SetAppID(kOnlineFixAppId);
            CloudRedirectHost::AddApp(appId);
            LOG_MISC_INFO("SpawnProcess: appid {} -> {}, suppressFlip={}, cmd=\"{}\"",
                          appId, kOnlineFixAppId, suppress, cmdLine);
        } else {
            g_OnlineFixRealAppId.store(0, std::memory_order_release);
            g_NetworkingSocketsActive.store(false, std::memory_order_release);
            g_SuppressAppIdFlip.store(true, std::memory_order_release);
        }
    }

    // ── SteamController_OptedInMask ──────────────────────────────────────────
    // Called by CUser_BuildSpawnEnvBlock with pGameID's appid to
    // compute EnableConfiguratorSupport and the SDL_* env vars.
    // With 480 the spawned game inherits Spacewar's Steam Input
    // opt-in and gameoverlayrenderer hijacks the XInput stream.
    HOOK_FUNC(OptedInMask, int64,void* pThis, AppId_t appId)
    {
        const AppId_t realAppId = g_OnlineFixRealAppId.load(std::memory_order_relaxed);
        if (appId == kOnlineFixAppId && realAppId != 0) {
            LOG_MISC_INFO("OptedInMask: appid {} -> {}", appId, realAppId);
            appId = realAppId;
        }
        return oOptedInMask(pThis, appId);
    }

    // ── CUser_BuildSpawnEnvBlock ─────────────────────────────────────────────
    // pOverlayCGameID drives SteamOverlayGameId, which the in-game
    // overlay reads for screenshot tags, community URLs, and asset
    // selection.  pCGameID drives SteamGameId / SteamAppId; leave it
    // at 480 so the in-game ownership bypass holds.
    HOOK_FUNC(BuildSpawnEnvBlock, int64,
              void* pThis, CGameID* pCGameID, void* a3, void* env,
              CGameID* pOverlayCGameID, void* a6, int a7,
              void* a8, void* a9, unsigned int a10, char a11)
    {
        const AppId_t realAppId = g_OnlineFixRealAppId.load(std::memory_order_relaxed);
        if (realAppId != 0 && pOverlayCGameID
            && pOverlayCGameID->AppID(true) == kOnlineFixAppId) 
        {
            LOG_MISC_INFO("BuildSpawnEnvBlock: SetAppID in OverlayCGameID {} -> {}",
                          pOverlayCGameID->AppID(true), realAppId);
            pOverlayCGameID->SetAppID(realAppId);
        }
        return oBuildSpawnEnvBlock(pThis, pCGameID, a3, env,
                                    pOverlayCGameID, a6, a7,
                                    a8, a9, a10, a11);
    }

    // ── GetRedirectedAppId ───────────────────────────────────────────────────
    // Queries Valve's appidredirect table in CUserRemoteStorage.
    // When OnlineFix is active and AppID is 480, redirect to real AppID so all
    // quota queries, remotecache.vdf operations, and AutoCloud logic use real AppID.
    HOOK_FUNC(GetRedirectedAppId, AppId_t, AppId_t appId)
    {
        if (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
            const AppId_t realAppId = Hooks_Misc::ResolveAppId();
            if (realAppId != 0) {
                LOG_MISC_DEBUG("GetRedirectedAppId: redirecting AppID {} -> {}", appId, realAppId);
                return realAppId;
            }
        }
        return oGetRedirectedAppId ? oGetRedirectedAppId(appId) : appId;
    }

    // ── RemoteStorage_ResolvePath ────────────────────────────────────────────
    // CUserRemoteStorage::ResolvePath calculates physical on-disk paths:
    // userdata/<SteamID>/<AppID>/remote/<filename> or /local/<filename>.
    // When OnlineFix is active and AppID is 480, redirect to real AppID so
    // file reads and writes go directly to the real AppID's folder.
    HOOK_FUNC(RemoteStorage_ResolvePath, bool,
              void* pThis, AppId_t appId, int32 ePathType,
              const char* pchRelativePath, void* pstrResolvedPath)
    {
        if (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
            const AppId_t realAppId = Hooks_Misc::ResolveAppId();
            if (realAppId != 0) {
                LOG_MISC_DEBUG("RemoteStorage_ResolvePath: appid {} -> {}, ePathType={}, path='{}'",
                              appId, realAppId, ePathType, pchRelativePath ? pchRelativePath : "(null)");
                appId = realAppId;
            }
        }
        return oRemoteStorage_ResolvePath ? oRemoteStorage_ResolvePath(pThis, appId, ePathType, pchRelativePath, pstrResolvedPath) : false;
    }

}

namespace Hooks_Misc {
    void Install() {
        RESOLVE_C(CUtlBufferEnsureCapacity);

        ARM_CAPTURE_C(GetAppIDForCurrentPipe);
        ARM_CAPTURE_C(GetAppDataFromAppInfo);

        ARM_INT3_C(SpawnProcess, true, &OnSpawnProcessHit, nullptr);

        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildSpawnEnvBlock);
        INSTALL_HOOK_C(OptedInMask);
        INSTALL_HOOK_C(GetRedirectedAppId);
        INSTALL_HOOK_C(RemoteStorage_ResolvePath);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildSpawnEnvBlock);
        UNINSTALL_HOOK(OptedInMask);
        UNINSTALL_HOOK(GetRedirectedAppId);
        UNINSTALL_HOOK(RemoteStorage_ResolvePath);
        UNHOOK_END();
    }

    AppId_t GetAppIDForCurrentPipeWrap() {
        if (!CAPTURE_READY(GetAppIDForCurrentPipe)) {
            LOG_MISC_WARN("GetAppIDForCurrentPipeWrap called before capture — returning 0");
            return 0;
        }
        auto appid = oGetAppIDForCurrentPipe(g_steamEngine);
        if (!appid) {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId=0(Not GamePipe)");
        } else {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId={}", appid);
        }
        return appid;
    }

    
    AppId_t ResolveAppId() {
        const AppId_t realAppId = g_OnlineFixRealAppId.load(std::memory_order_relaxed);
        if (realAppId != 0) return realAppId;
        return GetAppIDForCurrentPipeWrap();
    }

    bool IsOnlineFixActive() {
        return g_OnlineFixRealAppId.load(std::memory_order_relaxed) != 0;
    }

    bool IsNetworkingSocketsActive() {
        return g_NetworkingSocketsActive.load(std::memory_order_relaxed);
    }

    void NotifyNetworkingSocketsUsed() {
        if (g_OnlineFixRealAppId.load(std::memory_order_relaxed) != 0) {
            if (!g_NetworkingSocketsActive.exchange(true, std::memory_order_relaxed)) {
                if (g_SuppressAppIdFlip.load(std::memory_order_relaxed)) {
                    LOG_MISC_INFO("NetworkingSockets active: keeping real appid {}", g_OnlineFixRealAppId.load(std::memory_order_relaxed));
                } else {
                    LOG_MISC_INFO("NetworkingSockets active: GetAppID now reports 480 for cert match");
                }
            }
        }
    }

    bool ShouldReportOnlineFixAppId() {
        // The flip exists so a P2P socket's appid matches the 480 session cert,
        // which some titles need (#146). It is blunt though: from the moment it
        // trips, every GetAppID answer is the fake appid for the rest of the
        // process's life. Games that ask Steam for their own appid during later
        // startup then get 480 and misbehave — Bodycam (2406770) black-screens
        // straight after login this way, and How to Fish (4001890) exits with 86.
        //
        // Both behaviours are needed by different games, and the call itself
        // gives no way to tell them apart, so -p2pflip opts in per launch.
        if (g_SuppressAppIdFlip.load(std::memory_order_relaxed)) return false;
        return g_OnlineFixRealAppId.load(std::memory_order_relaxed) != 0 && g_NetworkingSocketsActive.load(std::memory_order_relaxed);
    }
    
    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity, bool updatePut)
    {
        if (!pWrite) return false;
        if (oCUtlBufferEnsureCapacity) {
            LOG_MISC_DEBUG("Before ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            oCUtlBufferEnsureCapacity(pWrite, newCapacity);
            LOG_MISC_DEBUG("After ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            if (updatePut) pWrite->m_Put = newCapacity;
            return true;
        }
        LOG_MISC_WARN("EnsureBufferCapacity: oCUtlBufferEnsureCapacity not resolved");
        return false;
    }

    // ── Game name ────────────────────────────────────────────────
    std::string GetGameNameByAppID(AppId_t appId)
    {
        {
            std::scoped_lock lock(g_GameNameMutex);
            auto it = g_GameNameCache.find(appId);
            if (it != g_GameNameCache.end()) return it->second;
        }

        std::string name;

        if (CAPTURE_READY(GetAppDataFromAppInfo)) {
            char buf[256] = {};
            // "common/name" triggers auto-localization: the function detects
            // prefix "common" (keyType=2) + key "name", then tries
            // "name_localized/<current_lang>" before falling back to "name".
            // Returns strlen+1 on success, -1 on failure.
            int64 len = oGetAppDataFromAppInfo(g_pCAppInfoCache, appId, "common/name",
                reinterpret_cast<uint8*>(buf), sizeof(buf));
            if (len > 1)
                name.assign(buf, static_cast<size_t>(len - 1));
        }

        LOG_MISC_DEBUG("GetGameNameByAppID({}): {}", appId, name);

        // Only cache valid non-empty names so uninitialized/early calls don't poison the cache
        if (!name.empty()) {
            std::scoped_lock lock(g_GameNameMutex);
            if (g_GameNameCache.size() >= 512) {
                g_GameNameCache.clear();
            }
            g_GameNameCache.emplace(appId, name);
        }
        return name;
    }

}
