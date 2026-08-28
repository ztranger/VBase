@echo off
setlocal enableextensions
REM Package the desktop client into a self-contained, zippable VBaseDesktop folder:
REM   vbase_desktop.exe + MSVC runtime DLLs + assets + run.cmd, then a .zip next to it.
REM By default it builds the desktop first (incremental, via build.bat) so the package is
REM fresh; pass "nobuild" as the 1st arg to skip the build and package the current exe.
REM Output lands in desktop\build\ (VBaseDesktop\ + VBaseDesktop.zip) - change OUTDIR to
REM relocate. (ASCII comments only: cmd reads .bat in the OEM codepage.)

set "HERE=%~dp0"
set "APPASSETS=%HERE%..\app\src\main\assets"
set "EXE=%HERE%build\vbase_desktop.exe"
set "OUTDIR=%HERE%build"

REM --- 1) build (incremental) unless "nobuild" was passed ---
if /I not "%~1"=="nobuild" call "%HERE%build.bat" || exit /b 1
if not exist "%EXE%" (
    echo ERROR: "%EXE%" not found - run without "nobuild" to build it first.
    exit /b 1
)

REM --- 2) locate the VS install (VBASE_VS_DIR override, else vswhere) for redist DLLs ---
set "VSDIR="
if defined VBASE_VS_DIR if exist "%VBASE_VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%VBASE_VS_DIR%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VSDIR if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo ERROR: Visual Studio install not found. & exit /b 1 )

REM --- 3) find the MSVC runtime DLLs: newest VC redist CRT x64 dir, else System32 ---
set "CRTVER="
for /f "usebackq delims=" %%d in (`dir /b /ad /o-n "%VSDIR%\VC\Redist\MSVC" 2^>nul`) do if not defined CRTVER set "CRTVER=%%d"
set "DLLSRC="
if defined CRTVER for /f "usebackq delims=" %%c in (`dir /b /ad "%VSDIR%\VC\Redist\MSVC\%CRTVER%\x64\Microsoft.VC*.CRT" 2^>nul`) do if not defined DLLSRC set "DLLSRC=%VSDIR%\VC\Redist\MSVC\%CRTVER%\x64\%%c"
if not defined DLLSRC if exist "%SystemRoot%\System32\msvcp140.dll" set "DLLSRC=%SystemRoot%\System32"
if not defined DLLSRC ( echo ERROR: MSVC runtime DLLs not found ^(no VC redist and no System32 copy^). & exit /b 1 )

REM --- 4) stage into a clean folder ---
set "STAGE=%OUTDIR%\VBaseDesktop"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" || exit /b 1
copy /y "%EXE%" "%STAGE%\" >nul
for %%f in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do copy /y "%DLLSRC%\%%f" "%STAGE%\" >nul
for %%f in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do if not exist "%STAGE%\%%f" ( echo ERROR: failed to copy %%f from "%DLLSRC%". & exit /b 1 )
xcopy /e /i /q /y "%APPASSETS%" "%STAGE%\assets" >nul || exit /b 1

REM launcher (arg 1 = server IP, default 127.0.0.1; assets sit next to the exe)
(
echo @echo off
echo set "IP=%%~1"
echo if "%%IP%%"=="" set "IP=127.0.0.1"
echo "%%~dp0vbase_desktop.exe" "%%IP%%" "%%~dp0assets"
) > "%STAGE%\run.cmd"

REM --- 5) zip ---
set "ZIP=%OUTDIR%\VBaseDesktop.zip"
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%ZIP%' -Force" || exit /b 1

echo.
echo PACKAGE_OK
echo   folder: %STAGE%
echo   zip:    %ZIP%
endlocal
exit /b 0
