#pragma once

#include "SteamSession.h"
#include "VdfParser.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OST::ExtractTickets {

[[nodiscard]] std::string TicketLine(const char* name, const std::optional<std::vector<uint8_t>>& ticket);

bool WriteOutputs(uint32_t appId,
                  const std::optional<std::vector<uint8_t>>& ownership,
                  const std::optional<std::vector<uint8_t>>& encrypted,
                  const std::vector<DepotKeyInfo>& depotKeys,
                  const std::vector<DlcInfo>& dlcs,
                  const std::unordered_map<uint32_t, uint64_t>& appTokens);

} // namespace OST::ExtractTickets
