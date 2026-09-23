#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OST::ExtractTickets {

struct LuaFallbackData {
    std::unordered_map<uint32_t, std::string> depotKeys;
    std::unordered_map<uint32_t, std::string> depotManifests;
    std::unordered_map<uint32_t, std::string> manifestFiles;
    std::unordered_map<uint32_t, std::string> dlcNames;
    std::unordered_set<uint32_t> dlcIds;
    std::unordered_map<uint32_t, uint32_t> depotToDlc;
    std::unordered_map<uint32_t, uint64_t> appTokens;

    [[nodiscard]] bool Empty() const noexcept {
        return depotKeys.empty() && depotManifests.empty() &&
               manifestFiles.empty() && dlcIds.empty() && appTokens.empty();
    }
};

std::vector<std::string> GetOstLuaSearchDirectories(const std::string& steamPath);

LuaFallbackData ParseLuaFallbackData(const std::string& steamPath, uint32_t targetAppId);

} // namespace OST::ExtractTickets
