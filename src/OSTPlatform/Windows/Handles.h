#pragma once

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <utility>

// Private move-only RAII wrappers for Win32 resources owned by Windows/*.cpp.
namespace OSTPlatform::Windows {

struct NullHandlePolicy {
    static bool IsValid(HANDLE h) noexcept { return h != nullptr; }
    static void Close(HANDLE h) noexcept { ::CloseHandle(h); }
};

struct InvalidHandlePolicy {
    static bool IsValid(HANDLE h) noexcept { return h != nullptr && h != INVALID_HANDLE_VALUE; }
    static void Close(HANDLE h) noexcept { ::CloseHandle(h); }
};

template <typename Policy>
class UniqueHandleT {
public:
    UniqueHandleT() noexcept = default;
    explicit UniqueHandleT(HANDLE handle) noexcept : handle_(handle) {}

    UniqueHandleT(UniqueHandleT&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueHandleT& operator=(UniqueHandleT&& other) noexcept {
        if (this != &other) {
            Reset(other.handle_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    UniqueHandleT(const UniqueHandleT&) = delete;
    UniqueHandleT& operator=(const UniqueHandleT&) = delete;

    ~UniqueHandleT() { Reset(); }

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return Policy::IsValid(handle_); }

    void Reset(HANDLE handle = nullptr) noexcept {
        if (Policy::IsValid(handle_)) Policy::Close(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

using UniqueHandle = UniqueHandleT<NullHandlePolicy>;
using UniqueFileHandle = UniqueHandleT<InvalidHandlePolicy>;

class UniqueWinHttpHandle {
public:
    UniqueWinHttpHandle() noexcept = default;
    explicit UniqueWinHttpHandle(HINTERNET handle) noexcept : handle_(handle) {}

    UniqueWinHttpHandle(UniqueWinHttpHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueWinHttpHandle& operator=(UniqueWinHttpHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.handle_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    UniqueWinHttpHandle(const UniqueWinHttpHandle&) = delete;
    UniqueWinHttpHandle& operator=(const UniqueWinHttpHandle&) = delete;

    ~UniqueWinHttpHandle() { Reset(); }

    HINTERNET get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

    void Reset(HINTERNET handle = nullptr) noexcept {
        if (handle_) ::WinHttpCloseHandle(handle_);
        handle_ = handle;
    }

private:
    HINTERNET handle_ = nullptr;
};

class UniqueRegKey {
public:
    UniqueRegKey() noexcept = default;

    UniqueRegKey(UniqueRegKey&& other) noexcept : key_(other.key_) { other.key_ = nullptr; }
    UniqueRegKey& operator=(UniqueRegKey&& other) noexcept {
        if (this != &other) {
            Reset();
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }

    UniqueRegKey(const UniqueRegKey&) = delete;
    UniqueRegKey& operator=(const UniqueRegKey&) = delete;

    ~UniqueRegKey() { Reset(); }

    HKEY get() const noexcept { return key_; }
    explicit operator bool() const noexcept { return key_ != nullptr; }

    HKEY* put() noexcept {
        Reset();
        return &key_;
    }

    void Reset() noexcept {
        if (key_) ::RegCloseKey(key_);
        key_ = nullptr;
    }

private:
    HKEY key_ = nullptr;
};

class UniqueMapView {
public:
    UniqueMapView() noexcept = default;
    explicit UniqueMapView(const uint8_t* base) noexcept : base_(base) {}

    UniqueMapView(UniqueMapView&& other) noexcept : base_(other.base_) { other.base_ = nullptr; }
    UniqueMapView& operator=(UniqueMapView&& other) noexcept {
        if (this != &other) {
            Reset();
            base_ = other.base_;
            other.base_ = nullptr;
        }
        return *this;
    }

    UniqueMapView(const UniqueMapView&) = delete;
    UniqueMapView& operator=(const UniqueMapView&) = delete;

    ~UniqueMapView() { Reset(); }

    const uint8_t* get() const noexcept { return base_; }
    explicit operator bool() const noexcept { return base_ != nullptr; }

    void Reset() noexcept {
        if (base_) ::UnmapViewOfFile(base_);
        base_ = nullptr;
    }

private:
    const uint8_t* base_ = nullptr;
};

template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ~ScopeExit() {
        if (active_) fn_();
    }

    void Dismiss() noexcept { active_ = false; }

private:
    Fn fn_;
    bool active_ = true;
};

template <typename Fn>
ScopeExit(Fn) -> ScopeExit<Fn>;

} // namespace OSTPlatform::Windows
