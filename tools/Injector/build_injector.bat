@echo off
setlocal
echo [OpenSteamTool] Compiling portable Injector (C#)...

set "CSC=%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%SystemRoot%\Microsoft.NET\Framework\v4.0.30319\csc.exe"

if not exist "%CSC%" (
    echo [Error] csc.exe compiler not found!
    pause
    exit /b 1
)

"%CSC%" /nologo /target:winexe /platform:x64 /optimize+ /win32icon:"%~dp0app.ico" /out:"%~dp0ost-Injector.exe" "%~dp0Injector.cs"

if %ERRORLEVEL% equ 0 (
    echo.
    echo =======================================================
    echo [SUCCESS] ost-Injector.exe compiled successfully!
    echo   1. Native WinExe Subsystem without window-hiding hack
    echo   2. Full PE version metadata and embedded icon
    echo   3. Automatic console attachment for manual launch
    echo =======================================================
) else (
    echo.
    echo [ERROR] Compilation failed!
)

pause
