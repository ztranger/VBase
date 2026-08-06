@echo off
REM Build the dedicated server via MSVC (VS Build Tools/Community) with the Ninja generator.
REM Ninja compiles in parallel across all cores (NMake is single-threaded), so a full Jolt
REM build is ~3x faster and incremental builds are precise: editing CMakeLists no longer
REM forces a full Jolt rebuild. The VS install is auto-detected via vswhere, so this survives
REM moving to a new PC or a different VS edition/path (was hard-coded to BuildTools before).
REM (ASCII comments only: cmd reads .bat in the OEM codepage, so Cyrillic here would be
REM mojibake and break parsing.)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found - is Visual Studio / Build Tools installed?
    exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: no Visual Studio install with C++ tools found - install the "Desktop development with C++" workload.
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

REM Wipe a build dir configured by a different generator (old NMake) so CMake won't error.
if exist "%~dp0build\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%~dp0build\CMakeCache.txt" >nul
    if errorlevel 1 rmdir /s /q "%~dp0build"
)

"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "Ninja" -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release || exit /b 1
"%CMAKE%" --build "%~dp0build" || exit /b 1
echo BUILD_OK
