#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OST::ExtractTickets {

struct DepotKeyInfo {
    uint32_t depotId{0};
    std::string hexKey;           // 64 hex characters (32 bytes AES key)
    std::string manifestId;       // optional manifest id
    std::string manifestFilePath; // optional full path to cached .manifest file
    uint32_t dlcId{0};            // 0 for base game, or DLC AppID if associated with a DLC
};

[[nodiscard]] std::vector<std::string> FindSteamLibraryFolders(const std::string& steamPath);
[[nodiscard]] bool IsAppInstalledLocally(const std::string& steamPath, uint32_t appId);

[[nodiscard]] std::vector<std::string> GetDepotcacheDirs(const std::string& steamPath, const std::vector<std::string>& libraries);
[[nodiscard]] std::vector<std::string> GetDepotcacheDirs(const std::string& steamPath);

[[nodiscard]] std::string FindDepotManifestFile(const std::vector<std::string>& depotcacheDirs,
                                               uint32_t depotId,
                                               std::string& inOutManifestId);

void ParseAcfDepots(const std::string& acfPath,
                    std::unordered_map<uint32_t, std::string>& outDepots,
                    std::unordered_set<uint32_t>& outDlcIds,
                    std::unordered_map<uint32_t, uint32_t>& outDepotToDlc);

[[nodiscard]] std::unordered_map<uint32_t, std::string> ParseConfigVdfDepotKeys(const std::string& steamPath);

} // namespace OST::ExtractTickets
