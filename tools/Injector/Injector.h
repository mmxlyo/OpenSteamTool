#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace Injector {

    // Process & Module Inspection
    bool IsModuleLoaded(DWORD pid, const std::wstring& moduleName);
    std::vector<DWORD> FindProcessesByName(const std::wstring& processName);

    // Injection Primitives
    bool InjectDllByHandle(HANDLE hProcess, const std::wstring& dllPath, bool isSilent = false);

    // Path & Registry Resolution
    std::wstring GetExecutableDirectory();
    std::wstring GetIniFilePath(const std::wstring& iniFileName);
    std::wstring GetSteamPathFromRegistry();
    std::wstring ResolveAbsoluteDllPath(const std::wstring& rawDllPath, const std::wstring& baseDir);
    bool FileExists(const std::wstring& filePath);

    // Execution Modes
    void RunInteractive(const std::wstring& baseDir, const std::wstring& exePath, const std::wstring& dllPath);
    int  RunWatcher(const std::wstring& baseDir, const std::wstring& dllPath);
    int  RunSilentOnce(const std::wstring& baseDir, const std::wstring& dllPath);

    // Logging & Notifications
    void LogMessage(const std::wstring& baseDir, const std::string& msg, bool isSilent = false);
    void ShowErrorAlert(const std::wstring& message);

} // namespace Injector
