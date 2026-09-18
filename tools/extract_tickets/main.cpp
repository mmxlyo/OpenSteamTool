#include "AppInfoParser.h"
#include "OutputWriter.h"
#include "RaiiGuards.h"
#include "SteamSession.h"
#include "Utils.h"
#include "VdfParser.h"
#include "steam.h"

#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OST::ExtractTickets {

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

    auto steamPathOpt = FindSteamInstallPath();
    std::string steamPath = steamPathOpt ? *steamPathOpt : "";

    std::string steamClientPath;
    HMODULE steamClient = LoadSteamClient64(steamPath, steamClientPath);
    ISteamClient* client = steamClient ? CreateSteamClient(steamClient) : nullptr;

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
        if (auto* utils = client->GetISteamUtils(pipe, kSteamUtilsInterfaceVersion)) {
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

    const bool ok = WriteOutputs(*appId, ownership, encrypted, depotKeys, dlcs, appTokens);
    return ok ? 0 : 1;
}
#endif

} // namespace OST::ExtractTickets

int main(int argc, char** argv) {
#if !defined(_WIN64)
    std::cerr << "[ERROR] extract_tickets 必须编译为 64 位 Windows 程序。\n"
              << "        extract_tickets must be built as a 64-bit Windows executable.\n";
    OST::ExtractTickets::WaitForExit();
    return 1;
#else
    OST::ExtractTickets::ConsoleCodePageGuard cpGuard;
    const int rc = OST::ExtractTickets::Run(argc, argv);
    OST::ExtractTickets::WaitForExit();
    return rc;
#endif
}
