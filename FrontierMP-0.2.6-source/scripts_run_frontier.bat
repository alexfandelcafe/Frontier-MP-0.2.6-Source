@echo off
setlocal
cd /d "%~dp0"

echo FrontierMP Launcher
echo.
if not exist "build\vs2026-x64\bin\Release\FrontierMP.exe" (
    echo ERROR: FrontierMP.exe is missing. Run scripts_build_vs2026.bat first and fix any build errors.
    pause
    exit /b 2
)
if not exist "build\vs2026-x64\bin\Release\FrontierClient.dll" (
    echo ERROR: FrontierClient.dll is missing. The client DLL did not compile successfully.
    echo Run scripts_build_vs2026.bat and inspect the frontier_client errors.
    pause
    exit /b 3
)
echo.
set /p "RDRPATH=Enter the full path to RDR.exe: "
if "%RDRPATH%"=="" (
    echo ERROR: No RDR.exe path supplied.
    pause
    exit /b 1
)

set /p "PLAYERNAME=Player name [Player]: "
if "%PLAYERNAME%"=="" set "PLAYERNAME=Player"

echo.
"%~dp0build\vs2026-x64\bin\Release\FrontierMP.exe" --game="%RDRPATH%" --server=127.0.0.1 --port=30120 --name="%PLAYERNAME%"
set "RC=%ERRORLEVEL%"

echo.
echo FrontierMP exited with code %RC%.
pause
exit /b %RC%
