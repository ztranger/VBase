@echo off
REM Запуск десктоп-клиента из корня проекта. Рабочая директория — desktop\build,
REM чтобы относительные пути к ассетам разрешались как ../../app/src/main/assets.
REM Бинарник вызываем по полному пути (не полагаемся на поиск в текущей папке).
REM Аргументы пробрасываются: run-desktop.cmd [serverIp] [assetsDir] [scenePath]
cd /d "%~dp0desktop\build"
"%~dp0desktop\build\vbase_desktop.exe" %*
