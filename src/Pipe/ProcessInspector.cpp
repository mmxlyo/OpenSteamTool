#include "Pipe/ProcessInspector.h"

#include "OSTPlatform/include/Process.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Logging/Log.h"
#include "Utils/Support/Stopwatch.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace ProcessInspector {
namespace {

    std::string BaseNameFromPath(const std::string& path) {
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos) return path;
        return path.substr(slash + 1);
    }

    std::optional<AppId_t> AppIdFromGameIdString(std::string_view value) {
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(value);
        if (!parsed) return std::nullopt;

        const AppId_t appId = static_cast<AppId_t>(*parsed & 0xFFFFFFu);
        return appId == k_uAppIdInvalid ? std::nullopt : std::optional<AppId_t>(appId);
    }

    std::optional<AppId_t> AppIdFromAppIdString(std::string_view value) {
        const auto parsed = OSTPlatform::Numbers::ParseUInt32(value);
        if (!parsed || *parsed == k_uAppIdInvalid) return std::nullopt;
        return *parsed;
    }

    std::optional<std::string> QueryProcessImagePath(PID_t pid) {
        return OSTPlatform::Process::GetImagePath(pid);
    }

} // namespace

std::optional<uint64> GetProcessCreationTime(PID_t pid) {
    return OSTPlatform::Process::GetCreationTime(pid);
}

bool IsSteamProcessName(std::string_view name) {
    for (std::string_view steamProc : kSteamProcessNames) {
        if (name.size() == steamProc.size() && _strnicmp(name.data(), steamProc.data(), name.size()) == 0)
            return true;
    }
    return false;
}

ProcessEnvironment ReadSteamEnvironment(PID_t pid) {
    ProcessEnvironment env{};
    if (auto value = OSTPlatform::Process::GetEnvironmentVariableValue(pid, L"SteamAppId")) {
        env.steamAppId = AppIdFromAppIdString(*value);
    }
    if (auto value = OSTPlatform::Process::GetEnvironmentVariableValue(pid, L"SteamGameId")) {
        env.steamGameIdAppId = AppIdFromGameIdString(*value);
    }
    if (auto value = OSTPlatform::Process::GetEnvironmentVariableValue(pid, L"SteamOverlayGameId")) {
        env.steamOverlayGameIdAppId = AppIdFromGameIdString(*value);
    }

    LOG_PIPE_DEBUG("ProcessInspector: pid={} steam env {}", pid, env.DebugString());
    return env;
}

ProcessSnapshot InspectProcess(PID_t pid) {
    const Utils::Stopwatch timer;
    ProcessSnapshot snapshot{};
    snapshot.pid = pid;
    snapshot.creationTime = GetProcessCreationTime(pid).value_or(0);

    if (auto imagePath = QueryProcessImagePath(pid)) {
        snapshot.imagePath = *imagePath;
        snapshot.imageName = BaseNameFromPath(snapshot.imagePath);
    }
    snapshot.steamClientProcess = IsSteamProcessName(snapshot.imageName);
    snapshot.environment = ReadSteamEnvironment(pid);
    snapshot.likelyGameProcess = !snapshot.steamClientProcess && snapshot.environment.HasSteamAppEnvironment();

    LOG_PIPE_INFO("ProcessInspector: inspected {} elapsed_ms={:.3f}",
                    snapshot.DebugString(), timer.ElapsedMs());
    return snapshot;
}

} // namespace ProcessInspector
