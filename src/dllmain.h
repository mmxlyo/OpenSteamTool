#ifndef DLLMAIN_H
#define DLLMAIN_H

#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"

#include <string>
#include <fstream>
#include <filesystem>
#include <array>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <regex>
#include <memory>
#include <atomic>
#include <format>
#include <chrono>
#include <thread>

#include "Steam/Types.h"
#include "Steam/Enums.h"
#include "Steam/Structs.h"
#include "Steam/Callback.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/Config.h"


#include <string.h>

inline OSTPlatform::DynamicLibrary::ModuleHandle client_hModule = nullptr;
inline OSTPlatform::DynamicLibrary::ModuleHandle ui_hModule = nullptr;

inline std::atomic<bool> g_HooksInstalled{false};
inline std::atomic<bool> g_IsDiversionActive{false};

inline constexpr size_t kRuntimePathCapacity = 1024;

inline char SteamInstallPath[kRuntimePathCapacity] = {};
inline char SteamclientPath[kRuntimePathCapacity]  = {};
inline char SteamUIPath[kRuntimePathCapacity]      = {};
inline char DiversionPath[kRuntimePathCapacity]    = {};
inline char LuaDir[kRuntimePathCapacity]           = {};
inline char ConfigPath[kRuntimePathCapacity]       = {};
inline char DllDir[kRuntimePathCapacity]           = {};

inline bool IsPortableMode() {
    if (DllDir[0] == '\0' || SteamInstallPath[0] == '\0') {
        return false;
    }
    std::error_code ec;
    if (std::filesystem::equivalent(OSTPlatform::Encoding::PathFromUtf8(DllDir),
                                    OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath), ec)) {
        return false;
    }
    return _wcsicmp(OSTPlatform::Encoding::Utf8ToWide(DllDir).c_str(),
                    OSTPlatform::Encoding::Utf8ToWide(SteamInstallPath).c_str()) != 0;
}

inline const char* GetStorageDirectory() {
    if (IsPortableMode()) {
        return DllDir;
    }
    if (SteamInstallPath[0] != '\0') {
        return SteamInstallPath;
    }
    if (DllDir[0] != '\0') {
        return DllDir;
    }
    return "";
}

// The fake AppId used by -onlinefix (SpaceWar).
constexpr AppId_t kOnlineFixAppId = 480;

#endif // DLLMAIN_H
