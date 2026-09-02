#pragma once

// Реализация Audio на miniaudio (кроссплатформенно: Windows WASAPI / Android AAudio+OpenSL).
// pimpl — miniaudio.h тяжёлый и не должен течь в потребителей (его включает только .cpp).
// Звуки грузятся из памяти (байты от AssetSource) через resource manager — работает и в APK.

#include <memory>

#include "engine/audio/Audio.h"

struct AssetSource;  // engine/assets/AssetSource.h (fwd — без тяжёлого include в заголовке)

// Загрузить все игровые звуки (по словарю SoundId) + музыку из ассетов в движок.
// Кроссплатформенно: AssetSource читает с диска (десктоп) или из APK (Android).
void loadGameAudio(Audio& audio, AssetSource& assets);

class MiniAudioEngine : public Audio {
public:
    MiniAudioEngine();
    ~MiniAudioEngine() override;

    bool init() override;
    void shutdown() override;
    bool available() const override;

    void loadSound(SoundId id, const void* data, size_t size) override;
    void loadMusic(const void* data, size_t size) override;

    void play(SoundId id) override;
    void startMusic() override;
    void stopMusic() override;

    void setMasterVolume(float v) override;
    void setMusicVolume(float v) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
