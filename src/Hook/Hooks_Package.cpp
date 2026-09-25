#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "dllmain.h"
#include "Utils/HookSupport/VehCommon.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace {
    RESOLVE_FUNC(CUtlMemoryGrow,               void*, CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    RESOLVE_FUNC(ProcessPendingLicenseUpdates, bool,  void*);

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*,g_pCPackageInfo,void* pThis, uint32 packageId, uint64 accessToken);
    
    std::mutex                 g_packageInfoMutex;
    std::atomic<void*>         g_pCUser{nullptr};
    std::atomic<PackageInfo*>  g_pInjectedPackageInfo{nullptr};
    std::atomic<bool>          g_licenseInitialized{false};
    std::atomic<bool>          g_licenseRefreshPending{false};

    constexpr PackageId_t kInjectedPackageId = 0;
    constexpr uint64_t kInjectedPkgAccessToken = 10660652434190618804ull;

    bool MarkLicenseAsChangedAndProcessUpdates() {
        void* pUser = g_pCUser.load(std::memory_order_relaxed);
        if (!pUser || !oMarkLicenseAsChanged || !oProcessPendingLicenseUpdates) {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: dependencies not ready, skipping");
            return false;
        }
        oMarkLicenseAsChanged(pUser, kInjectedPackageId, true);
        oProcessPendingLicenseUpdates(pUser);
        LOG_PACKAGE_DEBUG("MarkLicenseAsChangedAndProcessUpdates: marked package {} as changed and processed updates", kInjectedPackageId);
        return true;
    }

    void TryProcessPendingLicenseRefresh() {
        if (!g_licenseRefreshPending.load(std::memory_order_relaxed))
            return;
        if (MarkLicenseAsChangedAndProcessUpdates())
            g_licenseRefreshPending.store(false, std::memory_order_relaxed);
    }

    bool CUtlMemoryGrowWrap(CUtlVector<AppId_t>* pVec, int grow_size) {
        if (!oCUtlMemoryGrow) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: oCUtlMemoryGrow not ready, cannot grow");
            return false;
        }
        return oCUtlMemoryGrow(pVec, grow_size);
    }

    bool InitFakeLicenseOnce(PackageInfo* pPkg) {
        std::lock_guard lock(g_packageInfoMutex);
        if (g_licenseInitialized.load(std::memory_order_relaxed)) return true;

        // check package status before injecting
        if (pPkg->Status != EPackageStatus::Available) {
            LOG_PACKAGE_WARN("InitFakeLicenseOnce: package status is not Available ({}), skipping injection", static_cast<int>(pPkg->Status));
            return false;
        }

        // Inject all depots from config into the fake license. 
        std::vector<AppId_t> appIds = LuaConfig::GetAllDepotIds();
        if (!appIds.empty()) {
            std::vector<AppId_t> toAdd;
            toAdd.reserve(appIds.size());
            for (AppId_t id : appIds) {
                bool alreadyPresent = false;
                for (uint32_t i = 0; i < pPkg->AppIdVec.m_Size; ++i) {
                    if (pPkg->AppIdVec.m_Memory.m_pMemory[i] == id) {
                        alreadyPresent = true;
                        break;
                    }
                }
                if (!alreadyPresent) {
                    toAdd.push_back(id);
                }
            }

            if (!toAdd.empty()) {
                uint32 oldSize = pPkg->AppIdVec.m_Size;
                uint32 numToAdd = static_cast<uint32>(toAdd.size());
                LOG_PACKAGE_INFO("InitFakeLicense(PackageId={}): adding {} apps, oldSize={}", kInjectedPackageId, numToAdd, oldSize);
                if (!CUtlMemoryGrowWrap(&pPkg->AppIdVec, numToAdd)) {
                    LOG_PACKAGE_WARN("InitFakeLicense(PackageId={}): failed to grow AppId vector", kInjectedPackageId);
                    return false;
                }
                for (uint32 i = 0; i < numToAdd; i++)
                    pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = toAdd[i];
                pPkg->AppIdVec.m_Size = oldSize + numToAdd;
            }
        }

        g_licenseInitialized.store(true, std::memory_order_release);
        g_licenseRefreshPending.store(true, std::memory_order_relaxed);
        TryProcessPendingLicenseRefresh();
        return true;
    }

    bool TryInitFakeLicenseOnce() {
        if (g_licenseInitialized.load(std::memory_order_relaxed)) return true;
        if (CAPTURE_READY(GetPackageInfo)) {
            PackageInfo* pPkg = oGetPackageInfo(g_pCPackageInfo, kInjectedPackageId, kInjectedPkgAccessToken);
            if (!pPkg) {
                LOG_PACKAGE_WARN("TryInitFakeLicenseOnce: GetPackageInfo returned null for injected package");
                return false;
            }
            if (!g_pInjectedPackageInfo.load(std::memory_order_relaxed)) {
                g_pInjectedPackageInfo.store(pPkg, std::memory_order_release);
            }
            return InitFakeLicenseOnce(pPkg);
        }
        return false;
    }

    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn) {
        if (!g_pCUser.load(std::memory_order_relaxed)) {
            g_pCUser.store(pObj, std::memory_order_relaxed);
            LOG_PACKAGE_DEBUG("CheckAppOwnership: captured CUser {}", pObj);
        }

        bool result = oCheckAppOwnership(pObj, appId, pOwn);
        TryInitFakeLicenseOnce();
        TryProcessPendingLicenseRefresh();

        const bool isTrulyOwned = pOwn && result &&
                                  (pOwn->PackageId != kInjectedPackageId) &&
                                  (pOwn->PackageId != 0) &&
                                  (pOwn->ExistInPackageNums > 1) &&
                                  pOwn->bOwnsLicense &&
                                  !pOwn->bLicenseExpired &&
                                  !pOwn->bFamilyShared &&
                                  !pOwn->bBorrowed;
        if (isTrulyOwned) {
            LuaConfig::MarkOwned(appId);
        }

        if (LuaConfig::HasDepot(appId, false)) {
            if (pOwn) {
                if (isTrulyOwned) {
                    pOwn->ReleaseState = EAppReleaseState::Released;
                } else {
                    pOwn->PackageId    = kInjectedPackageId;
                    pOwn->ReleaseState = EAppReleaseState::Released;
                    pOwn->bOwnsLicense = true; // This forces DLCs on steam family shared games that u dont own when adding their appid via .lua
                    pOwn->bFreeLicense = false;
                    return true;
                }
            } else {
                return true;
            }
        } else {
            // App is not active in LuaConfig:
            // 1. If explicitly marked as removed in the UI session (and not genuinely owned):
            if (Hooks_SteamUI::IsRemoved(appId) && !LuaConfig::IsOwned(appId)) {
                if (pOwn) {
                    pOwn->bOwnsLicense = false;
                    pOwn->PackageId = 0;
                }
                return false;
            }
            if (!result && pOwn) {
                pOwn->bOwnsLicense = false;
            }
        }

        return result;
    }
}

