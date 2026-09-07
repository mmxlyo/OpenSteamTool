@echo off
chcp 65001 >nul
echo =======================================================
echo   OpenSteamTool - Setup Auto Inject Task
echo =======================================================
echo.
echo Creating scheduled task "OpenSteamTool_AutoInject"...
schtasks /create /tn "OpenSteamTool_AutoInject" /tr "\"%~dp0ost-Injector.exe\" -watch" /sc onlogon /rl highest /f
if %errorlevel% equ 0 (
    echo.
    echo =======================================================
    echo [SUCCESS] Scheduled task "OpenSteamTool_AutoInject" created!
    echo The background watcher will start upon logon and automatically
    echo inject OpenSteamTool.dll whenever Steam starts.
    echo =======================================================
) else (
    echo.
    echo =======================================================
    echo [FAILED] Failed to create scheduled task.
    echo Please right-click this script and select "Run as administrator".
    echo =======================================================
)
echo.
pause
