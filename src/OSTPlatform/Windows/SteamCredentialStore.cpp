#include "include/SteamCredentialStore.h"

#include "include/Log.h"
#include "include/Numbers.h"
#include "include/Encoding.h"
#include "include/DynamicLibrary.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace OSTPlatform::SteamCredentialStore {
namespace {

constexpr const wchar_t* kActiveProcessKeyPath = L"Software\\Valve\\Steam\\ActiveProcess";
constexpr const wchar_t* kValueActiveUser = L"ActiveUser";
constexpr const wchar_t* kValueUniverse = L"Universe";

constexpr size_t kMaxTicketFileSize = 1024 * 1024; // 1 MB ceiling to protect against corrupt files
constexpr size_t kSteamIdTicketMinimumSize = 16;

struct AppCredentialEntry {
    std::vector<uint8_t> appTicket;
    std::vector<uint8_t> eTicket;
    uint64_t steamId{0};
    bool appTicketLoaded{false};
    bool eTicketLoaded{false};
    bool steamIdLoaded{false};
};

std::shared_mutex g_credentialMutex;
std::unordered_map<uint32_t, AppCredentialEntry> g_credentials;

std::mutex g_pathMutex;
std::filesystem::path g_storageDir;

std::filesystem::path GetSafeStorageDir() {
    std::lock_guard lock(g_pathMutex);
    if (!g_storageDir.empty()) {
        return g_storageDir;
    }
    const auto exeDir = DynamicLibrary::GetModuleDirectory(nullptr);
    if (!exeDir.empty()) {
        g_storageDir = exeDir / "config" / "credentials";
    } else {
        g_storageDir = std::filesystem::current_path() / "config" / "credentials";
    }
    return g_storageDir;
}

std::filesystem::path GetAppFilePath(uint32_t appId, std::wstring_view fileName) {
    return GetSafeStorageDir() / std::to_wstring(appId) / fileName;
}

bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return false;
    }
    const auto size = in.tellg();
    if (size <= 0 || static_cast<size_t>(size) > kMaxTicketFileSize) {
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(buf.data()), size)) {
        return false;
    }
    out = std::move(buf);
    return true;
}

bool AtomicWriteBinary(const std::filesystem::path& targetPath, const uint8_t* data, size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(targetPath.parent_path(), ec);
    if (ec) {
        return false;
    }

    static std::atomic<uint64_t> s_seq{0};
    const auto seq = s_seq.fetch_add(1, std::memory_order_relaxed);
    const auto tid = GetCurrentThreadId();
    const std::filesystem::path tempPath = targetPath.wstring() + L".tmp." + std::to_wstring(tid) + L"." + std::to_wstring(seq);

    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        if (size > 0 && data != nullptr) {
            out.write(reinterpret_cast<const char*>(data), size);
        }
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    if (!MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
}

bool ReadSteamIdFile(const std::filesystem::path& path, uint64_t& outSteamId) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }
    std::string text;
    if (!(in >> text) || text.empty()) {
        return false;
    }
    auto parsed = Numbers::ParseUInt64(text);
    if (!parsed) {
        parsed = Numbers::ParseHexUInt64(text);
    }
    if (!parsed || *parsed == 0) {
        return false;
    }
    outSteamId = *parsed;
    return true;
}

bool AtomicWriteText(const std::filesystem::path& targetPath, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(targetPath.parent_path(), ec);
    if (ec) {
        return false;
    }

    static std::atomic<uint64_t> s_seq{0};
    const auto seq = s_seq.fetch_add(1, std::memory_order_relaxed);
    const auto tid = GetCurrentThreadId();
    const std::filesystem::path tempPath = targetPath.wstring() + L".tmp." + std::to_wstring(tid) + L"." + std::to_wstring(seq);

    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << text << "\n";
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    if (!MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
}

Status FromLStatus(LSTATUS status) {
    switch (status) {
    case ERROR_SUCCESS:
        return Status::Ok;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NOT_FOUND:
        return Status::NotFound;
    default:
        return Status::Failed;
    }
}

} // namespace

const char* ToString(Status status) {
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::NotFound: return "NotFound";
    case Status::Unsupported: return "Unsupported";
    case Status::Failed: return "Failed";
    }
    return "Unknown";
}

void SetStorageDirectory(const std::filesystem::path& dir) {
    {
        std::lock_guard lock(g_pathMutex);
        g_storageDir = dir;
        std::error_code ec;
        std::filesystem::create_directories(g_storageDir, ec);
    }
    {
        std::unique_lock lock(g_credentialMutex);
        g_credentials.clear();
    }
}

std::filesystem::path GetStorageDirectory() {
    return GetSafeStorageDir();
}

