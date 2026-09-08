#include <windows.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "steam.h"

namespace {

bool IsDecimal(std::string_view value) {
    if (value.empty()) return false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

std::optional<uint32_t> ParseAppId(const std::string& value) {
    if (!IsDecimal(value)) return std::nullopt;

    unsigned long parsed{0};
    try {
        size_t consumed{0};
        parsed = std::stoul(value, &consumed, 10);
        if (consumed != value.size() || parsed > 0xFFFFFFFFul) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }

    return static_cast<uint32_t>(parsed);
}

std::optional<uint32_t> ReadAppIdFromConsole() {
    std::cout << "AppID: ";

    std::string input;
    if (!std::getline(std::cin, input)) return std::nullopt;
    return ParseAppId(input);
}

std::optional<std::string> QueryRegistryString(HKEY root, const char* subKey, const char* valueName) {
    HKEY key{nullptr};
    if (RegOpenKeyExA(root, subKey, 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD valueType{0};
    DWORD valueSize{0};
    LSTATUS status{RegQueryValueExA(key, valueName, nullptr, &valueType, nullptr, &valueSize)};
    if (status != ERROR_SUCCESS || valueType != REG_SZ || valueSize == 0) {
        RegCloseKey(key);
        return std::nullopt;
    }

    std::string value(valueSize, '\0');
    status = RegQueryValueExA(
        key,
        valueName,
        nullptr,
        nullptr,
        reinterpret_cast<LPBYTE>(value.data()),
        &valueSize);
    RegCloseKey(key);

    if (status != ERROR_SUCCESS) return std::nullopt;
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

std::optional<std::string> FindSteamInstallPath() {
    constexpr const char* kSteamKey{"Software\\Valve\\Steam"};

    if (auto path{QueryRegistryString(HKEY_CURRENT_USER, kSteamKey, "SteamPath")}) {
        std::cout << "Found SteamPath in HKEY_CURRENT_USER: " << *path << "\n";
        return path;
    }

    return std::nullopt;
}

std::string JoinPath(std::string base, const char* name) {
    for (char& ch : base) {
        if (ch == '/') ch = '\\';
    }
    if (!base.empty() && base.back() != '\\') base += '\\';
    base += name;
    return base;
}

std::string NormalizeDir(std::string dir) {
    for (char& ch : dir) {
        if (ch == '/') ch = '\\';
    }
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();
    return dir;
}

HMODULE LoadSteamClient64(std::string& loadedPath) {
    auto steamPath{FindSteamInstallPath()};
    if (!steamPath) {
        std::cerr << "Failed to find Steam install path in registry.\n";
        return nullptr;
    }

    const std::string steamDir{NormalizeDir(*steamPath)};
    loadedPath = JoinPath(*steamPath, "steamclient64.dll");

    // steamclient64.dll pulls in tier0_s64.dll / vstdlib_s64.dll from the Steam
    // directory. Add that directory to the search path and load with
    // LOAD_WITH_ALTERED_SEARCH_PATH so those dependencies resolve; otherwise the
    // load fails with ERROR_MOD_NOT_FOUND (126).
    SetDllDirectoryA(steamDir.c_str());
    HMODULE module{LoadLibraryExA(loadedPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)};
    if (!module) {
        std::cerr << "Failed to load " << loadedPath << " (GetLastError=" << GetLastError() << ").\n";
        return nullptr;
    }

    return module;
}

ISteamClient* CreateSteamClient(HMODULE module) {
    auto createInterface{reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"))};
    if (!createInterface) {
        std::cerr << "steamclient64.dll has no CreateInterface export.\n";
        return nullptr;
    }

    int returnCode{0};
    auto* client{reinterpret_cast<ISteamClient*>(createInterface(kSteamClientInterfaceVersion, &returnCode))};
    if (!client) {
        std::cerr << "CreateInterface(" << kSteamClientInterfaceVersion
                  << ") failed (returnCode=" << returnCode << ").\n";
        return nullptr;
    }
    return client;
}

// Open a pipe and attach to the already-running global user
bool OpenSession(ISteamClient* client, HSteamPipe& pipe, HSteamUser& user) {
    pipe = client->CreateSteamPipe();
    if (!pipe) {
        std::cerr << "CreateSteamPipe failed. Is Steam running?\n";
        return false;
    }

    user = client->ConnectToGlobalUser(pipe);
    if (!user) {
        std::cerr << "ConnectToGlobalUser failed. Is a user logged in?\n";
        client->BReleaseSteamPipe(pipe);
        pipe = 0;
        return false;
    }

    return true;
}

// App ownership ticket: ISteamAppTicket hands back the raw signed buffer plus
// offsets into it. nAppID is explicit, so this works for any owned app.
std::optional<std::vector<uint8_t>> ExtractAppOwnershipTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId) {
    auto* appTicket{reinterpret_cast<ISteamAppTicket*>(
        client->GetISteamGenericInterface(user, pipe, kSteamAppTicketInterfaceVersion))};
    if (!appTicket) {
        std::cerr << "GetISteamGenericInterface(" << kSteamAppTicketInterfaceVersion
                  << ") returned null.\n";
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(2048);
    uint32_t appIdOffset{0};
    uint32_t steamIdOffset{0};
    uint32_t signatureOffset{0};
    uint32_t signatureSize{0};
    const uint32_t written{appTicket->GetAppOwnershipTicketData(
        appId,
        buffer.data(),
        static_cast<uint32_t>(buffer.size()),
        &appIdOffset,
        &steamIdOffset,
        &signatureOffset,
        &signatureSize)};

    if (written == 0 || written > buffer.size()) {
        std::cerr << "GetAppOwnershipTicketData returned no ticket for AppID " << appId
                  << " (own the app and have it cached locally?).\n";
        return std::nullopt;
    }

    buffer.resize(written);
    std::cout << "Ownership ticket " << written << " bytes"
              << " (appIdOffset=" << appIdOffset
              << " steamIdOffset=" << steamIdOffset
              << " signatureOffset=" << signatureOffset
              << " signatureSize=" << signatureSize << ")\n";
    return buffer;
}

// Encrypted app ticket: asynchronous request whose result arrives as
// EncryptedAppTicketResponse_t. We have no callback dispatcher, so we poll
// ISteamUtils::IsAPICallCompleted and then read the result + ticket.
// See https://partner.steamgames.com/doc/api/ISteamUser#RequestEncryptedAppTicket
std::optional<std::vector<uint8_t>> ExtractEncryptedAppTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId) {
    auto* utils{client->GetISteamUtils(pipe, kSteamUtilsInterfaceVersion)};
    auto* steamUser{client->GetISteamUser(user, pipe, kSteamUserInterfaceVersion)};
    if (!utils || !steamUser) {
        std::cerr << "GetISteamUtils/GetISteamUser returned null.\n";
        return std::nullopt;
    }

    const SteamAPICall_t hCall{steamUser->RequestEncryptedAppTicket(nullptr, 0)};
    if (!hCall) {
        std::cerr << "RequestEncryptedAppTicket failed to start for AppID " << appId << ".\n";
        return std::nullopt;
    }

    // Bounded poll so a wedged client can never hang the tool.
    constexpr int kMaxWaitMs{15000};
    constexpr int kStepMs{50};
    bool failed{false};
    int waited{0};
    while (!utils->IsAPICallCompleted(hCall, &failed)) {
        if (waited >= kMaxWaitMs) {
            std::cerr << "Timed out waiting for EncryptedAppTicketResponse_t.\n";
            return std::nullopt;
        }
        Sleep(kStepMs);
        waited += kStepMs;
    }

    EncryptedAppTicketResponse_t response{};
    const bool gotResult{utils->GetAPICallResult(
        hCall,
        &response,
        sizeof(response),
        EncryptedAppTicketResponse_t::k_iCallback,
        &failed)};
    if (!gotResult || failed) {
        std::cerr << "GetAPICallResult failed for EncryptedAppTicketResponse_t.\n";
        return std::nullopt;
    }
    if (response.m_eResult != k_EResultOK) {
        std::cerr << "RequestEncryptedAppTicket returned EResult "
                  << static_cast<int>(response.m_eResult) << ".\n";
        return std::nullopt;
    }

    // Pass a null buffer first to learn the size, then fetch.
    uint32_t cbTicket{0};
    steamUser->GetEncryptedAppTicket(nullptr, 0, &cbTicket);
    if (cbTicket == 0) {
        std::cerr << "Encrypted app ticket is empty.\n";
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(cbTicket);
    if (!steamUser->GetEncryptedAppTicket(buffer.data(), static_cast<int>(buffer.size()), &cbTicket)) {
        std::cerr << "GetEncryptedAppTicket failed.\n";
        return std::nullopt;
    }

    buffer.resize(cbTicket);
    std::cout << "Encrypted ticket " << cbTicket << " bytes\n";
    return buffer;
}

// Dump the raw ticket bytes as a classic hex view: offset, 16 hex bytes, ASCII.
void PrintHex(const char* label, const std::vector<uint8_t>& data) {
    std::cout << label << " (" << data.size() << " bytes):\n";

    constexpr size_t kBytesPerRow{16};
    static const char kHex[]{"0123456789abcdef"};

    for (size_t row{0}; row < data.size(); row += kBytesPerRow) {
        // Offset column.
        std::string line;
        for (int shift{12}; shift >= 0; shift -= 4) {
            line += kHex[(row >> shift) & 0xF];
        }
        line += "  ";

        // Hex column.
        std::string ascii;
        for (size_t col{0}; col < kBytesPerRow; ++col) {
            if (row + col < data.size()) {
                const uint8_t byte{data[row + col]};
                line += kHex[byte >> 4];
                line += kHex[byte & 0xF];
                line += ' ';
                ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
            } else {
                line += "   ";
            }
            if (col == 7) line += ' ';
        }

        std::cout << line << " " << ascii << "\n";
    }
}

std::string ToHexString(const std::vector<uint8_t>& data) {
    static const char kHex[]{"0123456789abcdef"};
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0xF];
    }
    return out;
}

bool WriteBinaryFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        std::cerr << "Failed to create " << path << ".\n";
        return false;
    }
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!output) {
        std::cerr << "Failed to write " << path << ".\n";
        return false;
    }
    return true;
}

// Build the plain-text summary line for one ticket. Present -> the hex string,
// absent -> "null".
std::string TicketLine(const char* name, const std::optional<std::vector<uint8_t>>& ticket) {
    if (!ticket) return std::string{name} + ":null\n";
    return std::string{name} + "(" + std::to_string(ticket->size()) + "bytes):"
           + ToHexString(*ticket) + "\n";
}

// Everything lands in a single <appid> folder: the raw binary tickets
// (only when present) plus a plain-text summary file.
bool WriteOutputs(uint32_t appId,
                  const std::optional<std::vector<uint8_t>>& ownership,
                  const std::optional<std::vector<uint8_t>>& encrypted) {
    const std::string dir{std::to_string(appId)};
    if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::cerr << "Failed to create directory " << dir
                  << " (GetLastError=" << GetLastError() << ").\n";
        return false;
    }

