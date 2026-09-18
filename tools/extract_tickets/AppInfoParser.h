#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace OST::ExtractTickets {

// Parses <steamPath>/appcache/appinfo.vdf to extract non-zero PICS AccessTokens.
// If targetAppIds is provided, only extracts tokens for AppIDs present in that set.
// Returns a map of AppID -> AccessToken.
[[nodiscard]] std::unordered_map<uint32_t, uint64_t> ParseAppInfoTokens(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>* targetAppIds = nullptr);

} // namespace OST::ExtractTickets