Status GetAppTicket(uint32_t appId, std::vector<uint8_t>& ticket) {
    {
        std::shared_lock lock(g_credentialMutex);
        auto it = g_credentials.find(appId);
        if (it != g_credentials.end() && it->second.appTicketLoaded) {
            if (!it->second.appTicket.empty()) {
                ticket = it->second.appTicket;
                OSTP_LOG_DEBUG("SteamCredentialStore: read AppTicket (memory) for appid={} bytes={}", appId, ticket.size());
                return Status::Ok;
            }
            return Status::NotFound;
        }
    }

    const auto file = GetAppFilePath(appId, L"AppTicket.bin");
    std::vector<uint8_t> diskTicket;
    ReadBinaryFile(file, diskTicket);

    {
        std::unique_lock lock(g_credentialMutex);
        auto& entry = g_credentials[appId];
        if (!entry.appTicketLoaded) {
            entry.appTicket = std::move(diskTicket);
            entry.appTicketLoaded = true;
        }
        if (!entry.appTicket.empty()) {
            ticket = entry.appTicket;
            OSTP_LOG_DEBUG("SteamCredentialStore: loaded AppTicket from disk for appid={} bytes={}", appId, ticket.size());
            return Status::Ok;
        }
    }

    OSTP_LOG_DEBUG("SteamCredentialStore: AppTicket missing for appid={}", appId);
    return Status::NotFound;
}

Status WriteAppTicket(uint32_t appId, const std::vector<uint8_t>& data) {
    {
        std::unique_lock lock(g_credentialMutex);
        auto& entry = g_credentials[appId];
        entry.appTicket = data;
        entry.appTicketLoaded = true;
    }

    const auto file = GetAppFilePath(appId, L"AppTicket.bin");
    if (AtomicWriteBinary(file, data.data(), data.size())) {
        OSTP_LOG_INFO("SteamCredentialStore: persisted AppTicket for appid={} bytes={}", appId, data.size());
    } else {
        OSTP_LOG_WARN("SteamCredentialStore: failed to persist AppTicket to disk for appid={}", appId);
    }
    return Status::Ok;
}

Status GetETicket(uint32_t appId, std::vector<uint8_t>& ticket) {
    {
        std::shared_lock lock(g_credentialMutex);
        auto it = g_credentials.find(appId);
        if (it != g_credentials.end() && it->second.eTicketLoaded) {
            if (!it->second.eTicket.empty()) {
                ticket = it->second.eTicket;
                OSTP_LOG_DEBUG("SteamCredentialStore: read ETicket (memory) for appid={} bytes={}", appId, ticket.size());
                return Status::Ok;
            }
            return Status::NotFound;
        }
    }

    const auto file = GetAppFilePath(appId, L"ETicket.bin");
    std::vector<uint8_t> diskTicket;
    ReadBinaryFile(file, diskTicket);

    {
        std::unique_lock lock(g_credentialMutex);
        auto& entry = g_credentials[appId];
        if (!entry.eTicketLoaded) {
            entry.eTicket = std::move(diskTicket);
            entry.eTicketLoaded = true;
        }
        if (!entry.eTicket.empty()) {
            ticket = entry.eTicket;
            OSTP_LOG_DEBUG("SteamCredentialStore: loaded ETicket from disk for appid={} bytes={}", appId, ticket.size());
            return Status::Ok;
        }
    }

    OSTP_LOG_DEBUG("SteamCredentialStore: ETicket missing for appid={}", appId);
    return Status::NotFound;
}

Status WriteETicket(uint32_t appId, const std::vector<uint8_t>& data) {
    {
        std::unique_lock lock(g_credentialMutex);
        auto& entry = g_credentials[appId];
        entry.eTicket = data;
        entry.eTicketLoaded = true;
    }

    const auto file = GetAppFilePath(appId, L"ETicket.bin");
    if (AtomicWriteBinary(file, data.data(), data.size())) {
        OSTP_LOG_INFO("SteamCredentialStore: persisted ETicket for appid={} bytes={}", appId, data.size());
    } else {
        OSTP_LOG_WARN("SteamCredentialStore: failed to persist ETicket to disk for appid={}", appId);
    }
    return Status::Ok;
}

Status GetSteamId(uint32_t appId, uint64_t& steamId) {
    {
        std::shared_lock lock(g_credentialMutex);
        auto it = g_credentials.find(appId);
        if (it != g_credentials.end() && it->second.steamIdLoaded) {
            if (it->second.steamId != 0) {
                steamId = it->second.steamId;
                OSTP_LOG_DEBUG("SteamCredentialStore: read SteamID (memory) for appid={} steamid={}", appId, steamId);
                return Status::Ok;
            }
            return Status::NotFound;
        }
    }

    const auto file = GetAppFilePath(appId, L"SteamID.txt");
    uint64_t diskSteamId = 0;
    ReadSteamIdFile(file, diskSteamId);

    {
        std::unique_lock lock(g_credentialMutex);
        auto& entry = g_credentials[appId];
        if (!entry.steamIdLoaded) {
            entry.steamId = diskSteamId;
            entry.steamIdLoaded = true;
        }
        if (entry.steamId != 0) {
            steamId = entry.steamId;
            OSTP_LOG_DEBUG("SteamCredentialStore: loaded SteamID from disk for appid={} steamid={}", appId, steamId);
            return Status::Ok;
        }
    }

    OSTP_LOG_DEBUG("SteamCredentialStore: SteamID missing for appid={}", appId);
    return Status::NotFound;
}

