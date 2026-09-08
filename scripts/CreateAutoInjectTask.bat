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
    echo Starting background watcher service right now...
    schtasks /run /tn "OpenSteamTool_AutoInject"
    echo The background watcher is now running and will auto-start upon logon,
    echo automatically injecting OpenSteamTool.dll whenever Steam starts.
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
