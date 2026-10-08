#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    static bool ParsePlainUint(std::string_view body, uint64_t* out) {
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) {
            body.remove_prefix(1);
        }
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) {
            body.remove_suffix(1);
        }
        if (body.empty()) return false;
        uint64_t code = 0;
        auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        if (ec != std::errc{} || ptr != body.data() + body.size()) return false;
        *out = code;
        return true;
    }

    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) {
        size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;
        size_t q1 = body.find('"', key + 9);
        if (q1 == std::string_view::npos) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        return ParsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // Adding a new provider: add one row to kProviders below.
    // host / port / tls / path are all derived from the URL template
    // by Make() at compile time.

    struct Provider {
        std::string_view name;          // matches [manifest] url = "..."
        const char*      urlTemplate;   // full literal with one %llu — for log & path
        Parser           parse;
        const wchar_t*   headers;
    };

    consteval Provider Make(std::string_view name, const char* url, Parser parse,
                            const wchar_t* headers = nullptr) {
        return {name, url, parse, headers};
    }

    static constexpr Provider kProviders[] = {
        Make("manifestdex",   "https://manifest.manifestdex.com/%llu",         ParsePlainUint,
             L"User-Agent: ManifestDeX/1.0\r\n"),
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",       ParsePlainUint),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",           ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",  ParseSteamRunJson),
    };

    static const Provider* g_configured = &kProviders[0];   // manifestdex
    static std::mutex g_stateMutex;

    static constexpr size_t kProviderCount = sizeof(kProviders) / sizeof(kProviders[0]);
    static constexpr auto kProviderCooldown = std::chrono::seconds(60);
    static constexpr auto kRepeatRetryMinAge = std::chrono::seconds(2);
    static constexpr auto kRepeatRetryWindow = std::chrono::seconds(90);
    static constexpr auto kAttemptStateTtl = std::chrono::minutes(5);
    static std::chrono::steady_clock::time_point g_cooldown[kProviderCount];
    static uint64_t g_okSeq[kProviderCount] = {};

    struct ManifestKey {
        uint64_t gid;
        uint32_t depotId;

        bool operator==(const ManifestKey&) const = default;
    };

    struct ManifestKeyHash {
        size_t operator()(const ManifestKey& key) const noexcept {
            const size_t h1 = std::hash<uint64_t>{}(key.gid);
            const size_t h2 = std::hash<uint32_t>{}(key.depotId);
            return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
        }
    };

    struct ProviderAttemptState {
        size_t providerIndex;
        std::chrono::steady_clock::time_point servedAt;
    };

    static std::unordered_map<ManifestKey, ProviderAttemptState, ManifestKeyHash> g_lastServed;

    bool SetProvider(std::string_view name) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (const auto& p : kProviders) {
            if (p.name == name) {
                g_configured = &p;
                return true;
            }
        }
        return false;
    }

    std::string_view ActiveProviderName() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_configured ? g_configured->name : "";
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
    }

    // ── fetch ─────────────────────────────────────────────────────

    static bool FetchProvider(const Provider& p, uint64_t gid, uint64_t* outCode) {
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();

        char urlLog[256];
        std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate, gid);

        auto r = OSTPlatform::Http::Execute(
            L"GET",
            urlLog,
            nullptr,
            0,
            p.headers,
            timeouts.resolve,
            timeouts.connect,
            timeouts.send,
            timeouts.recv);

        LOG_MANIFEST_INFO("Manifest {} status={} gid={}", p.name, r.status, gid);

        if (!r.ok || r.status != 200) return false;
        return p.parse(r.body, outCode);
    }

    static bool FetchWithFallback(uint64_t gid, uint64_t depotId, uint64_t* outCode) {
        size_t start = 0;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            const auto now = std::chrono::steady_clock::now();

            for (auto it = g_lastServed.begin(); it != g_lastServed.end();) {
                if (now - it->second.servedAt > kAttemptStateTtl) {
                    it = g_lastServed.erase(it);
                } else {
                    ++it;
                }
            }

            const ManifestKey key{gid, static_cast<uint32_t>(depotId)};
            const auto previous = g_lastServed.find(key);
            if (previous != g_lastServed.end()) {
                const auto age = now - previous->second.servedAt;
                if (age >= kRepeatRetryMinAge && age <= kRepeatRetryWindow) {
                    start = (previous->second.providerIndex + 1) % kProviderCount;
                    LOG_MANIFEST_INFO(
                        "Manifest repeat request depot={} gid={}: previous provider={} age_ms={} -> start={}",
                        depotId,
                        gid,
                        kProviders[previous->second.providerIndex].name,
                        std::chrono::duration_cast<std::chrono::milliseconds>(age).count(),
                        kProviders[start].name);
                } else if (g_configured) {
                    start = static_cast<size_t>(g_configured - kProviders);
                }
            } else if (g_configured) {
                start = static_cast<size_t>(g_configured - kProviders);
            }
        }

        for (size_t n = 0; n < kProviderCount; ++n) {
            const size_t i = (start + n) % kProviderCount;
            uint64_t seq = 0;

            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                const auto now = std::chrono::steady_clock::now();
                if (g_cooldown[i] > now) {
                    LOG_MANIFEST_DEBUG("Skip cooling provider {}", kProviders[i].name);
                    continue;
                }
                seq = g_okSeq[i];
            }

            if (FetchProvider(kProviders[i], gid, outCode)) {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                ++g_okSeq[i];
                g_cooldown[i] = {};
                g_lastServed[ManifestKey{gid, static_cast<uint32_t>(depotId)}] = {
                    i,
                    std::chrono::steady_clock::now()
                };
                return true;
            }

            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                if (g_okSeq[i] == seq) {
                    g_cooldown[i] = std::chrono::steady_clock::now() + kProviderCooldown;
                }
            }
        }
        return false;
    }

    // ── public ────────────────────────────────────────────────────

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return FetchWithFallback(manifestGid, depotId, outRequestCode);
    }
}
