@echo off
rem Double-click (or run) to install/update the Cabled virtual camera DLL for Frame Server.
rem Runs vcam-dev-install.ps1 without changing the system's script policy. Asks for admin (UAC).
rem Pass -Uninstall to remove it.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0vcam-dev-install.ps1" %*
echo.
pause
