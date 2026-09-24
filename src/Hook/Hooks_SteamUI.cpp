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
    constexpr int  kMaxRetry      = 500;
    constexpr auto kRetryInterval = 10ms;

    static std::string_view ExtractFileName(std::string_view path) {
        const size_t pos = path.find_last_of("\\/");
        return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
    }

    static std::wstring_view ExtractFileNameW(std::wstring_view path) {
        const size_t pos = path.find_last_of(L"\\/");
        return (pos == std::wstring_view::npos) ? path : path.substr(pos + 1);
    }

    static bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
        return (a.size() == b.size()) && (_strnicmp(a.data(), b.data(), a.size()) == 0);
    }

    static bool EqualsIgnoreCaseW(std::wstring_view a, std::wstring_view b) noexcept {
        return (a.size() == b.size()) && (_wcsnicmp(a.data(), b.data(), a.size()) == 0);
    }

    static bool IsSteamClientPath(const char* path) {
        if (!path) return false;
        const std::string_view fn = ExtractFileName(path);
        return EqualsIgnoreCase(fn, "steamclient64.dll") ||
               EqualsIgnoreCase(fn, "steamclient.dll")   ||
               EqualsIgnoreCase(fn, "steamclient64")     ||
               EqualsIgnoreCase(fn, "steamclient");
    }

    static bool IsSteamClientPathW(const wchar_t* path) {
        if (!path) return false;
        const std::wstring_view fn = ExtractFileNameW(path);
        return EqualsIgnoreCaseW(fn, L"steamclient64.dll") ||
               EqualsIgnoreCaseW(fn, L"steamclient.dll")   ||
               EqualsIgnoreCaseW(fn, L"steamclient64")     ||
               EqualsIgnoreCaseW(fn, L"steamclient");
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
            if (!phModule) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            constexpr DWORD kKnownFlags = GET_MODULE_HANDLE_EX_FLAG_PIN |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                                          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
            if ((dwFlags & ~kKnownFlags) != 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
                (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                if (!oGetModuleHandleExA((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                         reinterpret_cast<LPCSTR>(client_hModule), phModule))
                {
                    *phModule = nullptr;
                    return FALSE;
                }
                return TRUE;
            }
            *phModule = reinterpret_cast<HMODULE>(client_hModule);
            return TRUE;
        }
        return oGetModuleHandleExA(dwFlags, lpModuleName, phModule);
    }

    BOOL WINAPI hkGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPathW(lpModuleName))
        {
            if (!phModule) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            constexpr DWORD kKnownFlags = GET_MODULE_HANDLE_EX_FLAG_PIN |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                                          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
            if ((dwFlags & ~kKnownFlags) != 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
                (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                if (!oGetModuleHandleExW((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                         reinterpret_cast<LPCWSTR>(client_hModule), phModule))
                {
                    *phModule = nullptr;
                    return FALSE;
                }
                return TRUE;
            }
            *phModule = reinterpret_cast<HMODULE>(client_hModule);
            return TRUE;
        }
        return oGetModuleHandleExW(dwFlags, lpModuleName, phModule);
    }


    HOOK_FUNC(LoadModuleWithPath, void*, const char* path, bool flags)
    {
        LOG_STEAMUI_INFO("LoadModuleWithPath called with path: {}, flags: {}",
                         path ? path : "(null)", flags);

        const bool isSteamClient = IsSteamClientPath(path);

        if (isSteamClient) {
            bool logged = false;
            for (int i = 0; i < kMaxRetry && !g_HooksInstalled.load(); ++i) {
                if (!logged) {
                    LOG_STEAMUI_DEBUG("LoadModuleWithPath: waiting for hooks to be installed...");
                    logged = true;
                }
                std::this_thread::sleep_for(kRetryInterval);
            }
        }

        void* h = oLoadModuleWithPath(path, flags);

        if (isSteamClient) {
            if (!g_HooksInstalled.load()) {
                LOG_STEAMUI_WARN("LoadModuleWithPath: hooks initialization timed out after {} ms, aborting diversion",
                                 kMaxRetry * static_cast<int>(kRetryInterval.count()));
                return h;
            }

            if (client_hModule) {
                if (g_IsDiversionActive.load()) {
                    LOG_STEAMUI_INFO("LoadModuleWithPath: diverted {} (original {}) -> diversion {}",
                                     path ? path : "steamclient64.dll", h, static_cast<void*>(client_hModule));
                } else {
                    LOG_STEAMUI_INFO("LoadModuleWithPath: returned fallback steamclient64.dll ({})",
                                     static_cast<void*>(client_hModule));
                }
                return client_hModule;
            }
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
            if (pApp->OwnershipFlags == k_EAppOwnershipFlags_None)
            {
                pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                    k_EAppOwnershipFlags_OwnsLicense | k_EAppOwnershipFlags_LicensePermanent);
            }
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

    // Apps to drop from or restore to the library UI
    std::mutex g_removalMutex;
    std::vector<AppId_t> g_pendingRemovals;
    std::vector<AppId_t> g_pendingAdditions;
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
        if (pController && !g_pController)
        {
            g_pController = pController;
        }

        if (CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange))
        {
            std::vector<AppId_t> drainingRemovals;
            std::vector<AppId_t> drainingAdditions;
            {
                std::lock_guard<std::mutex> lock(g_removalMutex);
                if (!g_pendingRemovals.empty()) {
                    drainingRemovals.swap(g_pendingRemovals);
                }
                if (!g_pendingAdditions.empty()) {
                    drainingAdditions.swap(g_pendingAdditions);
                }
            }

            if (!drainingAdditions.empty())
            {
                std::vector<AppId_t> parentsToNotify;

                for (AppId_t appId : drainingAdditions)
                {
                    if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                    {
                        pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                            k_EAppOwnershipFlags_OwnsLicense | k_EAppOwnershipFlags_LicensePermanent);

                        uint32_t t = LuaConfig::GetPurchaseTime(appId);
                        if (t)
                        {
                            pApp->PurchasedTime = t;
                        }

                        const bool isDlc = ((pApp->eProtoAppType & 32) != 0) ||
                                           (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid);

                        if (isDlc)
                        {
                            if (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid && pApp->ParentAppID != appId)
                            {
                                if (std::ranges::find(parentsToNotify, pApp->ParentAppID) == parentsToNotify.end())
                                {
                                    parentsToNotify.push_back(pApp->ParentAppID);
                                }
                            }
                        }
                    }

                    LOG_STEAMUI_INFO("RunFrame: restoring added appId {}", appId);
                    oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
                }

                for (AppId_t parentId : parentsToNotify)
                {
                    LOG_STEAMUI_INFO("RunFrame: notifying parent appId {} of DLC addition", parentId);
                    oMarkAppChange(g_pAppChangeSource, parentId, EAppChangeFlags::AppInfoOrConfig);
                }
            }

            if (!drainingRemovals.empty())
            {
                std::vector<AppId_t> newlyRemoved;
                std::vector<AppId_t> parentsToNotify;

                for (AppId_t appId : drainingRemovals)
                {
                    if (LuaConfig::IsOwned(appId) || LuaConfig::HasDepot(appId, false))
                    {
                        LOG_STEAMUI_DEBUG("RunFrame: appId {} is still owned or active in config, skipping removal", appId);
                        continue;
                    }

                    if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                    {
                        pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                        pApp->PurchasedTime = 0;
                        pApp->MasterSubAppID = 0;

                        const bool isDlc = ((pApp->eProtoAppType & 32) != 0) ||
                                           (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid);

                        if (isDlc)
                        {
                            if (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid && pApp->ParentAppID != appId)
                            {
                                if (std::ranges::find(parentsToNotify, pApp->ParentAppID) == parentsToNotify.end())
                                {
                                    parentsToNotify.push_back(pApp->ParentAppID);
                                }
                            }
                            newlyRemoved.push_back(appId);
                        }
                        else if (pApp->AppStateFlags == k_EAppStateUninstalled)
                        {
                            newlyRemoved.push_back(appId);
                        }
                    }

                    oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
                }

                for (AppId_t parentId : parentsToNotify)
                {
                    LOG_STEAMUI_INFO("RunFrame: notifying parent appId {} of DLC change", parentId);
                    oMarkAppChange(g_pAppChangeSource, parentId, EAppChangeFlags::AppInfoOrConfig);
                }

                if (!newlyRemoved.empty())
                {
                    std::lock_guard<std::mutex> lock(g_removalMutex);
                    g_removedAppIds.insert(newlyRemoved.begin(), newlyRemoved.end());
                }
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
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW))) _ost_detour_transaction_ok_ = false;

        HOOK_END();
    }

    void Uninstall()
    {
        UNHOOK_BEGIN();
        if (!OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA))) _ost_detour_transaction_ok_ = false;
        if (!OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW))) _ost_detour_transaction_ok_ = false;

        UNINSTALL_HOOK(LoadModuleWithPath);
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(BuildCompleteAppOverviewChange);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }


    void QueueRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingAdditions, appId);
        if (std::ranges::find(g_pendingRemovals, appId) == g_pendingRemovals.end()) {
            g_pendingRemovals.push_back(appId);
        }
    }

    void CancelRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
    }

    void QueueAddition(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
        if (std::ranges::find(g_pendingAdditions, appId) == g_pendingAdditions.end()) {
            g_pendingAdditions.push_back(appId);
        }
    }
}
