#include "RemoteToml.h"
#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace RemoteToml {

namespace {
    constexpr const char* kGithubTemplate =
        "https://raw.githubusercontent.com/OpenSteam001/steam-monitor/"
        "{channel}/{component}/{sha256}.toml";
    constexpr const char* kJsdelivrTemplate =
        "https://cdn.jsdelivr.net/gh/OpenSteam001/steam-monitor@"
        "{channel}/{component}/{sha256}.toml";

    static bool HasPlaceholder(std::string_view text, std::string_view placeholder)
    {
        return text.find(placeholder) != std::string_view::npos;
    }

    static bool IsValidTemplate(std::string_view urlTemplate)
    {
        return HasPlaceholder(urlTemplate, "{channel}") &&
               HasPlaceholder(urlTemplate, "{component}") &&
               HasPlaceholder(urlTemplate, "{sha256}");
    }

    static void ReplaceAll(std::string& text,
                           std::string_view from,
                           std::string_view to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string ExpandTemplate(std::string urlTemplate,
                                      const Request& request,
                                      std::string_view sha256)
    {
        ReplaceAll(urlTemplate, "{channel}", request.channel);
        ReplaceAll(urlTemplate, "{component}", request.component);
        ReplaceAll(urlTemplate, "{sha256}", sha256);
        return urlTemplate;
    }

    static std::vector<std::string> BuildUrlTemplates()
    {
        const std::string remoteUrlTemplate = Config::GetRemoteUrlTemplate();
        if (remoteUrlTemplate.empty())
            return { kGithubTemplate, kJsdelivrTemplate };

        if (!IsValidTemplate(remoteUrlTemplate)) {
            LOG_WARN("RemoteToml: remote.url_template must contain "
                     "{channel}, {component}, and {sha256}; remote fetch disabled");
            return {};
        }

        return { remoteUrlTemplate };
    }
} // namespace

Result Fetch(const Request& request)
{
    namespace fs = std::filesystem;
    using OSTPlatform::Encoding::PathFromUtf8;
    using OSTPlatform::Encoding::PathToUtf8;
    Result out;

    // 1. SHA-256 of the DLL.
    const auto hashStart = std::chrono::steady_clock::now();
    out.sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    const auto hashMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hashStart).count();

    if (out.sha256.empty()) {
        LOG_WARN("RemoteToml({}/{}): Sha256OfFile failed for {} ({} ms)",
                 request.channel, request.component, request.dllPath, hashMs);
        return out;
    }
    LOG_INFO("RemoteToml({}/{}): sha256 = {} ({} ms)",
             request.channel, request.component, out.sha256, hashMs);

    // 2. Cache path & dir.
    fs::path steamRoot = PathFromUtf8(request.dllPath).parent_path();
    fs::path baseDir = PathFromUtf8(GetStorageDirectory());
    if (baseDir.empty()) {
        baseDir = steamRoot;
    }
    fs::path cacheDir  = baseDir / "opensteamtool" / request.channel / request.component;
    fs::path cachePath = cacheDir / (out.sha256 + ".toml");

    std::error_code mkdirEc;
    fs::create_directories(cacheDir, mkdirEc);
    if (mkdirEc) {
        LOG_WARN("RemoteToml({}/{}): could not create cache dir {} ({})",
                 request.channel, request.component, PathToUtf8(cacheDir), mkdirEc.message());
    }

    // Check local cache first (pattern & IPC files are immutable per DLL SHA-256).
    fs::path localPath = cachePath;
    std::error_code ec;
    if (!fs::exists(localPath, ec) && IsPortableMode()) {
        fs::path steamCachePath = steamRoot / "opensteamtool" / request.channel / request.component / (out.sha256 + ".toml");
        if (fs::exists(steamCachePath, ec)) {
            localPath = steamCachePath;
        }
    }

    ec.clear();
    if (fs::exists(localPath, ec) && !ec) {
        const auto sz = fs::file_size(localPath, ec);
        if (!ec && sz > 0) {
            std::ifstream ifs(localPath, std::ios::binary);
            if (ifs) {
                std::string buf((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
                if (!buf.empty()) {
                    LOG_INFO("RemoteToml({}/{}): loaded from cache {}",
                             request.channel, request.component, PathToUtf8(localPath));
                    out.body = std::move(buf);
                    out.ok = true;
                    out.fromCache = true;
                    return out;
                }
            }
            LOG_WARN("RemoteToml({}/{}): cache file exists but failed to read or empty: {}",
                     request.channel, request.component, PathToUtf8(localPath));
        }
    }

    // 3. Cache miss -> Try remote (mirror chain with early-out on 404).
    const std::vector<std::string> urlTemplates = BuildUrlTemplates();
    OSTPlatform::Http::Result http;
    std::string lastUrl;

    for (size_t i = 0; i < urlTemplates.size(); ++i) {
        lastUrl = ExpandTemplate(urlTemplates[i], request, out.sha256);
        LOG_INFO("RemoteToml({}/{}): downloading {}",
                 request.channel, request.component, lastUrl);

        http = OSTPlatform::Http::Execute(L"GET", lastUrl.c_str(),
                                          nullptr, 0, nullptr);

        if (http.ok && http.status == 200) break;

        if (http.ok && http.status == 404) {
            LOG_WARN("RemoteToml({}/{}): mirror has no such file (HTTP 404): {}",
                     request.channel, request.component, lastUrl);
            break;   // all mirrors serve same data
        }

        if (i + 1 < urlTemplates.size()) {
            LOG_WARN("RemoteToml({}/{}): mirror failed ({} ok={} HTTP={}), falling back",
                     request.channel, request.component, lastUrl, http.ok, http.status);
        }
    }

    // 4. Remote OK → write cache, return body.
    if (http.ok && http.status == 200 && !http.body.empty()) {
        std::ofstream ofs(cachePath, std::ios::binary);
        if (ofs) {
            ofs.write(http.body.data(),
                      static_cast<std::streamsize>(http.body.size()));
            LOG_INFO("RemoteToml({}/{}): cached to {}",
                     request.channel, request.component, PathToUtf8(cachePath));
        } else {
            LOG_WARN("RemoteToml({}/{}): could not open {} for writing",
                     request.channel, request.component, PathToUtf8(cachePath));
        }
        out.body = std::move(http.body);
        out.ok = true;
        return out;
    }

    // 5. Total failure — caller handles popup / degraded mode.
    LOG_WARN("RemoteToml({}/{}): no source available (last URL: {} HTTP {})",
             request.channel, request.component,
             lastUrl.empty() ? "<none>" : lastUrl, http.status);
    return out;
}

} // namespace RemoteToml
