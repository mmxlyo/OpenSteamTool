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
    bool forceEticket{false};

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--force-eticket" || arg == "-f") {
            forceEticket = true;
        } else if (!appId) {
            appId = ParseAppId(arg);
            if (!appId) {
                std::cerr << "[ERROR] 无效的 AppID / Invalid AppID: " << arg << "\n";
                return 1;
            }
        }
    }

    if (!appId) {
        appId = ReadAppIdFromConsole();
        if (!appId) {
            std::cerr << "[ERROR] 无效的 AppID / Invalid AppID.\n";
            return 1;
        }
    }

    auto steamPathOpt = FindSteamInstallPath();
    std::string steamPath = steamPathOpt ? *steamPathOpt : "";

    const bool isInstalled = IsAppInstalledLocally(steamPath, *appId);
    const bool injectAppId = isInstalled || forceEticket;

    // Only inject SteamAppId if the game is installed locally, or if --force-eticket is specified.
    // If an uninstalled game runs with SteamAppId, Steam Client's AppManager registers the
    // process as running the app. Upon disconnect, because no ACF manifest or game files exist
    // on disk, Steam corrupts the in-memory app state into StateUpdateRequired (changing the UI
    // button to "Start Install" / "开始安装") and causes uninstallation to hang indefinitely.
    // AppOwnershipTicket (AppTicket), Depot decryption keys, and Access Tokens do NOT require SteamAppId!
    // SteamGameId and SteamOverlayGameId are never needed and have been completely removed.
    if (injectAppId) {
        const std::string appIdStr{std::to_string(*appId)};
        SetEnvironmentVariableA("SteamAppId", appIdStr.c_str());
        if (!isInstalled && forceEticket) {
            std::cout << "[WARN] 已启用 --force-eticket 强制注入未安装游戏的运行时环境。\n"
                      << "       --force-eticket enabled for uninstalled app runtime context injection.\n"
                      << "[WARN] 注意：这可能会导致 Steam 将其短暂识别为运行中，若状态异常可通过重启 Steam 恢复。\n"
                      << "       Caution: this may cause Steam to mark it as running; restart Steam to restore if corrupted.\n\n";
        }
    } else {
        std::cout << "[INFO] 目标 AppID " << *appId << " 未在本地库中安装，已启用【安全提取模式】。\n"
                  << "       Target AppID " << *appId << " is not installed locally; enabled [Safe Extraction Mode].\n"
                  << "[INFO] 正在提取所有权凭证 (AppTicket)、Depot 解密密钥与访问令牌 (Token)...\n"
                  << "       Extracting ownership ticket (AppTicket), depot decryption keys, and tokens...\n"
                  << "[INFO] 安全模式跳过运行上下文注入，彻底杜绝 Steam 客户端出现【开始安装】及卡卸载缺陷。\n"
                  << "       Safe mode skips runtime context injection, completely preventing Steam client state corruption and uninstallation hang.\n\n";
    }

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

        if (injectAppId) {
            encrypted = ExtractEncryptedAppTicket(client, pipe, user, *appId);
            if (encrypted) PrintHex("Encrypted ticket", *encrypted);
        } else {
            std::cout << "[INFO] 未安装游戏已在安全模式下跳过 ETicket 提取 (Lua 将自动保留模板并标记为 null)。\n"
                      << "       Safe mode skipped ETicket extraction for uninstalled app (marked as null in Lua).\n";
        }
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
