@echo off
REM Запуск редактора сцен из корня проекта. Рабочая директория — editor\build,
REM чтобы относительные пути к ассетам разрешались как ../../app/src/main/assets.
REM Аргументы пробрасываются: run-editor.cmd [assetsDir] [scenePath]
cd /d "%~dp0editor\build"
"%~dp0editor\build\vbase_editor.exe" %*
