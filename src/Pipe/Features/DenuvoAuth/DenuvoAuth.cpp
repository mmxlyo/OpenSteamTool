#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"

#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"
#include "Utils/Logging/Log.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Config/LuaConfig.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Pipe/ProcessInspector.h"

#include <cwctype>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace PipeManager::DenuvoAuth {
namespace {

    constexpr uint32 kEndDenuvoVerificationHandshake = 2;

    enum class Stage {
        None,
        Authorizing,
        EndAuthorization,
    };

    const char* ToString(Stage stage) {
        switch (stage) {
        case Stage::None:             return "None";
        case Stage::Authorizing:      return "Authorizing";
        case Stage::EndAuthorization: return "EndAuthorization";
        }
        return "?";
    }

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

        const EUniverse universe = ParseUniverse(universeName);
        if (universe == k_EUniverseInvalid) {
            LOG_PIPE_WARN("DenuvoAuth: active Steam user has unsupported universe");
            return std::nullopt;
        }

        CSteamID steamId;
        steamId.Set(accountId, universe, k_EAccountTypeIndividual);
        return steamId.ConvertToUint64();
    }

    struct ProcessAuth {
        bool scanned = false;
        bool denuvo = false;
        Stage stage = Stage::None;
        uint32 pid = 0;
        uint32 handshakeCount = 0;

        std::optional<PipeKey> authorizationPipe;
        AppId_t authorizedAppId = k_uAppIdInvalid;

        std::string DebugString() const {
            return std::format("denuvo={} stage={} handshakeCount={} auth_appid={} pid={}",
                               denuvo, ToString(stage), handshakeCount, authorizedAppId, pid);
        }

        void OnHandshake(const PipeContext& ctx, const PipeKey& pipeKey) {
            ++handshakeCount;

            if (!denuvo) {
                stage = Stage::None;
                return;
            }

            if (!authorizationPipe.has_value()) {
                authorizationPipe = pipeKey;
                authorizedAppId = ctx.appId;
                pid = ctx.process.pid;
                stage = Stage::Authorizing;
                LOG_PIPE_INFO("DenuvoAuth: authorization pipe selected {}", this->DebugString());
            }

            if (stage == Stage::Authorizing &&
                handshakeCount >= kEndDenuvoVerificationHandshake) {
                stage = Stage::EndAuthorization;
                LOG_PIPE_INFO("DenuvoAuth: authorization window ended {}", this->DebugString());
                if(LuaConfig::IsOwned(authorizedAppId)){
                    WriteSteamIdOnEndAuthorization();
                }
            }
        }

        bool CanUseAuthorizedIdentity(const PipeKey& key) const {
            return denuvo && stage == Stage::Authorizing && authorizationPipe == key;
        }

        void WriteSteamIdOnEndAuthorization() const {
            if (authorizedAppId == k_uAppIdInvalid) {
                LOG_PIPE_WARN("DenuvoAuth: end authorization skipped SteamID persist without auth app");
                return;
            }

            const std::optional<uint64> steamId = GetCurrentSteamIdForDenuvoAuth();
            if (!steamId || *steamId == 0) {
                LOG_PIPE_WARN("DenuvoAuth: end authorization no current SteamID source auth_appid={}",
                               authorizedAppId);
                return;
            }

            if (AppTicket::WriteSteamID(authorizedAppId, *steamId)) {
                LOG_PIPE_INFO("DenuvoAuth: persisted end-authorization SteamID auth_appid={} steamid={}",
                              authorizedAppId, *steamId);
                return;
            }

            LOG_PIPE_WARN("DenuvoAuth: failed to persist end-authorization SteamID auth_appid={} steamid={}",
                          authorizedAppId, *steamId);
        }
    };

    std::mutex g_authMutex;
    std::unordered_map<ProcessKey, ProcessAuth, ProcessKeyHash> g_processAuth;
    std::unordered_map<PipeKey, ProcessKey, PipeKeyHash> g_pipeProcess;

    ProcessAuth* FindAuthForPipe(const PipeKey& pipeKey) {
        const auto pipeIt = g_pipeProcess.find(pipeKey);
        if (pipeIt == g_pipeProcess.end()) return nullptr;

        const auto authIt = g_processAuth.find(pipeIt->second);
        return authIt == g_processAuth.end() ? nullptr : &authIt->second;
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
        if (!auth.denuvo) auth.stage = Stage::None;
    } else {
        LOG_PIPE_TRACE("DenuvoAuth: reusing cached protection result {} denuvo={}",
                       ctx.process.DebugString(), auth.denuvo);
    }

    auth.OnHandshake(ctx, pipeKey);
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

    if (!auth->CanUseAuthorizedIdentity(pipeKey)) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not in authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
        return false;
    }
    LOG_PIPE_INFO("DenuvoAuth: pipe in authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
    return true;
}

} // namespace PipeManager::DenuvoAuth
