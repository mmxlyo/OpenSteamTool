using System;
using System.IO;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Collections.Generic;
using System.Reflection;
using Microsoft.Win32;

[assembly: AssemblyTitle("OpenSteamTool Auto Injector")]
[assembly: AssemblyDescription("OpenSteamTool Portable Background Helper and Auto Injector")]
[assembly: AssemblyConfiguration("")]
[assembly: AssemblyCompany("OpenSteamTool")]
[assembly: AssemblyProduct("OpenSteamTool")]
[assembly: AssemblyCopyright("Copyright © 2024-2026 OpenSteamTool")]
[assembly: AssemblyTrademark("")]
[assembly: AssemblyCulture("")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

namespace OpenSteamToolInjector
{
    class Program
    {
        #region Win32 API

        const uint PROCESS_ALL_ACCESS = 0x1F0FFF;
        const uint PROCESS_CREATE_THREAD = 0x0002;
        const uint PROCESS_QUERY_INFORMATION = 0x0400;
        const uint PROCESS_VM_OPERATION = 0x0008;
        const uint PROCESS_VM_WRITE = 0x0020;
        const uint PROCESS_VM_READ = 0x0010;

        const uint MEM_COMMIT = 0x1000;
        const uint MEM_RESERVE = 0x2000;
        const uint MEM_RELEASE = 0x8000;
        const uint PAGE_READWRITE = 0x04;

        const uint INFINITE = 0xFFFFFFFF;

        const uint TH32CS_SNAPMODULE = 0x00000008;
        const uint TH32CS_SNAPMODULE32 = 0x00000010;

        const int STD_OUTPUT_HANDLE = -11;
        const int STD_INPUT_HANDLE = -10;
        const int STD_ERROR_HANDLE = -12;
        const int ATTACH_PARENT_PROCESS = -1;
        const uint FILE_TYPE_UNKNOWN = 0x0000;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        struct MODULEENTRY32
        {
            public uint dwSize;
            public uint th32ModuleID;
            public uint th32ProcessID;
            public uint GlblcntUsage;
            public uint ProccntUsage;
            public IntPtr modBaseAddr;
            public uint modBaseSize;
            public IntPtr hModule;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
            public string szModule;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string szExePath;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr CreateToolhelp32Snapshot(uint dwFlags, uint th32ProcessID);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        static extern bool Module32First(IntPtr hSnapshot, ref MODULEENTRY32 lpme);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        static extern bool Module32Next(IntPtr hSnapshot, ref MODULEENTRY32 lpme);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
        static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, IntPtr dwSize, uint flAllocationType, uint flProtect);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress, IntPtr dwSize, uint dwFreeType);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, IntPtr nSize, out IntPtr lpNumberOfBytesWritten);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        static extern IntPtr GetModuleHandle(string lpModuleName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi, ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, IntPtr dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, IntPtr lpThreadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetExitCodeThread(IntPtr hThread, out uint lpExitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern uint GetPrivateProfileString(string lpAppName, string lpKeyName, string lpDefault, StringBuilder lpReturnedString, uint nSize, string lpFileName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool WritePrivateProfileString(string lpAppName, string lpKeyName, string lpString, string lpFileName);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern int MessageBox(IntPtr hWnd, string text, string caption, uint type);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool AttachConsole(int dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool AllocConsole();

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr GetStdHandle(int nStdHandle);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint GetFileType(IntPtr hFile);

        #endregion

        static void EnsureInteractiveConsole()
        {
            try
            {
                IntPtr hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                uint fileType = (hOut != IntPtr.Zero && hOut != new IntPtr(-1)) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;

                if (fileType == FILE_TYPE_UNKNOWN)
                {
                    if (!AttachConsole(ATTACH_PARENT_PROCESS))
                    {
                        AllocConsole();
                    }
                    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                }

                if (hOut != IntPtr.Zero && hOut != new IntPtr(-1))
                {
                    Microsoft.Win32.SafeHandles.SafeFileHandle safeOut = new Microsoft.Win32.SafeHandles.SafeFileHandle(hOut, false);
                    FileStream fsOut = new FileStream(safeOut, FileAccess.Write);
                    StreamWriter writer = new StreamWriter(fsOut, Console.OutputEncoding) { AutoFlush = true };
                    Console.SetOut(writer);
                    Console.SetError(writer);
                }

                IntPtr hIn = GetStdHandle(STD_INPUT_HANDLE);
                if (hIn != IntPtr.Zero && hIn != new IntPtr(-1))
                {
                    Microsoft.Win32.SafeHandles.SafeFileHandle safeIn = new Microsoft.Win32.SafeHandles.SafeFileHandle(hIn, false);
                    FileStream fsIn = new FileStream(safeIn, FileAccess.Read);
                    StreamReader reader = new StreamReader(fsIn, Console.InputEncoding);
                    Console.SetIn(reader);
                }
            }
            catch { }
        }

        static void SafeSetColor(ConsoleColor color)
        {
            try { Console.ForegroundColor = color; } catch { }
        }

        static void SafeResetColor()
        {
            try { Console.ResetColor(); } catch { }
        }

        static void SafeSetTitle(string title)
        {
            try { Console.Title = title; } catch { }
        }

        static string GetSteamPathFromRegistry()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"SOFTWARE\Valve\Steam"))
                {
                    if (key != null)
                    {
                        object val = key.GetValue("SteamExe");
                        if (val != null && !string.IsNullOrEmpty(val.ToString()))
                        {
                            string path = val.ToString().Replace('/', '\\');
                            if (File.Exists(path))
                                return path;
                        }

                        object pathVal = key.GetValue("SteamPath");
                        if (pathVal != null && !string.IsNullOrEmpty(pathVal.ToString()))
                        {
                            string combined = Path.Combine(pathVal.ToString().Replace('/', '\\'), "steam.exe");
                            if (File.Exists(combined))
                                return combined;
                        }
                    }
                }
            }
            catch { }
            return string.Empty;
        }

        static void ReadIniSettings(string iniPath, out string exePath, out string dllPath)
        {
            exePath = "";
            dllPath = "";
            if (!File.Exists(iniPath)) return;

            try
            {
                string currentSection = "";
                foreach (string rawLine in File.ReadAllLines(iniPath, Encoding.UTF8))
                {
                    string line = rawLine.Trim();
                    if (string.IsNullOrEmpty(line) || line.StartsWith(";") || line.StartsWith("#"))
                        continue;

                    if (line.StartsWith("[") && line.EndsWith("]"))
                    {
                        currentSection = line.Substring(1, line.Length - 2).Trim();
                        continue;
                    }

                    int eq = line.IndexOf('=');
                    if (eq > 0 && currentSection.Equals("Settings", StringComparison.OrdinalIgnoreCase))
                    {
                        string key = line.Substring(0, eq).Trim();
                        string val = line.Substring(eq + 1).Trim();
                        if (key.Equals("ExePath", StringComparison.OrdinalIgnoreCase)) exePath = val;
                        else if (key.Equals("DllPath", StringComparison.OrdinalIgnoreCase)) dllPath = val;
                    }
                }
            }
            catch { }
        }

        static bool IsModuleLoaded(int pid, string targetModuleName)
        {
            IntPtr hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, (uint)pid);
            if (hSnap == IntPtr.Zero || hSnap == (IntPtr)(-1))
            {
                try
                {
                    Process p = Process.GetProcessById(pid);
                    foreach (ProcessModule m in p.Modules)
                    {
                        if (string.Equals(m.ModuleName, targetModuleName, StringComparison.OrdinalIgnoreCase))
                            return true;
                    }
                }
                catch { }
                return false;
            }

            try
            {
                MODULEENTRY32 me = new MODULEENTRY32();
                me.dwSize = (uint)Marshal.SizeOf(typeof(MODULEENTRY32));

                if (Module32First(hSnap, ref me))
                {
                    do
                    {
                        if (string.Equals(me.szModule, targetModuleName, StringComparison.OrdinalIgnoreCase))
                            return true;
                    }
                    while (Module32Next(hSnap, ref me));
                }
            }
            finally
            {
                CloseHandle(hSnap);
            }
            return false;
        }

        static bool InjectDllByHandle(IntPtr hProcess, string dllPath, bool isSilent = false)
        {
            byte[] bytes = Encoding.Unicode.GetBytes(dllPath + "\0");
            IntPtr size = new IntPtr(bytes.Length);

            IntPtr remoteMem = VirtualAllocEx(hProcess, IntPtr.Zero, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remoteMem == IntPtr.Zero)
            {
                if (!isSilent)
                {
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine("[-] VirtualAllocEx failed. Error: " + Marshal.GetLastWin32Error());
                    Console.ResetColor();
                }
                return false;
            }

            try
            {
                IntPtr written;
                if (!WriteProcessMemory(hProcess, remoteMem, bytes, size, out written))
                {
                    if (!isSilent)
                    {
                        Console.ForegroundColor = ConsoleColor.Red;
                        Console.WriteLine("[-] WriteProcessMemory failed. Error: " + Marshal.GetLastWin32Error());
                        Console.ResetColor();
                    }
                    return false;
                }

                IntPtr hKernel32 = GetModuleHandle("kernel32.dll");
                IntPtr loadLibraryWAddr = GetProcAddress(hKernel32, "LoadLibraryW");
                if (loadLibraryWAddr == IntPtr.Zero)
                {
                    if (!isSilent)
                    {
                        Console.ForegroundColor = ConsoleColor.Red;
                        Console.WriteLine("[-] Failed to find LoadLibraryW. Error: " + Marshal.GetLastWin32Error());
                        Console.ResetColor();
                    }
                    return false;
                }

                IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, IntPtr.Zero, loadLibraryWAddr, remoteMem, 0, IntPtr.Zero);
                if (hThread == IntPtr.Zero)
                {
                    if (!isSilent)
                    {
                        Console.ForegroundColor = ConsoleColor.Red;
                        Console.WriteLine("[-] CreateRemoteThread failed. Error: " + Marshal.GetLastWin32Error());
                        Console.ResetColor();
                    }
                    return false;
                }

                try
                {
                    WaitForSingleObject(hThread, INFINITE);
                    uint exitCode;
                    if (GetExitCodeThread(hThread, out exitCode))
                    {
                        if (exitCode == 0)
                        {
                            if (!isSilent)
                            {
                                Console.ForegroundColor = ConsoleColor.Yellow;
                                Console.WriteLine("[-] Warning: LoadLibraryW returned 0 (NULL). DLL might have failed in DllMain or dependencies missing.");
                                Console.ResetColor();
                            }
                            return false;
                        }
                    }
                    return true;
                }
                finally
                {
                    CloseHandle(hThread);
                }
            }
            finally
            {
                VirtualFreeEx(hProcess, remoteMem, IntPtr.Zero, MEM_RELEASE);
            }
        }

        static void ShowErrorAlert(string message)
        {
            MessageBox(IntPtr.Zero, message, "OpenSteamTool Injector Error", 0x10 | 0x10000);
        }

        static void LogMessage(string baseDir, string msg, bool isSilent = false)
        {
            if (!isSilent)
            {
                Console.WriteLine(msg);
            }
            try
            {
                string logFile = Path.Combine(baseDir, "inject.log");
                File.AppendAllText(logFile, string.Format("[{0:yyyy-MM-dd HH:mm:ss}] {1}\r\n", DateTime.Now, msg));
            }
            catch { }
        }

        static void RunWatcher(string baseDir, string absDllPath)
        {
            bool createdNew;
            using (Mutex mutex = new Mutex(true, "Global\\OpenSteamTool_AutoInject_Watcher", out createdNew))
            {
                if (!createdNew)
                {
                    LogMessage(baseDir, "[Watcher] 检测到已有另一个后台监听实例在运行，本实例自动退出。", true);
                    return;
                }

                LogMessage(baseDir, "[Watcher] 自动注入后台监听已启动，等待 steam.exe 启动...", true);

                if (!File.Exists(absDllPath))
                {
                    LogMessage(baseDir, "[Watcher] 警告: 未找到 Payload DLL 文件: " + absDllPath, true);
                }

                HashSet<int> injectedPids = new HashSet<int>();

                while (true)
                {
                    try
                    {
                        Process[] steams = Process.GetProcessesByName("steam");
                        if (steams.Length > 0)
                        {
                            HashSet<int> currentPids = new HashSet<int>();
                            foreach (Process p in steams)
                            {
                                currentPids.Add(p.Id);
                            }
                            injectedPids.RemoveWhere(pid => !currentPids.Contains(pid));

                            foreach (Process p in steams)
                            {
                                int pid = p.Id;
                                if (!injectedPids.Contains(pid))
                                {
                                    if (IsModuleLoaded(pid, "OpenSteamTool.dll"))
                                    {
                                        injectedPids.Add(pid);
                                        continue;
                                    }

                                    // 等待 steamui.dll 准备就绪
                                    bool uiReady = false;
                                    for (int i = 0; i < 60; i++)
                                    {
                                        if (p.HasExited) break;
                                        if (IsModuleLoaded(pid, "steamui.dll"))
                                        {
                                            uiReady = true;
                                            break;
                                        }
                                        Thread.Sleep(500);
                                    }

                                    if (uiReady && !p.HasExited)
                                    {
                                        // 稍微延迟 500ms 保证初始化完全
                                        Thread.Sleep(500);

                                        uint access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
                                        IntPtr hProcess = OpenProcess(access, false, pid);
                                        if (hProcess != IntPtr.Zero)
                                        {
                                            try
                                            {
                                                if (InjectDllByHandle(hProcess, absDllPath, true))
                                                {
                                                    injectedPids.Add(pid);
                                                    LogMessage(baseDir, string.Format("[Watcher] 成功自动注入 OpenSteamTool 到 Steam (PID: {0})", pid), true);
                                                }
                                                else
                                                {
                                                    LogMessage(baseDir, string.Format("[Watcher] 注入失败 (PID: {0})", pid), true);
                                                }
                                            }
                                            finally
                                            {
                                                CloseHandle(hProcess);
                                            }
                                        }
                                        else
                                        {
                                            LogMessage(baseDir, string.Format("[Watcher] 无法打开 Steam 进程 (PID: {0})，错误码: {1}。若 Steam 以管理员运行，请以管理员身份运行注入器。", pid, Marshal.GetLastWin32Error()), true);
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            if (injectedPids.Count > 0)
                            {
                                injectedPids.Clear();
                            }
                        }
                    }
                    catch (Exception ex)
                    {
                        LogMessage(baseDir, "[Watcher] 循环异常: " + ex.Message, true);
                    }

                    Thread.Sleep(1500);
                }
            }
        }

        static int RunSilentOnce(string baseDir, string absDllPath)
        {
            try
            {
                Process[] steams = Process.GetProcessesByName("steam");
                if (steams.Length == 0)
                {
                    return 0;
                }

                Process target = steams[0];
                int pid = target.Id;

                if (IsModuleLoaded(pid, "OpenSteamTool.dll"))
                {
                    return 0;
                }

                bool uiReady = false;
                for (int i = 0; i < 60; i++)
                {
                    if (target.HasExited) return 0;
                    if (IsModuleLoaded(pid, "steamui.dll"))
                    {
                        uiReady = true;
                        break;
                    }
                    Thread.Sleep(500);
                }

                if (!uiReady || target.HasExited) return 0;

                Thread.Sleep(500);

                uint access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
                IntPtr hProcess = OpenProcess(access, false, pid);
                if (hProcess != IntPtr.Zero)
                {
                    try
                    {
                        if (InjectDllByHandle(hProcess, absDllPath, true))
                        {
                            LogMessage(baseDir, string.Format("[Silent] 成功静默注入 OpenSteamTool 到 Steam (PID: {0})", pid), true);
                            return 0;
                        }
                        else
                        {
                            LogMessage(baseDir, string.Format("[Silent] 注入失败 (PID: {0})", pid), true);
                            return 1;
                        }
                    }
                    finally
                    {
                        CloseHandle(hProcess);
                    }
                }
            }
            catch (Exception ex)
            {
                LogMessage(baseDir, "[Silent] 异常: " + ex.Message, true);
            }
            return 0;
        }

        static int Main(string[] args)
        {
            bool isWatchMode = false;
            bool isSilentMode = false;

            foreach (string arg in args)
            {
                string a = arg.Trim().ToLowerInvariant();
                if (a == "-watch" || a == "--watch" || a == "-daemon" || a == "/watch")
                {
                    isWatchMode = true;
                }
                else if (a == "-silent" || a == "--silent" || a == "-s" || a == "/s")
                {
                    isSilentMode = true;
                }
            }

            // 只有非静默且非后台监听模式（交互模式）才动态接入或分配控制台
            if (!isWatchMode && !isSilentMode)
            {
                EnsureInteractiveConsole();
            }

            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string iniPath = Path.Combine(baseDir, "config.ini");
            string steamReg = GetSteamPathFromRegistry();

            if (!File.Exists(iniPath))
            {
                string defaultExe = !string.IsNullOrEmpty(steamReg) ? steamReg : @"C:\Program Files (x86)\Steam\steam.exe";
                string defaultDll = @"C:\Program Files (x86)\Steam\OpenSteamTool.dll";
                File.WriteAllText(iniPath, "[Settings]\r\nExePath=" + defaultExe + "\r\nDllPath=" + defaultDll + "\r\n", Encoding.ASCII);
            }

            string exePath = "";
            string dllPath = "";
            ReadIniSettings(iniPath, out exePath, out dllPath);

            if (string.IsNullOrEmpty(exePath))
            {
                exePath = !string.IsNullOrEmpty(steamReg) ? steamReg : @"C:\Program Files (x86)\Steam\steam.exe";
            }

            if (string.IsNullOrEmpty(dllPath))
            {
                dllPath = @"C:\Program Files (x86)\Steam\OpenSteamTool.dll";
            }

            string absDllPath = Path.IsPathRooted(dllPath) ? dllPath : Path.GetFullPath(Path.Combine(baseDir, dllPath));

            // 模式 1：后台常驻监听模式 (-watch)
            if (isWatchMode)
            {
                RunWatcher(baseDir, absDllPath);
                return 0;
            }

            // 模式 2：单次静默注入模式 (-silent)
            if (isSilentMode)
            {
                return RunSilentOnce(baseDir, absDllPath);
            }

            // 模式 3：常规交互式控制台模式 (直接双击)
            SafeSetTitle("OpenSteamTool Auto Injector");
            SafeSetColor(ConsoleColor.Cyan);
            Console.WriteLine("=================================================");
            Console.WriteLine("       OpenSteamTool Auto Injector               ");
            Console.WriteLine("       Supported modes: manual, -silent, -watch   ");
            Console.WriteLine("=================================================");
            SafeResetColor();
            Console.WriteLine();

            try
            {
                Console.WriteLine("[+] Target Executable : " + exePath);
                Console.WriteLine("[+] Payload DLL       : " + absDllPath);
                Console.WriteLine();

                if (!File.Exists(absDllPath))
                {
                    string err = "Error: Payload DLL was not found at:\n" + absDllPath + "\n\nPlease ensure OpenSteamTool.dll exists.";
                    SafeSetColor(ConsoleColor.Red);
                    Console.WriteLine("[-] " + err);
                    SafeResetColor();
                    ShowErrorAlert(err);
                    return 1;
                }

                Process targetProcess = null;
                Process[] existing = Process.GetProcessesByName("steam");
                if (existing.Length > 0)
                {
                    targetProcess = existing[0];
                    SafeSetColor(ConsoleColor.Green);
                    Console.WriteLine("[+] Found running Steam process (PID: " + targetProcess.Id + ")");
                    SafeResetColor();

                    if (IsModuleLoaded(targetProcess.Id, "OpenSteamTool.dll"))
                    {
                        SafeSetColor(ConsoleColor.Yellow);
                        Console.WriteLine("[!] 当前 Steam 进程已加载过 OpenSteamTool.dll！");
                        Console.WriteLine("[!] 无需重复注入。");
                        SafeResetColor();
                        Console.WriteLine();
                        Console.WriteLine("This console will close in 3 seconds...");
                        Thread.Sleep(3000);
                        return 0;
                    }
                }

                if (targetProcess == null)
                {
                    if (!File.Exists(exePath))
                    {
                        string err = "Error: Target Steam executable does not exist at:\n" + exePath;
                        SafeSetColor(ConsoleColor.Red);
                        Console.WriteLine("[-] " + err);
                        SafeResetColor();
                        ShowErrorAlert(err);
                        return 1;
                    }

                    Console.WriteLine("[+] Launching Steam process...");
                    ProcessStartInfo psi = new ProcessStartInfo();
                    psi.FileName = exePath;
                    psi.WorkingDirectory = Path.GetDirectoryName(exePath);
                    psi.UseShellExecute = true;
                    targetProcess = Process.Start(psi);
                    Console.WriteLine("[+] Steam launched (PID: " + targetProcess.Id + ")");
                }

                Console.WriteLine("[+] Waiting for steamui.dll to be loaded in Steam process...");
                bool moduleFound = false;
                DateTime startWait = DateTime.UtcNow;

                while ((DateTime.UtcNow - startWait).TotalSeconds < 30)
                {
                    if (IsModuleLoaded(targetProcess.Id, "steamui.dll"))
                    {
                        moduleFound = true;
                        break;
                    }
                    Thread.Sleep(200);
                }

                if (!moduleFound)
                {
                    throw new TimeoutException("Timeout reached: steamui.dll was not loaded within 30 seconds.");
                }

                SafeSetColor(ConsoleColor.Green);
                Console.WriteLine("[+] steamui.dll detected in Steam process!");
                SafeResetColor();

                Console.WriteLine("[+] Injecting OpenSteamTool.dll into Steam...");
                uint access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
                IntPtr hProcess = OpenProcess(access, false, targetProcess.Id);

                if (hProcess == IntPtr.Zero)
                {
                    throw new InvalidOperationException("Failed to open Steam process. Try running as Administrator. Error: " + Marshal.GetLastWin32Error());
                }

                try
                {
                    if (!InjectDllByHandle(hProcess, absDllPath))
                    {
                        throw new InvalidOperationException("DLL injection failed into Steam process.");
                    }
                }
                finally
                {
                    CloseHandle(hProcess);
                }

                SafeSetColor(ConsoleColor.Green);
                Console.WriteLine();
                Console.WriteLine("[+] =============================================");
                Console.WriteLine("[+]  SUCCESS: OpenSteamTool injected perfectly! ");
                Console.WriteLine("[+] =============================================");
                SafeResetColor();
                Console.WriteLine();
                Console.WriteLine("This console will close in 3 seconds...");
                Thread.Sleep(3000);
                return 0;
            }
            catch (Exception ex)
            {
                SafeSetColor(ConsoleColor.Red);
                Console.WriteLine();
                Console.WriteLine("[-] Error: " + ex.Message);
                SafeResetColor();
                ShowErrorAlert(ex.Message);
                Console.WriteLine("Press any key to exit...");
                try { Console.ReadKey(); } catch { Console.ReadLine(); }
                return 1;
            }
        }
    }
}