    bool ok{true};
    if (ownership) ok = WriteBinaryFile(JoinPath(dir, "appticket.bin"), *ownership) && ok;
    if (encrypted) ok = WriteBinaryFile(JoinPath(dir, "eticket.bin"), *encrypted) && ok;

    const std::string text{
        "appid:" + std::to_string(appId) + "\n"
        + TicketLine("appticket", ownership)
        + TicketLine("eticket", encrypted)};

    const std::string textPath{JoinPath(dir, "tickets.txt")};
    std::ofstream summary{textPath, std::ios::trunc};
    if (!summary || !(summary << text)) {
        std::cerr << "Failed to write " << textPath << ".\n";
        return false;
    }

    // Generate ready-to-use Lua script
    std::string luaText;
    luaText += "-- Auto-generated by extract_tickets for AppID: " + std::to_string(appId) + "\n";
    luaText += "addappid(" + std::to_string(appId) + ")\n\n";

    if (ownership) {
        luaText += "-- App Ownership Ticket (AppTicket)\n";
        luaText += "setAppTicket(" + std::to_string(appId) + ", \"" + ToHexString(*ownership) + "\")\n\n";
    }

    if (encrypted) {
        luaText += "-- Encrypted App Ticket (ETicket)\n";
        luaText += "setETicket(" + std::to_string(appId) + ", \"" + ToHexString(*encrypted) + "\")\n\n";
    }

