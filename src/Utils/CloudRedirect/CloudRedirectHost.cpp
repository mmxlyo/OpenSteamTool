#include "CloudRedirectHost.h"
#include "dllmain.h"

#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Hook/Hooks_Misc.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <vector>

namespace CloudRedirectHost {
namespace {

    // --- CloudRedirect third-party client ABI (CloudRedirect/src/common/cr_api.h)
    // Declared locally so OpenSteamTool does not need CloudRedirect's headers.
    using CR_NotifyFn        = void (*)(int level, const char* title, const char* message);
    using CR_InitCloudSave_t = bool (*)(const char* steamPath, CR_NotifyFn notify);
    using CR_HandleCloudRpc_t = bool (*)(const char* method, uint32_t appId, uint32_t accountId,
                                         const uint8_t* reqBody, uint32_t reqLen,
                                         uint8_t* respBuf, uint32_t respMaxLen,
                                         uint32_t* respLen, int32_t* eresult);
    using CR_AddApp_t   = void (*)(uint32_t appId);
    using CR_RemoveApp_t = void (*)(uint32_t appId);
    using CR_IsApp_t    = bool (*)(uint32_t appId);
    using CR_SetApps_t          = void (*)(const uint32_t* appIds, uint32_t count);
    using CR_Shutdown_t         = void (*)();
    using CR_EnableStatsSync_t   = void (*)(bool, bool);
    using CR_SetAccountId_t      = void (*)(uint32_t);
    using CR_NotifyAppRunning_t  = void (*)(uint32_t, bool);
    using CR_NotifyStatsStored_t = void (*)(uint32_t);
    using CR_GetAchievements_t    = uint32_t (*)(uint32_t, CloudRedirectHost::AchievementBlock*, uint32_t);
    using CR_InstallVtableHooks_t = bool (*)();

    std::mutex        g_mutex;
    std::atomic<bool> g_active{false};
    OSTPlatform::DynamicLibrary::ModuleHandle g_module = nullptr;

    CR_InitCloudSave_t     g_initCloudSave     = nullptr;
    CR_HandleCloudRpc_t    g_handleCloudRpc    = nullptr;
    CR_SetApps_t           g_setApps           = nullptr;
    CR_AddApp_t            g_addApp            = nullptr;
    CR_IsApp_t             g_isApp             = nullptr;
    CR_Shutdown_t          g_shutdownFn        = nullptr;
    CR_EnableStatsSync_t    g_enableStatsSync    = nullptr;
    CR_SetAccountId_t       g_setAccountId       = nullptr;
    CR_NotifyAppRunning_t   g_notifyAppRunning   = nullptr;
    CR_NotifyStatsStored_t  g_notifyStatsStored  = nullptr;
    CR_GetAchievements_t    g_getAchievements    = nullptr;
    CR_InstallVtableHooks_t g_installVtableHooks = nullptr;

    // Routes CloudRedirect's notifications into OpenSteamTool's log instead of
    // popping a MessageBox from inside Steam.
    void CloudNotify(int level, const char* title, const char* message) {
        const char* t = title ? title : "CloudRedirect";
        const char* m = message ? message : "";
        switch (level) {
        case 2:  LOG_ERROR("[CloudRedirect] {}: {}", t, m); break;
        case 1:  LOG_WARN("[CloudRedirect] {}: {}", t, m);  break;
        default: LOG_INFO("[CloudRedirect] {}: {}", t, m);  break;
        }
    }

    std::filesystem::path ResolveLibraryPath(const std::string& steamRoot,
                                             const std::string& configured) {
        using OSTPlatform::Encoding::PathFromUtf8;
        std::error_code ec;
        const std::filesystem::path filename = PathFromUtf8(configured.empty() ? "cloud_redirect.dll" : configured);
        if (filename.is_absolute()) {
            return filename;
        }

        if (DllDir[0] != '\0') {
            auto p = PathFromUtf8(DllDir) / filename;
            if (std::filesystem::exists(p, ec) && !ec) return p;
        }
        if (ConfigPath[0] != '\0') {
            auto configParent = PathFromUtf8(ConfigPath).parent_path();
            auto steamRootPath = PathFromUtf8(steamRoot);
            if (configParent != steamRootPath) {
                auto p = configParent / filename;
                if (std::filesystem::exists(p, ec) && !ec) return p;
            }
        }
        return PathFromUtf8(steamRoot) / filename;
    }

    template <typename T>
    bool ResolveSymbol(OSTPlatform::DynamicLibrary::ModuleHandle module,
                       const char* name, T& out) {
        out = reinterpret_cast<T>(OSTPlatform::DynamicLibrary::GetSymbol(module, name));
        if (!out) {
            LOG_WARN("CloudRedirect: export {} not found in cloud_redirect.dll", name);
            return false;
        }
        return true;
    }

