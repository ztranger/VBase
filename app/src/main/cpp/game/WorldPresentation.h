#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "engine/audio/Audio.h"     // SoundId, SoundEvent
#include "engine/core/MathUtil.h"   // Vec3, Mat4
#include "game/SceneTypes.h"        // CombatMarker, DamageNumber, ImpactSpark, BuildPoof, DyingMob

// P2-18 шаг 2: ПРЕЗЕНТАЦИЯ мира — боевая косметика (HP-бары / числа урона / искры / вспышки /
// тряска камеры), звуковые события кадра, «трупы» убитых мобов и матрицы последнего боевого кадра
// (для проекции оверлея в GameUi). Scene ПРОИЗВОДИТ эти данные (диф hp из снапшотов, без ImGui) и
// строит из них RenderFrame; GameUi проецирует мировые точки. Чистые данные + мелкий хелпер
// emitSound (антиспам). Без рендера/сети/мировой симуляции.
struct WorldPresentation {
    std::vector<CombatMarker> markers;              // HP-бары (пересобираются каждый render из интерп. позиций)
    std::vector<DamageNumber> damageNumbers;        // всплывающие числа урона (живут ~kDmgLife)
    std::unordered_map<uint32_t, float> maxHpSeen;  // id -> наблюдаемый максимум hp (доля бара)
    std::unordered_map<uint32_t, float> flash;      // id -> остаток hit-flash/scale-punch, сек
    float localFlash = 0.0f;                        // hit-flash/punch своего героя
    std::vector<ImpactSpark> sparks;                // искры-вспышки в точке хита (живут ~kSparkLife)
    std::vector<BuildPoof> poofs;                   // «пуфы» постройки (живут ~kPoofLife)
    std::vector<SoundEvent> sounds;                 // звуковые события кадра (антиспам в emitSound)
    std::vector<DyingMob> dyingMobs;                // «трупы» убитых мобов (клип смерти на месте гибели)
    float shakeTime = 0.0f;                         // остаток тряски камеры, сек
    float shakeAmp = 0.0f;                          // пиковая амплитуда тряски (world units)
    uint32_t sparkSeed = 0x9e3779b9;                // накопитель псевдослучайных направлений искр
    uint8_t prevPhase = 0;                          // прошлый GamePhase — звук исхода матча по фронту
    Mat4 lastView, lastProj;                        // матрицы последнего боевого кадра (проекция маркеров)

    // Добавить звук с кэпом повторов за кадр: в бою за кадр много одновременных хитов — не даём
    // одному звуку задублироваться больше kMax раз (иначе каша/перегруз микса).
    void emitSound(SoundId id) {
        constexpr int kMax = 4;
        int n = 0;
        for (const SoundEvent& e : sounds) if (e.id == id) ++n;
        if (n < kMax) sounds.push_back({id});
    }
};
