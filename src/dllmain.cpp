#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "OSTPlatform/include/Thread.h"

#include <chrono>
#include <thread>
#include <windows.h>

// Prepare key runtime paths.
// Portable: Steam components (steamclient64.dll, steamui.dll, etc.) are located
// in Steam's real installation directory, while configuration and Lua scripts can be
// loaded from the portable DLL directory or fallback to Steam's installation directory.
bool InitializeSteamComponents(OSTPlatform::DynamicLibrary::ModuleHandle selfModule)
{
    using OSTPlatform::Encoding::PathFromUtf8;
    using OSTPlatform::Encoding::PathToUtf8;
    using OSTPlatform::Encoding::Utf8ToWide;

    // 1. Locate Steam's actual install directory (where steam.exe and steamclient64.dll reside).
    // Injected into steam.exe: GetModuleDirectory(nullptr) returns the directory of steam.exe.
    auto steamExeDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(nullptr);
    std::string steamPath = PathToUtf8(steamExeDir);
    if (steamPath.empty()) {
        steamPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    }
    if (steamPath.empty()) {
        return false;
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", steamPath.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",    SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",          SteamInstallPath);

    // 2. Locate OpenSteamTool DLL directory (portable mode support).
    auto dllDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(selfModule);
    std::string dllPath = PathToUtf8(dllDir);
    if (dllPath.empty()) {
        dllPath = steamPath;
    }
    sprintf_s(DllDir, kRuntimePathCapacity, "%s", dllPath.c_str());

    // Evaluate portable mode once after SteamInstallPath and DllDir are set.
    const auto dllFsPath   = PathFromUtf8(DllDir);
    const auto steamFsPath = PathFromUtf8(SteamInstallPath);
    std::error_code ecEquiv;
    const bool sameDir = std::filesystem::equivalent(dllFsPath, steamFsPath, ecEquiv);
    g_IsPortableMode = !sameDir && (_wcsicmp(dllFsPath.c_str(), steamFsPath.c_str()) != 0);

    sprintf_s(DiversionPath, kRuntimePathCapacity, "%s\\bin\\diversion64.dll", GetStorageDirectory());

    // 3. Resolve config and lua directory:
    // Check DllDir first (portable folder), fallback to SteamInstallPath.
    std::filesystem::path tomlFs = dllFsPath / "opensteamtool.toml";
    std::string tomlPath = PathToUtf8(tomlFs);
    std::error_code ecToml;
    if (!std::filesystem::exists(tomlFs, ecToml) || ecToml) {
        std::filesystem::path steamTomlFs = steamFsPath / "opensteamtool.toml";
        std::error_code ecSteamToml;
        if ((std::filesystem::exists(steamTomlFs, ecSteamToml) && !ecSteamToml) || dllPath.empty()) {
            tomlPath = PathToUtf8(steamTomlFs);
        }
    }
    sprintf_s(ConfigPath, kRuntimePathCapacity, "%s", tomlPath.c_str());

    const auto storageBase = PathFromUtf8(GetStorageDirectory());
    const auto luaFs = storageBase / "config" / "lua";
    std::error_code ec;
    std::filesystem::create_directories(luaFs, ec);
    const std::string luaPath = PathToUtf8(luaFs);
    sprintf_s(LuaDir, kRuntimePathCapacity, "%s", luaPath.c_str());

    const auto credFs = storageBase / "config" / "credentials";
    OSTPlatform::SteamCredentialStore::SetStorageDirectory(credFs);

    // 4. Diversion shadow module cloning & loading:
    // Clone steamclient64.dll into bin\diversion64.dll so all hooks and patches
    // are isolated to the diversion module while original steamclient64.dll stays 100% clean.
    WIN32_FILE_ATTRIBUTE_DATA origAttr{}, divAttr{};
    std::wstring wideSteamclientPath = Utf8ToWide(SteamclientPath);
    std::wstring wideDiversionPath   = Utf8ToWide(DiversionPath);
    const bool origExists = GetFileAttributesExW(wideSteamclientPath.c_str(), GetFileExInfoStandard, &origAttr) != 0;
    const bool divExists  = GetFileAttributesExW(wideDiversionPath.c_str(), GetFileExInfoStandard, &divAttr) != 0;

    bool isUpToDate = false;
    if (origExists && divExists) {
        if (origAttr.nFileSizeHigh == divAttr.nFileSizeHigh &&
            origAttr.nFileSizeLow  == divAttr.nFileSizeLow &&
            origAttr.ftLastWriteTime.dwLowDateTime  == divAttr.ftLastWriteTime.dwLowDateTime &&
            origAttr.ftLastWriteTime.dwHighDateTime == divAttr.ftLastWriteTime.dwHighDateTime)
        {
            isUpToDate = true;
        }
    }

    bool copyOk = isUpToDate;
    if (isUpToDate) {
        LOG_DEBUG("Diversion module is already up to date ({}), skipping copy", DiversionPath);
    } else {
        std::filesystem::path diversionFsPath = PathFromUtf8(DiversionPath);
        std::error_code ecDir;
        std::filesystem::create_directories(diversionFsPath.parent_path(), ecDir);
        if (divExists && (divAttr.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(wideDiversionPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        }

        // Retry up to 3 times in case the old Steam process is still releasing the file handle
        constexpr int kMaxCopyRetries = 3;
        DWORD gle = ERROR_SUCCESS;
        int actualAttempts = 0;
        for (int attempt = 1; attempt <= kMaxCopyRetries; ++attempt) {
            actualAttempts = attempt;
            if (CopyFileW(wideSteamclientPath.c_str(), wideDiversionPath.c_str(), FALSE)) {
                copyOk = true;
                LOG_INFO("Cloned steamclient64.dll -> {}", DiversionPath);
                break;
            }
            gle = GetLastError();
            if (gle != ERROR_SHARING_VIOLATION && gle != ERROR_ACCESS_DENIED) {
                break;
            }
            if (attempt < kMaxCopyRetries) {
                Sleep(50);
            }
        }

        if (!copyOk) {
            LOG_WARN("CopyFileW to diversion64.dll failed after {} attempt(s) (err={})", actualAttempts, gle);
        }
    }

    if (copyOk) {
        client_hModule = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(DiversionPath));
        if (client_hModule) {
            g_IsDiversionActive.store(true);
            LOG_INFO("Loaded diversion module from {}", DiversionPath);
        } else {
            LOG_WARN("Load diversion module failed (path={}, err={}), falling back to real steamclient64.dll",
                     DiversionPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        }
    }

    if (!client_hModule) {
        g_IsDiversionActive.store(false);
        client_hModule = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(SteamclientPath));
        if (!client_hModule) {
            LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                      SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
            return false;
        }
        LOG_INFO("Loaded fallback steamclient64.dll from {} (Diversion inactive)", SteamclientPath);
    }

    ui_hModule = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(SteamUIPath));
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

    // Install SteamUI hooks early so LoadModuleWithPath can intercept
    // and synchronize with client hook installation.
    SteamUI::CoreHook();

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs = Config::GetLuaPaths();
    watchDirs.push_back(std::string(LuaDir));
    // In portable mode, also watch Steam's config/lua if it already exists
    if (IsPortableMode()) {
        const auto steamLuaFs = OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / "config" / "lua";
        std::error_code ecLua;
        if (std::filesystem::exists(steamLuaFs, ecLua) && !ecLua) {
            std::string steamLua = OSTPlatform::Encoding::PathToUtf8(steamLuaFs);
            if (steamLua != std::string(LuaDir)) {
                watchDirs.push_back(std::move(steamLua));
            }
        }
    }

    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    g_HooksInstalled.store(true);
    LOG_INFO("OpenSteamTool init complete ({})",
             g_IsDiversionActive.load() ? "Diversion active" : "Diversion bypassed, using original steamclient64");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        // Keep this module pinned so explicit FreeLibrary cannot unload code
        // while hooks and worker threads may still reference it.
        HMODULE pinnedModule = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCSTR>(&DllMain), &pinnedModule);

        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_HooksInstalled.store(false);
        g_IsDiversionActive.store(false);
        // During process termination (pvReserved != nullptr), OS terminates all threads
        // before calling DllMain; avoid joining dead threads or performing loader-lock unhooks.
        if (pvReserved == nullptr) {
            ConfigFileWatcher::Stop();
            LuaFileWatcher::Stop();
            SteamUI::CoreUnhook();
            SteamClient::CoreUnhook();
            CloudRedirectHost::Shutdown();
        }
    }

    return TRUE;
}
