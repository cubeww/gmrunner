#include "extensions/shim_maizemusic.h"

#include "base/encoding.h"
#include "base/log.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace gm {

namespace {

// BASS_FX / DirectSound DX8 effect codes used by MaizeMusic mmMusicSetFX(handle, fxType)
DspEffectType mm_fx_type_to_dsp(i32 fx_type) noexcept {
    switch (fx_type) {
        case 0: return DspEffectType::Chorus;
        case 1: return DspEffectType::Compressor;
        case 2: return DspEffectType::Distortion;
        case 3: return DspEffectType::Echo;
        case 4: return DspEffectType::Flange;
        case 5: return DspEffectType::Gargle;
        case 6: return DspEffectType::ParamEq;
        case 7: return DspEffectType::SfxReverb;
        case 8: return DspEffectType::Reverb;
        default: return DspEffectType::None;
    }
}

struct MmMusicEntry {
    i32 ext_sound_id = 0;
    i32 channel_id = 0;
    bool looping = false;
    f32 volume = 1.0f;
    f32 pan = 0.0f;
    f32 tempo_pct = 0.0f;      // BASS_ATTRIB_TEMPO: -95..+5000 (%)
    f32 pitch_semi = 0.0f;     // BASS_ATTRIB_TEMPO_PITCH: -60..+60 (semitones)
    f32 speed_mult = 1.0f;     // Speed multiplier (1.0 = normal)
    bool has_loop_sec = false;
    f64 loop_start_sec = 0.0;
    f64 loop_end_sec = 0.0;
    std::vector<i32> fx_handles;
};

class MaizeMusicShim final : public IDllShim {
public:
    std::string_view canonical_name() const noexcept override { return "MaizeMusic"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "maizemusic" || normalized_dll == "bass" || normalized_dll == "bass_fx";
    }

    bool has_function(std::string_view func_name) const noexcept override {
        return func_name.starts_with("mm");
    }

    void on_unload(IExtensionHost& host) override {
        auto& audio = host.ext_audio();
        for (auto& [h, e] : streams_) {
            if (e.channel_id > 0) audio.channel_stop(e.channel_id);
            if (e.ext_sound_id > 0) audio.ext_free_sound(e.ext_sound_id);
        }
        streams_.clear();
        fx_map_.clear();
        data_buffer_.clear();
    }

