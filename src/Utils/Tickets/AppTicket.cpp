#include "AppTicket.h"
#include "Utils/Logging/Log.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace AppTicket {
    namespace {
        constexpr AppId_t kLocalAppTicketSourceAppId = 7;

        struct AppTicketEntry {
            std::vector<uint8_t> appTicket;
            std::vector<uint8_t> eTicket;
        };

        std::atomic<SourceTicketProvider> g_sourceTicketProvider{nullptr};
        std::shared_mutex g_ticketMutex;
        std::unordered_map<AppId_t, AppTicketEntry> g_tickets;
    } // namespace

    void SetSourceTicketProvider(SourceTicketProvider provider) {
        g_sourceTicketProvider.store(provider, std::memory_order_release);
    }

    std::vector<uint8_t> GetSteamConfigStoreTicket(AppId_t appId) {
        if (auto provider = g_sourceTicketProvider.load(std::memory_order_acquire)) {
            return provider(appId);
        }
        return {};
    }

    std::vector<uint8_t> GetCachedAppOwnershipTicket(AppId_t appId) {
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.appTicket.empty()) {
            LOG_INFO("Successfully retrieved App Ownership Ticket, AppId: {}, Ticket Size: {}",
                     appId, it->second.appTicket.size());
            return it->second.appTicket;
        }
        LOG_TRACE("Read App Ownership Ticket for AppId {}: ticket unavailable", appId);
        return {};
    }

    // Exploit steamdrmp's off-by-four ticket parsing vulnerability:
    static std::vector<uint8_t> ForgeLocalAppOwnershipTicket(AppId_t appId) {
        std::vector<uint8_t> source = GetSteamConfigStoreTicket(kLocalAppTicketSourceAppId);
        if (source.size() <= kAppTicketSignatureSize) {
            LOG_DEBUG("ForgeLocalAppOwnershipTicket for AppId {}: no source appticket", appId);
            return {};
        }

        const size_t signedSize = source.size() - kAppTicketSignatureSize;
        std::vector<uint8_t> ticket;
        ticket.reserve(source.size() + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin(), source.begin() + signedSize);

        const uint8_t* appIdBytes = reinterpret_cast<const uint8_t*>(&appId);
        ticket.insert(ticket.end(), appIdBytes, appIdBytes + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin() + signedSize, source.end());

        LOG_INFO("Forged App Ownership Ticket, AppId: {}, SourceAppId: {}, Physical Size: {}, Total Size: {}",
                 appId, kLocalAppTicketSourceAppId, ticket.size(), source.size());
        return ticket;
    }

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket, AppTicketSource source) {
        ticket = {};
        
        if (source == AppTicketSource::MemoryOnly || source == AppTicketSource::MemoryThenForge) {
            ticket.data = GetCachedAppOwnershipTicket(appId);
            if (!ticket.data.empty() && ticket.data.size() >= kSteamIdTicketMinimumSize) {
                ticket.totalSize = static_cast<uint32>(ticket.data.size());
                ticket.appIdOffset = kAppTicketAppIdOffset;
                ticket.steamIdOffset = kAppTicketSteamIdOffset;
                std::memcpy(&ticket.signatureOffset, ticket.data.data(), sizeof(uint32));
                ticket.signatureSize = kAppTicketSignatureSize;
                return true;
            }
        }

        if (source == AppTicketSource::MemoryOnly) return false;

        ticket.data = ForgeLocalAppOwnershipTicket(appId);
        if (ticket.data.empty()) return false;

        ticket.totalSize = static_cast<uint32>(ticket.data.size() - sizeof(AppId_t));
        ticket.appIdOffset = ticket.totalSize - kAppTicketSignatureSize;
        ticket.steamIdOffset = kAppTicketSteamIdOffset;
        ticket.signatureOffset = ticket.appIdOffset + sizeof(AppId_t);
        ticket.signatureSize = kAppTicketSignatureSize;
        return true;
    }

    std::vector<uint8_t> GetCachedEncryptedTicket(AppId_t appId) {
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.eTicket.empty()) {
            LOG_INFO("Successfully retrieved Encrypted App Ticket, AppId: {}, Ticket Size: {}",
                     appId, it->second.eTicket.size());
            return it->second.eTicket;
        }
        LOG_TRACE("Read Encrypted App Ticket for AppId {}: ticket unavailable", appId);
        return {};
    }

    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        std::unique_lock lock(g_ticketMutex);
        auto& entry = g_tickets[appId];
        entry.appTicket = data;
        LOG_INFO("Wrote AppTicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool RemoveAppOwnershipTicket(AppId_t appId) {
        std::unique_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end()) {
            it->second.appTicket.clear();
        }
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        std::unique_lock lock(g_ticketMutex);
        auto& entry = g_tickets[appId];
        entry.eTicket = data;
        LOG_INFO("Wrote ETicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool RemoveEncryptedTicket(AppId_t appId) {
        std::unique_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end()) {
            it->second.eTicket.clear();
        }
        return true;
    }

    bool ClearCachedTickets(AppId_t appId) {
        std::unique_lock lock(g_ticketMutex);
        return g_tickets.erase(appId) > 0;
    }

    uint64_t ExtractSteamIdFromTicketBytes(const uint8_t* data, size_t size) {
        // Layout: ticket bytes start with [uint32 Size][uint32 Version][uint64 SteamID][...].
        if (!data || size < kSteamIdTicketMinimumSize) return 0;
        uint64_t steamId = 0;
        std::memcpy(&steamId, data + kAppTicketSteamIdOffset, sizeof(uint64_t));
        return steamId;
    }

    uint64_t ExtractSteamIdFromTicketBytes(const std::vector<uint8_t>& ticket) {
        return ExtractSteamIdFromTicketBytes(ticket.data(), ticket.size());
    }

    uint64_t ExtractSteamIdFromTicketBytes(std::span<const uint8_t> ticket) {
        return ExtractSteamIdFromTicketBytes(ticket.data(), ticket.size());
    }

    uint64_t GetTicketSteamID(AppId_t appId) {
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.appTicket.empty()) {
            const uint64_t steamId = ExtractSteamIdFromTicketBytes(
                it->second.appTicket.data(), it->second.appTicket.size());
            if (steamId != 0) {
                return steamId;
            }
        }
        return 0;
    }
}
