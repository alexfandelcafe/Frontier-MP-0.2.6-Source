@echo off
setlocal
cd /d "%~dp0"

set FRONTIER_RDRMP_COMPAT=1
set FRONTIER_NATIVE_UI_BOOTSTRAP=0

set SERVER_EXE=build\vs2022-x64\bin\Release\frontier_rdrmp_server.exe
set LAUNCHER_EXE=build\vs2022-x64\bin\Release\FrontierMP.exe

if not exist "%SERVER_EXE%" (
    echo ERROR: %SERVER_EXE% not found.
    echo Ejecuta scripts_build_final.bat primero.
    exit /b 2
)

if not exist "%LAUNCHER_EXE%" (
    echo ERROR: %LAUNCHER_EXE% not found.
    echo Ejecuta scripts_build_final.bat primero.
    exit /b 3
)

set /p RDRPATH=Ruta completa de RDR.exe: 
if "%RDRPATH%"=="" exit /b 1

set /p PLAYERNAME=Nombre [Player]: 
if "%PLAYERNAME%"=="" set PLAYERNAME=Player

echo.
echo Starting RDRMP-compatible ENet server on UDP 4674...
start "FrontierMP RDRMP Server" "%SERVER_EXE%" 4674

echo.
echo Starting RDR1 with FrontierMP...
"%LAUNCHER_EXE%" --game="%RDRPATH%" --server=127.0.0.1 --port=4674 --name="%PLAYERNAME%"
set RC=%ERRORLEVEL%

echo.
echo FrontierMP exited with code %RC%.
exit /b %RC%
