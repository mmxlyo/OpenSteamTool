#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Thread.h"

#include <windows.h>

// Prepare key runtime paths.
// Portable: Steam components (steamclient64.dll, steamui.dll, etc.) are located
// in Steam's real installation directory, while configuration and Lua scripts can be
// loaded from the portable DLL directory or fallback to Steam's installation directory.
bool InitializeSteamComponents(OSTPlatform::DynamicLibrary::ModuleHandle selfModule)
{
    // 1. Locate Steam's actual install directory (where steam.exe and steamclient64.dll reside).
    // Injected into steam.exe: GetModuleDirectory(nullptr) returns the directory of steam.exe.
    auto steamExeDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(nullptr);
    std::string steamPath = steamExeDir.string();
    if (steamPath.empty()) {
        steamPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    }
    if (steamPath.empty()) {
        return false;
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", steamPath.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    sprintf_s(DiversionPath,   kRuntimePathCapacity, "%s\\bin\\diversion.dll", SteamInstallPath);

    // 2. Locate OpenSteamTool DLL directory (portable mode support).
    auto dllDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(selfModule);
    std::string dllPath = dllDir.string();
    if (dllPath.empty()) {
        dllPath = steamPath;
    }
    sprintf_s(DllDir, kRuntimePathCapacity, "%s", dllPath.c_str());

    // 3. Resolve config and lua directory:
    // Check DllDir first (portable folder), fallback to SteamInstallPath.
    std::string tomlPath = (std::filesystem::path(DllDir) / "opensteamtool.toml").string();
    if (!std::filesystem::exists(tomlPath)) {
        std::string steamToml = (std::filesystem::path(SteamInstallPath) / "opensteamtool.toml").string();
        if (std::filesystem::exists(steamToml) || dllPath.empty()) {
            tomlPath = steamToml;
        }
    }
    sprintf_s(ConfigPath, kRuntimePathCapacity, "%s", tomlPath.c_str());

    std::string luaPath = (std::filesystem::path(DllDir) / "config" / "lua").string();
    if (!std::filesystem::exists(luaPath)) {
        std::string steamLua = (std::filesystem::path(SteamInstallPath) / "config" / "lua").string();
        if (std::filesystem::exists(steamLua) || dllPath.empty()) {
            luaPath = steamLua;
        }
    }
    sprintf_s(LuaDir, kRuntimePathCapacity, "%s", luaPath.c_str());

    client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
    if (!client_hModule) {
        LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                  SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    LOG_INFO("Loaded steamclient64.dll from {}", SteamclientPath);

    ui_hModule = OSTPlatform::DynamicLibrary::Load(SteamUIPath);
    if (!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    Log::Init(selfModule);
    LOG_INFO("OpenSteamTool init thread started");

    if (!InitializeSteamComponents(selfModule)) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // Load pattern files for steamclient64.dll and steamui.dll.
    // Each call computes the SHA-256 of the DLL on disk, checks the local
    // cache, and downloads from GitHub if needed.  Both calls are synchronous
    // but run on this worker thread, never under the loader lock.
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs = Config::GetLuaPaths();
    watchDirs.push_back(std::string(LuaDir));
    // If DllDir and SteamInstallPath are different, also watch Steam's config/lua if it exists
    if (_stricmp(SteamInstallPath, DllDir) != 0) {
        std::string steamLua = (std::filesystem::path(SteamInstallPath) / "config" / "lua").string();
        if (std::filesystem::exists(steamLua) && steamLua != std::string(LuaDir)) {
            watchDirs.push_back(steamLua);
        }
    }

    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamUI::CoreHook();
    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    LOG_INFO("OpenSteamTool init complete");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
