#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "VdfParser.h"
#include "steam.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OST::ExtractTickets {

struct DlcInfo {
    uint32_t dlcId{0};
    std::string name;
};

[[nodiscard]] HMODULE LoadSteamClient64(const std::string& steamPath, std::string& loadedPath);
[[nodiscard]] ISteamClient* CreateSteamClient(HMODULE module);
[[nodiscard]] bool OpenSession(ISteamClient* client, HSteamPipe& pipe, HSteamUser& user);

[[nodiscard]] std::optional<std::vector<uint8_t>> ExtractAppOwnershipTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId);

[[nodiscard]] std::optional<std::vector<uint8_t>> ExtractEncryptedAppTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId);

[[nodiscard]] std::vector<DepotKeyInfo> ExtractDepotDecryptionKeys(
    const std::string& steamPath,
    uint32_t appId,
    ISteamClient* client,
    HSteamPipe pipe,
    HSteamUser user,
    std::vector<DlcInfo>& outDlcs);

} // namespace OST::ExtractTickets
