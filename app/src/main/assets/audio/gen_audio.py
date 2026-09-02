#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Генератор игровых звуков VBase (процедурно, детерминированно).

Пишет 16-bit PCM mono WAV рядом с собой (app/src/main/assets/audio/*.wav).
Грузятся кроссплатформенно через AssetSource (десктоп — с диска, Android — из APK)
и проигрываются miniaudio. Реальные ассеты потом можно подменить теми же именами.

SFX короткие и с относительной громкостью, «запечённой» в амплитуду (у play() нет
пер-звук громкости). Музыка — ambient-дрон, СТРОГО периодический (все частоты и LFO
кратны 1/loopLen), поэтому miniaudio-луп бесшовен (sample[last+1] == sample[0]).

Запуск:  python app/src/main/assets/audio/gen_audio.py
"""

import os
import wave
import numpy as np

SR = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
rng = np.random.default_rng(1337)  # детерминизм


def _write(name, samples, peak):
    """Нормировать к пику `peak` (запекает относительную громкость) и записать WAV."""
    x = np.asarray(samples, dtype=np.float64)
    m = np.max(np.abs(x))
    if m > 1e-9:
        x = x / m * peak
    # микро-фейд 2 мс по краям — против щелчков на старте/конце (для не-петель)
    n = int(SR * 0.002)
    if x.size > 2 * n:
        ramp = np.linspace(0.0, 1.0, n)
        x[:n] *= ramp
        x[-n:] *= ramp[::-1]
    pcm = np.clip(x, -1.0, 1.0)
    pcm = (pcm * 32767.0).astype('<i2')
    path = os.path.join(HERE, name)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print("  %-16s %5.2fs  peak=%.2f" % (name, x.size / SR, peak))


def _t(dur):
    return np.linspace(0.0, dur, int(SR * dur), endpoint=False)


def _env(t, attack, decay):
    """Экспоненциальный AD-конверт: быстрая атака, экспоненциальный спад."""
    a = np.clip(t / max(attack, 1e-4), 0.0, 1.0)
    d = np.exp(-t / max(decay, 1e-4))
    return a * d


def _noise(n):
    return rng.uniform(-1.0, 1.0, n)


# --- SFX ---

def gen_hit():
    # Удар: короткий низкий «тумп» + шумовой транзиент.
    t = _t(0.13)
    thump = np.sin(2 * np.pi * 130.0 * t) * _env(t, 0.002, 0.05)
    noise = _noise(t.size) * _env(t, 0.001, 0.02)
    _write('hit.wav', 0.8 * thump + 0.6 * noise, 0.62)


def gen_core_hit():
    # Удар по ядру: тяжёлый низкий бум с падением высоты + плотный шум. Громче.
    t = _t(0.32)
    f = 70.0 - 30.0 * (t / t[-1])                       # 70 -> 40 Гц
    boom = np.sin(2 * np.pi * np.cumsum(f) / SR) * _env(t, 0.003, 0.12)
    noise = _noise(t.size) * _env(t, 0.001, 0.04)
    _write('core_hit.wav', 0.9 * boom + 0.4 * noise, 0.92)


def gen_enemy_death():
    # Смерть моба: нисходящий тон + костяные «клацы» (короткие шумовые тики).
    t = _t(0.36)
    f = 420.0 * np.exp(-3.0 * t)                          # быстро падает
    tone = np.sin(2 * np.pi * np.cumsum(f) / SR) * _env(t, 0.003, 0.10)
    clatter = np.zeros(t.size)
    for off in (0.02, 0.09, 0.16, 0.24):                 # тики костей
        i = int(off * SR)
        seg = min(int(0.03 * SR), t.size - i)
        if seg > 0:
            clatter[i:i + seg] += _noise(seg) * np.exp(-np.linspace(0, 1, seg) * 6.0)
    _write('enemy_death.wav', 0.7 * tone + 0.5 * clatter, 0.7)


def gen_shoot():
    # Выстрел (маг/башня): магический «пиу» — быстрый свип высоты + чуть шума.
    t = _t(0.16)
    f = 700.0 + 900.0 * np.exp(-14.0 * t)                # 1600 -> 700 Гц
    pew = np.sin(2 * np.pi * np.cumsum(f) / SR) * _env(t, 0.002, 0.05)
    air = _noise(t.size) * _env(t, 0.001, 0.02) * 0.25
    _write('shoot.wav', pew + air, 0.5)


def gen_build():
    # Постройка: два деревянных «тук-тук».
    t = _t(0.24)
    out = np.zeros(t.size)
    for off, fr in ((0.0, 220.0), (0.085, 180.0)):
        i = int(off * SR)
        seg = t.size - i
        tt = _t(seg / SR)
        knock = (np.sin(2 * np.pi * fr * tt) + 0.4 * _noise(seg)) * _env(tt, 0.001, 0.03)
        out[i:] += knock
    _write('build.wav', out, 0.66)


def _note(freq, dur, decay, wave_mix=0.0):
    # Нота: синус + немного треугольника (wave_mix) с экспоненциальным спадом.
    t = _t(dur)
    s = np.sin(2 * np.pi * freq * t)
    tri = 2.0 * np.abs(2.0 * (freq * t - np.floor(freq * t + 0.5))) - 1.0
    return (s * (1.0 - wave_mix) + tri * wave_mix) * _env(t, 0.004, decay)


def gen_victory():
    # Победа: восходящее мажорное арпеджио C-E-G-C с хвостом.
    freqs = [523.25, 659.25, 783.99, 1046.50]
    step = int(0.16 * SR)
    total = step * len(freqs) + int(0.45 * SR)
    out = np.zeros(total)
    for k, fr in enumerate(freqs):
        seg = _note(fr, (total - k * step) / SR, 0.35, 0.3)
        i = k * step
        out[i:i + seg.size] += seg * 0.7
    _write('victory.wav', out, 0.7)


def gen_defeat():
    # Поражение: нисходящий минорный мотив, медленнее и мрачнее.
    freqs = [440.0, 349.23, 261.63, 220.0]
    step = int(0.22 * SR)
    total = step * len(freqs) + int(0.55 * SR)
    out = np.zeros(total)
    for k, fr in enumerate(freqs):
        seg = _note(fr, (total - k * step) / SR, 0.5, 0.2)
        i = k * step
        out[i:i + seg.size] += seg * 0.7
    _write('defeat.wav', out, 0.62)


# --- Музыка: ambient-дрон, строго периодический (бесшовный луп) ---

def gen_music():
    loop = 16.0                       # длина петли, сек
    f0 = 1.0 / loop                   # базовая частота петли
    n = int(SR * loop)
    t = np.arange(n) / SR

    def snap(f):                      # частота -> ближайшее кратное f0 (точная периодичность)
        return max(round(f / f0), 1) * f0

    # Аккорд Am add9 (спокойный, «подвешенный»): вес ниже у высоких.
    notes = [(110.0, 0.9), (164.81, 0.6), (220.0, 0.7),
             (261.63, 0.5), (329.63, 0.4), (493.88, 0.25)]
    sig = np.zeros(n)
    for j, (freq, amp) in enumerate(notes):
        f = snap(freq)
        # LFO амплитуды: целое число циклов за петлю (k) -> периодично.
        k = (j % 3) + 1
        lfo = 1.0 + 0.35 * np.sin(2 * np.pi * (k * f0) * t + j * 1.3)
        sig += amp * lfo * np.sin(2 * np.pi * f * t + j * 0.7)

    # Мягкий суб-бас (тоника октавой ниже) + очень медленное общее «дыхание» (2 цикла/петля).
    sig += 0.5 * np.sin(2 * np.pi * snap(55.0) * t)
    breath = 0.85 + 0.15 * np.sin(2 * np.pi * (2 * f0) * t)
    sig *= breath

    # Записываем БЕЗ краевого фейда (это петля): нормировка вручную, тихо.
    x = sig / np.max(np.abs(sig)) * 0.32
    pcm = (np.clip(x, -1.0, 1.0) * 32767.0).astype('<i2')
    path = os.path.join(HERE, 'music.wav')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print("  %-16s %5.2fs  peak=0.32 (loop)" % ('music.wav', loop))


def main():
    print("Генерация звуков VBase ->", HERE)
    gen_hit()
    gen_core_hit()
    gen_enemy_death()
    gen_shoot()
    gen_build()
    gen_victory()
    gen_defeat()
    gen_music()
    print("Готово.")


if __name__ == '__main__':
    main()
