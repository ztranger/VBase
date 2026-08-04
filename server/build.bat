@echo off
REM Build the dedicated server via MSVC (VS Build Tools) with the Ninja generator.
REM Ninja compiles in parallel across all cores (NMake is single-threaded), so a full
REM Jolt build is ~3x faster and incremental builds are precise: editing CMakeLists no
REM longer forces a full Jolt rebuild. (ASCII comments only: cmd reads .bat in the OEM
REM codepage, so Cyrillic here would be mojibake and break parsing.)
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

REM Wipe a build dir configured by a different generator (old NMake) so CMake won't error.
if exist "%~dp0build\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%~dp0build\CMakeCache.txt" >nul
    if errorlevel 1 rmdir /s /q "%~dp0build"
)

"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "Ninja" -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release || exit /b 1
"%CMAKE%" --build "%~dp0build" || exit /b 1
echo BUILD_OK
