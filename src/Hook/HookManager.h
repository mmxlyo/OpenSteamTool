#pragma once

namespace HookManager {
    void InstallUIBootstrap();
    void InstallUIHooks();
    void UninstallUIHooks();

    void InstallClientHooks();
    void UninstallClientHooks();

    void DetachWorkerThreads();
}
