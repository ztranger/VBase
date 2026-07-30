@echo off
REM Запуск выделенного сервера из корня проекта. Рабочая директория — server\build,
REM чтобы относительные пути к ассетам сцены разрешались как ../../app/src/main/assets.
REM Бинарник вызываем по полному пути (не полагаемся на поиск в текущей папке).
REM Аргументы пробрасываются: run-server.cmd [port] [assetsDir] [scenePath]
cd /d "%~dp0server\build"
"%~dp0server\build\vbase_server.exe" %*