namespace Hooks_Package {
    void Install() {
        RESOLVE_C(CUtlMemoryGrow);
        RESOLVE_C(MarkLicenseAsChanged);
        RESOLVE_C(ProcessPendingLicenseUpdates);

        ARM_CAPTURE_C(GetPackageInfo);

        HOOK_BEGIN();
        INSTALL_HOOK_C(CheckAppOwnership);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(CheckAppOwnership);
        UNHOOK_END();
    }

    void NotifyLicenseChanged() {
        PackageInfo* pPkg = g_pInjectedPackageInfo.load(std::memory_order_acquire);
        if (!pPkg) {
            TryInitFakeLicenseOnce();
            pPkg = g_pInjectedPackageInfo.load(std::memory_order_acquire);
        }
        if (!pPkg) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injected PackageInfo not ready, cannot notify");
            return;
        }

        // ── Remove depots that were unloaded ──
        std::vector<AppId_t> removals = LuaConfig::TakePendingRemovals();
        std::vector<AppId_t> additions = LuaConfig::TakePendingAdditions();
        uint32_t removedCount = 0;
        std::unordered_set<AppId_t> addedIds;

        {
            std::lock_guard lock(g_packageInfoMutex);
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} removals", removals.size());
            for (AppId_t id : removals) {
                while (pPkg->AppIdVec.FindAndFastRemove(id)) {
                    ++removedCount;
                    LOG_PACKAGE_DEBUG("NotifyLicenseChanged: removed AppId {}", id);
                }
            }

            // ── Add depots that are newly loaded ──
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} additions", additions.size());
            if (!additions.empty()) {
                addedIds.reserve(additions.size());
                std::vector<AppId_t> toAdd;
                toAdd.reserve(additions.size());
                for (AppId_t id : additions) {
                    if (!addedIds.insert(id).second) {
                        continue;
                    }
                    Hooks_SteamUI::QueueAddition(id);

                    bool alreadyPresent = false;
                    for (uint32_t i = 0; i < pPkg->AppIdVec.m_Size; ++i) {
                        if (pPkg->AppIdVec.m_Memory.m_pMemory[i] == id) {
                            alreadyPresent = true;
                            break;
                        }
                    }
                    if (!alreadyPresent) {
                        toAdd.push_back(id);
                    }
                }

                if (!toAdd.empty()) {
                    uint32 oldSize = pPkg->AppIdVec.m_Size;
                    if (CUtlMemoryGrowWrap(&pPkg->AppIdVec, static_cast<int>(toAdd.size()))) {
                        for (size_t i = 0; i < toAdd.size(); ++i) {
                            pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = toAdd[i];
                            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: inserted AppId {} at [{}]", toAdd[i], oldSize + i);
                        }
                        pPkg->AppIdVec.m_Size = oldSize + static_cast<uint32_t>(toAdd.size());
                    } else {
                        LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to grow AppId vector for additions");
                    }
                }
            }
        }

        if (addedIds.empty() && removals.empty()) {
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: no changes");
            return;
        }

        // Mark package 0 as changed and trigger library refresh.
        if (!MarkLicenseAsChangedAndProcessUpdates()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to mark license as changed");
        } else {
            LOG_PACKAGE_INFO("NotifyLicenseChanged: {} added, {} removed", addedIds.size(), removedCount);
        }

        // Queue UI removals for the main-thread RunFrame hook to drain.
        // Never touch MarkAppChange from this (FileWatcher) thread.
        size_t queuedRemovalCount = 0;
        std::unordered_set<AppId_t> queuedRemovals;
        for (AppId_t id : removals) {
            // ParseFile unloads the old file before parsing the replacement.
            // Do not queue that transient removal when the id was added again.
            if (!addedIds.contains(id) && !LuaConfig::IsOwned(id) && queuedRemovals.insert(id).second) {
                Hooks_SteamUI::QueueRemoval(id);
                ++queuedRemovalCount;
            }
        }
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: queued {} UI removals, skipped {} transient removals",
                          queuedRemovalCount, removals.size() - queuedRemovalCount);
    }
}