    RValue invoke(
        IExtensionHost& host,
        std::string_view func_name,
        std::span<const RValue> args,
        ExternalValueType) override {
        auto& audio = host.ext_audio();

        // Lifecycle
        if (func_name == "mmInit") {
            initialized_ = true;
            audio.group_set_volume(static_cast<i32>(AudioGroupId::MaizeMusic), global_volume_);
            return RValue(1.0);
        }
        if (func_name == "mmFree") {
            on_unload(host);
            initialized_ = false;
            return RValue(1.0);
        }

        // Device & CPU queries
        if (func_name == "mmGetDeviceNumber") {
            return RValue(1.0);
        }
        if (func_name == "mmGetDeviceName") {
            return RValue(std::string("Default Audio Device"));
        }
        if (func_name == "mmSetDevice") {
            return RValue(1.0);
        }
        if (func_name == "mmGetCPU" || func_name == "mmGetMusicCPU") {
            return RValue(0.5);
        }

        // Recording stubs (no-op, clean return)
        if (func_name == "mmRecordStart" || func_name == "mmRecordStop" ||
            func_name == "mmRecordSetInput" || func_name == "mmRecordGetVolume") {
            return RValue(0.0);
        }

        // Global volume
        if (func_name == "mmMusicSetGlobalVolume") {
            global_volume_ = static_cast<f32>(std::clamp(shim_real(args, 0, 1.0), 0.0, 1.0));
            audio.group_set_volume(static_cast<i32>(AudioGroupId::MaizeMusic), global_volume_);
            return RValue(1.0);
        }
        if (func_name == "mmMusicGetGlobalVolume") {
            return RValue(static_cast<f64>(global_volume_));
        }

        // Loading
        if (func_name == "mmLoadMusic" || func_name == "mmLoadMusicExt" ||
            func_name == "mmLoadMusicMem" || func_name == "mmLoadMusicMemExt") {
            const std::string path = shim_str(args, 0);
            const bool loop = shim_bool(args, 1, false);
            const bool is_mem = (func_name == "mmLoadMusicMem" || func_name == "mmLoadMusicMemExt");
            const bool is_ext = (func_name == "mmLoadMusicExt" || func_name == "mmLoadMusicMemExt");

            auto bytes_res = host.ext_vfs().read_all_bytes(path);
            if (!bytes_res) {
                GM_LOG_DEBUG("ext", "MaizeMusic %s: file not found '%s'",
                             std::string(func_name).c_str(), path.c_str());
                return RValue(0.0);
            }

            std::span<const u8> slice = bytes_res.value();
            if (is_ext && args.size() >= 4) {
                const usize offset = static_cast<usize>(std::max(0.0, shim_real(args, 2, 0.0)));
                const usize length = static_cast<usize>(std::max(0.0, shim_real(args, 3, 0.0)));
                if (offset < slice.size()) {
                    const usize avail = slice.size() - offset;
                    slice = slice.subspan(offset, length > 0 ? std::min(length, avail) : avail);
                }
            }

            const i32 ext_id = audio.ext_load_from_memory(
                slice, false, !is_mem, static_cast<i32>(AudioGroupId::MaizeMusic), path);
            if (ext_id <= 0) return RValue(0.0);

            MmMusicEntry e{};
            e.ext_sound_id = ext_id;
            e.looping = loop;
            const i32 h = next_handle_++;
            streams_[h] = std::move(e);
            return RValue(static_cast<f64>(h));
        }
        if (func_name == "mmLoadMusicRes") {
            const i32 gm_snd_id = shim_i32(args, 0, -1);
            const bool loop = shim_bool(args, 1, false);
            const auto& game = host.ext_game();
            if (gm_snd_id < 0 || static_cast<usize>(gm_snd_id) >= game.sounds.size() ||
                !game.sounds[static_cast<usize>(gm_snd_id)].exists ||
                game.sounds[static_cast<usize>(gm_snd_id)].data.empty()) {
                return RValue(0.0);
            }
            const auto& s = game.sounds[static_cast<usize>(gm_snd_id)];
            const i32 ext_id = audio.ext_load_from_memory(
                s.data, false, false, static_cast<i32>(AudioGroupId::MaizeMusic), s.extension);
            if (ext_id <= 0) return RValue(0.0);
            MmMusicEntry e{};
            e.ext_sound_id = ext_id;
            e.looping = loop;
            const i32 h = next_handle_++;
            streams_[h] = std::move(e);
            return RValue(static_cast<f64>(h));
        }
        if (func_name == "mmLoadMusicUrl") {
            return RValue(0.0);
        }

        // Data buffer readback (does not require stream handle)
        if (func_name == "mmMusicReadDataPos") {
            const i32 pos = shim_i32(args, 0, 0);
            if (pos < 0 || static_cast<usize>(pos) >= data_buffer_.size()) return RValue(0.0);
            return RValue(static_cast<f64>(data_buffer_[static_cast<usize>(pos)]));
        }

        // Effect parameter updates by FX handle
        if (func_name == "mmMusicEffectReset") {
            const i32 fx_h = shim_i32(args, 0, 0);
            return RValue(audio.dsp_reset(fx_h) ? 1.0 : 0.0);
        }
        if (func_name == "mmMusicEffectChorus") {
            const i32 fx_h = shim_i32(args, 0, 0);
            const f32 wet = static_cast<f32>(std::clamp(shim_real(args, 1, 50.0) * 0.01, 0.0, 1.0));
            const f32 freq = static_cast<f32>(shim_real(args, 4, 1.1));
            const f32 delay = static_cast<f32>(shim_real(args, 6, 16.0));
            audio.dsp_set_param_value(fx_h, 0, 1.0f - wet * 0.5f);
            audio.dsp_set_param_value(fx_h, 1, wet);
            audio.dsp_set_param_value(fx_h, 4, delay);
            audio.dsp_set_param_value(fx_h, 5, freq);
            return RValue(1.0);
        }
        if (func_name == "mmMusicEffectEcho") {
            const i32 fx_h = shim_i32(args, 0, 0);
            const f32 wet = static_cast<f32>(std::clamp(shim_real(args, 1, 50.0) * 0.01, 0.0, 1.0));
            const f32 fb = static_cast<f32>(std::clamp(shim_real(args, 2, 50.0) * 0.01, 0.0, 0.95));
            const f32 ld = static_cast<f32>(shim_real(args, 3, 500.0));
            audio.dsp_set_param_value(fx_h, 0, ld);
            audio.dsp_set_param_value(fx_h, 1, fb);
            audio.dsp_set_param_value(fx_h, 3, 1.0f - wet * 0.5f);
            audio.dsp_set_param_value(fx_h, 4, wet);
            return RValue(1.0);
        }
        if (func_name == "mmMusicEffectFlanger") {
            const i32 fx_h = shim_i32(args, 0, 0);
            const f32 wet = static_cast<f32>(std::clamp(shim_real(args, 1, 50.0) * 0.01, 0.0, 1.0));
            const f32 depth = static_cast<f32>(std::clamp(shim_real(args, 2, 100.0) * 0.01, 0.01, 1.0));
            const f32 freq = static_cast<f32>(shim_real(args, 4, 0.25));
            audio.dsp_set_param_value(fx_h, 0, 1.0f - wet * 0.5f);
            audio.dsp_set_param_value(fx_h, 1, wet);
            audio.dsp_set_param_value(fx_h, 2, depth);
            audio.dsp_set_param_value(fx_h, 3, freq);
            return RValue(1.0);
        }
        if (func_name == "mmMusicEffectGargle") {
            const i32 fx_h = shim_i32(args, 0, 0);
            audio.dsp_set_param_value(fx_h, 0, static_cast<f32>(shim_real(args, 1, 20.0)));
            audio.dsp_set_param_value(fx_h, 1, static_cast<f32>(shim_real(args, 2, 0.0)));
            return RValue(1.0);
        }
        if (func_name == "mmMusicEffectReverb") {
            const i32 fx_h = shim_i32(args, 0, 0);
            audio.dsp_set_param_value(fx_h, 0, static_cast<f32>(shim_real(args, 1, 0.0)));
            audio.dsp_set_param_value(fx_h, 1, static_cast<f32>(shim_real(args, 2, 0.0)));
            audio.dsp_set_param_value(fx_h, 2, static_cast<f32>(shim_real(args, 3, 1000.0) * 0.001));
            audio.dsp_set_param_value(fx_h, 3, static_cast<f32>(shim_real(args, 4, 0.5)));
            return RValue(1.0);
        }
        if (func_name == "mmMusicEffectCompressor") {
            const i32 fx_h = shim_i32(args, 0, 0);
            audio.dsp_set_param_value(fx_h, 3, static_cast<f32>(shim_real(args, 1, 0.0)));
            audio.dsp_set_param_value(fx_h, 1, static_cast<f32>(shim_real(args, 2, 10.0)));
            audio.dsp_set_param_value(fx_h, 2, static_cast<f32>(shim_real(args, 3, 200.0)));
            audio.dsp_set_param_value(fx_h, 0, static_cast<f32>(shim_real(args, 4, -20.0)));
            return RValue(1.0);
        }
        if (func_name == "mmMusicEffectEqualizer") {
            const i32 fx_h = shim_i32(args, 0, 0);
            audio.dsp_set_param_value(fx_h, 0, static_cast<f32>(shim_real(args, 1, 8000.0)));
            audio.dsp_set_param_value(fx_h, 1, static_cast<f32>(std::clamp(shim_real(args, 2, 12.0) / 12.0, 0.2, 5.0)));
            audio.dsp_set_param_value(fx_h, 2, static_cast<f32>(std::pow(10.0, shim_real(args, 3, 0.0) / 20.0)));
            return RValue(1.0);
        }

        // Stream-handle functions
        const i32 h = shim_i32(args, 0, 0);
        auto it = streams_.find(h);
        if (it == streams_.end()) {
            return RValue(0.0);
        }
        auto& e = it->second;

        if (func_name == "mmFreeMusic") {
            if (e.channel_id > 0) audio.channel_stop(e.channel_id);
            audio.ext_free_sound(e.ext_sound_id);
            streams_.erase(it);
            return RValue(1.0);
        }
        if (func_name == "mmMusicPlay") {
            if (e.channel_id > 0) {
                audio.channel_stop(e.channel_id);
                e.channel_id = 0;
            }
            e.channel_id = audio.channel_create(e.ext_sound_id, e.looping, false);
            if (e.channel_id > 0) {
                apply_channel_attrs(audio, e);
            }
            return RValue(e.channel_id > 0 ? 1.0 : 0.0);
        }
        if (func_name == "mmMusicPause") {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                audio.channel_set_paused(e.channel_id, true);
                return RValue(1.0);
            }
            return RValue(0.0);
        }
        if (func_name == "mmMusicResume") {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                audio.channel_set_paused(e.channel_id, false);
                return RValue(1.0);
            }
            // If stopped, start playback on resume as a safe fallback
            e.channel_id = audio.channel_create(e.ext_sound_id, e.looping, false);
            if (e.channel_id > 0) apply_channel_attrs(audio, e);
            return RValue(e.channel_id > 0 ? 1.0 : 0.0);
        }
        if (func_name == "mmMusicStop") {
            if (e.channel_id > 0) {
                audio.channel_stop(e.channel_id);
                e.channel_id = 0;
            }
            return RValue(1.0);
        }
        if (func_name == "mmMusicIsActive") {
            // BASS_ACTIVE_STOPPED = 0, BASS_ACTIVE_PLAYING = 1, BASS_ACTIVE_STALLED = 2, BASS_ACTIVE_PAUSED = 3
            if (e.channel_id <= 0 || !audio.channel_is_playing(e.channel_id)) return RValue(0.0);
            return RValue(audio.channel_is_paused(e.channel_id) ? 3.0 : 1.0);
        }
        if (func_name == "mmMusicSlideVolume") {
            const f32 target = static_cast<f32>(std::clamp(shim_real(args, 1, 1.0), 0.0, 1.0));
            const f32 dur_ms = static_cast<f32>(std::max(0.0, shim_real(args, 2, 0.0)));
            e.volume = target;
            if (e.channel_id > 0) {
                audio.channel_fade_volume(e.channel_id, target, dur_ms);
            }
            return RValue(1.0);
        }
        if (func_name == "mmMusicSetVolume") {
            e.volume = static_cast<f32>(std::clamp(shim_real(args, 1, 1.0), 0.0, 1.0));
            if (e.channel_id > 0) audio.channel_set_volume(e.channel_id, e.volume);
            return RValue(1.0);
        }
        if (func_name == "mmMusicGetVolume") {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                return RValue(static_cast<f64>(audio.channel_get_volume(e.channel_id)));
            }
            return RValue(static_cast<f64>(e.volume));
        }
        if (func_name == "mmMusicSetPan") {
            e.pan = static_cast<f32>(std::clamp(shim_real(args, 1, 0.0), -1.0, 1.0));
            if (e.channel_id > 0) audio.channel_set_pan(e.channel_id, e.pan);
            return RValue(1.0);
        }
        if (func_name == "mmMusicGetPan") {
            return RValue(static_cast<f64>(e.pan));
        }
        if (func_name == "mmMusicGetLength") {
            return RValue(audio.ext_sound_length_ms(e.ext_sound_id) * 0.001);
        }
        if (func_name == "mmMusicGetPosition") {
            if (e.channel_id <= 0 || !audio.channel_is_playing(e.channel_id)) return RValue(0.0);
            return RValue(audio.channel_get_position_ms(e.channel_id) * 0.001);
        }
        if (func_name == "mmMusicSetPosition") {
            const f64 sec = std::max(0.0, shim_real(args, 1, 0.0));
            if (e.channel_id > 0) audio.channel_set_position_ms(e.channel_id, sec * 1000.0);
            return RValue(1.0);
        }
        if (func_name == "mmMusicSetTempo") {
            e.tempo_pct = static_cast<f32>(std::clamp(shim_real(args, 1, 0.0), -95.0, 5000.0));
            if (e.channel_id > 0) audio.channel_set_pitch(e.channel_id, compute_effective_rate(e));
            return RValue(1.0);
        }
        if (func_name == "mmMusicGetTempo") {
            return RValue(static_cast<f64>(e.tempo_pct));
        }
        if (func_name == "mmMusicSetPitch") {
            e.pitch_semi = static_cast<f32>(std::clamp(shim_real(args, 1, 0.0), -60.0, 60.0));
            if (e.channel_id > 0) audio.channel_set_pitch(e.channel_id, compute_effective_rate(e));
            return RValue(1.0);
        }
        if (func_name == "mmMusicGetPitch") {
            return RValue(static_cast<f64>(e.pitch_semi));
        }
        if (func_name == "mmMusicSetSpeed") {
            const f64 spd = shim_real(args, 1, 1.0);
            e.speed_mult = static_cast<f32>(spd > 10.0 ? (spd / 44100.0) : std::clamp(spd, 0.05, 8.0));
            if (e.channel_id > 0) audio.channel_set_pitch(e.channel_id, compute_effective_rate(e));
            return RValue(1.0);
        }
        if (func_name == "mmMusicGetSpeed") {
            return RValue(static_cast<f64>(e.speed_mult));
        }
        if (func_name == "mmMusicSetLoopSection") {
            e.has_loop_sec = true;
            e.loop_start_sec = std::max(0.0, shim_real(args, 1, 0.0));
            e.loop_end_sec = std::max(e.loop_start_sec + 0.001, shim_real(args, 2, 0.0));
            audio.ext_sound_set_loop_points_ms(e.ext_sound_id, e.loop_start_sec * 1000.0, e.loop_end_sec * 1000.0);
            if (e.channel_id > 0) {
                audio.channel_set_loop_points_ms(e.channel_id, e.loop_start_sec * 1000.0, e.loop_end_sec * 1000.0);
            }
            return RValue(1.0);
        }
        if (func_name == "mmMusicRemoveLoopSection") {
            e.has_loop_sec = false;
            audio.ext_sound_clear_loop_points(e.ext_sound_id);
            if (e.channel_id > 0) audio.channel_clear_loop_points(e.channel_id);
            return RValue(1.0);
        }
        if (func_name == "mmMusicGetLeftLevel" || func_name == "mmMusciGetRightLevel" ||
            func_name == "mmMusicGetRightLevel") {
            f32 l = 0.0f, r = 0.0f;
            if (e.channel_id > 0) audio.channel_get_levels(e.channel_id, l, r);
            const f32 val = (func_name == "mmMusicGetLeftLevel") ? l : r;
            return RValue(static_cast<f64>(std::clamp(val, 0.0f, 1.0f) * 32768.0f));
        }
        if (func_name == "mmMusicGetData") {
            const u32 sz = static_cast<u32>(std::clamp(shim_i32(args, 1, 256), 16, 4096));
            if (e.channel_id > 0) {
                audio.channel_get_spectrum_snapshot(e.channel_id, 0, sz, 3, data_buffer_);
                return RValue(1.0);
            }
            data_buffer_.assign(sz, 0.0f);
            return RValue(0.0);
        }
        if (func_name == "mmMusicGetWaveData") {
            const u32 sz = static_cast<u32>(std::clamp(shim_i32(args, 1, 256), 16, 4096));
            if (e.channel_id > 0) {
                audio.channel_get_wave_snapshot(e.channel_id, 0, sz, data_buffer_);
                return RValue(1.0);
            }
            data_buffer_.assign(sz, 0.0f);
            return RValue(0.0);
        }
        if (func_name == "mmMusicSetFX") {
            const DspEffectType t = mm_fx_type_to_dsp(shim_i32(args, 1, -1));
            if (t == DspEffectType::None || e.channel_id <= 0) return RValue(0.0);
            const i32 fx_h = audio.dsp_add_to_channel(e.channel_id, t);
            if (fx_h > 0) {
                e.fx_handles.push_back(fx_h);
                fx_map_[fx_h] = h;
            }
            return RValue(static_cast<f64>(fx_h));
        }
        if (func_name == "mmMusicRemoveFX") {
            const i32 fx_h = shim_i32(args, 1, 0);
            if (e.channel_id > 0) audio.dsp_remove_from_channel(e.channel_id, fx_h);
            fx_map_.erase(fx_h);
            return RValue(1.0);
        }

        return RValue(0.0);
    }

