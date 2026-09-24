#include "PendingAPICalls.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace PendingAPICalls {

namespace {

    struct TicketEntry {
        AppId_t appId = k_uAppIdInvalid;
        std::chrono::steady_clock::time_point insertedAt{};
    };

    std::mutex g_mutex;
    std::unordered_map<SteamAPICall_t, TicketEntry> g_encryptedTickets;

} // namespace

void RecordEncryptedTicket(SteamAPICall_t call, AppId_t appID)
{
    if (call == k_uAPICallInvalid || appID == k_uAppIdInvalid) return;

    std::scoped_lock lock(g_mutex);
    if (g_encryptedTickets.size() >= 256) {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(g_encryptedTickets, [now](const auto& pair) {
            return (now - pair.second.insertedAt) > std::chrono::minutes(2);
        });
        if (g_encryptedTickets.size() >= 256) {
            auto oldestIt = std::min_element(
                g_encryptedTickets.begin(), g_encryptedTickets.end(),
                [](const auto& a, const auto& b) {
                    return a.second.insertedAt < b.second.insertedAt;
                });
            if (oldestIt != g_encryptedTickets.end()) {
                g_encryptedTickets.erase(oldestIt);
            }
        }
    }
    g_encryptedTickets[call] = TicketEntry{appID, std::chrono::steady_clock::now()};
}

std::optional<AppId_t> TakeEncryptedTicket(SteamAPICall_t call)
{
    std::scoped_lock lock(g_mutex);
    const auto it = g_encryptedTickets.find(call);
    if (it == g_encryptedTickets.end()) return std::nullopt;

    const AppId_t appID = it->second.appId;
    g_encryptedTickets.erase(it);
    return appID;
}

} // namespace PendingAPICalls