    const std::string luaPath{JoinPath(dir, std::to_string(appId) + ".lua")};
    std::ofstream luaFile{luaPath, std::ios::trunc};
    if (!luaFile || !(luaFile << luaText)) {
        std::cerr << "Failed to write " << luaPath << ".\n";
        ok = false;
    }

    std::cout << "Wrote " << dir << "\\ (" << std::to_string(appId) << ".lua, tickets.txt";
    if (ownership) std::cout << ", appticket.bin";
    if (encrypted) std::cout << ", eticket.bin";
    std::cout << ")\n";
    std::cout << "[INFO] Ready-to-use Lua script saved to: " << luaPath << "\n";
    return ok;
}

void WaitForExit() {
    std::cout << "\nPress Enter to exit...";
    std::string dummy;
    std::getline(std::cin, dummy);
}

#if defined(_WIN64)
int Run(int argc, char** argv) {
    std::optional<uint32_t> appId;
    if (argc >= 2) {
        appId = ParseAppId(argv[1]);
        if (!appId) {
            std::cerr << "Invalid AppID: " << argv[1] << "\n";
            return 1;
        }
    } else {
        appId = ReadAppIdFromConsole();
        if (!appId) {
            std::cerr << "Invalid AppID.\n";
            return 1;
        }
    }

    // Run in the target app's context so GetAppID and RequestEncryptedAppTicket
    // resolve to this AppID. Must be set before steamclient64.dll initializes.
    const std::string appIdStr{std::to_string(*appId)};
    SetEnvironmentVariableA("SteamAppId", appIdStr.c_str());
    SetEnvironmentVariableA("SteamGameId", appIdStr.c_str());

    std::string steamClientPath;
    HMODULE steamClient{LoadSteamClient64(steamClientPath)};
    if (!steamClient) return 1;

    std::cout << "Loaded " << steamClientPath << "\n";

    ISteamClient* client{CreateSteamClient(steamClient)};
    if (!client) {
        FreeLibrary(steamClient);
        return 1;
    }

    HSteamPipe pipe{0};
    HSteamUser user{0};
    if (!OpenSession(client, pipe, user)) {
        FreeLibrary(steamClient);
        return 1;
    }

    if (auto* utils{client->GetISteamUtils(pipe, kSteamUtilsInterfaceVersion)}) {
        std::cout << "ConnectedUniverse=" << static_cast<int>(utils->GetConnectedUniverse())
                  << " ClientAppID=" << utils->GetAppID() << "\n";
    }

    auto ownership{ExtractAppOwnershipTicket(client, pipe, user, *appId)};
    if (ownership) PrintHex("Ownership ticket", *ownership);

    auto encrypted{ExtractEncryptedAppTicket(client, pipe, user, *appId)};
    if (encrypted) PrintHex("Encrypted ticket", *encrypted);

    const bool ok{WriteOutputs(*appId, ownership, encrypted)};

    client->BReleaseSteamPipe(pipe);
    FreeLibrary(steamClient);
    return ok ? 0 : 1;
}
#endif

} // namespace

int main(int argc, char** argv) {
#if !defined(_WIN64)
    std::cerr << "extract_tickets must be built as a 64-bit Windows executable.\n";
    WaitForExit();
    return 1;
#else
    const int rc{Run(argc, argv)};
    WaitForExit();
    return rc;
#endif
}
