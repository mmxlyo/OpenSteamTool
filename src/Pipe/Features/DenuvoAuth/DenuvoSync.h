#pragma once

#include "Pipe/PipeTypes.h"
#include <string>

namespace PipeManager::DenuvoAuth {

    // Called when CUser_SpawnProcess is intercepted in Hooks_Misc.
    // Checks the zero-operation gatekeeper (!hasLua && !hasDPlus -> returns immediately).
    // If -d+ is present, records the intent and triggers sync/generation if authorized.
    void OnSpawnProcess(AppId_t appId, const char* pExePath, const char* cmdLine);

    // Checks if the given app was launched with -d+ in this session.
    bool IsDPlusLaunch(AppId_t appId);

    // Clears the -d+ launch flag for the given app.
    void ClearDPlusLaunch(AppId_t appId);

    // Core synchronization / package generation function.
    // Guaranteed:
    // 1. Never writes .bin files to disk! (credentials/ only has SteamID.txt)
    // 2. Locks all installed manifests in <AppId>.lua (inserts/uncomments/updates setManifestid).
    // 3. Comments out -- setAppTicket and -- setETicket in <AppId>.lua.
    // 4. Overwrites SteamID.txt when the active account owns the game.
    bool SyncOrGenerate(AppId_t appId, const std::string& exePath, bool isDPlus);

    // Called when a live EncryptedAppTicket is captured from Steam Client for an owned game.
    // Refreshes the in-memory ETicket and updates the -- setETicket hex in <AppId>.lua.
    void OnEncryptedTicketCaptured(AppId_t appId, const uint8_t* data, size_t size);

} // namespace PipeManager::DenuvoAuth
