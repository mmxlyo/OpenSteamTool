#pragma once

#include "Pipe/PipeTypes.h"
#include <string>

namespace PipeManager::DenuvoAuth {

    // Checks whether cmdLine contains a specific argument (case-insensitive, token boundary aware).
    bool HasCmdLineArg(const char* cmdLine, const char* arg);

    // Checks whether cmdLine contains any variant of -nodenuvo (-nodenuvo, -no-denuvo, -no_denuvo).
    bool HasNoDenuvoArg(const char* cmdLine);

    // Checks whether cmdLine contains any variant of -forcedenuvo (-forcedenuvo, -force-denuvo, -force_denuvo).
    bool HasForcedDenuvoArg(const char* cmdLine);

    // Called when CUser_SpawnProcess is intercepted in Hooks_Misc.
    // Checks the zero-operation gatekeeper (!hasLua && !hasDPlus -> returns immediately).
    // If -d+ is present, records the intent and triggers sync/generation if authorized.
    void OnSpawnProcess(AppId_t appId, const char* pExePath, const char* cmdLine);

    // Checks if the given app was launched with -d+ in this session.
    bool IsDPlusLaunch(AppId_t appId);

    // Clears the -d+ launch flag for the given app.
    void ClearDPlusLaunch(AppId_t appId);

    // Core synchronization / package generation function (-d+ / genuine ownership).
    // Guaranteed:
    // 1. Never writes .bin files to disk! (credentials/ only has SteamID.txt)
    // 2. Locks all installed manifests in <AppId>.lua (inserts/uncomments/updates setManifestid).
    // 3. Persists SteamID.txt for the active account to support offline Denuvo identity.
    bool SyncOrGenerate(AppId_t appId, const std::string& exePath, bool isDPlus);

    // Called when a live EncryptedAppTicket is captured from Steam Client for an owned game.
    // Refreshes the in-memory ETicket in SteamCredentialStore.
    void OnEncryptedTicketCaptured(AppId_t appId, const uint8_t* data, size_t size);

    // Called when a live AppOwnershipTicket is captured from Steam Client (eMsg 858 or IPC) for an owned game.
    // Stores the ticket in memory in SteamCredentialStore and updates SteamID.txt.
    void OnOwnershipTicketCaptured(AppId_t appId, const uint8_t* data, size_t size);

} // namespace PipeManager::DenuvoAuth
