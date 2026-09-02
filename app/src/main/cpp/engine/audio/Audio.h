#pragma once

// Аудио за интерфейсом (как Renderer/AssetSource): игровой слой просит SoundId, платформа
// держит конкретную реализацию (miniaudio) и проигрывает. Заголовок ПОРТАТИВНЫЙ — не тянет
// ни miniaudio, ни платформу, поэтому его можно включать из game/ (как engine/net/Net.h).

#include <cstddef>
#include <cstdint>

// Словарь звуков: общий контракт игры и аудио-движка. Порядок = индекс в таблицах.
enum class SoundId : uint8_t {
    Hit,          // урон по бою (мили/попадание снаряда)
    CoreHit,      // урон по ядру (тяжелее, громче)
    EnemyDeath,   // смерть моба
    Shoot,        // выстрел снарядом (маг/башня)
    Build,        // постройка здания
    Victory,      // исход матча — победа
    Defeat,       // исход матча — поражение
    Count
};

// Событие звука из игрового слоя: Scene накапливает, платформа сливает и проигрывает.
struct SoundEvent { SoundId id; };

// Файл ассета для звука (грузится через AssetSource; путь относительный, как у прочих ассетов).
inline const char* soundFileName(SoundId id) {
    switch (id) {
        case SoundId::Hit:        return "audio/hit.wav";
        case SoundId::CoreHit:    return "audio/core_hit.wav";
        case SoundId::EnemyDeath: return "audio/enemy_death.wav";
        case SoundId::Shoot:      return "audio/shoot.wav";
        case SoundId::Build:      return "audio/build.wav";
        case SoundId::Victory:    return "audio/victory.wav";
        case SoundId::Defeat:     return "audio/defeat.wav";
        default:                  return "";
    }
}

// Интерфейс аудио-движка. Реализация (MiniAudioEngine) живёт в engine/audio, конкретика
// (устройство/микс) — внутри неё. Если движок не поднялся (нет устройства) — все методы no-op.
class Audio {
public:
    virtual ~Audio() = default;

    virtual bool init() = 0;       // открыть устройство; false = звука не будет (не критично)
    virtual void shutdown() = 0;
    virtual bool available() const = 0;

    // Зарегистрировать закодированный WAV под SoundId (байты движок держит у себя — копирует).
    virtual void loadSound(SoundId id, const void* data, size_t size) = 0;
    // Фоновая музыка (один луп): закодированный WAV; стартует stream+loop по startMusic().
    virtual void loadMusic(const void* data, size_t size) = 0;

    virtual void play(SoundId id) = 0;        // fire-and-forget, накладывается сам
    virtual void startMusic() = 0;
    virtual void stopMusic() = 0;

    virtual void setMasterVolume(float v) = 0;  // 0..1 — общий SFX+музыка
    virtual void setMusicVolume(float v) = 0;   // 0..1 — только музыка
};
