@echo off
chcp 65001 >nul
echo =======================================================
echo   OpenSteamTool - Remove Auto Inject Task
echo =======================================================
echo.
echo Deleting scheduled task "OpenSteamTool_AutoInject"...
schtasks /delete /tn "OpenSteamTool_AutoInject" /f
if %errorlevel% equ 0 (
    echo.
    echo =======================================================
    echo [SUCCESS] Scheduled task removed successfully!
    echo =======================================================
) else (
    echo.
    echo =======================================================
    echo [INFO] Task does not exist or has already been removed.
    echo =======================================================
)
echo.
pause
