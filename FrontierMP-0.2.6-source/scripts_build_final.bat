@echo off
setlocal
cd /d "%~dp0"

echo ==============================================
echo FrontierMP 1.0.0 - RDR1 final build
echo ==============================================
echo.

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake.exe no esta en PATH.
    echo Agrega CMake al PATH y vuelve a ejecutar.
    exit /b 1
)

set PRESET=windows-vs2022-x64
if not "%FRONTIER_VS_PRESET%"=="" set PRESET=%FRONTIER_VS_PRESET%

echo [1/3] CMake configure: %PRESET%
cmake --preset %PRESET%
if errorlevel 1 (
    echo.
    echo CONFIGURE FAILED. Warnings/errors arriba.
    exit /b %errorlevel%
)

echo.
echo [2/3] Build Release con salida VERBOSE
cmake --build --preset %PRESET%-release --parallel --verbose
if errorlevel 1 (
    echo.
    echo BUILD FAILED. Warnings/errors arriba.
    exit /b %errorlevel%
)

echo.
echo [3/3] Tests
if exist "build\vs2022-x64" (
    ctest --test-dir "build\vs2022-x64" -C Release --output-on-failure
    if errorlevel 1 exit /b %errorlevel%
)

echo.
echo BUILD + TESTS OK
exit /b 0
