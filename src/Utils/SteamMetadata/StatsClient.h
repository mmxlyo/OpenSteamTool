#pragma once

#include "Steam/Types.h"

#include <cstdint>

namespace StatsClient {

    constexpr uint64_t kDefaultStatSteamId = 76561198028121353ULL;

    // Non-blocking lookup. If in cache, returns true and writes *outSteamId.
    // If cache miss, triggers async background pre-warm and returns false immediately.
    bool TryGetCachedStatSteamId(AppId_t appId, uint64_t* outSteamId);

    // Asynchronously pre-warms the cache for appId in a detached background thread.
    // If already cached, in-flight, or in cooldown, this is a no-op.
    void PrewarmStatSteamId(AppId_t appId);

    // Synchronous fetch with network I/O.
    // WARNING: Do NOT call this on time-critical or mutex-protected paths (e.g., g_SendMutex).
    bool FetchStatSteamId(AppId_t appId, uint64_t* outSteamId);

} // namespace StatsClient
