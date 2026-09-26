#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"

#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"
#include "Utils/Logging/Log.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Config/LuaConfig.h"
#include "Pipe/ProcessInspector.h"
#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "OSTPlatform/include/Process.h"
#include "OSTPlatform/include/Encoding.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

    bool ReadActiveSteamUser(uint32_t& accountId, std::wstring& universe) {
        constexpr const wchar_t* kActiveProcessKeyPath = L"Software\\Valve\\Steam\\ActiveProcess";
        constexpr const wchar_t* kValueActiveUser = L"ActiveUser";
        constexpr const wchar_t* kValueUniverse = L"Universe";

        DWORD activeUser = 0;
        DWORD bytes = sizeof(activeUser);
        LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueActiveUser,
                                      RRF_RT_REG_DWORD, nullptr, &activeUser, &bytes);
        if (status != ERROR_SUCCESS || bytes != sizeof(activeUser) || activeUser == 0) {
            return false;
        }

        bytes = 0;
        status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueUniverse,
                              RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
        if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
            return false;
        }

        std::wstring universeBuf(bytes / sizeof(wchar_t), L'\0');
        status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueUniverse,
                              RRF_RT_REG_SZ, nullptr, universeBuf.data(), &bytes);
        if (status != ERROR_SUCCESS) {
            return false;
        }

        while (!universeBuf.empty() && universeBuf.back() == L'\0') {
            universeBuf.pop_back();
        }
        if (universeBuf.empty()) {
            return false;
        }

        accountId = activeUser;
        universe = std::move(universeBuf);
        return true;
    }

} // namespace

    std::optional<uint64> GetCurrentActiveSteamId() {
        uint32 accountId = 0;
        std::wstring universeName;
        if (!ReadActiveSteamUser(accountId, universeName)) {
            LOG_PIPE_WARN("DenuvoAuth: active Steam user unavailable from registry");
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

namespace {

    struct ProcessAuth {
        bool scanned = false;
        bool denuvo = false;
        bool startupArmed = false;
        uint32 pid = 0;

        std::chrono::steady_clock::time_point authDeadline{};
        AppId_t authorizedAppId = k_uAppIdInvalid;

        std::string DebugString() const {
            const auto now = std::chrono::steady_clock::now();
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(authDeadline - now).count();
            return std::format("denuvo={} active={} remaining_ms={} auth_appid={} pid={}",
                               denuvo, now <= authDeadline, remainingMs > 0 ? remainingMs : 0, authorizedAppId, pid);
        }

        void OnHandshake(const PipeContext& ctx, const PipeKey& pipeKey) {
            if (ctx.appId != k_uAppIdInvalid) {
                authorizedAppId = ctx.appId;
            }
            pid = ctx.process.pid;

            if (denuvo) {
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
            }
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
        bool isNoDenuvo = LuaConfig::IsNoDenuvo(ctx.appId);
        bool isForcedDenuvo = LuaConfig::IsForcedDenuvo(ctx.appId);

        if (!isNoDenuvo && !isForcedDenuvo && ctx.process.pid != 0) {
            if (const auto cmd = OSTPlatform::Process::GetProcessCommandLine(ctx.process.pid)) {
                if (HasNoDenuvoArg(cmd->c_str())) {
                    isNoDenuvo = true;
                    LuaConfig::SetCmdLineNoDenuvo(ctx.appId, true);
                    LOG_PIPE_INFO("DenuvoAuth: detected -nodenuvo in process command line for appid={}", ctx.appId);
                } else if (HasForcedDenuvoArg(cmd->c_str())) {
                    isForcedDenuvo = true;
                    LuaConfig::SetCmdLineForcedDenuvo(ctx.appId, true);
                    LOG_PIPE_INFO("DenuvoAuth: detected -forcedenuvo in process command line for appid={}", ctx.appId);
                }
            }
        }

        if (isNoDenuvo) {
            denuvo = false;
            LOG_PIPE_INFO("DenuvoAuth: nodenuvo appid={} — skipping ProtectionScan and forcing non-Denuvo", ctx.appId);
        } else if (isForcedDenuvo) {
            denuvo = true;
            LOG_PIPE_INFO("DenuvoAuth: forcedenuvo appid={} — skipping ProtectionScan and forcing Denuvo", ctx.appId);
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

bool IsDenuvoPipe(const CPipeClient* pipe) {
    if (!pipe) return false;
    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    const ProcessAuth* auth = FindAuthForPipe(pipeKey);
    return auth && auth->denuvo;
}

AppId_t GetAuthorizedAppId(const CPipeClient* pipe) {
    if (!pipe) return k_uAppIdInvalid;
    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    const ProcessAuth* auth = FindAuthForPipe(pipeKey);
    return auth ? auth->authorizedAppId : k_uAppIdInvalid;
}

} // namespace PipeManager::DenuvoAuth