Status WriteSteamId(uint32_t appId, uint64_t steamId) {
    {
        std::unique_lock lock(g_credentialMutex);
        auto& entry = g_credentials[appId];
        entry.steamId = steamId;
        entry.steamIdLoaded = true;
    }

    const auto file = GetAppFilePath(appId, L"SteamID.txt");
    if (AtomicWriteText(file, std::to_string(steamId))) {
        OSTP_LOG_INFO("SteamCredentialStore: persisted SteamID for appid={} steamid={}", appId, steamId);
    } else {
        OSTP_LOG_WARN("SteamCredentialStore: failed to persist SteamID to disk for appid={}", appId);
    }
    return Status::Ok;
}

Status GetTicketSteamId(uint32_t appId, uint64_t& steamId) {
    {
        std::shared_lock lock(g_credentialMutex);
        auto it = g_credentials.find(appId);
        if (it != g_credentials.end() && it->second.appTicketLoaded) {
            if (it->second.appTicket.size() >= kSteamIdTicketMinimumSize) {
                std::memcpy(&steamId, it->second.appTicket.data() + 8, sizeof(uint64_t));
                OSTP_LOG_DEBUG("SteamCredentialStore: read Ticket SteamID (memory) for appid={} steamid={}", appId, steamId);
                return Status::Ok;
            }
            return Status::NotFound;
        }
    }

    std::vector<uint8_t> ticket;
    const auto status = GetAppTicket(appId, ticket);
    if (status == Status::Ok && ticket.size() >= kSteamIdTicketMinimumSize) {
        std::memcpy(&steamId, ticket.data() + 8, sizeof(uint64_t));
        return Status::Ok;
    }
    return Status::NotFound;
}

bool RemoveCredentials(uint32_t appId) {
    {
        std::unique_lock lock(g_credentialMutex);
        g_credentials.erase(appId);
    }
    std::error_code ec;
    const auto appDir = GetSafeStorageDir() / std::to_wstring(appId);
    if (!std::filesystem::exists(appDir, ec)) {
        return true;
    }
    const bool removed = std::filesystem::remove_all(appDir, ec) > 0;
    if (removed) {
        OSTP_LOG_INFO("SteamCredentialStore: removed credentials directory for appid={}", appId);
    }
    return !ec;
}

Status GetActiveUser(uint32_t& accountId, std::wstring& universe) {
    DWORD activeUser = 0;
    DWORD bytes = sizeof(activeUser);
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueActiveUser,
                                  RRF_RT_REG_DWORD, nullptr, &activeUser, &bytes);
    if (status != ERROR_SUCCESS) {
        const auto mapped = FromLStatus(status);
        if (mapped == Status::NotFound) {
            OSTP_LOG_DEBUG("SteamCredentialStore: ActiveUser missing status={}", status);
        } else {
            OSTP_LOG_WARN("SteamCredentialStore: failed to read ActiveUser status={}", status);
        }
        return mapped;
    }
    if (bytes != sizeof(activeUser)) {
        OSTP_LOG_WARN("SteamCredentialStore: ActiveUser has unexpected size bytes={}", bytes);
        return Status::NotFound;
    }
    if (activeUser == 0) {
        OSTP_LOG_DEBUG("SteamCredentialStore: ActiveUser is zero");
        return Status::NotFound;
    }

    bytes = 0;
    status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueUniverse,
                          RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS) {
        const auto mapped = FromLStatus(status);
        if (mapped == Status::NotFound) {
            OSTP_LOG_DEBUG("SteamCredentialStore: ActiveProcess Universe missing status={}", status);
        } else {
            OSTP_LOG_WARN("SteamCredentialStore: failed to query ActiveProcess Universe size status={}", status);
        }
        return mapped;
    }
    if (bytes < sizeof(wchar_t)) {
        OSTP_LOG_WARN("SteamCredentialStore: ActiveProcess Universe value too short bytes={}", bytes);
        return Status::NotFound;
    }

    std::wstring rawUniverse(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueUniverse,
                          RRF_RT_REG_SZ, nullptr, rawUniverse.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        OSTP_LOG_WARN("SteamCredentialStore: failed to read ActiveProcess Universe status={}", status);
        return FromLStatus(status);
    }

    const size_t chars = bytes / sizeof(wchar_t);
    rawUniverse.resize(chars > 0 ? chars - 1 : 0);
    if (rawUniverse.empty()) {
        OSTP_LOG_WARN("SteamCredentialStore: ActiveProcess Universe is empty");
        return Status::NotFound;
    }

    accountId = activeUser;
    universe = std::move(rawUniverse);
    OSTP_LOG_DEBUG("SteamCredentialStore: read ActiveUser accountid={} universe={}({} chars)",
                   accountId, Encoding::WideToUtf8(universe), universe.size());
    return Status::Ok;
}

} // namespace OSTPlatform::SteamCredentialStore
