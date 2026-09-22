@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: CMake was not found on PATH.
    echo Open "Developer PowerShell for VS 2026" or "x64 Native Tools Command Prompt for VS 2026" and run this script again.
    exit /b 1
)
echo Using CMake:
cmake --version
cmake --preset windows-vs2026-x64
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset windows-vs2026-x64-release
if errorlevel 1 exit /b %errorlevel%

echo.
echo Checking generated FrontierMP binaries...
if exist "build\vs2026-x64\bin\Release\FrontierMP.exe" (
    echo   OK: FrontierMP.exe
) else (
    echo   ERROR: FrontierMP.exe was not generated.
    exit /b 2
)
if exist "build\vs2026-x64\bin\Release\FrontierClient.dll" (
    echo   OK: FrontierClient.dll
) else (
    echo   ERROR: FrontierClient.dll was not generated.
    echo   Search the build output above for frontier_client build errors.
    exit /b 3
)
if exist "build\vs2026-x64\bin\Release\frontier_server.exe" (
    echo   OK: frontier_server.exe
) else (
    echo   WARNING: frontier_server.exe was not generated.
)

if not exist "build\vs2026-x64\bin\Release\FrontierMP.exe" exit /b 2
if not exist "build\vs2026-x64\bin\Release\FrontierClient.dll" exit /b 3

echo Build completed successfully.
exit /b 0
