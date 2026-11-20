#include "audio/audio_engine.h"

#include "base/encoding.h"
#include "base/log.h"

#include <miniaudio.h>

extern "C" {
int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace gm {

struct AudioEngine::Impl {
    ma_device device{};
    bool device_started = false;
};

namespace {

void ma_data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    (void)input;
    auto* engine = static_cast<AudioEngine*>(device->pUserData);
    auto* out = static_cast<f32*>(output);
    if (engine) {
        engine->mix_frames(out, frame_count);
    } else {
        std::memset(out, 0, static_cast<usize>(frame_count) * 2u * sizeof(f32));
    }
}

void resample_stereo_linear(
    const f32* src,
    usize src_frames,
    u32 src_rate,
    std::vector<f32>& dst,
    u32 dst_rate) {
    if (src_frames == 0 || src_rate == 0) {
        dst.clear();
        return;
    }
    if (src_rate == dst_rate) {
        dst.assign(src, src + src_frames * 2u);
        return;
    }
    const f64 ratio = static_cast<f64>(src_rate) / static_cast<f64>(dst_rate);
    const usize dst_frames = static_cast<usize>(static_cast<f64>(src_frames) / ratio);
    dst.resize(dst_frames * 2u);
    for (usize i = 0; i < dst_frames; ++i) {
        const f64 pos = static_cast<f64>(i) * ratio;
        const usize i0 = static_cast<usize>(pos);
        const usize i1 = std::min(i0 + 1, src_frames - 1);
        const f32 frac = static_cast<f32>(pos - static_cast<f64>(i0));
        dst[i * 2u + 0] = src[i0 * 2u + 0] * (1.0f - frac) + src[i1 * 2u + 0] * frac;
        dst[i * 2u + 1] = src[i0 * 2u + 1] * (1.0f - frac) + src[i1 * 2u + 1] * frac;
    }
}

} // namespace

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {
    voices_.resize(64);
}

AudioEngine::~AudioEngine() {
    shutdown();
}

Result<void> AudioEngine::init() {
    if (initialized_) {
        return Result<void>::success();
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = kOutputSampleRate;
    cfg.dataCallback = &ma_data_callback;
    cfg.pUserData = this;

    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        GM_LOG_WARN("audio", "Failed to initialize miniaudio device; continuing in silent mode");
        initialized_ = true;
        return Result<void>::success();
    }

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        GM_LOG_WARN("audio", "Failed to start miniaudio device; continuing in silent mode");
        initialized_ = true;
        return Result<void>::success();
    }

    impl_->device_started = true;
    initialized_ = true;
    GM_LOG_INFO("audio", "Initialized miniaudio playback (%u Hz stereo f32)", kOutputSampleRate);
    return Result<void>::success();
}

void AudioEngine::shutdown() noexcept {
    if (!initialized_) return;
    if (impl_ && impl_->device_started) {
        ma_device_uninit(&impl_->device);
        impl_->device_started = false;
    }
    std::scoped_lock lock(mutex_);
    for (auto& v : voices_) v.active = false;
    sounds_.clear();
    initialized_ = false;
}

