@echo off
setlocal EnableExtensions
pushd "%~dp0" || exit /b 1
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\sunshine-windows.ps1" -Action Install %*
set "SUNSHINE_RESULT=%ERRORLEVEL%"
popd
if not defined SUNSHINE_NO_PAUSE pause
exit /b %SUNSHINE_RESULT%
