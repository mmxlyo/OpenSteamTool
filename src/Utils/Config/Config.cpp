#include "Config.h"
#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/ManifestClient.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <mutex>

namespace Config {
namespace {

    struct Snapshot {
        std::string manifestProvider = "opensteamtool";
        ManifestTimeouts manifestTimeouts;
        LogLevel logLevel = LogLevel::Debug;
        std::string logDir;
        std::vector<std::string> luaPaths;
        std::string remoteUrlTemplate;
        bool statsEnableApi = true;
        std::vector<InjectDll> injectDlls;
        CloudSettings cloud;
    };

    std::mutex g_mutex;
    bool g_loadedOnce = false;

    const char* ToString(LogLevel level) {
        switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        }
        return "???";
    }

    Snapshot MakeDefaultSnapshot(const std::string& configPath) {
        Snapshot snapshot;
        const char* storageDir = GetStorageDirectory();
        if (storageDir && storageDir[0] != '\0') {
            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                OSTPlatform::Encoding::PathFromUtf8(storageDir) / "opensteamtool");
        } else {
            std::filesystem::path p = OSTPlatform::Encoding::PathFromUtf8(configPath);
            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(p.parent_path() / "opensteamtool");
        }
        return snapshot;
    }

    void ApplySnapshot(const Snapshot& snapshot) {
        manifestTimeoutResolve = snapshot.manifestTimeouts.resolve;
        manifestTimeoutConnect = snapshot.manifestTimeouts.connect;
        manifestTimeoutSend    = snapshot.manifestTimeouts.send;
        manifestTimeoutRecv    = snapshot.manifestTimeouts.recv;
        logLevel               = snapshot.logLevel;
        logDir                 = snapshot.logDir;
        luaPaths               = snapshot.luaPaths;
        remoteUrlTemplate      = snapshot.remoteUrlTemplate;
        statsEnableApi         = snapshot.statsEnableApi;
        injectDlls             = snapshot.injectDlls;
        cloudEnabled           = snapshot.cloud.enabled;
        cloudLibrary           = snapshot.cloud.library;
    }

    void ApplyManifestProvider(const std::string& provider) {
        if (!ManifestClient::SetProvider(provider)) {
            LOG_WARN("Unknown manifest.url \"{}\", keeping default", provider);
            ManifestClient::SetProvider("opensteamtool");
        }
    }

    LoadResult ApplySnapshotLocked(const Snapshot& snapshot) {
        std::lock_guard lock(g_mutex);
        LoadResult result;
        result.luaPathsChanged = luaPaths != snapshot.luaPaths;
        ApplySnapshot(snapshot);
        g_loadedOnce = true;
        result.applied = true;
        return result;
    }

} // namespace

    LoadResult Load(const std::string& configPath) {
        Snapshot snapshot = MakeDefaultSnapshot(configPath);
        std::error_code ec;
        if (!std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(configPath), ec)) {
            LOG_INFO("Config file not found, using defaults");
            ApplyManifestProvider(snapshot.manifestProvider);
            LoadResult result = ApplySnapshotLocked(snapshot);
            LOG_INFO("Config loaded: manifest.url={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={}",
                     ManifestClient::ActiveProviderName(),
                     ToString(GetLogLevel()),
                     (uint32_t)GetLuaPaths().size(),
                     GetStatsEnableApi(),
                     GetRemoteUrlTemplate().empty() ? "<default>" : GetRemoteUrlTemplate());
            return result;
        }

        try {
            auto tbl = toml::parse_file(OSTPlatform::Encoding::PathFromUtf8(configPath).wstring());

            // [manifest]
            if (auto manifest = tbl["manifest"].as_table()) {
                if (auto val = (*manifest)["url"].value<std::string>()) {
                    snapshot.manifestProvider = *val;
                }
                if (auto val = (*manifest)["timeout_resolve_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.resolve = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_connect_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.connect = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_send_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.send = static_cast<uint32_t>(*val);
                if (auto val = (*manifest)["timeout_recv_ms"].value<int64_t>())
                    snapshot.manifestTimeouts.recv = static_cast<uint32_t>(*val);
            }

            // [log]
            if (auto log = tbl["log"].as_table()) {
                if (auto val = (*log)["level"].value<std::string>()) {
                    if (*val == "trace")           snapshot.logLevel = LogLevel::Trace;
                    else if (*val == "debug")       snapshot.logLevel = LogLevel::Debug;
                    else if (*val == "info")        snapshot.logLevel = LogLevel::Info;
                    else if (*val == "warn")        snapshot.logLevel = LogLevel::Warn;
                    else if (*val == "error")       snapshot.logLevel = LogLevel::Error;
                }
                if (auto val = (*log)["dir"].value<std::string>()) {
                    std::filesystem::path p = OSTPlatform::Encoding::PathFromUtf8(*val);
                    if (p.is_relative()) {
                        const char* storageDir = GetStorageDirectory();
                        if (storageDir && storageDir[0] != '\0') {
                            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                                OSTPlatform::Encoding::PathFromUtf8(storageDir) / p);
                        } else {
                            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                                OSTPlatform::Encoding::PathFromUtf8(configPath).parent_path() / p);
                        }
                    } else {
                        snapshot.logDir = *val;
                    }
                }
            }

            // [lua]
            if (auto lua = tbl["lua"].as_table()) {
                if (auto arr = (*lua)["paths"].as_array()) {
                    for (auto& elem : *arr) {
                        if (auto str = elem.value<std::string>()) {
                            snapshot.luaPaths.push_back(*str);
                        }
                    }
                }
            }

            // [remote]
            if (auto remote = tbl["remote"].as_table()) {
                if (auto val = (*remote)["url_template"].value<std::string>()) {
                    snapshot.remoteUrlTemplate = *val;
                }
            }

            // [stats]
            if (auto stats = tbl["stats"].as_table()) {
                if (auto val = (*stats)["enable_api"].value<bool>()) {
                    snapshot.statsEnableApi = *val;
                }
            }

            // [[inject]]
            if (auto arr = tbl["inject"].as_array()) {
                std::filesystem::path configDir = OSTPlatform::Encoding::PathFromUtf8(configPath).parent_path();
                for (auto& node : *arr) {
                    auto t = node.as_table();
                    if (!t) continue;
                    auto path = (*t)["path"].value<std::string>();
                    if (!path || path->empty()) continue;

                    // Relative paths resolve next to opensteamtool.toml, DLL dir, or steam.exe
                    std::filesystem::path full = OSTPlatform::Encoding::PathFromUtf8(*path);
                    if (full.is_relative()) {
                        std::filesystem::path candidate = configDir / full;
                        std::error_code ec;
                        if (std::filesystem::exists(candidate, ec)) {
                            full = candidate;
                        } else if (DllDir[0] != '\0' && std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(DllDir) / full, ec)) {
                            full = OSTPlatform::Encoding::PathFromUtf8(DllDir) / full;
                        } else if (SteamInstallPath[0] != '\0' && std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / full, ec)) {
                            full = OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / full;
                        } else {
                            full = candidate;
                        }
                    }
                    std::error_code ec;
                    if (!std::filesystem::exists(full, ec)) {
                        LOG_WARN("inject dll not found: {}", OSTPlatform::Encoding::PathToUtf8(full));
                        continue;
                    }

                    InjectDll dll;
                    dll.path = OSTPlatform::Encoding::PathToUtf8(full);
                    if (auto val = (*t)["when_cmdline"].value<std::string>()) dll.whenCmdline = *val;
                    if (auto val = (*t)["all_games"].value<bool>())           dll.allGames   = *val;
                    if (auto ids = (*t)["when_appids"].as_array())
                        for (auto& id : *ids)
                            if (auto v = id.value<int64_t>()) dll.whenAppids.insert(static_cast<AppId_t>(*v));
                    snapshot.injectDlls.push_back(std::move(dll));
                }
            }

            // [cloud]
            if (auto cloud = tbl["cloud"].as_table()) {
                if (auto val = (*cloud)["enabled"].value<bool>())
                    snapshot.cloud.enabled = *val;
                if (auto val = (*cloud)["library"].value<std::string>())
                    snapshot.cloud.library = *val;
            }

            ApplyManifestProvider(snapshot.manifestProvider);
            LoadResult result = ApplySnapshotLocked(snapshot);
            LOG_INFO("Config loaded: manifest.url={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={}",
                     ManifestClient::ActiveProviderName(),
                     ToString(snapshot.logLevel),
                     (uint32_t)snapshot.luaPaths.size(),
                     snapshot.statsEnableApi,
                     snapshot.remoteUrlTemplate.empty() ? "<default>" : snapshot.remoteUrlTemplate);
            return result;

        } catch (const toml::parse_error& e) {
            LOG_WARN("Config parse error: {}", e.what());
        } catch (...) {
            LOG_WARN("Config load failed");
        }
        bool shouldApplyDefault = false;
        {
            std::lock_guard lock(g_mutex);
            shouldApplyDefault = !g_loadedOnce;
        }
        if (shouldApplyDefault) {
            ApplyManifestProvider(snapshot.manifestProvider);
            std::lock_guard lock(g_mutex);
            const bool luaChanged = luaPaths != snapshot.luaPaths;
            ApplySnapshot(snapshot);
            g_loadedOnce = true;
            return {true, luaChanged};
        }
        return {};
    }

    ManifestTimeouts GetManifestTimeouts() {
        std::lock_guard lock(g_mutex);
        return {
            manifestTimeoutResolve,
            manifestTimeoutConnect,
            manifestTimeoutSend,
            manifestTimeoutRecv,
        };
    }

    LogLevel GetLogLevel() {
        std::lock_guard lock(g_mutex);
        return logLevel;
    }

    std::string GetLogDir() {
        std::lock_guard lock(g_mutex);
        return logDir;
    }

    std::vector<std::string> GetLuaPaths() {
        std::lock_guard lock(g_mutex);
        return luaPaths;
    }

    std::string GetRemoteUrlTemplate() {
        std::lock_guard lock(g_mutex);
        return remoteUrlTemplate;
    }

    bool GetStatsEnableApi() {
        std::lock_guard lock(g_mutex);
        return statsEnableApi;
    }

    CloudSettings GetCloudSettings() {
        std::lock_guard lock(g_mutex);
        return {
            cloudEnabled,
            cloudLibrary,
        };
    }

}
