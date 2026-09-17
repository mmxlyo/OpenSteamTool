#include "Pipe/PipeManager.h"

#include "Pipe/PipeTypes.h"
#include "Pipe/ProcessInspector.h"
#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"
#include "Pipe/Features/Injection/Injection.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/LuaConfig.h"
#include "Hook/Hooks_Misc.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace PipeManager {
namespace {

    std::mutex g_processMutex;
    std::unordered_map<ProcessKey, ProcessInspector::ProcessSnapshot, ProcessKeyHash> g_processes;

    // steamclient doesn't always finish binding a brand-new pipe to its appid by
    // the literal handshake instant — observed empirically: GetAppIDForCurrentPipe
    // returns invalid at handshake time, then returns the correct appid ~20ms
    // later once the game's first real IPC call lands (e.g. Suicide Squad: KTJL).
    // OnHandshake only ever runs once per pipe, so a wrong trackedApp=false on
    // that single call permanently mis-tracks the process: DenuvoAuth::Apply
    // bails forever and never gets another chance. Retry briefly instead of
    // accepting the first sample.
    constexpr int kAppIdResolveRetries = 10;
    constexpr std::chrono::milliseconds kAppIdResolveRetryDelay{20};

    ProcessKey MakeProcessKey(const ProcessInspector::ProcessSnapshot& snapshot) {
        return ProcessKey{snapshot.pid, snapshot.creationTime};
    }

    std::optional<ProcessInspector::ProcessSnapshot> TryReuseCachedProcess(PID_t pid) {
        const auto currentCreationTime = ProcessInspector::GetProcessCreationTime(pid);
        if (!currentCreationTime) return std::nullopt;

        const ProcessKey currentKey{pid, *currentCreationTime};
        std::scoped_lock lock(g_processMutex);
        const auto it = g_processes.find(currentKey);
        if (it == g_processes.end()) return std::nullopt;

        LOG_PIPE_DEBUG("PipeManager: reusing cached process snapshot pid={} process={}",
                       pid, it->second.DebugString());
        return it->second;
    }

    ProcessInspector::ProcessSnapshot ResolveProcess(PID_t pid) {
        if (auto cached = TryReuseCachedProcess(pid)) return *cached;

        // First sighting: inspect once and cache so sibling pipes reuse it. A dead
        // process yields creationTime=0, so skip caching that junk key.
        const ProcessInspector::ProcessSnapshot snapshot = ProcessInspector::InspectProcess(pid);
        const ProcessKey processKey = MakeProcessKey(snapshot);
        if (processKey.IsValid()) {
            std::scoped_lock lock(g_processMutex);
            if (g_processes.size() >= 256) {
                std::erase_if(g_processes, [&](const auto& pair) {
                    if (pair.first == processKey) return false;
                    auto currentCreation = ProcessInspector::GetProcessCreationTime(pair.first.pid);
                    return !currentCreation || *currentCreation != pair.first.creationTime;
                });
            }
            g_processes[processKey] = snapshot;
        }
        return snapshot;
    }

