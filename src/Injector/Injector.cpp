#include "Injector.h"

#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <set>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cwchar>
#include <ctime>
#include <filesystem>

namespace Injector {

    bool IsModuleLoaded(DWORD pid, const std::wstring& moduleName) {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (hSnap == INVALID_HANDLE_VALUE) return false;

        MODULEENTRY32W me = { sizeof(me) };
        bool found = false;

        if (Module32FirstW(hSnap, &me)) {
            do {
                if (_wcsicmp(moduleName.c_str(), me.szModule) == 0) {
                    found = true;
                    break;
                }
            } while (Module32NextW(hSnap, &me));
        }

        CloseHandle(hSnap);
        return found;
    }

    std::vector<DWORD> FindProcessesByName(const std::wstring& processName) {
        std::vector<DWORD> pids;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return pids;

        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(processName.c_str(), pe.szExeFile) == 0) {
                    pids.push_back(pe.th32ProcessID);
                }
            } while (Process32NextW(hSnap, &pe));
        }

        CloseHandle(hSnap);
        return pids;
    }

    bool InjectDllByHandle(HANDLE hProcess, const std::wstring& dllPath, bool isSilent) {
        SIZE_T byteCount = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remoteMem = VirtualAllocEx(hProcess, nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteMem) {
            if (!isSilent) {
                std::wcerr << L"[-] VirtualAllocEx failed. Error: " << GetLastError() << std::endl;
            }
            return false;
        }

        if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), byteCount, nullptr)) {
            if (!isSilent) {
                std::wcerr << L"[-] WriteProcessMemory failed. Error: " << GetLastError() << std::endl;
            }
            VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
            return false;
        }

        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadLibraryWAddr = GetProcAddress(hKernel32, "LoadLibraryW");
        if (!loadLibraryWAddr) {
            if (!isSilent) {
                std::wcerr << L"[-] Failed to locate LoadLibraryW. Error: " << GetLastError() << std::endl;
            }
            VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
            return false;
        }

        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryWAddr),
            remoteMem, 0, nullptr);

        if (!hThread) {
            if (!isSilent) {
                std::wcerr << L"[-] CreateRemoteThread failed. Error: " << GetLastError() << std::endl;
            }
            VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
            return false;
        }

        WaitForSingleObject(hThread, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);

        if (exitCode == 0) {
            DWORD pid = GetProcessId(hProcess);
            if (pid != 0 && IsModuleLoaded(pid, L"OpenSteamTool.dll")) {
                return true;
            }
            if (!isSilent) {
                std::wcerr << L"[-] Warning: LoadLibraryW returned NULL. The DLL initialization may have failed." << std::endl;
            }
            return false;
        }

        return true;
    }

    std::wstring GetExecutableDirectory() {
        wchar_t buffer[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, buffer, MAX_PATH);
        std::wstring exePath(buffer);
        size_t lastSlash = exePath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return exePath.substr(0, lastSlash);
        }
        return L".";
    }

    std::wstring GetIniFilePath(const std::wstring& iniFileName) {
        return GetExecutableDirectory() + L"\\" + iniFileName;
    }

    std::wstring GetSteamPathFromRegistry() {
        HKEY hKey = nullptr;
        std::wstring steamExePath = L"";

        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t buffer[MAX_PATH] = { 0 };
            DWORD bufferSize = sizeof(buffer);
            DWORD type = REG_SZ;

            if (RegQueryValueExW(hKey, L"SteamExe", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bufferSize) == ERROR_SUCCESS) {
                steamExePath = buffer;
                for (wchar_t& ch : steamExePath) {
                    if (ch == L'/') ch = L'\\';
                }
            } else {
                bufferSize = sizeof(buffer);
                if (RegQueryValueExW(hKey, L"SteamPath", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bufferSize) == ERROR_SUCCESS) {
                    std::wstring steamDir = buffer;
                    for (wchar_t& ch : steamDir) {
                        if (ch == L'/') ch = L'\\';
                    }
                    steamExePath = steamDir + L"\\steam.exe";
                }
            }
            RegCloseKey(hKey);
        }
        return steamExePath;
    }

    std::wstring ResolveAbsoluteDllPath(const std::wstring& rawDllPath, const std::wstring& baseDir) {
        std::filesystem::path raw(rawDllPath);
        if (raw.is_absolute()) {
            return raw.lexically_normal().wstring();
        }
        std::filesystem::path base(baseDir);
        return (base / raw).lexically_normal().wstring();
    }

    bool FileExists(const std::wstring& filePath) {
        DWORD attributes = GetFileAttributesW(filePath.c_str());
        return (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY));
    }

    void LogMessage(const std::wstring& baseDir, const std::string& msg, bool isSilent) {
        if (!isSilent) {
            std::cout << msg << std::endl;
        }
        try {
            std::wstring logFile = baseDir + L"\\inject.log";
            std::ofstream ofs(logFile, std::ios::app | std::ios::binary);
            if (ofs.is_open()) {
                auto now = std::chrono::system_clock::now();
                auto timeT = std::chrono::system_clock::to_time_t(now);
                std::tm tmNow;
                localtime_s(&tmNow, &timeT);

                std::ostringstream ss;
                ss << "[" << std::put_time(&tmNow, "%Y-%m-%d %H:%M:%S") << "] " << msg << "\r\n";
                std::string line = ss.str();
                ofs.write(line.c_str(), line.size());
            }
        } catch (...) {}
    }

    void ShowErrorAlert(const std::wstring& message) {
        MessageBoxW(NULL, message.c_str(), L"OpenSteamTool Injector Error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }

    int RunWatcher(const std::wstring& baseDir, const std::wstring& dllPath) {
        HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\OpenSteamTool_AutoInject_Watcher");
        if (!hMutex && GetLastError() == ERROR_ACCESS_DENIED) {
            hMutex = CreateMutexW(NULL, TRUE, L"Local\\OpenSteamTool_AutoInject_Watcher");
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hMutex) CloseHandle(hMutex);
            return 0; // Instance already running
        }

        LogMessage(baseDir, "[Watcher] 自动注入后台监听已启动，等待 steam.exe 启动...", true);
        std::set<DWORD> injectedPids;

        constexpr DWORD kInjectAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                       PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

        while (true) {
            std::vector<DWORD> pids = FindProcessesByName(L"steam.exe");
            if (!pids.empty()) {
                std::set<DWORD> currentPids(pids.begin(), pids.end());
                for (auto it = injectedPids.begin(); it != injectedPids.end(); ) {
                    if (!currentPids.count(*it)) {
                        it = injectedPids.erase(it);
                    } else {
                        ++it;
                    }
                }

                for (DWORD pid : pids) {
                    if (injectedPids.count(pid)) continue;

                    if (IsModuleLoaded(pid, L"OpenSteamTool.dll")) {
                        injectedPids.insert(pid);
                        continue;
                    }

                    // Wait for steamui.dll to be loaded
                    bool uiReady = false;
                    for (int i = 0; i < 60; ++i) {
                        HANDLE hCheck = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        if (!hCheck) break;
                        DWORD exitCode = 0;
                        GetExitCodeProcess(hCheck, &exitCode);
                        CloseHandle(hCheck);
                        if (exitCode != STILL_ACTIVE) break;

                        if (IsModuleLoaded(pid, L"steamui.dll")) {
                            uiReady = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }

                    if (uiReady) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        HANDLE hProcess = OpenProcess(kInjectAccess, FALSE, pid);
                        if (hProcess) {
                            if (InjectDllByHandle(hProcess, dllPath, true)) {
                                injectedPids.insert(pid);
                                LogMessage(baseDir, "[Watcher] 成功自动注入 OpenSteamTool 到 Steam (PID: " + std::to_string(pid) + ")", true);
                            } else {
                                LogMessage(baseDir, "[Watcher] 注入失败 (PID: " + std::to_string(pid) + ")", true);
                            }
                            CloseHandle(hProcess);
                        }
                    }
                }
            } else {
                if (!injectedPids.empty()) {
                    injectedPids.clear();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }

        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    int RunSilentOnce(const std::wstring& baseDir, const std::wstring& dllPath) {
        std::vector<DWORD> pids = FindProcessesByName(L"steam.exe");
        if (pids.empty()) return 0;

        DWORD pid = pids[0];
        if (IsModuleLoaded(pid, L"OpenSteamTool.dll")) return 0;

        bool uiReady = false;
        for (int i = 0; i < 60; ++i) {
            if (IsModuleLoaded(pid, L"steamui.dll")) {
                uiReady = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!uiReady) return 0;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        constexpr DWORD kInjectAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                       PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
        HANDLE hProcess = OpenProcess(kInjectAccess, FALSE, pid);
        if (hProcess) {
            bool ok = InjectDllByHandle(hProcess, dllPath, true);
            CloseHandle(hProcess);
            if (ok) {
                LogMessage(baseDir, "[Silent] 成功静默注入 OpenSteamTool 到 Steam (PID: " + std::to_string(pid) + ")", true);
                return 0;
            } else {
                LogMessage(baseDir, "[Silent] 注入失败 (PID: " + std::to_string(pid) + ")", true);
                return 1;
            }
        }
        return 0;
    }

    void RunInteractive(const std::wstring& baseDir, const std::wstring& exePath, const std::wstring& dllPath) {
        SetConsoleTitleW(L"OpenSteamTool Injector (ost-Injector)");

        std::cout << "=================================================" << std::endl;
        std::cout << "       OpenSteamTool Injector (ost-Injector)     " << std::endl;
        std::cout << "       Supported modes: manual, -silent, -watch   " << std::endl;
        std::cout << "=================================================" << std::endl;
        std::cout << std::endl;

        std::wcout << L"[+] Target Executable : " << exePath << std::endl;
        std::wcout << L"[+] Payload DLL       : " << dllPath << std::endl;
        std::cout << std::endl;

        if (!FileExists(dllPath)) {
            std::wstring err = L"Error: Payload DLL was not found at:\n" + dllPath + L"\n\nPlease ensure OpenSteamTool.dll exists.";
            std::wcerr << L"[-] " << err << std::endl;
            ShowErrorAlert(err);
            return;
        }

        std::vector<DWORD> existingPids = FindProcessesByName(L"steam.exe");
        constexpr DWORD kInjectAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                       PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

        if (!existingPids.empty()) {
            DWORD pid = existingPids[0];
            std::cout << "[+] Found running Steam process (PID: " << pid << ")" << std::endl;

            if (IsModuleLoaded(pid, L"OpenSteamTool.dll")) {
                std::cout << "[!] 当前 Steam 进程已加载过 OpenSteamTool.dll！" << std::endl;
                std::cout << "[!] 无需重复注入。" << std::endl;
                std::cout << "This console will close in 3 seconds..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(3));
                return;
            }

            std::cout << "[+] Waiting for steamui.dll to load..." << std::endl;
            for (int i = 0; i < 60; ++i) {
                if (IsModuleLoaded(pid, L"steamui.dll")) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            std::cout << "[+] Injecting DLL into running Steam..." << std::endl;
            HANDLE hProcess = OpenProcess(kInjectAccess, FALSE, pid);
            if (hProcess) {
                if (InjectDllByHandle(hProcess, dllPath, false)) {
                    std::cout << "[+] Injection completed successfully." << std::endl;
                } else {
                    std::wcerr << L"[-] Injection failed." << std::endl;
                    ShowErrorAlert(L"DLL injection into running Steam process failed.");
                }
                CloseHandle(hProcess);
            } else {
                std::wcerr << L"[-] OpenProcess failed. Error: " << GetLastError() << std::endl;
                ShowErrorAlert(L"Failed to open Steam process. Try running as Administrator.");
            }
            return;
        }

        // Steam is not running: launch it
        if (!FileExists(exePath)) {
            std::wstring err = L"Error: Target Steam executable does not exist at:\n" + exePath;
            std::wcerr << L"[-] " << err << std::endl;
            ShowErrorAlert(err);
            return;
        }

        size_t lastSlash = exePath.find_last_of(L"\\/");
        std::wstring workingDir = (lastSlash == std::wstring::npos) ? L"" : exePath.substr(0, lastSlash);

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        std::vector<wchar_t> cmdBuffer(exePath.begin(), exePath.end());
        cmdBuffer.push_back(L'\0');

        std::cout << "[+] Launching Steam executable..." << std::endl;
        if (!CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
            workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi)) {
            std::wstring err = L"CreateProcessW failed. Error: " + std::to_wstring(GetLastError());
            std::wcerr << L"[-] " << err << std::endl;
            ShowErrorAlert(err);
            return;
        }

        std::cout << "[+] Waiting for steamui.dll to load..." << std::endl;
        bool moduleFound = false;
        auto startTime = std::chrono::steady_clock::now();

        while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count() < 30) {
            if (IsModuleLoaded(pi.dwProcessId, L"steamui.dll")) {
                moduleFound = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (!moduleFound) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            std::wcerr << L"[-] Timeout reached. steamui.dll never loaded." << std::endl;
            ShowErrorAlert(L"Timeout waiting for Steam UI to initialize.");
            return;
        }

        std::cout << "[+] Injecting DLL into spawned Steam..." << std::endl;
        if (!InjectDllByHandle(pi.hProcess, dllPath, false)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            std::wcerr << L"[-] DLL injection failed." << std::endl;
            ShowErrorAlert(L"DLL injection failed.");
            return;
        }

        std::cout << "[+] Injection completed successfully." << std::endl;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

} // namespace Injector

int main(int argc, char* argv[]) {
    bool isWatchMode = false;
    bool isSilentMode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        for (char& c : arg) c = static_cast<char>(tolower(c));
        if (arg == "-watch" || arg == "--watch" || arg == "-daemon" || arg == "/watch") {
            isWatchMode = true;
        } else if (arg == "-silent" || arg == "--silent" || arg == "-s" || arg == "/s") {
            isSilentMode = true;
        }
    }

    if (isWatchMode || isSilentMode) {
        HWND hWnd = GetConsoleWindow();
        if (hWnd) ShowWindow(hWnd, SW_HIDE);
    }

    std::wstring baseDir = Injector::GetExecutableDirectory();
    std::wstring iniPath = Injector::GetIniFilePath(L"config.ini");
    std::wstring steamReg = Injector::GetSteamPathFromRegistry();

    if (!Injector::FileExists(iniPath)) {
        std::wstring defaultExe = !steamReg.empty() ? steamReg : L"C:\\Program Files (x86)\\Steam\\steam.exe";
        std::wstring defaultDll = L"OpenSteamTool.dll";
        WritePrivateProfileStringW(L"Settings", L"ExePath", defaultExe.c_str(), iniPath.c_str());
        WritePrivateProfileStringW(L"Settings", L"DllPath", defaultDll.c_str(), iniPath.c_str());
    }

    wchar_t wExeBuffer[MAX_PATH] = { 0 };
    wchar_t wDllBuffer[MAX_PATH] = { 0 };
    GetPrivateProfileStringW(L"Settings", L"ExePath", L"", wExeBuffer, MAX_PATH, iniPath.c_str());
    GetPrivateProfileStringW(L"Settings", L"DllPath", L"", wDllBuffer, MAX_PATH, iniPath.c_str());

    std::wstring exePath = wExeBuffer;
    std::wstring rawDllPath = wDllBuffer;

    if (exePath.empty()) {
        exePath = !steamReg.empty() ? steamReg : L"C:\\Program Files (x86)\\Steam\\steam.exe";
    }
    if (rawDllPath.empty()) {
        rawDllPath = L"OpenSteamTool.dll";
    }

    std::wstring absDllPath = Injector::ResolveAbsoluteDllPath(rawDllPath, baseDir);

    if (isWatchMode) {
        return Injector::RunWatcher(baseDir, absDllPath);
    }
    if (isSilentMode) {
        return Injector::RunSilentOnce(baseDir, absDllPath);
    }

    Injector::RunInteractive(baseDir, exePath, absDllPath);
    return 0;
}
