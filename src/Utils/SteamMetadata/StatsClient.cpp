#include "StatsClient.h"

#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace StatsClient {
namespace {

    std::mutex g_mutex;
    std::unordered_map<AppId_t, uint64_t> g_cache;
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_failedLookups;
    constexpr std::chrono::seconds kFailureCooldown{60};

    constexpr uint32_t kResolveMs = 1500;
    constexpr uint32_t kConnectMs = 1500;
    constexpr uint32_t kSendMs    = 2000;
    constexpr uint32_t kRecvMs    = 3000;

    bool ParseSteamId(std::string_view body, uint64_t* outSteamId) {
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(body);
        if (!parsed || *parsed == 0) return false;
        *outSteamId = *parsed;
        return true;
    }

} // namespace

bool FetchStatSteamId(AppId_t appId, uint64_t* outSteamId) {
    if (!outSteamId || appId == k_uAppIdInvalid) return false;

    if (!Config::GetStatsEnableApi()) {
        LOG_ACHIEVEMENT_DEBUG("Stats SteamID API disabled for appid={}", appId);
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_cache.find(appId);
        if (it != g_cache.end()) {
            *outSteamId = it->second;
            LOG_ACHIEVEMENT_DEBUG("Stats SteamID API cache hit appid={} steamid={}", appId, *outSteamId);
            return true;
        }

        auto failIt = g_failedLookups.find(appId);
        if (failIt != g_failedLookups.end()) {
            if (now - failIt->second < kFailureCooldown) {
                LOG_ACHIEVEMENT_TRACE("Stats SteamID API in cooldown for appid={}", appId);
                return false;
            }
            g_failedLookups.erase(failIt);
        }
    }

    char url[128];
    std::snprintf(url, sizeof(url), "https://stats.opensteamtool.com/%u", appId);

    auto r = OSTPlatform::Http::Execute(
        L"GET", url, nullptr, 0, nullptr,
        kResolveMs, kConnectMs, kSendMs, kRecvMs);
    LOG_ACHIEVEMENT_INFO("Stats SteamID API status={} appid={}", r.status, appId);

    if (!r.ok || r.status != 200) {
        LOG_ACHIEVEMENT_WARN("Stats SteamID API failed appid={} status={} ok={}", appId, r.status, r.ok);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_failedLookups.size() >= 512) {
            g_failedLookups.clear();
        }
        g_failedLookups[appId] = now;
        return false;
    }

    uint64_t steamId = 0;
    if (!ParseSteamId(r.body, &steamId)) {
        LOG_ACHIEVEMENT_WARN("Stats SteamID API returned invalid body appid={} bytes={}", appId, r.body.size());
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_failedLookups.size() >= 512) {
            g_failedLookups.clear();
        }
        g_failedLookups[appId] = now;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_failedLookups.erase(appId);
        if (g_cache.size() >= 512) {
            g_cache.clear();
        }
        g_cache[appId] = steamId;
    }

    *outSteamId = steamId;
    LOG_ACHIEVEMENT_INFO("Stats SteamID API resolved appid={} steamid={}", appId, steamId);
    return true;
}

} // namespace StatsClient
