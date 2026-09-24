#include "include/RemoteProcess.h"

#include "include/Encoding.h"
#include "include/Log.h"
#include "include/PE.h"

#include "Windows/Handles.h"

#include <tlhelp32.h>
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OSTPlatform::RemoteProcess {
namespace {

constexpr DWORD kRemoteLoadTimeoutMs = 10000;

constexpr DWORD kInjectAccess =
    PROCESS_CREATE_THREAD |
    PROCESS_QUERY_INFORMATION |
    PROCESS_VM_OPERATION |
    PROCESS_VM_WRITE |
    PROCESS_VM_READ;

struct RemoteModule {
    std::wstring name;
    std::filesystem::path path;
    uintptr_t base = 0;
};

Architecture DetectArchitecture(HANDLE process) {
    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64)) {
        OSTP_LOG_DEBUG("IsWow64Process failed (error={})", GetLastError());
        return Architecture::Unknown;
    }
    if (wow64) return Architecture::X86;

#if defined(_WIN64)
    return Architecture::X64;
#else
    return Architecture::X86;
#endif
}

std::wstring Lower(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return value;
}

bool EqualsInsensitive(std::wstring_view lhs, std::wstring_view rhs) {
    return Lower(std::wstring(lhs)) == Lower(std::wstring(rhs));
}

std::wstring ModuleNameFromForwarder(std::string_view name) {
    std::wstring wide;
    wide.reserve(name.size() + 4);
    for (char ch : name) wide.push_back(static_cast<unsigned char>(ch));
    if (wide.find(L'.') == std::wstring::npos) wide += L".dll";
    return wide;
}

std::vector<RemoteModule> EnumerateRemoteModules(uint32_t pid, Architecture architecture) {
    const DWORD flags = architecture == Architecture::X86
        ? TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32
        : TH32CS_SNAPMODULE;

    Windows::UniqueFileHandle snapshot;
    for (int retry = 0; retry < 5; ++retry) {
        snapshot.Reset(CreateToolhelp32Snapshot(flags, pid));
        if (!snapshot && architecture == Architecture::X86) {
            snapshot.Reset(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE32, pid));
        }
        if (snapshot) break;
        if (GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(10);
    }
    if (!snapshot) {
        OSTP_LOG_WARN("CreateToolhelp32Snapshot(pid={}, flags=0x{:X}) failed (error={})",
                      pid, flags, GetLastError());
        return {};
    }

    std::vector<RemoteModule> modules;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot.get(), &entry)) {
        do {
            modules.push_back(RemoteModule{
                entry.szModule,
                std::filesystem::path(entry.szExePath),
                reinterpret_cast<uintptr_t>(entry.modBaseAddr),
            });
        } while (Module32NextW(snapshot.get(), &entry));
    }

    return modules;
}

const RemoteModule* FindModule(std::span<const RemoteModule> modules, std::wstring_view name) {
    for (const RemoteModule& module : modules) {
        if (EqualsInsensitive(module.name, name)) return &module;
    }
    return nullptr;
}

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    if (value.size() < prefix.size()) return false;
    return EqualsInsensitive(value.substr(0, prefix.size()), prefix);
}

std::optional<uintptr_t> ResolveRemoteExport(
    std::span<const RemoteModule> modules,
    std::wstring_view moduleName,
    std::string_view symbolName,
    int depth = 0) {
    if (depth > 8) return std::nullopt;

    const RemoteModule* module = FindModule(modules, moduleName);
    if (!module && StartsWithInsensitive(moduleName, L"api-ms-win-")) {
        module = FindModule(modules, L"kernelbase.dll");
    }
    if (!module) {
        OSTP_LOG_WARN("ResolveRemoteExport: module '{}' not found", Encoding::WideToUtf8(moduleName));
        return std::nullopt;
    }

    const PE::Image image(module->path);
    const auto exported = image.FindExport(symbolName);
    if (!exported) {
        OSTP_LOG_WARN("ResolveRemoteExport: export {}!{} not found",
                      Encoding::PathToUtf8(module->path), std::string(symbolName));
        return std::nullopt;
    }

    if (!exported->IsForwarder()) {
        return module->base + exported->rva;
    }

    const size_t dot = exported->forwarder.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= exported->forwarder.size()) {
        OSTP_LOG_WARN("ResolveRemoteExport: unsupported forwarder '{}'", exported->forwarder);
        return std::nullopt;
    }

    const std::string_view forwardModule(exported->forwarder.data(), dot);
    const std::string_view forwardSymbol(exported->forwarder.data() + dot + 1, exported->forwarder.size() - dot - 1);
    if (!forwardSymbol.empty() && forwardSymbol.front() == '#') {
        OSTP_LOG_WARN("ResolveRemoteExport: ordinal forwarder '{}' is not supported", exported->forwarder);
        return std::nullopt;
    }

    return ResolveRemoteExport(modules, ModuleNameFromForwarder(forwardModule), forwardSymbol, depth + 1);
}

