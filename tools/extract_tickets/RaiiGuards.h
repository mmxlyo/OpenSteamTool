#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>

#include "steam.h"

namespace OST::ExtractTickets {

// RAII wrapper for Windows HANDLE
struct ScopedHandle {
    HANDLE handle{INVALID_HANDLE_VALUE};

    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) noexcept : handle(h) {}

    ~ScopedHandle() noexcept {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                CloseHandle(handle);
            }
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    operator HANDLE() const noexcept { return handle; }
    [[nodiscard]] bool IsValid() const noexcept { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
};

// RAII wrapper for MapViewOfFile memory pointer
struct ScopedFileMappingView {
    const void* address{nullptr};

    ScopedFileMappingView() = default;
    explicit ScopedFileMappingView(const void* addr) noexcept : address(addr) {}

    ~ScopedFileMappingView() noexcept {
        if (address) {
            UnmapViewOfFile(address);
        }
    }

    ScopedFileMappingView(const ScopedFileMappingView&) = delete;
    ScopedFileMappingView& operator=(const ScopedFileMappingView&) = delete;

    ScopedFileMappingView(ScopedFileMappingView&& other) noexcept : address(other.address) {
        other.address = nullptr;
    }

    ScopedFileMappingView& operator=(ScopedFileMappingView&& other) noexcept {
        if (this != &other) {
            if (address) {
                UnmapViewOfFile(address);
            }
            address = other.address;
            other.address = nullptr;
        }
        return *this;
    }

    template <typename T>
    [[nodiscard]] const T* As() const noexcept { return static_cast<const T*>(address); }

    explicit operator bool() const noexcept { return address != nullptr; }
};

// RAII wrapper for Windows HKEY
struct ScopedHKey {
    HKEY key{nullptr};

    ScopedHKey() = default;
    explicit ScopedHKey(HKEY k) noexcept : key(k) {}

    ~ScopedHKey() noexcept {
        if (key != nullptr) {
            RegCloseKey(key);
        }
    }

    ScopedHKey(const ScopedHKey&) = delete;
    ScopedHKey& operator=(const ScopedHKey&) = delete;

    ScopedHKey(ScopedHKey&& other) noexcept : key(other.key) {
        other.key = nullptr;
    }

    ScopedHKey& operator=(ScopedHKey&& other) noexcept {
        if (this != &other) {
            if (key != nullptr) {
                RegCloseKey(key);
            }
            key = other.key;
            other.key = nullptr;
        }
        return *this;
    }

    operator HKEY() const noexcept { return key; }
    [[nodiscard]] bool IsValid() const noexcept { return key != nullptr; }
};

// RAII wrapper for Windows FindFirstFile/FindNextFile HANDLE
struct ScopedFindHandle {
    HANDLE handle{INVALID_HANDLE_VALUE};

    ScopedFindHandle() = default;
    explicit ScopedFindHandle(HANDLE h) noexcept : handle(h) {}

    ~ScopedFindHandle() noexcept {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            FindClose(handle);
        }
    }

    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;

    ScopedFindHandle(ScopedFindHandle&& other) noexcept : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }

    ScopedFindHandle& operator=(ScopedFindHandle&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                FindClose(handle);
            }
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    operator HANDLE() const noexcept { return handle; }
    [[nodiscard]] bool IsValid() const noexcept { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
};

// RAII guard for Steam client session, loaded module, and process environment
struct SteamSessionGuard {
    ISteamClient* client{nullptr};
    HSteamPipe pipe{0};
    HSteamUser user{0};
    HMODULE module{nullptr};

    SteamSessionGuard() = default;
    SteamSessionGuard(ISteamClient* c, HSteamPipe p, HSteamUser u, HMODULE m) noexcept
        : client(c), pipe(p), user(u), module(m) {}

    ~SteamSessionGuard() noexcept {
        Reset();
    }

    SteamSessionGuard(const SteamSessionGuard&) = delete;
    SteamSessionGuard& operator=(const SteamSessionGuard&) = delete;

    SteamSessionGuard(SteamSessionGuard&& other) noexcept
        : client(other.client), pipe(other.pipe), user(other.user), module(other.module) {
        other.client = nullptr;
        other.pipe = 0;
        other.user = 0;
        other.module = nullptr;
    }

    SteamSessionGuard& operator=(SteamSessionGuard&& other) noexcept {
        if (this != &other) {
            Reset();
            client = other.client;
            pipe = other.pipe;
            user = other.user;
            module = other.module;
            other.client = nullptr;
            other.pipe = 0;
            other.user = 0;
            other.module = nullptr;
        }
        return *this;
    }

    void Reset() noexcept {
        if (client) {
            if (pipe && user) {
                client->ReleaseUser(pipe, user);
                user = 0;
            }
            if (pipe) {
                client->BReleaseSteamPipe(pipe);
                pipe = 0;
            }
            client = nullptr;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
            SetDllDirectoryA(nullptr);
        }
        // Clear spoofed Steam environment variables
        SetEnvironmentVariableA("SteamAppId", nullptr);
        SetEnvironmentVariableA("SteamGameId", nullptr);
        SetEnvironmentVariableA("SteamOverlayGameId", nullptr);
    }
};

// RAII guard for Windows console code page restoration
struct ConsoleCodePageGuard {
    UINT oldOutputCP{0};
    UINT oldCP{0};

    ConsoleCodePageGuard() {
        oldOutputCP = GetConsoleOutputCP();
        oldCP = GetConsoleCP();
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    ~ConsoleCodePageGuard() {
        SetConsoleOutputCP(oldOutputCP);
        SetConsoleCP(oldCP);
    }

    ConsoleCodePageGuard(const ConsoleCodePageGuard&) = delete;
    ConsoleCodePageGuard& operator=(const ConsoleCodePageGuard&) = delete;
};

} // namespace OST::ExtractTickets