bool AudioEngine::decode_audio_to_stereo_f32(std::span<const u8> encoded, std::vector<f32>& out_pcm) {
    out_pcm.clear();
    if (encoded.size() < 4) {
        return false;
    }

    // Check for OGG Vorbis magic "OggS" first -> stb_vorbis.
    if (encoded[0] == 'O' && encoded[1] == 'g' && encoded[2] == 'g' && encoded[3] == 'S') {
        int channels = 0;
        int sample_rate = 0;
        short* decoded = nullptr;
        const int frames = stb_vorbis_decode_memory(
            encoded.data(), static_cast<int>(encoded.size()), &channels, &sample_rate, &decoded);
        if (frames <= 0 || !decoded || channels <= 0) {
            if (decoded) std::free(decoded);
            return false;
        }
        std::vector<f32> stereo(static_cast<usize>(frames) * 2u);
        constexpr f32 kScale = 1.0f / 32768.0f;
        for (int i = 0; i < frames; ++i) {
            const f32 l = static_cast<f32>(decoded[i * channels]) * kScale;
            const f32 r = (channels >= 2) ? (static_cast<f32>(decoded[i * channels + 1]) * kScale) : l;
            stereo[static_cast<usize>(i) * 2u + 0] = l;
            stereo[static_cast<usize>(i) * 2u + 1] = r;
        }
        std::free(decoded);
        resample_stereo_linear(stereo.data(), static_cast<usize>(frames), static_cast<u32>(sample_rate), out_pcm, kOutputSampleRate);
        return !out_pcm.empty();
    }

    // Skip MIDI ("MThd") gracefully until SF2 soundfont synth is configured.
    if (encoded[0] == 'M' && encoded[1] == 'T' && encoded[2] == 'h' && encoded[3] == 'd') {
        return false;
    }

    // Use miniaudio built-in decoder (WAV, MP3, FLAC) directly into stereo f32 @ 44100 Hz.
    ma_decoder_config dec_cfg = ma_decoder_config_init(ma_format_f32, 2, kOutputSampleRate);
    ma_decoder decoder{};
    if (ma_decoder_init_memory(encoded.data(), encoded.size(), &dec_cfg, &decoder) != MA_SUCCESS) {
        return false;
    }

    constexpr usize kChunkFrames = 4096;
    std::vector<f32> chunk(kChunkFrames * 2u);
    for (;;) {
        ma_uint64 frames_read = 0;
        const ma_result res = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &frames_read);
        if (frames_read > 0) {
            out_pcm.insert(out_pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(frames_read * 2u));
        }
        if (res != MA_SUCCESS || frames_read < kChunkFrames) {
            break;
        }
    }
    ma_decoder_uninit(&decoder);
    return !out_pcm.empty();
}

void AudioEngine::load_game_sounds(const GameData& game) {
    std::scoped_lock lock(mutex_);
    sounds_.clear();
    sounds_.resize(game.sounds.size());

    usize decoded_ok = 0;
    usize active_with_data = 0;
    for (usize i = 0; i < game.sounds.size(); ++i) {
        const auto& src = game.sounds[i];
        if (!src.exists) continue;
        auto& dst = sounds_[i];
        dst.exists = true;
        dst.kind = src.kind;
        dst.volume = static_cast<f32>(std::clamp(src.volume, 0.0, 1.0));
        dst.pan = static_cast<f32>(std::clamp(src.pan, -1.0, 1.0));
        if (!src.data.empty()) {
            ++active_with_data;
            if (decode_audio_to_stereo_f32(src.data, dst.pcm_stereo)) {
                dst.loaded = true;
                ++decoded_ok;
            } else {
                GM_LOG_DEBUG("audio", "Sound[%zu] '%s' (%s, %zu bytes) not decoded",
                             i, src.name.c_str(), src.extension.c_str(), src.data.size());
            }
        }
    }
    GM_LOG_INFO("audio", "Loaded %zu / %zu embedded game sounds", decoded_ok, active_with_data);
}

i32 AudioEngine::add_sound_from_memory(std::span<const u8> encoded, i32 kind, std::string_view) {
    DecodedSound s{};
    s.exists = true;
    s.kind = kind;
    s.loaded = decode_audio_to_stereo_f32(encoded, s.pcm_stereo);
    std::scoped_lock lock(mutex_);
    const i32 id = static_cast<i32>(sounds_.size());
    sounds_.push_back(std::move(s));
    return id;
}

bool AudioEngine::replace_sound_from_memory(i32 sound_id, std::span<const u8> encoded, i32 kind, std::string_view) {
    if (sound_id < 0) return false;
    std::vector<f32> pcm;
    const bool ok = decode_audio_to_stereo_f32(encoded, pcm);
    std::scoped_lock lock(mutex_);
    if (static_cast<usize>(sound_id) >= sounds_.size()) return false;
    for (auto& v : voices_) {
        if (v.active && v.sound_id == sound_id) v.active = false;
    }
    auto& s = sounds_[static_cast<usize>(sound_id)];
    s.exists = true;
    s.kind = kind;
    s.pcm_stereo = std::move(pcm);
    s.loaded = ok;
    return ok;
}

