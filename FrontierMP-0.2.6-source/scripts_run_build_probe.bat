@echo off
setlocal
cd /d "%~dp0"

echo FrontierMP Build Probe
echo.
set /p "RDRPATH=Enter the full path to RDR.exe: "
echo.
if "%RDRPATH%"=="" (
    echo ERROR: No path was supplied.
    pause
    exit /b 1
)
if not exist "%RDRPATH%" (
    echo ERROR: File not found:
    echo %RDRPATH%
    echo.
    pause
    exit /b 1
)

echo Running probe...
echo.
"%~dp0build\vs2026-x64\bin\Release\frontier_build_probe.exe" "%RDRPATH%"
echo.
pause
