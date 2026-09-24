#pragma once

#include "dllmain.h"

// Hooks targeting steamui.dll:

namespace Hooks_SteamUI {
    void Install();
    void Uninstall();

    // Queues an appId for removal from the library UI
    void QueueRemoval(AppId_t appId);
    // Cancels a queued removal when the app is added again before the UI drains it.
    void CancelRemoval(AppId_t appId);
    // Queues an appId for addition/restoration in the library UI
    void QueueAddition(AppId_t appId);
    // Checks if an appId is currently marked as removed in the UI
    bool IsRemoved(AppId_t appId);
}
