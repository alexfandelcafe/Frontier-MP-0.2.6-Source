@echo off
setlocal
cd /d "%~dp0"
cmake --preset windows-vs2022-x64
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset windows-vs2022-x64-release
exit /b %errorlevel%