    // Env-based appid first (cheap, and a missing env var will never appear no
    // matter how long we wait, so it's only tried once). Falls back to the
    // pipe's own appid, retrying briefly since that binding can lag the
    // handshake by a few milliseconds. Returns k_uAppIdInvalid if every
    // attempt comes up empty.
    AppId_t ResolveAppIdWithRetry(const ProcessInspector::ProcessSnapshot& snapshot, bool& outFromPipe) {
        outFromPipe = false;

        // Steam's own internal processes (steam.exe, steamwebhelper, etc.) are never games.
        if (snapshot.steamClientProcess) {
            return k_uAppIdInvalid;
        }

        const AppId_t envAppId = snapshot.ResolveAppId();
        if (envAppId != k_uAppIdInvalid) return envAppId;

        // Fall back to an explicit process-name mapping from addprocess() in LuaConfig
        // (checked before sleeping to avoid delay for configured processes).
        if (!snapshot.imageName.empty()) {
            const AppId_t configAppId = LuaConfig::GetAppIdForProcess(snapshot.imageName);
            if (configAppId != k_uAppIdInvalid) {
                LOG_PIPE_DEBUG("PipeManager: process-name config appid image={} appid={}",
                               snapshot.imageName, configAppId);
                return configAppId;
            }
        }

        // Check pipe appid immediately first without sleeping.
        const AppId_t initialPipeAppId = Hooks_Misc::ResolveAppId();
        if (initialPipeAppId != k_uAppIdInvalid) {
            outFromPipe = true;
            return initialPipeAppId;
        }

        // For non-game client processes without steam environment, limit retry count
        // to avoid stalling the IPC handshake thread for 200ms.
        const int maxRetries = snapshot.likelyGameProcess ? kAppIdResolveRetries : 2;
        for (int attempt = 1; attempt < maxRetries; ++attempt) {
            std::this_thread::sleep_for(kAppIdResolveRetryDelay);
            const AppId_t pipeAppId = Hooks_Misc::ResolveAppId();
            if (pipeAppId != k_uAppIdInvalid) {
                outFromPipe = true;
                LOG_PIPE_DEBUG("PipeManager: pipe appid resolved on retry attempt={} appid={}",
                               attempt, pipeAppId);
                return pipeAppId;
            }
        }

        return k_uAppIdInvalid;
    }

} // namespace

void OnHandshake(CPipeClient* pipe) {
    if (!pipe) {
        LOG_PIPE_INFO("PipeManager: ignore null pipe handshake");
        return;
    }

    const PipeKey pipeKey = MakePipeKey(pipe);
    if (pipeKey.pid == 0) {
        LOG_PIPE_DEBUG("PipeManager: ignore handshake with invalid pipe key {}", pipeKey.DebugString());
        return;
    }

    const ProcessInspector::ProcessSnapshot snapshot = ResolveProcess(pipeKey.pid);
    const ProcessKey processKey = MakeProcessKey(snapshot);

    if (!processKey.IsValid()) {
        LOG_PIPE_DEBUG("PipeManager: ignore handshake with invalid process key {} snapshot={}",
                       processKey.DebugString(), snapshot.DebugString());
        return;
    }

    // Env-based appid first; fall back to the steamclient pipe's appid (with a
    // short retry — see ResolveAppIdWithRetry) for games that launch WITHOUT
    // exporting SteamAppId (a launcher/child-process — e.g. Suicide Squad: KTJL,
    // which comes up SteamAppId=0). The pipe appid (GetAppIDForCurrentPipe) is
    // authoritative for this pipe, so without this those games never get
    // tracked and DenuvoAuth never runs (-> 88500012).
    bool appIdFromPipe = false;
    const AppId_t appId = ResolveAppIdWithRetry(snapshot, appIdFromPipe);
    const bool trackedApp = appId != k_uAppIdInvalid && LuaConfig::HasDepot(appId, false);

    // likelyGameProcess is env-derived (needs SteamAppId exported), so it's false
    // for env-less games. A pipe that resolves to a CONFIGURED depot is a tracked
    // game regardless, and DenuvoAuth::Apply requires gameProcess && trackedApp —
    // so treat a tracked depot as a game process even without the env.
    const bool gameProcess = snapshot.likelyGameProcess || trackedApp;

    PipeContext ctx{};
    ctx.pipe = pipe;
    ctx.process = processKey;
    ctx.appId = appId;
    ctx.gameProcess = gameProcess;
    ctx.trackedApp = trackedApp;
    ctx.owned = trackedApp && LuaConfig::IsOwned(appId);

    LOG_PIPE_INFO("PipeManager: handshake {} process={} appid={} appIdFromPipe={} gameProcess={} trackedApp={} snapshot={}",
                  pipeKey.DebugString(), processKey.DebugString(), appId,
                  appIdFromPipe, gameProcess, trackedApp, snapshot.DebugString());

    // Feature side effects run without holding the registry lock.
    DenuvoAuth::Apply(ctx);
    Injection::Apply(ctx);
}

} // namespace PipeManager
