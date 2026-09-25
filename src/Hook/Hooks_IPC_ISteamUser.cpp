#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "PendingAPICalls.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Tickets/EticketClient.h"
#include "Pipe/PipeManager.h"
#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"
#include "Utils/Logging/Log.h"
#include "Hooks_Misc.h"
#include "Utils/Config/LuaConfig.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
    using namespace IPCMessages::IClientUser;

    // Fresh, nonce-bound etickets minted on-demand in RequestEncryptedAppTicket
    // (keyed by appId) and consumed by GetEncryptedAppTicket on the same launch.
    // Lets the strict-Denuvo path serve a ticket matching the launch nonce while
    // keeping GetEncryptedAppTicket's credential-store serve as the fallback.
    std::mutex g_freshEticketMutex;
    std::unordered_map<AppId_t, std::vector<uint8_t>> g_freshEticket;

    // [Post-Handler]: IClientUser::GetSteamID
    void HandlerPost_IClientUser_GetSteamID(CPipeClient* pipe,CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        AppId_t appId = Hooks_Misc::ResolveAppId();
        if (appId == 0 || !LuaConfig::HasDepot(appId)) return;
        GetSteamIDResp resp{pWrite};
        if (!resp.ok()) return;

        // Only spoof SteamID when the pipe belongs to a Denuvo process currently
        // within its adaptive authorization window (startup grace period or ticket lease).
        // Outside the authorization window, the user's real SteamID is preserved so that
        // save paths (ISteamUser::GetUserDataFolder / userdata/<RealAccountID>/) remain
        // bound to the user's actual account rather than the pool account.
        if (!PipeManager::DenuvoAuth::IsAuthorizedPipe(pipe)) {
            LOG_IPC_TRACE("IClientUser::GetSteamID: AppId={} outside auth window, keeping real SteamID {}",
                          appId, resp.DebugString());
            return;
        }

        const uint64 spoofed = AppTicket::GetSpoofSteamID(appId);
        if (!spoofed) {
            return;
        }

        LOG_IPC_DEBUG("IClientUser::GetSteamID: AppId={} [Auth-Window Active] Original: {} -> Spoofed: 0x{:X}({})", 
                      appId, resp.DebugString(), spoofed, spoofed);
        resp.set_returnValue(spoofed);
    }

    // [Post-Handler]: IClientUser::GetAppOwnershipTicketExtendedData
    void HandlerPost_IClientUser_GetAppOwnershipTicketExtendedData(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        GetAppOwnershipTicketExtendedDataReq req{pRead};
        if (!req.ok()) return;

        LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData:{}", req.DebugString());
        if (req.cbMaxTicket() < 0) return;

        AppId_t appId = req.unAppID() == kOnlineFixAppId ? Hooks_Misc::ResolveAppId() : req.unAppID();
        if (appId == 0) return;

        // If Steam's genuine implementation already returned a valid ticket (account owns the game),
        // leave it untouched and pass through cleanly.
        GetAppOwnershipTicketExtendedDataResp origResp{pWrite, static_cast<size_t>(req.cbMaxTicket())};
        if (origResp.ok() && origResp.returnValue() > 0) {
            LuaConfig::MarkOwned(appId);

            if (LuaConfig::HasDepot(appId, false)) {
                uint64_t steamId = 0;
                auto ticketSpan = origResp.pTicket();
                if (!ticketSpan.empty()) {
                    const size_t ticketSize = (std::min)(ticketSpan.size(), static_cast<size_t>(origResp.returnValue()));
                    steamId = AppTicket::ExtractSteamIdFromTicketBytes(ticketSpan.first(ticketSize));
                }
                if (steamId == 0) {
                    if (const auto active = PipeManager::DenuvoAuth::GetCurrentActiveSteamId(); active) {
                        steamId = *active;
                    }
                }
                if (steamId != 0) {
                    AppTicket::WriteSteamID(appId, steamId);
                    LOG_IPC_DEBUG("GetAppOwnershipTicketExtendedData: genuine ticket for appId={} -> persisted SteamID.txt: {}", appId, steamId);
                }
            }

            PipeManager::DenuvoAuth::OnTicketRequested(pipe, appId);
            return;
        }

        if (!LuaConfig::HasDepot(appId)) return;

        AppTicket::AppOwnershipTicket ticket{};
        // Refresh the Denuvo authorization lease window when an ownership ticket is requested.
        PipeManager::DenuvoAuth::OnTicketRequested(pipe, appId);
        
        AppTicket::AppTicketSource ticketSource;
        if (PipeManager::DenuvoAuth::IsAuthorizedPipe(pipe)) {
            ticketSource = AppTicket::AppTicketSource::CredentialStoreOnly;
        } else {
            // Outside the auth window: prefer credential-store ticket (pool SteamID)
            // over ForgeOnly (which uses app 7's ticket and carries the real SteamID).
            // When the 858 network spoof is also active, both paths must agree on the
            // same SteamID or Denuvo cross-checks them and rejects (error 54).
            LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} not in authorization window, credential store preferred", appId);
            ticketSource = AppTicket::AppTicketSource::CredentialStoreThenForge;
        }        
        if (!AppTicket::GetAppOwnershipTicket(appId, ticket, ticketSource)) return;

        if (ticket.data.size() > static_cast<size_t>(req.cbMaxTicket())) {
            LOG_IPC_WARN("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} ticket too large ({} bytes) for buffer ({} bytes)",
                         appId, ticket.data.size(), req.cbMaxTicket());
            return;
        }

        const uint32 requiredCapacity = static_cast<uint32>(sizeof(uint32))           // EIPCResult
                                      + static_cast<uint32>(sizeof(uint32))           // returnValue
                                      + static_cast<uint32>(req.cbMaxTicket())        // pTicket
                                      + static_cast<uint32>(sizeof(uint32) * 4);      // piAppId, piSteamId, piSignature, pcbSignature
        if (pWrite && pWrite->m_Put < static_cast<int>(requiredCapacity)) {
            if (!Hooks_Misc::EnsureBufferCapacity(pWrite, requiredCapacity, true)) {
                return;
            }
        }

        GetAppOwnershipTicketExtendedDataResp resp{pWrite, static_cast<size_t>(req.cbMaxTicket())};
        if (!resp.ok()) return;

        resp.set_returnValue(ticket.totalSize);
        if (!resp.set_pTicket(ticket.data)) return;
        resp.set_piAppId(ticket.appIdOffset);
        resp.set_piSteamId(ticket.steamIdOffset);
        resp.set_piSignature(ticket.signatureOffset);
        resp.set_pcbSignature(ticket.signatureSize);

        LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} {}", 
                        appId,resp.DebugString());
    }

    // [Post-Handler]: IClientUser::RequestEncryptedAppTicket
    // Reads the hAsyncCall steamclient already wrote into the response,
    // so we know which AppId to mint an eticket for in GetAPICallResult.
    void HandlerPost_IClientUser_RequestEncryptedAppTicket(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        RequestEncryptedAppTicketResp resp{pWrite};
        if (!resp.ok()) return;

        AppId_t appId = Hooks_Misc::ResolveAppId();
        if (appId == 0 || !LuaConfig::HasDepot(appId)) return;

        // Refresh the Denuvo authorization lease window when an encrypted ticket is requested.
        PipeManager::DenuvoAuth::OnTicketRequested(pipe, appId);

        bool haveFresh = false;
        // Strict Denuvo passes a per-launch nonce (pData) here and rejects a
        // stale/cached ticket (88500012). Try an on-demand mint bound to that
        // exact nonce; cache it for GetEncryptedAppTicket. Any failure falls
        // through to the static credential store below.
        {
            RequestEncryptedAppTicketReq req{pRead};
            std::span<const uint8_t> nonce;
            if (req.ok()) nonce = req.pData();
            // Whatever account the credential store's current static ticket already
            // belongs to (0 if none) — lets the backend pin the mint to that
            // SAME account instead of risking a different pool pick.
            const uint64_t existingSteamId = AppTicket::GetTicketSteamID(appId);
            // Mint a fresh eticket whenever the credential store already has a ticket
            // for this app (existingSteamId != 0). The minted eticket is pinned to
            // the same pool account via existingSteamId, which matches GetSteamID's
            // spoof (also sourced from the credential store via CredentialStoreThenForge)
            // — no error-54 risk. This fixes error 05 for games launched more than
            // 30 min after activation (stored ticket expired, fresh mint is current).
            if (!LuaConfig::IsOwned(appId) && existingSteamId != 0) {
                if (auto fresh = EticketClient::FetchFreshEticket(appId, nonce, existingSteamId)) {
                    std::lock_guard<std::mutex> lock(g_freshEticketMutex);
                    g_freshEticket[appId] = std::move(*fresh);
                    haveFresh = true;
                }
            }
        }

        if (!haveFresh) {
            std::lock_guard<std::mutex> lock(g_freshEticketMutex);
            haveFresh = g_freshEticket.find(appId) != g_freshEticket.end();
        }

        std::vector<uint8_t> ticket = AppTicket::GetEncryptedTicketFromCredentialStore(appId);
        if (ticket.empty() && !haveFresh) {
            LOG_IPC_DEBUG("RequestEncryptedAppTicket: AppId={} - no cached eticket, skip", appId);
            return;
        }

        const SteamAPICall_t hAsyncCall = resp.returnValue();
        PendingAPICalls::RecordEncryptedTicket(hAsyncCall, appId);
        LOG_IPC_DEBUG("RequestEncryptedAppTicket: AppId={} hAsyncCall=0x{:X} - recorded (fresh={})",
                      appId, hAsyncCall, haveFresh);
    }

    // [Post-Handler]: IClientUser::GetEncryptedAppTicket
    void HandlerPost_IClientUser_GetEncryptedAppTicket(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        AppId_t appId = Hooks_Misc::ResolveAppId();
        if (appId == 0 || !LuaConfig::HasDepot(appId, false)) return;

        // Refresh the Denuvo authorization lease window when reading the encrypted ticket.
        PipeManager::DenuvoAuth::OnTicketRequested(pipe, appId);

        // 1. If Steam client returned a genuine encrypted ticket (e.g. authorized account):
        GetEncryptedAppTicketResp existingResp{pWrite};
        if (existingResp.ok() && existingResp.returnValue()) {
            auto ticketSpan = existingResp.pTicket();
            if (!ticketSpan.empty() || existingResp.pcbTicket() > 0) {
                LuaConfig::MarkOwned(appId);
                if (const auto active = PipeManager::DenuvoAuth::GetCurrentActiveSteamId(); active) {
                    AppTicket::WriteSteamID(appId, *active);
                    LOG_IPC_DEBUG("GetEncryptedAppTicket: genuine ticket for appId={}, persisted SteamID.txt: {}", appId, *active);
                }
                return;
            }
        }

        // For owned games, Steam client natively manages genuine tickets.
        // If Steam returned no ticket (e.g. offline/network error), never overwrite pWrite with cached/stale ticket.
        if (LuaConfig::IsOwned(appId)) {
            return;
        }

        // 2. Fallback to cached or freshly minted tickets (offline / unowned accounts)
        std::vector<uint8_t> ticket;
        {
            std::lock_guard<std::mutex> lock(g_freshEticketMutex);
            auto it = g_freshEticket.find(appId);
            if (it != g_freshEticket.end()) ticket = it->second;
        }
        const bool fromFresh = !ticket.empty();
        if (ticket.empty()) {
            ticket = AppTicket::GetEncryptedTicketFromCredentialStore(appId);
        }
        if (ticket.empty()) {
            LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} - no cached eticket, skip", appId);
            return;
        }
        LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} serving source={}", appId, fromFresh ? "fresh" : "store");

        uint32 ticketSize = static_cast<uint32>(ticket.size());
        const uint32 requiredCapacity = static_cast<uint32>(sizeof(uint32))   // EIPCResult
                                      + static_cast<uint32>(sizeof(bool))     // returnValue
                                      + static_cast<uint32>(sizeof(uint32))   // pcbTicket
                                      + ticketSize;                           // pTicket
        if (pWrite && pWrite->m_Put < static_cast<int>(requiredCapacity)) {
            if (!Hooks_Misc::EnsureBufferCapacity(pWrite, requiredCapacity, true)) {
                LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} - failed to ensure buffer size", appId);
                return;
            }
        }

        GetEncryptedAppTicketResp resp{pWrite};
        if (!resp.ok()) return;

        resp.set_returnValue(true);
        resp.set_pcbTicket(ticketSize);
        if (!resp.set_pTicket(ticket)) return;

        LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} {}", appId, resp.DebugString());
    }

} // namespace

namespace Hooks_IPC_ISteamUser {
    void Register() {
        IPCHandlerEntry UserEntries[] = {
            ADD_IPC_POST_HANDLER(IClientUser, GetSteamID),
            ADD_IPC_POST_HANDLER(IClientUser, GetAppOwnershipTicketExtendedData),
            ADD_IPC_POST_HANDLER(IClientUser, RequestEncryptedAppTicket),
            ADD_IPC_POST_HANDLER(IClientUser, GetEncryptedAppTicket),
        };
        Hooks_IPC::RegisterHandlers(UserEntries);
    }
}
