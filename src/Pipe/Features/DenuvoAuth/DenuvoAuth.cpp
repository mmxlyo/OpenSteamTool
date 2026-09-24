#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"

#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"
#include "Utils/Logging/Log.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Config/LuaConfig.h"
#include "Pipe/ProcessInspector.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "OSTPlatform/include/Encoding.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace PipeManager::DenuvoAuth {
namespace {

    constexpr std::chrono::milliseconds kStartupGraceDuration{2500};
    constexpr std::chrono::milliseconds kTicketLeaseDuration{3000};

    bool EqualsUniverseName(std::wstring_view lhs, std::wstring_view rhs) {
        if (lhs.size() != rhs.size()) return false;

        for (size_t i = 0; i < lhs.size(); ++i) {
            if (std::towlower(lhs[i]) != std::towlower(rhs[i])) return false;
        }

        return true;
    }

    EUniverse ParseUniverse(std::wstring_view universe) {
        if (EqualsUniverseName(universe, L"Public")) return k_EUniversePublic;
        if (EqualsUniverseName(universe, L"Beta")) return k_EUniverseBeta;
        if (EqualsUniverseName(universe, L"Internal")) return k_EUniverseInternal;
        if (EqualsUniverseName(universe, L"Dev")) return k_EUniverseDev;
        return k_EUniverseInvalid;
    }

    std::optional<uint64> GetCurrentSteamIdForDenuvoAuth() {
        uint32 accountId = 0;
        std::wstring universeName;
        const auto status = OSTPlatform::SteamCredentialStore::GetActiveUser(accountId, universeName);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_PIPE_WARN("DenuvoAuth: active Steam user unavailable ({})",
                           OSTPlatform::SteamCredentialStore::ToString(status));
            return std::nullopt;
        }

        EUniverse universe = ParseUniverse(universeName);
        if (universe == k_EUniverseInvalid) {
            LOG_PIPE_WARN("DenuvoAuth: active Steam user has unrecognized universe '{}', defaulting to Public",
                           OSTPlatform::Encoding::WideToUtf8(universeName));
            universe = k_EUniversePublic;
        }

        CSteamID steamId;
        steamId.Set(accountId, universe, k_EAccountTypeIndividual);
        return steamId.ConvertToUint64();
    }

    struct ProcessAuth {
        bool scanned = false;
        bool denuvo = false;
        bool startupArmed = false;
        bool steamIdPersisted = false;
        uint32 pid = 0;

        std::chrono::steady_clock::time_point authDeadline{};
        AppId_t authorizedAppId = k_uAppIdInvalid;

        std::string DebugString() const {
            const auto now = std::chrono::steady_clock::now();
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(authDeadline - now).count();
            return std::format("denuvo={} active={} remaining_ms={} auth_appid={} pid={} persisted={}",
                               denuvo, now <= authDeadline, remainingMs > 0 ? remainingMs : 0, authorizedAppId, pid, steamIdPersisted);
        }

        void OnHandshake(const PipeContext& ctx, const PipeKey& pipeKey) {
            if (!denuvo) return;

            if (ctx.appId != k_uAppIdInvalid) {
                authorizedAppId = ctx.appId;
            }
            pid = ctx.process.pid;

            // Startup grace period: arms 2500ms upon initial connection so Denuvo has ample
            // time to verify local offline tokens or launch handshake without leaking real SteamID (avoiding 88500012).
            if (!startupArmed) {
                startupArmed = true;
                const auto now = std::chrono::steady_clock::now();
                const auto startupDeadline = now + kStartupGraceDuration;
                if (startupDeadline > authDeadline) {
                    authDeadline = startupDeadline;
                }
                LOG_PIPE_INFO("DenuvoAuth: startup grace window armed for pid={} (+2500ms) {}", pid, this->DebugString());
            }

            TryPersistSteamId();
        }

        void ExtendTicketLease() {
            if (!denuvo) return;

            // When a ticket is fetched/requested, extend the lease by 3000ms.
            // Guarantees Denuvo's memcmp(Ticket->SteamID, GetSteamID()) verification easily passes (avoiding Error 54).
            const auto now = std::chrono::steady_clock::now();
            const auto newDeadline = now + kTicketLeaseDuration;
            if (newDeadline > authDeadline) {
                authDeadline = newDeadline;
                LOG_PIPE_INFO("DenuvoAuth: ticket lease extended for pid={} (+3000ms) {}", pid, this->DebugString());
            }

            TryPersistSteamId();
        }

        void TryPersistSteamId() {
            if (steamIdPersisted || !denuvo || authorizedAppId == k_uAppIdInvalid || authorizedAppId == 0) return;
            if (!LuaConfig::IsOwned(authorizedAppId)) return;

            const std::optional<uint64> steamId = GetCurrentSteamIdForDenuvoAuth();
            if (!steamId || *steamId == 0) {
                LOG_PIPE_WARN("DenuvoAuth: failed to get active SteamID for auth_appid={}", authorizedAppId);
                return;
            }

            if (AppTicket::WriteSteamID(authorizedAppId, *steamId)) {
                steamIdPersisted = true;
                LOG_PIPE_INFO("DenuvoAuth: persisted SteamID for auth_appid={} steamid={}", authorizedAppId, *steamId);
            } else {
                LOG_PIPE_WARN("DenuvoAuth: failed to persist SteamID for auth_appid={} steamid={}", authorizedAppId, *steamId);
            }
        }

        bool CanUseAuthorizedIdentity() const {
            if (!denuvo) return false;
            return std::chrono::steady_clock::now() <= authDeadline;
        }
    };

    std::mutex g_authMutex;
    std::unordered_map<ProcessKey, ProcessAuth, ProcessKeyHash> g_processAuth;
    std::unordered_map<PipeKey, ProcessKey, PipeKeyHash> g_pipeProcess;

    ProcessAuth* FindAuthForPipe(const PipeKey& pipeKey) {
        const auto pipeIt = g_pipeProcess.find(pipeKey);
        if (pipeIt != g_pipeProcess.end()) {
            const auto authIt = g_processAuth.find(pipeIt->second);
            if (authIt != g_processAuth.end()) return &authIt->second;
        }

        // Resilient fallback: match by active process if the specific pipe handle
        // was not handshaked yet or was evicted from g_pipeProcess
        if (pipeKey.pid != 0) {
            if (const auto currentCreation = ProcessInspector::GetProcessCreationTime(pipeKey.pid)) {
                const ProcessKey activeKey{pipeKey.pid, *currentCreation};
                const auto authIt = g_processAuth.find(activeKey);
                if (authIt != g_processAuth.end()) return &authIt->second;
            }
        }

        return nullptr;
    }

} // namespace