void AudioEngine::delete_sound(i32 sound_id) {
    std::scoped_lock lock(mutex_);
    if (sound_id < 0 || static_cast<usize>(sound_id) >= sounds_.size()) return;
    for (auto& v : voices_) {
        if (v.active && v.sound_id == sound_id) v.active = false;
    }
    sounds_[static_cast<usize>(sound_id)] = DecodedSound{};
}

bool AudioEngine::sound_exists(i32 sound_id) const noexcept {
    std::scoped_lock lock(mutex_);
    return sound_id >= 0 && static_cast<usize>(sound_id) < sounds_.size() &&
           sounds_[static_cast<usize>(sound_id)].exists;
}

void AudioEngine::play_internal(i32 sound_id, bool loop) {
    std::scoped_lock lock(mutex_);
    if (sound_id < 0 || static_cast<usize>(sound_id) >= sounds_.size()) return;
    const auto& snd = sounds_[static_cast<usize>(sound_id)];
    if (!snd.exists || !snd.loaded || snd.pcm_stereo.empty()) return;

    // GM8 kind 1 (background music): stop other kind-1 music first; kind 0 stops same sound if single-channel?
    // Actually in GM8 vanilla DirectSound, sound_play on kind=1 stops previous background music,
    // and sound_play on kind=0 restarts or layers up to voice limit.
    if (snd.kind == 1) {
        for (auto& v : voices_) {
            if (v.active && v.sound_id >= 0 &&
                static_cast<usize>(v.sound_id) < sounds_.size() &&
                sounds_[static_cast<usize>(v.sound_id)].kind == 1) {
                v.active = false;
            }
        }
    }

    // Find a free voice slot (or steal oldest).
    ActiveVoice* slot = nullptr;
    for (auto& v : voices_) {
        if (!v.active) {
            slot = &v;
            break;
        }
    }
    if (!slot) {
        slot = &voices_[0];
    }

    slot->sound_id = sound_id;
    slot->frame_pos = 0.0;
    slot->pitch = 1.0;
    slot->volume = snd.volume;
    slot->pan = snd.pan;
    slot->fade_target = snd.volume;
    slot->fade_rate_per_sec = 0.0f;
    slot->looping = loop;
    slot->active = true;
}

void AudioEngine::sound_play(i32 sound_id) {
    play_internal(sound_id, false);
}

void AudioEngine::sound_loop(i32 sound_id) {
    play_internal(sound_id, true);
}

void AudioEngine::sound_stop(i32 sound_id) {
    std::scoped_lock lock(mutex_);
    for (auto& v : voices_) {
        if (v.active && v.sound_id == sound_id) {
            v.active = false;
        }
    }
}

void AudioEngine::sound_stop_all() {
    std::scoped_lock lock(mutex_);
    for (auto& v : voices_) {
        v.active = false;
    }
}

bool AudioEngine::sound_isplaying(i32 sound_id) const {
    std::scoped_lock lock(mutex_);
    for (const auto& v : voices_) {
        if (v.active && v.sound_id == sound_id) {
            return true;
        }
    }
    return false;
}

void AudioEngine::sound_volume(i32 sound_id, f32 vol) {
    const f32 clamped = std::clamp(vol, 0.0f, 1.0f);
    std::scoped_lock lock(mutex_);
    if (sound_id >= 0 && static_cast<usize>(sound_id) < sounds_.size()) {
        sounds_[static_cast<usize>(sound_id)].volume = clamped;
    }
    for (auto& v : voices_) {
        if (v.active && v.sound_id == sound_id) {
            v.volume = clamped;
            v.fade_target = clamped;
            v.fade_rate_per_sec = 0.0f;
        }
    }
}

