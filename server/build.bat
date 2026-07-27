@echo off
REM Сборка выделenного сервера через MSVC (VS Build Tools).
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
"%CMAKE%" -S "%~dp0." -B "%~dp0build" -G "NMake Makefiles" || exit /b 1
"%CMAKE%" --build "%~dp0build" || exit /b 1
echo BUILD_OK
