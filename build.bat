@echo off
REM Build FurnitureSale.dll...

set "DESTINATION_DIR=C:\Users\Pseudonym_Tim\Desktop\Tools\Mewtator\mods\FurnitureSale"
set "MEWTATOR_DEPLOY=true"

setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Is Visual Studio installed?
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do (
    set "VSDIR=%%i"
)

if not defined VSDIR (
    echo ERROR: Could not find a Visual Studio installation.
    pause
    exit /b 1
)

if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: vcvarsall.bat not found at "%VSDIR%\VC\Auxiliary\Build\"
    pause
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

echo Building FurnitureSale.dll...

cl /LD /O2 /MT /GS- /W3 /D_CRT_SECURE_NO_WARNINGS /TC src\FurnitureSale.c src\mew_ui_api.c /I src /Fe:FurnitureSale.dll /link user32.lib

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED.
    pause
    exit /b 1
)

del /Q *.obj FurnitureSale.lib FurnitureSale.exp 2>nul

if /I "%MEWTATOR_DEPLOY%"=="true" (
    set "DEPLOY_DIR=%DESTINATION_DIR%"
) else (
    set "DEPLOY_DIR=%DESTINATION_DIR%\mods"
)

if not exist "%DEPLOY_DIR%" (
    mkdir "%DEPLOY_DIR%"
)

copy /Y FurnitureSale.dll "%DEPLOY_DIR%\FurnitureSale.dll"
copy /Y description.json "%DEPLOY_DIR%\description.json"
copy /Y preview.png "%DEPLOY_DIR%\preview.png"

if not exist "%DEPLOY_DIR%\data\text" (
    mkdir "%DEPLOY_DIR%\data\text"
)

copy /Y "data\text\combined.csv.append" "%DEPLOY_DIR%\data\text\combined.csv.append"

REM Deploy swfs folder and its contents...
if exist "%~dp0swfs" (
    xcopy "%~dp0swfs" "%DEPLOY_DIR%\swfs" /E /I /Y
) else (
    echo WARNING: swfs folder not found!
)
echo.
echo Build succeeded and deployed to %DEPLOY_DIR%

pause
