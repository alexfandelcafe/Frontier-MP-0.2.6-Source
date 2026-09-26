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
echo.

if not defined FRONTIER_CEF_PACKAGE_DIR (
    if exist "C:\cef_binary_154.0.28+g564dd6c+chromium-154.0.8037.58_windows64\include\cef_api_hash.h" (
        set "FRONTIER_CEF_PACKAGE_DIR=C:\cef_binary_154.0.28+g564dd6c+chromium-154.0.8037.58_windows64"
        echo Auto-detected CEF SDK: %FRONTIER_CEF_PACKAGE_DIR%
    )
)

if defined FRONTIER_CEF_PACKAGE_DIR (
    echo CEF SDK: %FRONTIER_CEF_PACKAGE_DIR%
    if not exist "%FRONTIER_CEF_PACKAGE_DIR%\include\cef_api_hash.h" (
        echo ERROR: FRONTIER_CEF_PACKAGE_DIR does not contain a CEF SDK.
        echo Expected: include\cef_api_hash.h
        exit /b 4
    )
    if not exist "%FRONTIER_CEF_PACKAGE_DIR%\libcef_dll\CMakeLists.txt" (
        echo ERROR: FRONTIER_CEF_PACKAGE_DIR is missing libcef_dll\CMakeLists.txt.
        exit /b 4
    )
    cmake --preset windows-vs2026-x64 -DFRONTIER_CEF_PACKAGE_DIR="%FRONTIER_CEF_PACKAGE_DIR%"
) else (
    echo CEF SDK: disabled
    echo Set FRONTIER_CEF_PACKAGE_DIR to a full CEF Windows distribution to build the CEF overlay.
    cmake --preset windows-vs2026-x64 -DFRONTIER_CEF_PACKAGE_DIR=""
)
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
if defined FRONTIER_CEF_PACKAGE_DIR (
    if exist "build\vs2026-x64\bin\Release\FrontierCefSubprocess.exe" (
        echo   OK: FrontierCefSubprocess.exe
    ) else (
        echo   ERROR: FrontierCefSubprocess.exe was not generated.
        exit /b 5
    )
    if exist "build\vs2026-x64\bin\Release\libcef.dll" (
        echo   OK: libcef.dll
    ) else (
        echo   ERROR: libcef.dll was not staged beside FrontierClient.dll.
        exit /b 6
    )
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
