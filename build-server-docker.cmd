@echo off
REM Build the dedicated server as a Linux Docker image (vbase-server).
REM Docker Desktop must be in Linux containers mode. Context is the repo root.
cd /d "%~dp0"
docker build -t vbase-server -f server/Dockerfile . || exit /b 1
echo IMAGE_OK
