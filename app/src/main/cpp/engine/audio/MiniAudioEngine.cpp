#include "engine/audio/MiniAudioEngine.h"

#include <cstdint>
#include <vector>

#include "miniaudio.h"

#include "engine/assets/AssetSource.h"
#include "engine/core/Log.h"

void loadGameAudio(Audio& audio, AssetSource& assets) {
    std::vector<uint8_t> buf;
    for (int i = 0; i < (int)SoundId::Count; ++i) {
        SoundId id = (SoundId)i;
        buf.clear();
        if (assets.read(soundFileName(id), buf) && !buf.empty())
            audio.loadSound(id, buf.data(), buf.size());
    }
    buf.clear();
    if (assets.read("audio/music.wav", buf) && !buf.empty())
        audio.loadMusic(buf.data(), buf.size());
}

namespace {
// Имя регистрации закодированных данных в resource manager (play_sound ищет по нему).
const char* regName(SoundId id) {
    static const char* kNames[(int)SoundId::Count] = {
        "s_hit", "s_core", "s_death", "s_shoot", "s_build", "s_win", "s_lose"};
    int i = (int)id;
    return (i >= 0 && i < (int)SoundId::Count) ? kNames[i] : "s_x";
}
}  // namespace

struct MiniAudioEngine::Impl {
    ma_engine engine{};
    bool ready = false;
    // Копии закодированных WAV — resource manager хранит УКАЗАТЕЛЬ (self-managed), держим живыми.
    std::vector<uint8_t> sfx[(int)SoundId::Count];
    bool sfxReg[(int)SoundId::Count] = {};
    std::vector<uint8_t> musicData;
    ma_sound music{};
    bool musicInited = false;
    float musicVol = 0.4f;
};

MiniAudioEngine::MiniAudioEngine() : d_(new Impl()) {}
MiniAudioEngine::~MiniAudioEngine() { shutdown(); }

bool MiniAudioEngine::init() {
    ma_engine_config cfg = ma_engine_config_init();
    ma_result r = ma_engine_init(&cfg, &d_->engine);
    if (r != MA_SUCCESS) {
        LOGW("Audio: устройство не открылось (код %d) — игра без звука", (int)r);
        return false;
    }
    d_->ready = true;
    LOGI("Audio: miniaudio запущен");
    return true;
}

void MiniAudioEngine::shutdown() {
    if (d_->musicInited) {
        ma_sound_uninit(&d_->music);
        d_->musicInited = false;
    }
    if (d_->ready) {
        ma_engine_uninit(&d_->engine);
        d_->ready = false;
    }
}

bool MiniAudioEngine::available() const { return d_->ready; }

void MiniAudioEngine::loadSound(SoundId id, const void* data, size_t size) {
    if (!d_->ready || data == nullptr || size == 0) return;
    int i = (int)id;
    if (i < 0 || i >= (int)SoundId::Count) return;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    d_->sfx[i].assign(b, b + size);  // копия — держим живой на время регистрации
    ma_resource_manager* rm = ma_engine_get_resource_manager(&d_->engine);
    ma_result r = ma_resource_manager_register_encoded_data(rm, regName(id), d_->sfx[i].data(),
                                                            d_->sfx[i].size());
    d_->sfxReg[i] = (r == MA_SUCCESS);
    if (r != MA_SUCCESS) LOGW("Audio: звук %d не зарегистрирован (код %d)", i, (int)r);
}

void MiniAudioEngine::loadMusic(const void* data, size_t size) {
    if (!d_->ready || data == nullptr || size == 0) return;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    d_->musicData.assign(b, b + size);
    ma_resource_manager* rm = ma_engine_get_resource_manager(&d_->engine);
    ma_resource_manager_register_encoded_data(rm, "music", d_->musicData.data(),
                                              d_->musicData.size());
}

void MiniAudioEngine::play(SoundId id) {
    if (!d_->ready) return;
    int i = (int)id;
    if (i < 0 || i >= (int)SoundId::Count || !d_->sfxReg[i]) return;
    ma_engine_play_sound(&d_->engine, regName(id), nullptr);  // fire-and-forget, миксуется сам
}

void MiniAudioEngine::startMusic() {
    if (!d_->ready || d_->musicData.empty()) return;
    if (d_->musicInited) {
        ma_sound_start(&d_->music);
        return;
    }
    // Грузим целиком (не STREAM): стриминговый путь не видит register_encoded_data (-7). Трек
    // короткий (~16 c), в памяти дёшев. NO_SPATIALIZATION — фоновая музыка без 3D-позиции.
    ma_result r = ma_sound_init_from_file(&d_->engine, "music", MA_SOUND_FLAG_NO_SPATIALIZATION,
                                          nullptr, nullptr, &d_->music);
    if (r != MA_SUCCESS) {
        LOGW("Audio: музыка не инициализирована (код %d)", (int)r);
        return;
    }
    d_->musicInited = true;
    ma_sound_set_looping(&d_->music, MA_TRUE);
    ma_sound_set_volume(&d_->music, d_->musicVol);
    ma_sound_start(&d_->music);
}

void MiniAudioEngine::stopMusic() {
    if (d_->musicInited) ma_sound_stop(&d_->music);
}

void MiniAudioEngine::setMasterVolume(float v) {
    if (d_->ready) ma_engine_set_volume(&d_->engine, v);  // общий множитель SFX+музыка
}

void MiniAudioEngine::setMusicVolume(float v) {
    d_->musicVol = v;
    if (d_->musicInited) ma_sound_set_volume(&d_->music, v);
}
