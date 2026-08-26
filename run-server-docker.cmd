@echo off
REM Run the dedicated server in Docker. UDP port is published on the host.
REM Extra args go to vbase_server: [port] [assetsDir] [scenePath] or --selftest.
REM Clients on this PC: 127.0.0.1    Phone: this machine's LAN IP (not 172.x).
cd /d "%~dp0"
docker image inspect vbase-server >nul 2>&1
if errorlevel 1 (
    echo Image vbase-server not found, building...
    call "%~dp0build-server-docker.cmd" || exit /b 1
)

set "PORT=7777"
if not "%~1"=="" if not "%~1"=="--selftest" set "PORT=%~1"

docker run --rm -it --init -p %PORT%:%PORT%/udp --name vbase-server vbase-server %*
