@echo off
REM Build the dedicated server with AddressSanitizer (MSVC /fsanitize=address).
REM Mirrors build.bat but uses a SEPARATE build dir (build-asan) and passes -DVBASE_ASAN=ON,
REM so ASan object files never contaminate the normal build/. Run the result the same way:
REM   server\build-asan\vbase_server.exe --selftest
REM ASan reports (heap/stack overflow, use-after-free, leaks) print to stderr and exit non-zero.
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

REM Wipe a build dir configured by a different generator (old NMake) so CMake won't error.
if exist "%~dp0build-asan\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%~dp0build-asan\CMakeCache.txt" >nul
    if errorlevel 1 rmdir /s /q "%~dp0build-asan"
)

"%CMAKE%" -S "%~dp0." -B "%~dp0build-asan" -G "Ninja" -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release -DVBASE_ASAN=ON || exit /b 1
"%CMAKE%" --build "%~dp0build-asan" || exit /b 1
echo BUILD_OK
