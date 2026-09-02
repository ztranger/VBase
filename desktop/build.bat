@echo off
REM Build the desktop client via MSVC (VS Build Tools/Community) with the Ninja generator.
REM Ninja compiles in parallel across all cores (NMake is single-threaded), so a full Jolt
REM build is ~3x faster and incremental builds are precise: editing CMakeLists no longer
REM forces a full Jolt rebuild. The VS install root is resolved dynamically (NOT a
REM hard-coded path): first from the VBASE_VS_DIR env var if you set one, otherwise
REM auto-detected via vswhere. Set VBASE_VS_DIR to the VS install root (the folder that
REM holds VC\Auxiliary\Build\vcvars64.bat) to pin a specific edition/location.
REM (ASCII comments only: cmd reads .bat in the OEM codepage, so Cyrillic here would be
REM mojibake and break parsing.)
set "VSDIR="
if defined VBASE_VS_DIR if exist "%VBASE_VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%VBASE_VS_DIR%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VSDIR if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: Visual Studio with C++ tools not found.
    echo Set VBASE_VS_DIR to your VS install root, or install the "Desktop development with C++" workload.
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

REM Vulkan shaders (SPIR-V) are NOT compiled by this build: the .spv are committed in
REM app/src/main/assets/shaders/vk/ and loaded from assets at runtime. So the desktop build
REM needs no Vulkan SDK / glslc. Regenerate shaders via app/src/main/cpp/shaders/gen_vk_shaders.py.

REM Build type: RelWithDebInfo (optimized /O2 + debug symbols). Deliberately NOT plain
REM Release: a pre-existing optimization-sensitive UB heisenbug in the client crashes only
REM under Release's full inlining (/Ob2); RelWithDebInfo (/Ob1) does not trigger it and also
REM ships a PDB for debugging. See docs/NEXT_STEPS. (ASCII comments only in .bat.)
REM Wipe a build dir configured by a different generator (old NMake) so CMake won't error.
if exist "%~dp0build\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%~dp0build\CMakeCache.txt" >nul
    if errorlevel 1 rmdir /s /q "%~dp0build"
)

"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "Ninja" -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
"%CMAKE%" --build "%~dp0build" || exit /b 1
echo BUILD_OK
