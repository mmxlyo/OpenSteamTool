#include "SteamSession.h"
#include "RaiiGuards.h"
#include "Utils.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace OST::ExtractTickets {

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
                    bool owned = isSubscribed || hasLicense || isInstalled;

                    if (owned && dlcId != appId) {
                        DlcInfo& d = dlcMap[dlcId];
                        d.dlcId = dlcId;
                        if (d.name.empty() && dlcName[0] != '\0') {
                            d.name = dlcName;
                        }
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

    // Any DLC found via ACF installed depots is confirmed owned
    for (uint32_t dlcId : knownDlcIds) {
        if (dlcId != appId) {
            DlcInfo& d = dlcMap[dlcId];
            d.dlcId = dlcId;
            knownDepotManifests.try_emplace(dlcId, "");
        }
    }

    outDlcs.clear();
    outDlcs.reserve(dlcMap.size());
    for (const auto& [id, info] : dlcMap) {
        outDlcs.push_back(info);
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
                std::string manifest;
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

} // namespace OST::ExtractTickets
