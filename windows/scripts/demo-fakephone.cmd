@echo off
rem Demo without an iPhone: starts the fake phone (streams the stamped H.264 test clip) in its own
rem window, then runs the engine here, feeding Cabled Camera. Pick the camera in OBS / a browser.
rem Ctrl+C stops the engine; close the "Cabled fake phone" window to stop the phone.
setlocal
set "ROOT=%~dp0..\.."
set "BIN=%ROOT%\build\bin\Release"
start "Cabled fake phone" "%BIN%\cabled-fakephone.exe" --clip "%ROOT%\windows\testdata\pattern_1080p30.h264"
timeout /t 1 /nobreak >nul
"%BIN%\cabled-engine.exe" --tcp 127.0.0.1:47800
