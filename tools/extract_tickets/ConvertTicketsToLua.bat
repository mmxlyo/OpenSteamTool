@echo off
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ConvertTicketsToLua.ps1" "%~1"
pause