LPTHREAD_START_ROUTINE ResolveRemoteLoadLibraryW(uint32_t pid, Architecture architecture) {
    const std::vector<RemoteModule> modules = EnumerateRemoteModules(pid, architecture);
    const auto address = ResolveRemoteExport(modules, L"kernel32.dll", "LoadLibraryW");
    if (!address) {
        OSTP_LOG_WARN("ResolveRemoteLoadLibraryW(pid={}, arch={}) failed", pid, ToString(architecture));
        return nullptr;
    }
    return reinterpret_cast<LPTHREAD_START_ROUTINE>(*address);
}

} // namespace

const char* ToString(Architecture architecture) {
    switch (architecture) {
    case Architecture::Unknown: return "Unknown";
    case Architecture::X86: return "X86";
    case Architecture::X64: return "X64";
    }
    return "Unknown";
}

const char* ToString(InjectStatus status) {
    switch (status) {
    case InjectStatus::Ok: return "Ok";
    case InjectStatus::OpenProcessFailed: return "OpenProcessFailed";
    case InjectStatus::UnknownArchitecture: return "UnknownArchitecture";
    case InjectStatus::AllocFailed: return "AllocFailed";
    case InjectStatus::WriteFailed: return "WriteFailed";
    case InjectStatus::ResolveLoadLibraryFailed: return "ResolveLoadLibraryFailed";
    case InjectStatus::CreateThreadFailed: return "CreateThreadFailed";
    case InjectStatus::WaitFailed: return "WaitFailed";
    case InjectStatus::RemoteLoadFailed: return "RemoteLoadFailed";
    }
    return "Unknown";
}

Architecture GetArchitecture(uint32_t pid) {
    Windows::UniqueHandle process(
        pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr);
    if (!process) {
        OSTP_LOG_DEBUG("OpenProcess(pid={}, access=0x{:X}) failed (error={})",
                       pid, PROCESS_QUERY_LIMITED_INFORMATION, GetLastError());
        return Architecture::Unknown;
    }

    return DetectArchitecture(process.get());
}

InjectStatus InjectLibrary(uint32_t pid, const std::filesystem::path& libraryPath) {
    Windows::UniqueHandle process(pid ? OpenProcess(kInjectAccess, FALSE, pid) : nullptr);
    if (!process) {
        OSTP_LOG_WARN("OpenProcess(pid={}, access=0x{:X}) failed (error={})", pid, kInjectAccess, GetLastError());
        return InjectStatus::OpenProcessFailed;
    }

    const Architecture architecture = DetectArchitecture(process.get());
    if (architecture == Architecture::Unknown) {
        OSTP_LOG_WARN("InjectLibrary(pid={}): target architecture is unknown", pid);
        return InjectStatus::UnknownArchitecture;
    }

    LPTHREAD_START_ROUTINE loadLibrary = ResolveRemoteLoadLibraryW(pid, architecture);
    if (!loadLibrary) {
        return InjectStatus::ResolveLoadLibraryFailed;
    }

    const std::wstring nativePath = libraryPath.wstring();
    const size_t byteSize = (nativePath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process.get(), nullptr, byteSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        OSTP_LOG_WARN("VirtualAllocEx(size={}) failed (error={})", byteSize, GetLastError());
        return InjectStatus::AllocFailed;
    }
    // Free the remote path on every exit path. It must outlive the remote
    // thread, which reads it before LoadLibraryW returns; function scope (after
    // the WaitForSingleObject below) satisfies that.
    Windows::ScopeExit freeRemotePath([&] {
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
    });

    SIZE_T written = 0;
    if (!WriteProcessMemory(process.get(), remotePath, nativePath.c_str(), byteSize, &written) ||
        written != byteSize) {
        OSTP_LOG_WARN("WriteProcessMemory(size={}) failed (written={}, error={})",
                      byteSize, static_cast<size_t>(written), GetLastError());
        return InjectStatus::WriteFailed;
    }

    Windows::UniqueHandle thread(
        CreateRemoteThread(process.get(), nullptr, 0, loadLibrary, remotePath, 0, nullptr));
    if (!thread) {
        OSTP_LOG_WARN("CreateRemoteThread failed (error={})", GetLastError());
        return InjectStatus::CreateThreadFailed;
    }

    const DWORD wait = WaitForSingleObject(thread.get(), kRemoteLoadTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        if (wait == WAIT_FAILED) {
            OSTP_LOG_WARN("WaitForSingleObject(remote thread) failed (error={})", GetLastError());
        } else if (wait == WAIT_TIMEOUT) {
            OSTP_LOG_WARN("WaitForSingleObject(remote thread) timed out after {} ms", kRemoteLoadTimeoutMs);
            // Remote thread may still be executing LoadLibraryW. Do NOT free remotePath under a living thread.
            freeRemotePath.Dismiss();
        } else {
            OSTP_LOG_WARN("WaitForSingleObject(remote thread) returned unexpected status {}", wait);
            freeRemotePath.Dismiss();
        }
        return InjectStatus::WaitFailed;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeThread(thread.get(), &exitCode)) {
        OSTP_LOG_WARN("GetExitCodeThread(remote thread) failed (error={})", GetLastError());
        return InjectStatus::RemoteLoadFailed;
    }
    if (exitCode == 0) {
        OSTP_LOG_WARN("Remote LoadLibraryW returned 0 for '{}'", Encoding::PathToUtf8(libraryPath));
        return InjectStatus::RemoteLoadFailed;
    }

    return InjectStatus::Ok;
}

} // namespace OSTPlatform::RemoteProcess
