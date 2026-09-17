#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <string_view>

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

    static std::atomic<const Provider*> g_active{&kProviders[0]};   // manifestdex

    bool SetProvider(std::string_view name) {
        for (const auto& p : kProviders) {
            if (p.name == name) { 
                g_active.store(&p, std::memory_order_release); 
                return true; 
            }
        }
        return false;
    }

    std::string_view ActiveProviderName() {
        const auto* p = g_active.load(std::memory_order_acquire);
        return p ? p->name : ""; 
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
    }

    // ── fetch ─────────────────────────────────────────────────────

    static bool FetchActive(uint64_t gid, uint64_t* outCode) {
        const auto* active = g_active.load(std::memory_order_acquire);
        if (!active) return false;
        const Provider& p = *active;
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

        return FetchActive(manifestGid, outRequestCode);
    }
}
