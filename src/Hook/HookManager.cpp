#include "HookManager.h"
#include "Hooks_Decryption.h"
#include "Hooks_IPC.h"
#include "Hooks_Manifest.h"
#include "Hooks_Misc.h"
#include "Hooks_NetPacket.h"
#include "Hooks_Package.h"
#include "Hooks_SteamUI.h"
#include "Utils/HookSupport/VehCommon.h"

namespace HookManager {

    void InstallUIHooks() {
        Hooks_SteamUI::Install();
    }

    void UninstallUIHooks() {
        Hooks_SteamUI::Uninstall();
    }

    void InstallClientHooks() {
        Hooks_Decryption::Install();
        Hooks_IPC::Install();
        Hooks_Manifest::Install();
        Hooks_Misc::Install();
        Hooks_NetPacket::Install();
        Hooks_Package::Install();
    }

    void UninstallClientHooks() {
        Hooks_Decryption::Uninstall();
        Hooks_IPC::Uninstall();
        Hooks_Manifest::Uninstall();
        Hooks_Misc::Uninstall();
        Hooks_NetPacket::Uninstall();
        Hooks_Package::Uninstall();
        VehCommon::DisarmAll();
        VehCommon::RemoveHandler();
    }
}
