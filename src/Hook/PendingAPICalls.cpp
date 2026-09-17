#include "PendingAPICalls.h"

#include <mutex>
#include <unordered_map>

namespace PendingAPICalls {

namespace {

    std::mutex g_mutex;
    std::unordered_map<SteamAPICall_t, AppId_t> g_encryptedTickets;

} // namespace

void RecordEncryptedTicket(SteamAPICall_t call, AppId_t appID)
{
    if (call == k_uAPICallInvalid || appID == k_uAppIdInvalid) return;

    std::scoped_lock lock(g_mutex);
    if (g_encryptedTickets.size() >= 256) {
        g_encryptedTickets.clear();
    }
    g_encryptedTickets[call] = appID;
}

std::optional<AppId_t> TakeEncryptedTicket(SteamAPICall_t call)
{
    std::scoped_lock lock(g_mutex);
    const auto it = g_encryptedTickets.find(call);
    if (it == g_encryptedTickets.end()) return std::nullopt;

    const AppId_t appID = it->second;
    g_encryptedTickets.erase(it);
    return appID;
}

} // namespace PendingAPICalls
