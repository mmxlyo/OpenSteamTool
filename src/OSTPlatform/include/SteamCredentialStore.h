#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace OSTPlatform::SteamCredentialStore {

    enum class Status {
        Ok,
        NotFound,
        Unsupported,
        Failed,
    };

    const char* ToString(Status status);

    void SetStorageDirectory(const std::filesystem::path& dir);
    std::filesystem::path GetStorageDirectory();

    // Read/write Steam per-app credentials. Each Get* reader writes its
    // out-parameter only on Status::Ok; on any other status the out-parameter is
    // left untouched. The signatures mirror their Write* counterparts so a value
    // travels through the same shape going in and coming out.
    Status GetAppTicket(uint32_t appId, std::vector<uint8_t>& ticket);
    Status WriteAppTicket(uint32_t appId, const std::vector<uint8_t>& data);

    Status GetETicket(uint32_t appId, std::vector<uint8_t>& ticket);
    Status WriteETicket(uint32_t appId, const std::vector<uint8_t>& data);

    Status GetSteamId(uint32_t appId, uint64_t& steamId);
    Status WriteSteamId(uint32_t appId, uint64_t steamId);

    // Fast zero-copy reader for the SteamID embedded in the AppTicket.
    Status GetTicketSteamId(uint32_t appId, uint64_t& steamId);

    bool RemoveCredentials(uint32_t appId);

    Status GetActiveUser(uint32_t& accountId, std::wstring& universe);

} // namespace OSTPlatform::SteamCredentialStore
