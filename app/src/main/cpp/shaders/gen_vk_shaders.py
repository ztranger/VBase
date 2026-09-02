#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Компиляция GLSL -> SPIR-V (Vulkan) по единому списку vk_shaders.txt.

Выход: app/src/main/assets/shaders/vk/<name>.spv — ЗАКОММИЧЕН в репо, чтобы чистый clone
собирал рабочий Vulkan-рендер БЕЗ внешнего Vulkan SDK (Android пакует .spv из ассетов как есть;
десктоп/CI регенерируют этим же скриптом). Единый список — источник правды и для CMake.

glslc берём приоритетно из NDK (bundled, пиновка проекта) — тогда вывод байт-идентичен между
десктопом, скриптом и CI, и .spv не «пляшут» между сборками.

Использование:
  python gen_vk_shaders.py            # перегенерировать все .spv в ассеты
  python gen_vk_shaders.py --check    # НИЧЕГО не писать: проверить, что закоммиченные .spv
                                       # актуальны (совпадают с пере-компиляцией). exit 1 = устарели.
Переопределить компилятор: переменная окружения GLSLC=/path/to/glslc.
"""

import os
import sys
import glob
import shutil
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = HERE                                            # исходники .vert/.frag тут же
LIST = os.path.join(HERE, "vk_shaders.txt")
OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "assets", "shaders", "vk"))
EXE = ".exe" if os.name == "nt" else ""


def _ndk_roots():
    """Возможные корни NDK из окружения + стандартные места установки SDK."""
    roots = []
    for v in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK"):
        if os.environ.get(v):
            roots.append(os.environ[v])            # прямой корень NDK
    for v in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(v):
            roots += glob.glob(os.path.join(os.environ[v], "ndk", "*"))
    la = os.environ.get("LOCALAPPDATA")
    if la:
        roots += glob.glob(os.path.join(la, "Android", "Sdk", "ndk", "*"))
    home = os.path.expanduser("~")
    roots += glob.glob(os.path.join(home, "Android", "Sdk", "ndk", "*"))          # linux
    roots += glob.glob(os.path.join(home, "Library", "Android", "sdk", "ndk", "*"))  # mac
    return roots


def find_glslc():
    """Найти glslc: env GLSLC -> NDK (bundled) -> Vulkan SDK -> PATH. None если нет."""
    env = os.environ.get("GLSLC")
    if env and os.path.isfile(env):
        return env
    for root in _ndk_roots():
        hits = glob.glob(os.path.join(root, "shader-tools", "*", "glslc" + EXE))
        if hits:
            return hits[0]
    vk = os.environ.get("VULKAN_SDK")
    if vk and os.path.isfile(os.path.join(vk, "Bin", "glslc" + EXE)):
        return os.path.join(vk, "Bin", "glslc" + EXE)
    return shutil.which("glslc")


def read_list():
    names = []
    with open(LIST, "r", encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if s and not s.startswith("#"):
                names.append(s)
    return names


def compile_one(glslc, name, out_path):
    src = os.path.join(SRC_DIR, name)
    if not os.path.isfile(src):
        print("  ОШИБКА: нет исходника %s" % src)
        return False
    # --target-env фиксируем явно -> детерминизм между версиями glslc. Includes glslc резолвит
    # относительно файла-источника (common.glsl/lighting.glsl рядом) — -I не нужен.
    cmd = [glslc, "--target-env=vulkan1.0", src, "-o", out_path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("  ОШИБКА glslc %s:\n%s" % (name, r.stderr.strip()))
        return False
    return True


def main():
    check = "--check" in sys.argv[1:]
    glslc = find_glslc()
    if not glslc:
        print("glslc не найден. Установите Android NDK или Vulkan SDK, либо задайте GLSLC=/path/to/glslc.")
        return 2
    print("glslc:", glslc)
    names = read_list()
    print(("Проверка" if check else "Компиляция") + " %d шейдеров -> %s" % (len(names), OUT_DIR))

    if not check:
        os.makedirs(OUT_DIR, exist_ok=True)

    stale, failed = [], []
    tmp = tempfile.mkdtemp(prefix="vkspv_") if check else None
    for name in names:
        committed = os.path.join(OUT_DIR, name + ".spv")
        target = os.path.join(tmp, name + ".spv") if check else committed
        if not compile_one(glslc, name, target):
            failed.append(name)
            continue
        if check:
            fresh = open(target, "rb").read()
            old = open(committed, "rb").read() if os.path.isfile(committed) else None
            if old != fresh:
                stale.append(name + (" (нет .spv)" if old is None else " (устарел)"))
        else:
            print("  ok %s.spv" % name)
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)

    if failed:
        print("ПРОВАЛ компиляции:", ", ".join(failed))
        return 1
    if check:
        if stale:
            print("УСТАРЕЛИ (перегенерируйте `python gen_vk_shaders.py` и закоммитьте):")
            for s in stale:
                print("  -", s)
            return 1
        print("Все .spv актуальны.")
    else:
        print("Готово: %d .spv в %s" % (len(names), OUT_DIR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
