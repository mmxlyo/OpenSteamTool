#include "Hooks_SteamUI.h"
#include "HookManager.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "Utils/HookSupport/VehCommon.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace
{
    using namespace std::chrono_literals;
    constexpr int  kMaxRetry      = 50;
    constexpr auto kRetryInterval = 100ms;

    static bool IsSteamClientPath(const char* path) {
        if (!path) return false;
        std::string_view p(path);
        auto endsWithCi = [](std::string_view str, std::string_view suffix) {
            if (str.size() < suffix.size()) return false;
            return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                });
        };
        auto equalsCi = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            return std::equal(a.begin(), a.end(), b.begin(),
                [](char c1, char c2) {
                    return std::tolower(static_cast<unsigned char>(c1)) ==
                           std::tolower(static_cast<unsigned char>(c2));
                });
        };
        return equalsCi(p, "steamclient64.dll") ||
               equalsCi(p, "steamclient.dll") ||
               equalsCi(p, "steamclient64") ||
               equalsCi(p, "steamclient") ||
               endsWithCi(p, "\\steamclient64.dll") ||
               endsWithCi(p, "\\steamclient.dll") ||
               endsWithCi(p, "/steamclient64.dll") ||
               endsWithCi(p, "/steamclient.dll");
    }

    static bool IsSteamClientPathW(const wchar_t* path) {
        if (!path) return false;
        std::wstring_view p(path);
        auto endsWithCiW = [](std::wstring_view str, std::wstring_view suffix) {
            if (str.size() < suffix.size()) return false;
            return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
                [](wchar_t a, wchar_t b) {
                    return std::towlower(a) == std::towlower(b);
                });
        };
        auto equalsCiW = [](std::wstring_view a, std::wstring_view b) {
            if (a.size() != b.size()) return false;
            return std::equal(a.begin(), a.end(), b.begin(),
                [](wchar_t c1, wchar_t c2) {
                    return std::towlower(c1) == std::towlower(c2);
                });
        };
        return equalsCiW(p, L"steamclient64.dll") ||
               equalsCiW(p, L"steamclient.dll") ||
               equalsCiW(p, L"steamclient64") ||
               equalsCiW(p, L"steamclient") ||
               endsWithCiW(p, L"\\steamclient64.dll") ||
               endsWithCiW(p, L"\\steamclient.dll") ||
               endsWithCiW(p, L"/steamclient64.dll") ||
               endsWithCiW(p, L"/steamclient.dll");
    }

    // Original pointers for system module lookup APIs
    static decltype(&GetModuleHandleA)   oGetModuleHandleA   = &GetModuleHandleA;
    static decltype(&GetModuleHandleW)   oGetModuleHandleW   = &GetModuleHandleW;
    static decltype(&GetModuleHandleExA) oGetModuleHandleExA = &GetModuleHandleExA;
    static decltype(&GetModuleHandleExW) oGetModuleHandleExW = &GetModuleHandleExW;

    HMODULE WINAPI hkGetModuleHandleA(LPCSTR lpModuleName)
    {
        if (client_hModule && IsSteamClientPath(lpModuleName)) {
            return reinterpret_cast<HMODULE>(client_hModule);
        }
        return oGetModuleHandleA(lpModuleName);
    }

    HMODULE WINAPI hkGetModuleHandleW(LPCWSTR lpModuleName)
    {
        if (client_hModule && IsSteamClientPathW(lpModuleName)) {
            return reinterpret_cast<HMODULE>(client_hModule);
        }
        return oGetModuleHandleW(lpModuleName);
    }

    BOOL WINAPI hkGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPath(lpModuleName))
        {
            if (phModule) {
                *phModule = reinterpret_cast<HMODULE>(client_hModule);
                if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                    HMODULE dummy = nullptr;
                    oGetModuleHandleExA((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCSTR>(client_hModule), &dummy);
                }
                return TRUE;
            }
            return FALSE;
        }
        return oGetModuleHandleExA(dwFlags, lpModuleName, phModule);
    }

    BOOL WINAPI hkGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPathW(lpModuleName))
        {
            if (phModule) {
                *phModule = reinterpret_cast<HMODULE>(client_hModule);
                if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                    HMODULE dummy = nullptr;
                    oGetModuleHandleExW((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCWSTR>(client_hModule), &dummy);
                }
                return TRUE;
            }
            return FALSE;
        }
        return oGetModuleHandleExW(dwFlags, lpModuleName, phModule);
    }


    HOOK_FUNC(LoadModuleWithPath, void*, const char* path, bool flags)
    {
        LOG_STEAMUI_INFO("LoadModuleWithPath called with path: {}, flags: {}",
                         path ? path : "(null)", flags);

        const bool isSteamClient = IsSteamClientPath(path);

        if (isSteamClient) {
            // Wait for all hooks on client_hModule to be fully initialized
            for (int i = 0; i < kMaxRetry && !g_HooksInstalled.load(); ++i) {
                LOG_STEAMUI_DEBUG("LoadModuleWithPath: waiting for hooks to be installed... (attempt {}/{})",
                                  i + 1, kMaxRetry);
                std::this_thread::sleep_for(kRetryInterval);
            }
        }

        void* h = oLoadModuleWithPath(path, flags);

        if (isSteamClient && client_hModule) {
            LOG_STEAMUI_INFO("LoadModuleWithPath: diverted {} (original {}) -> diversion {}",
                             path ? path : "steamclient64.dll", h, static_cast<void*>(client_hModule));
            return client_hModule;
        }

        return h;
    }

    RESOLVE_FUNC(RepeatedFieldUint32_Add, void, void* field, const uint32* value);

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    HOOK_FUNC(FillInAppOverview, void *, void *pThis, void *pAppOverview, CSteamApp *pApp)
    {
        if (pApp && LuaConfig::HasDepot(pApp->nAppID, false))
        {
            uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
            if (t)
            {
                pApp->PurchasedTime = t;
                LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                  pApp->PurchasedTime, pApp->nAppID);
            }
        }
        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // Apps to drop from the library: queued off-thread, marked on the UI thread.
    std::mutex g_removalMutex;
    std::vector<AppId_t> g_pendingRemovals;
    std::unordered_set<AppId_t> g_removedAppIds;

    // A full rebuild never lists removed_appid for apps still in the map
    // so re-assert our set after the snapshot is built.
    HOOK_FUNC(BuildCompleteAppOverviewChange, void, void *pController,
              CAppOverview_Change *pChange, void *optionalCallbackSlot)
    {
        oBuildCompleteAppOverviewChange(pController, pChange, optionalCallbackSlot);
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (pChange && !g_removedAppIds.empty() && oRepeatedFieldUint32_Add)
        {
            auto* field = pChange->mutable_removed_appid();
            for (AppId_t appId : g_removedAppIds){
                oRepeatedFieldUint32_Add(field, &appId);
            }
            LOG_STEAMUI_DEBUG("BuildCompleteAppOverviewChange: appended {} removed_appid entries",
                              g_removedAppIds.size());
        }
    }


    // Clearing ownership makes ShouldShowAppInLibrary() false (delta drops it,
    // the full snapshot skips it); MarkAppChange triggers the flush.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void *, void *pController)
    {
        if (CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange))
        {
            std::vector<AppId_t> draining;
            {
                std::lock_guard<std::mutex> lock(g_removalMutex);
                draining.swap(g_pendingRemovals);
            }
            for (AppId_t appId : draining)
            {
                if (LuaConfig::IsOwned(appId))
                {
                    LOG_STEAMUI_DEBUG("RunFrame: appId {} is owned again, skipping removal", appId);
                    continue;
                }
                if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                {
                    // Only remove from the library if it's not already uninstalled
                    pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                    if(pApp->AppStateFlags == k_EAppStateUninstalled){
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds.insert(appId);
                    }
                }
                
                oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
            }
        }
        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI
{
    void Install()
    {
        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        RESOLVE_U(RepeatedFieldUint32_Add);

        HOOK_BEGIN();
        INSTALL_HOOK_U(LoadModuleWithPath);
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(BuildCompleteAppOverviewChange);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);

        // System module handle redirection for Diversion shadow memory isolation
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW));

        HOOK_END();
    }

    void Uninstall()
    {
        UNHOOK_BEGIN();
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW));

        UNINSTALL_HOOK(LoadModuleWithPath);
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(BuildCompleteAppOverviewChange);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }


    void QueueRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        g_pendingRemovals.push_back(appId);
    }

    void CancelRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
    }
}