void AudioEngine::sound_pan(i32 sound_id, f32 pan) {
    const f32 clamped = std::clamp(pan, -1.0f, 1.0f);
    std::scoped_lock lock(mutex_);
    if (sound_id >= 0 && static_cast<usize>(sound_id) < sounds_.size()) {
        sounds_[static_cast<usize>(sound_id)].pan = clamped;
    }
    for (auto& v : voices_) {
        if (v.active && v.sound_id == sound_id) {
            v.pan = clamped;
        }
    }
}

void AudioEngine::sound_fade(i32 sound_id, f32 target_vol, f32 duration_ms) {
    const f32 target = std::clamp(target_vol, 0.0f, 1.0f);
    std::scoped_lock lock(mutex_);
    if (duration_ms <= 0.0f) {
        if (sound_id >= 0 && static_cast<usize>(sound_id) < sounds_.size()) {
            sounds_[static_cast<usize>(sound_id)].volume = target;
        }
        for (auto& v : voices_) {
            if (v.active && v.sound_id == sound_id) {
                v.volume = target;
                v.fade_target = target;
                v.fade_rate_per_sec = 0.0f;
            }
        }
        return;
    }
    const f32 dur_sec = duration_ms * 0.001f;
    for (auto& v : voices_) {
        if (v.active && v.sound_id == sound_id) {
            v.fade_target = target;
            v.fade_rate_per_sec = (target - v.volume) / dur_sec;
        }
    }
}

void AudioEngine::sound_global_volume(f32 vol) {
    std::scoped_lock lock(mutex_);
    global_volume_ = std::clamp(vol, 0.0f, 1.0f);
}

void AudioEngine::mix_frames(f32* out_stereo, u32 frame_count) noexcept {
    std::memset(out_stereo, 0, static_cast<usize>(frame_count) * 2u * sizeof(f32));

    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    const f32 dt_frame = 1.0f / static_cast<f32>(kOutputSampleRate);

    for (auto& v : voices_) {
        if (!v.active || v.sound_id < 0 || static_cast<usize>(v.sound_id) >= sounds_.size()) {
            continue;
        }
        const auto& snd = sounds_[static_cast<usize>(v.sound_id)];
        const usize total_frames = snd.pcm_stereo.size() / 2u;
        if (!snd.loaded || total_frames == 0) {
            v.active = false;
            continue;
        }

        const f32 gain_l = (v.pan <= 0.0f) ? 1.0f : (1.0f - v.pan);
        const f32 gain_r = (v.pan >= 0.0f) ? 1.0f : (1.0f + v.pan);

        for (u32 f = 0; f < frame_count; ++f) {
            if (v.fade_rate_per_sec != 0.0f) {
                v.volume += v.fade_rate_per_sec * dt_frame;
                if ((v.fade_rate_per_sec > 0.0f && v.volume >= v.fade_target) ||
                    (v.fade_rate_per_sec < 0.0f && v.volume <= v.fade_target)) {
                    v.volume = v.fade_target;
                    v.fade_rate_per_sec = 0.0f;
                }
            }

            const usize idx = static_cast<usize>(v.frame_pos);
            if (idx >= total_frames) {
                if (v.looping) {
                    v.frame_pos = std::fmod(v.frame_pos, static_cast<f64>(total_frames));
                } else {
                    v.active = false;
                    break;
                }
            }

            const usize fi = static_cast<usize>(v.frame_pos);
            const f32 vol = v.volume * global_volume_;
            out_stereo[f * 2u + 0] += snd.pcm_stereo[fi * 2u + 0] * vol * gain_l;
            out_stereo[f * 2u + 1] += snd.pcm_stereo[fi * 2u + 1] * vol * gain_r;
            v.frame_pos += v.pitch;
        }
    }

    // Soft-clip output to [-1, 1].
    const usize total_samples = static_cast<usize>(frame_count) * 2u;
    for (usize i = 0; i < total_samples; ++i) {
        out_stereo[i] = std::clamp(out_stereo[i], -1.0f, 1.0f);
    }
}

} // namespace gm