private:
    static f32 compute_effective_rate(const MmMusicEntry& e) noexcept {
        const f32 tempo_mult = std::max(0.05f, 1.0f + e.tempo_pct * 0.01f);
        const f32 pitch_mult = std::pow(2.0f, e.pitch_semi / 12.0f);
        return std::clamp(e.speed_mult * tempo_mult * pitch_mult, 0.05f, 8.0f);
    }

    static void apply_channel_attrs(AudioEngine& audio, const MmMusicEntry& e) {
        audio.channel_set_volume(e.channel_id, e.volume);
        audio.channel_set_pan(e.channel_id, e.pan);
        audio.channel_set_pitch(e.channel_id, compute_effective_rate(e));
        if (e.has_loop_sec) {
            audio.channel_set_loop_points_ms(e.channel_id, e.loop_start_sec * 1000.0, e.loop_end_sec * 1000.0);
        }
    }

    bool initialized_ = false;
    f32 global_volume_ = 1.0f;
    i32 next_handle_ = 1;
    std::unordered_map<i32, MmMusicEntry> streams_;
    std::unordered_map<i32, i32> fx_map_;
    std::vector<f32> data_buffer_;
};

} // namespace

std::unique_ptr<IDllShim> create_maizemusic_shim() {
    return std::make_unique<MaizeMusicShim>();
}

} // namespace gm
