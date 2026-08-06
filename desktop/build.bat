@echo off
REM Build the desktop client via MSVC (VS Build Tools/Community) with the Ninja generator.
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

REM Locate glslc (GLSL -> SPIR-V for the Vulkan backend). Prefer the Vulkan SDK pointed to
REM by the VULKAN_SDK env var; fall back to the old hard-coded SDK path. Without it the
REM Vulkan shaders cannot be built and the desktop build will fail at the shader step.
set "GLSLC="
if defined VULKAN_SDK if exist "%VULKAN_SDK%\Bin\glslc.exe" set "GLSLC=%VULKAN_SDK%\Bin\glslc.exe"
if not defined GLSLC if exist "C:\VulkanSDK\1.4.350.0\Bin\glslc.exe" set "GLSLC=C:\VulkanSDK\1.4.350.0\Bin\glslc.exe"
set "GLSLCARG="
if defined GLSLC (
    set "GLSLCARG=-DGLSLC=%GLSLC%"
) else (
    echo WARNING: glslc not found - install the Vulkan SDK ^(sets VULKAN_SDK^) or the build will fail compiling Vulkan shaders.
)

REM Wipe a build dir configured by a different generator (old NMake) so CMake won't error.
if exist "%~dp0build\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%~dp0build\CMakeCache.txt" >nul
    if errorlevel 1 rmdir /s /q "%~dp0build"
)

"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "Ninja" -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release %GLSLCARG% || exit /b 1
"%CMAKE%" --build "%~dp0build" || exit /b 1
echo BUILD_OK
