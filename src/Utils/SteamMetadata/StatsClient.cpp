#include "StatsClient.h"

#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace StatsClient {
namespace {

    using namespace std::chrono_literals;

    std::mutex g_mutex;
    std::unordered_map<AppId_t, uint64_t> g_cache;
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_failedLookups;
    std::unordered_set<AppId_t> g_inFlight;

    constexpr auto kFailureCooldown = 60s;

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

    bool ExecuteFetch(AppId_t appId, uint64_t* outSteamId) {
        char url[128];
        std::snprintf(url, sizeof(url), "https://stats.opensteamtool.com/%u", appId);

        auto r = OSTPlatform::Http::Execute(
            L"GET", url, nullptr, 0, nullptr,
            kResolveMs, kConnectMs, kSendMs, kRecvMs);
        LOG_ACHIEVEMENT_INFO("Stats SteamID API status={} appid={}", r.status, appId);

        const auto now = std::chrono::steady_clock::now();
        if (!r.ok || r.status != 200) {
            LOG_ACHIEVEMENT_WARN("Stats SteamID API failed appid={} status={} ok={}", appId, r.status, r.ok);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_inFlight.erase(appId);
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
            g_inFlight.erase(appId);
            if (g_failedLookups.size() >= 512) {
                g_failedLookups.clear();
            }
            g_failedLookups[appId] = now;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_inFlight.erase(appId);
            g_failedLookups.erase(appId);
            if (g_cache.size() >= 512) {
                g_cache.clear();
            }
            g_cache[appId] = steamId;
        }

        if (outSteamId) {
            *outSteamId = steamId;
        }
        LOG_ACHIEVEMENT_INFO("Stats SteamID API resolved appid={} steamid={}", appId, steamId);
        return true;
    }

} // namespace

bool TryGetCachedStatSteamId(AppId_t appId, uint64_t* outSteamId) {
    if (!outSteamId || appId == k_uAppIdInvalid) return false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_cache.find(appId);
        if (it != g_cache.end()) {
            *outSteamId = it->second;
            return true;
        }
    }

    PrewarmStatSteamId(appId);
    return false;
}

void PrewarmStatSteamId(AppId_t appId) {
    if (appId == k_uAppIdInvalid) return;

    if (!Config::GetStatsEnableApi()) {
        LOG_ACHIEVEMENT_DEBUG("Stats SteamID API disabled for appid={}", appId);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_cache.contains(appId)) {
            return;
        }
        if (g_inFlight.contains(appId)) {
            return;
        }
        auto failIt = g_failedLookups.find(appId);
        if (failIt != g_failedLookups.end()) {
            if (now - failIt->second < kFailureCooldown) {
                LOG_ACHIEVEMENT_TRACE("Stats SteamID API in cooldown for appid={}", appId);
                return;
            }
            g_failedLookups.erase(failIt);
        }
        g_inFlight.insert(appId);
    }

    LOG_ACHIEVEMENT_DEBUG("Starting background prewarm for stat steamid appid={}", appId);
    const bool started = OSTPlatform::Thread::StartDetached([appId]() -> uint32_t {
        ExecuteFetch(appId, nullptr);
        return 0;
    });
    if (!started) {
        LOG_ACHIEVEMENT_WARN("Failed to start detached prewarm thread for appid={}", appId);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_inFlight.erase(appId);
    }
}

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

        g_inFlight.insert(appId);
    }

    return ExecuteFetch(appId, outSteamId);
}

} // namespace StatsClient
