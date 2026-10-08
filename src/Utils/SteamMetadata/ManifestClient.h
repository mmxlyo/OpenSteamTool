#pragma once
#include "Steam/Types.h"
#include <string_view>

// ─────────────────────────────────────────────────────────────────
//  ManifestClient — HTTP client for depot manifest request codes.
//  Provider table is internal (see kProviders in ManifestClient.cpp);
//  adding a new provider only requires one row there.
//
//  Thread-safe — provider selection/cooldown state is synchronized while
//  network requests execute without holding the state lock.
// ─────────────────────────────────────────────────────────────────
namespace ManifestClient {

    // Select the configured preferred provider by its string name.
    // Returns false if no provider matches; the previous selection is kept.
    bool SetProvider(std::string_view name);

    // Name of the configured preferred provider (for logging / diagnostics).
    std::string_view ActiveProviderName();

    // Resolve a manifest GID to its request code. Tries Lua first
    // (fetch_manifest_code_ex, then fetch_manifest_code), then the
    // configured provider with automatic fallback/cooldown across the built-in
    // providers. Fallback does not change the configured preference. Returns
    // true and sets *outRequestCode on success.
    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId = 0, AppId_t depotId = 0);

    // Tear down the cached WinHTTP connection (call at unload).
    void Shutdown();
}
