#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace AppTicket {
    // Canonical AppTicket binary layout constants:
    // [uint32 Size][uint32 Version][uint64 SteamID][...]
    inline constexpr uint32 kAppTicketSteamIdOffset = 8;
    inline constexpr uint32 kAppTicketAppIdOffset = 16;
    inline constexpr uint32 kAppTicketSignatureSize = 128;
    inline constexpr size_t kSteamIdTicketMinimumSize = kAppTicketSteamIdOffset + sizeof(uint64_t); // 16

    enum class AppTicketSource {
        MemoryOnly,
        ForgeOnly,
        MemoryThenForge,
    };

    struct AppOwnershipTicket {
        std::vector<uint8_t> data;
        uint32 totalSize = 0;
        uint32 appIdOffset = kAppTicketAppIdOffset;
        uint32 steamIdOffset = kAppTicketSteamIdOffset;
        uint32 signatureOffset = 0;
        uint32 signatureSize = kAppTicketSignatureSize;
    };

    // External provider hook to query Steam client's internal ticket cache (IoC decoupling)
    using SourceTicketProvider = std::vector<uint8_t> (*)(AppId_t appId);
    void SetSourceTicketProvider(SourceTicketProvider provider);

    // Reads the raw ticket directly from Steam's internal ConfigStore via the registered provider.
    std::vector<uint8_t> GetSteamConfigStoreTicket(AppId_t appId);

    // Reads the app ownership ticket cached in memory.
    // Returns an empty vector when no ticket is available.
    std::vector<uint8_t> GetCachedAppOwnershipTicket(AppId_t appId);

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket, AppTicketSource source);

    // Reads the encrypted app ticket cached in memory.
    // Returns an empty vector when no ticket is available.
    std::vector<uint8_t> GetCachedEncryptedTicket(AppId_t appId);

    // Fast zero-copy query for the SteamID embedded inside the AppTicket.
    uint64_t GetTicketSteamID(AppId_t appId);

    // Parses the SteamID baked into app-ownership-ticket bytes (offset
    // kAppTicketSteamIdOffset). Returns 0 if the ticket is too short to
    // contain one. Lets callers identify which account a ticket belongs to
    // without duplicating the layout knowledge.
    uint64_t ExtractSteamIdFromTicketBytes(const uint8_t* data, size_t size);
    uint64_t ExtractSteamIdFromTicketBytes(const std::vector<uint8_t>& ticket);
    uint64_t ExtractSteamIdFromTicketBytes(std::span<const uint8_t> ticket);

    // Write AppTicket binary data to in-memory cache.
    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data);

    // Remove in-memory AppTicket for an appId.
    bool RemoveAppOwnershipTicket(AppId_t appId);

    // Write ETicket binary data to in-memory cache.
    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data);

    // Remove in-memory ETicket for an appId.
    bool RemoveEncryptedTicket(AppId_t appId);

    // Clear all cached tickets for an appId.
    bool ClearCachedTickets(AppId_t appId);
}