    std::vector<AppId_t> CollectRedirectedAppIds() {
        std::vector<AppId_t> depots = LuaConfig::GetAllDepotIds();
        if (Hooks_Misc::IsOnlineFixActive()) {
            const AppId_t fixAppId = Hooks_Misc::ResolveAppId();
            if (fixAppId != 0 && std::find(depots.begin(), depots.end(), fixAppId) == depots.end()) {
                depots.push_back(fixAppId);
            }
        }
        return depots;
    }

} // namespace

void Initialize(const char* steamInstallPath) {
    const Config::CloudSettings cloud = Config::GetCloudSettings();
    if (!cloud.enabled) {
        LOG_INFO("CloudRedirect: [cloud].enabled is false, cloud save redirection disabled");
        return;
    }
    if (!steamInstallPath || steamInstallPath[0] == '\0') {
        LOG_WARN("CloudRedirect: empty Steam install path, cannot initialise");
        return;
    }

    std::lock_guard lock(g_mutex);
    if (g_active.load(std::memory_order_acquire)) return;

    const std::filesystem::path libPath = ResolveLibraryPath(steamInstallPath, cloud.library);
    const std::string libPathUtf8 = OSTPlatform::Encoding::PathToUtf8(libPath);
    std::error_code libEc;
    if (!std::filesystem::exists(libPath, libEc) || libEc) {
        LOG_WARN("CloudRedirect: cloud_redirect.dll not found at {}", libPathUtf8);
        return;
    }

    g_module = OSTPlatform::DynamicLibrary::Load(libPath);
    if (!g_module) {
        LOG_WARN("CloudRedirect: failed to load {} (err={})",
                 libPathUtf8, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return;
    }

    bool ok = true;
    ok &= ResolveSymbol(g_module, "CR_InitCloudSave",  g_initCloudSave);
    ok &= ResolveSymbol(g_module, "CR_HandleCloudRpc", g_handleCloudRpc);
    ok &= ResolveSymbol(g_module, "CR_SetApps",        g_setApps);
    ok &= ResolveSymbol(g_module, "CR_IsApp",          g_isApp);
    ok &= ResolveSymbol(g_module, "CR_Shutdown",       g_shutdownFn);
    if (!ok) {
        LOG_WARN("CloudRedirect: cloud_redirect.dll is missing required exports, disabling");
        OSTPlatform::DynamicLibrary::Unload(g_module);
        g_module = nullptr;
        return;
    }

    // Optional (CR 2.2.5+)
    ResolveSymbol(g_module, "CR_AddApp",             g_addApp);
    ResolveSymbol(g_module, "CR_EnableStatsSync",    g_enableStatsSync);
    ResolveSymbol(g_module, "CR_SetAccountId",       g_setAccountId);
    ResolveSymbol(g_module, "CR_NotifyAppRunning",   g_notifyAppRunning);
    ResolveSymbol(g_module, "CR_NotifyStatsStored",  g_notifyStatsStored);
    ResolveSymbol(g_module, "CR_GetAchievements",    g_getAchievements);
    ResolveSymbol(g_module, "CR_InstallVtableHooks", g_installVtableHooks);

    try {
        if (!g_initCloudSave(steamInstallPath, &CloudNotify)) {
            LOG_WARN("CloudRedirect: CR_InitCloudSave failed, disabling cloud save redirection");
            OSTPlatform::DynamicLibrary::Unload(g_module);
            g_module = nullptr;
            return;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_InitCloudSave threw exception: {}", e.what());
        OSTPlatform::DynamicLibrary::Unload(g_module);
        g_module = nullptr;
        return;
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_InitCloudSave threw unknown exception");
        OSTPlatform::DynamicLibrary::Unload(g_module);
        g_module = nullptr;
        return;
    }

    g_active.store(true, std::memory_order_release);
    LOG_INFO("CloudRedirect: loaded {} and initialised cloud save redirection (diversion: {:p})",
             libPathUtf8, static_cast<void*>(client_hModule));

    if (g_enableStatsSync) {
        try {
            g_enableStatsSync(true, true);
            LOG_INFO("CloudRedirect: stats sync registered");
        } catch (const std::exception& e) {
            LOG_WARN("CloudRedirect: CR_EnableStatsSync threw: {}", e.what());
        } catch (...) {
            LOG_WARN("CloudRedirect: CR_EnableStatsSync threw unknown exception");
        }
    }

    // Push the current unlocked-app set without re-locking g_mutex.
    try {
        const std::vector<AppId_t> depots = CollectRedirectedAppIds();
        g_setApps(depots.empty() ? nullptr : depots.data(),
                  static_cast<uint32_t>(depots.size()));
        LOG_INFO("CloudRedirect: registered {} redirected app(s)", depots.size());
    } catch (const std::exception& e) {
        LOG_WARN("CloudRedirect: initial CR_SetApps threw: {}", e.what());
    } catch (...) {
        LOG_WARN("CloudRedirect: initial CR_SetApps threw unknown exception");
    }

    // Vtable hooks let CR handle Cloud RPCs synchronously (slot4 semantics).
    if (g_installVtableHooks) {
        try {
            if (g_installVtableHooks())
                LOG_INFO("CloudRedirect: vtable hooks installed (routed to diversion module)");
            else
                LOG_WARN("CloudRedirect: vtable hook install failed, using packet-layer path");
        } catch (const std::exception& e) {
            LOG_WARN("CloudRedirect: CR_InstallVtableHooks threw: {}", e.what());
        } catch (...) {
            LOG_WARN("CloudRedirect: CR_InstallVtableHooks threw unknown exception");
        }
    }
}

void SyncAppSet() {
    if (!g_active.load(std::memory_order_acquire) || !g_setApps) return;

    try {
        const std::vector<AppId_t> depots = CollectRedirectedAppIds();
        g_setApps(depots.empty() ? nullptr : depots.data(),
                  static_cast<uint32_t>(depots.size()));
        LOG_DEBUG("CloudRedirect: re-synced redirected app set ({} app(s))", depots.size());
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_SetApps failed: {}", e.what());
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_SetApps failed with unknown exception");
    }
}

bool IsActive() {
    return g_active.load(std::memory_order_acquire);
}

bool IsApp(uint32_t appId) {
    if (!g_active.load(std::memory_order_acquire)) return false;
    try {
        if (g_isApp && g_isApp(appId)) return true;
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_IsApp({}) failed: {}", appId, e.what());
        return false;
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_IsApp({}) failed with unknown exception", appId);
        return false;
    }
    if (Hooks_Misc::IsOnlineFixActive() && appId == Hooks_Misc::ResolveAppId()) return true;
    return false;
}

void AddApp(uint32_t appId) {
    if (!g_active.load(std::memory_order_acquire) || !g_addApp) return;
    try {
        g_addApp(appId);
        LOG_INFO("CloudRedirect: dynamically added app {}", appId);
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_AddApp({}) failed: {}", appId, e.what());
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_AddApp({}) failed with unknown exception", appId);
    }
}

bool HandleCloudRpc(const char* method, uint32_t appId, uint32_t accountId,
                    const uint8_t* reqBody, uint32_t reqLen,
                    uint8_t* respBuf, uint32_t respMaxLen,
                    uint32_t* respLen, int32_t* eresult) {
    if (!g_active.load(std::memory_order_acquire) || !g_handleCloudRpc) return false;
    try {
        return g_handleCloudRpc(method, appId, accountId, reqBody, reqLen,
                                respBuf, respMaxLen, respLen, eresult);
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_HandleCloudRpc({}, {}) failed: {}", method ? method : "null", appId, e.what());
        return false;
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_HandleCloudRpc({}, {}) failed with unknown exception", method ? method : "null", appId);
        return false;
    }
}

void SetAccountId(uint32_t accountId) {
    if (!g_active.load(std::memory_order_acquire) || !g_setAccountId) return;
    try {
        g_setAccountId(accountId);
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_SetAccountId({}) failed: {}", accountId, e.what());
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_SetAccountId({}) failed with unknown exception", accountId);
    }
}

void NotifyAppRunning(uint32_t appId, bool running) {
    if (!g_active.load(std::memory_order_acquire) || !g_notifyAppRunning) return;
    try {
        g_notifyAppRunning(appId, running);
        LOG_INFO("CloudRedirect: notified app {} running={}", appId, running);
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_NotifyAppRunning({}, {}) failed: {}", appId, running, e.what());
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_NotifyAppRunning({}, {}) failed with unknown exception", appId, running);
    }
}

void NotifyStatsStored(uint32_t appId) {
    if (!g_active.load(std::memory_order_acquire) || !g_notifyStatsStored) return;
    try {
        g_notifyStatsStored(appId);
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_NotifyStatsStored({}) failed: {}", appId, e.what());
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_NotifyStatsStored({}) failed with unknown exception", appId);
    }
}

uint32_t GetAchievements(uint32_t appId, AchievementBlock* out, uint32_t maxBlocks) {
    if (!g_active.load(std::memory_order_acquire) || !g_getAchievements) return 0;
    try {
        return g_getAchievements(appId, out, maxBlocks);
    } catch (const std::exception& e) {
        LOG_ERROR("CloudRedirect: CR_GetAchievements({}) failed: {}", appId, e.what());
        return 0;
    } catch (...) {
        LOG_ERROR("CloudRedirect: CR_GetAchievements({}) failed with unknown exception", appId);
        return 0;
    }
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (!g_active.exchange(false)) return;
    if (g_shutdownFn) {
        try {
            g_shutdownFn();
        } catch (const std::exception& e) {
            LOG_ERROR("CloudRedirect: CR_Shutdown failed: {}", e.what());
        } catch (...) {
            LOG_ERROR("CloudRedirect: CR_Shutdown failed with unknown exception");
        }
    }
    if (g_module) {
        OSTPlatform::DynamicLibrary::Unload(g_module);
        g_module = nullptr;
    }
    g_initCloudSave      = nullptr;
    g_handleCloudRpc     = nullptr;
    g_setApps            = nullptr;
    g_addApp             = nullptr;
    g_isApp              = nullptr;
    g_shutdownFn         = nullptr;
    g_enableStatsSync    = nullptr;
    g_setAccountId       = nullptr;
    g_notifyAppRunning   = nullptr;
    g_notifyStatsStored  = nullptr;
    g_getAchievements    = nullptr;
    g_installVtableHooks = nullptr;
    LOG_INFO("CloudRedirect: shut down");
}

} // namespace CloudRedirectHost