void Apply(const PipeContext& ctx) {
    if (!ctx.gameProcess || !ctx.trackedApp) return;

    const PipeKey pipeKey = MakePipeKey(ctx.pipe);
    if (!pipeKey.IsValid()) return;

    bool needsScan = false;
    {
        std::lock_guard lock(g_authMutex);
        auto it = g_processAuth.find(ctx.process);
        if (it == g_processAuth.end() || !it->second.scanned) {
            needsScan = true;
        }
    }

    bool denuvo = false;
    if (needsScan) {
        if (LuaConfig::IsNoDenuvo(ctx.appId)) {
            denuvo = false;
            LOG_PIPE_INFO("DenuvoAuth: nodenuvo appid={} — skipping ProtectionScan and forcing non-Denuvo", ctx.appId);
        } else if (LuaConfig::IsForcedDenuvo(ctx.appId)) {
            denuvo = true;
            LOG_PIPE_INFO("DenuvoAuth: forcedenuvo appid={} — skipping ProtectionScan", ctx.appId);
        } else {
            denuvo = ScanProtection(ctx.process.pid).denuvoDetected;
        }
    }

    std::lock_guard lock(g_authMutex);
    if (g_processAuth.size() >= 256) {
        std::erase_if(g_processAuth, [&](const auto& pair) {
            if (pair.first == ctx.process) return false;
            auto currentCreation = ProcessInspector::GetProcessCreationTime(pair.first.pid);
            return !currentCreation || *currentCreation != pair.first.creationTime;
        });
        std::erase_if(g_pipeProcess, [&](const auto& pair) {
            return !g_processAuth.contains(pair.second);
        });
    }
    if (g_pipeProcess.size() >= 512) {
        std::erase_if(g_pipeProcess, [&](const auto& pair) {
            if (pair.second == ctx.process) return false;
            return !g_processAuth.contains(pair.second);
        });
    }

    ProcessAuth& auth = g_processAuth[ctx.process];
    g_pipeProcess[pipeKey] = ctx.process;

    if (needsScan && !auth.scanned) {
        auth.scanned = true;
        auth.denuvo = denuvo;
    } else {
        LOG_PIPE_TRACE("DenuvoAuth: reusing cached protection result {} denuvo={}",
                       ctx.process.DebugString(), auth.denuvo);
    }

    auth.OnHandshake(ctx, pipeKey);
}

void OnTicketRequested(const CPipeClient* pipe, AppId_t appId) {
    std::lock_guard lock(g_authMutex);
    if (pipe) {
        const PipeKey pipeKey = MakePipeKey(pipe);
        ProcessAuth* auth = FindAuthForPipe(pipeKey);
        if (auth) {
            if (auth->denuvo) {
                if (auth->authorizedAppId == k_uAppIdInvalid && appId != k_uAppIdInvalid) {
                    auth->authorizedAppId = appId;
                }
                auth->ExtendTicketLease();
            }
            return;
        }
    }

    if (appId == k_uAppIdInvalid) return;

    for (auto& [procKey, auth] : g_processAuth) {
        if (auth.denuvo && (auth.authorizedAppId == appId || auth.authorizedAppId == k_uAppIdInvalid)) {
            if (auth.authorizedAppId == k_uAppIdInvalid) {
                auth.authorizedAppId = appId;
            }
            auth.ExtendTicketLease();
        }
    }
}

bool IsAuthorizedPipe(const CPipeClient* pipe) {
    if (!pipe) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not in authorization window: null pipe");
        return false;
    }

    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    const ProcessAuth* auth = FindAuthForPipe(pipeKey);
    if (!auth) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not tracked by DenuvoAuth {}", pipeKey.DebugString());
        return false;
    }

    if (!auth->CanUseAuthorizedIdentity()) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe outside authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
        return false;
    }
    LOG_PIPE_DEBUG("DenuvoAuth: pipe in authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
    return true;
}

} // namespace PipeManager::DenuvoAuth
