#include "extensions/core/extension.h"

#include "base/log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
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

class MaizeMusicExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "MaizeMusic"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_aliases({"maizemusic", "bass", "bass_fx"});

        // Lifecycle
        reg.bind("mmInit", [this](IExtensionHost& host) {
            initialized_ = true;
            host.ext_audio().group_set_volume(static_cast<i32>(AudioGroupId::MaizeMusic), global_volume_);
            return 1.0;
        });
        reg.bind("mmFree", [this](IExtensionHost& host) {
            on_unload(host);
            initialized_ = false;
            return 1.0;
        });

        // Device, CPU & recording stubs
        reg.bind_const({"mmGetDeviceNumber", "mmSetDevice"}, 1.0);
        reg.bind_const("mmGetDeviceName", "Default Audio Device");
        reg.bind_const({"mmGetCPU", "mmGetMusicCPU"}, 0.5);
        reg.bind_stub({"mmRecordStart", "mmRecordStop", "mmRecordSetInput", "mmRecordGetVolume", "mmLoadMusicUrl"}, 0.0);

        // Global volume
        reg.bind("mmMusicSetGlobalVolume", [this](const CallContext& ctx) {
            global_volume_ = static_cast<f32>(std::clamp(ctx.real(0, 1.0), 0.0, 1.0));
            ctx.host.ext_audio().group_set_volume(static_cast<i32>(AudioGroupId::MaizeMusic), global_volume_);
            return RValue(1.0);
        });
        reg.bind("mmMusicGetGlobalVolume", [this]() {
            return static_cast<f64>(global_volume_);
        });

        // Loading
        reg.bind<&MaizeMusicExtension::load_music>(this, {
            "mmLoadMusic", "mmLoadMusicExt", "mmLoadMusicMem", "mmLoadMusicMemExt"
        });
        reg.bind<&MaizeMusicExtension::load_music_res>(this, "mmLoadMusicRes");

        // Data buffer readback
        reg.bind("mmMusicReadDataPos", [this](const CallContext& ctx) {
            const i32 pos = ctx.i32_arg(0, 0);
            if (pos < 0 || static_cast<usize>(pos) >= data_buffer_.size()) return RValue(0.0);
            return RValue(static_cast<f64>(data_buffer_[static_cast<usize>(pos)]));
        });

        // Effect parameter updates by FX handle
        reg.bind("mmMusicEffectReset", [](const CallContext& ctx) {
            return RValue(ctx.host.ext_audio().dsp_reset(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
        });
        reg.bind("mmMusicEffectChorus", [](const CallContext& ctx) {
            auto& audio = ctx.host.ext_audio();
            const i32 fx_h = ctx.i32_arg(0, 0);
            const f32 wet = static_cast<f32>(std::clamp(ctx.real(1, 50.0) * 0.01, 0.0, 1.0));
            const f32 freq = static_cast<f32>(ctx.real(4, 1.1));
            const f32 delay = static_cast<f32>(ctx.real(6, 16.0));
            audio.dsp_set_param_value(fx_h, 0, 1.0f - wet * 0.5f);
            audio.dsp_set_param_value(fx_h, 1, wet);
            audio.dsp_set_param_value(fx_h, 4, delay);
            audio.dsp_set_param_value(fx_h, 5, freq);
            return RValue(1.0);
        });
        reg.bind("mmMusicEffectEcho", [](const CallContext& ctx) {
            auto& audio = ctx.host.ext_audio();
            const i32 fx_h = ctx.i32_arg(0, 0);
            const f32 wet = static_cast<f32>(std::clamp(ctx.real(1, 50.0) * 0.01, 0.0, 1.0));
            const f32 fb = static_cast<f32>(std::clamp(ctx.real(2, 50.0) * 0.01, 0.0, 0.95));
            const f32 ld = static_cast<f32>(ctx.real(3, 500.0));
            audio.dsp_set_param_value(fx_h, 0, ld);
            audio.dsp_set_param_value(fx_h, 1, fb);
            audio.dsp_set_param_value(fx_h, 3, 1.0f - wet * 0.5f);
            audio.dsp_set_param_value(fx_h, 4, wet);
            return RValue(1.0);
        });
        reg.bind("mmMusicEffectFlanger", [](const CallContext& ctx) {
            auto& audio = ctx.host.ext_audio();
            const i32 fx_h = ctx.i32_arg(0, 0);
            const f32 wet = static_cast<f32>(std::clamp(ctx.real(1, 50.0) * 0.01, 0.0, 1.0));
            const f32 depth = static_cast<f32>(std::clamp(ctx.real(2, 100.0) * 0.01, 0.01, 1.0));
            const f32 freq = static_cast<f32>(ctx.real(4, 0.25));
            audio.dsp_set_param_value(fx_h, 0, 1.0f - wet * 0.5f);
            audio.dsp_set_param_value(fx_h, 1, wet);
            audio.dsp_set_param_value(fx_h, 2, depth);
            audio.dsp_set_param_value(fx_h, 3, freq);
            return RValue(1.0);
        });
        reg.bind("mmMusicEffectGargle", [](const CallContext& ctx) {
            auto& audio = ctx.host.ext_audio();
            const i32 fx_h = ctx.i32_arg(0, 0);
            audio.dsp_set_param_value(fx_h, 0, static_cast<f32>(ctx.real(1, 20.0)));
            audio.dsp_set_param_value(fx_h, 1, static_cast<f32>(ctx.real(2, 0.0)));
            return RValue(1.0);
        });
        reg.bind("mmMusicEffectReverb", [](const CallContext& ctx) {
            auto& audio = ctx.host.ext_audio();
            const i32 fx_h = ctx.i32_arg(0, 0);
            audio.dsp_set_param_value(fx_h, 0, static_cast<f32>(ctx.real(1, 0.0)));
            audio.dsp_set_param_value(fx_h, 1, static_cast<f32>(ctx.real(2, 0.0)));
            audio.dsp_set_param_value(fx_h, 2, static_cast<f32>(ctx.real(3, 1000.0) * 0.001));
            audio.dsp_set_param_value(fx_h, 3, static_cast<f32>(ctx.real(4, 0.5)));
            return RValue(1.0);
        });
        reg.bind("mmMusicEffectCompressor", [](const CallContext& ctx) {
            auto& audio = ctx.host.ext_audio();
            const i32 fx_h = ctx.i32_arg(0, 0);
            audio.dsp_set_param_value(fx_h, 3, static_cast<f32>(ctx.real(1, 0.0)));
            audio.dsp_set_param_value(fx_h, 1, static_cast<f32>(ctx.real(2, 10.0)));
            audio.dsp_set_param_value(fx_h, 2, static_cast<f32>(ctx.real(3, 200.0)));
            audio.dsp_set_param_value(fx_h, 0, static_cast<f32>(ctx.real(4, -20.0)));
            return RValue(1.0);
        });
        reg.bind("mmMusicEffectEqualizer", [](const CallContext& ctx) {
            auto& audio = ctx.host.ext_audio();
            const i32 fx_h = ctx.i32_arg(0, 0);
            audio.dsp_set_param_value(fx_h, 0, static_cast<f32>(ctx.real(1, 8000.0)));
            audio.dsp_set_param_value(fx_h, 1, static_cast<f32>(std::clamp(ctx.real(2, 12.0) / 12.0, 0.2, 5.0)));
            audio.dsp_set_param_value(fx_h, 2, static_cast<f32>(std::pow(10.0, ctx.real(3, 0.0) / 20.0)));
            return RValue(1.0);
        });

        // Stream-handle functions
        reg.bind("mmFreeMusic", [this](const CallContext& ctx) {
            const i32 h = ctx.i32_arg(0, 0);
            auto it = streams_.find(h);
            if (it == streams_.end()) return RValue(0.0);
            auto& audio = ctx.host.ext_audio();
            if (it->second.channel_id > 0) audio.channel_stop(it->second.channel_id);
            audio.ext_free_sound(it->second.ext_sound_id);
            streams_.erase(it);
            return RValue(1.0);
        });

        bind_stream_op(reg, "mmMusicPlay", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            if (e.channel_id > 0) {
                audio.channel_stop(e.channel_id);
                e.channel_id = 0;
            }
            e.channel_id = audio.channel_create(e.ext_sound_id, e.looping, false);
            if (e.channel_id > 0) apply_channel_attrs(audio, e);
            return e.channel_id > 0 ? 1.0 : 0.0;
        });
        bind_stream_op(reg, "mmMusicPause", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                audio.channel_set_paused(e.channel_id, true);
                return 1.0;
            }
            return 0.0;
        });
        bind_stream_op(reg, "mmMusicResume", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                audio.channel_set_paused(e.channel_id, false);
                return 1.0;
            }
            // If stopped, start playback on resume as a safe fallback
            e.channel_id = audio.channel_create(e.ext_sound_id, e.looping, false);
            if (e.channel_id > 0) apply_channel_attrs(audio, e);
            return e.channel_id > 0 ? 1.0 : 0.0;
        });
        bind_stream_op(reg, "mmMusicStop", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            if (e.channel_id > 0) {
                audio.channel_stop(e.channel_id);
                e.channel_id = 0;
            }
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicIsActive", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            // BASS_ACTIVE_STOPPED = 0, BASS_ACTIVE_PLAYING = 1, BASS_ACTIVE_STALLED = 2, BASS_ACTIVE_PAUSED = 3
            if (e.channel_id <= 0 || !audio.channel_is_playing(e.channel_id)) return 0.0;
            return audio.channel_is_paused(e.channel_id) ? 3.0 : 1.0;
        });
        bind_stream_op(reg, "mmMusicSlideVolume", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            const f32 target = static_cast<f32>(std::clamp(ctx.real(1, 1.0), 0.0, 1.0));
            const f32 dur_ms = static_cast<f32>(std::max(0.0, ctx.real(2, 0.0)));
            e.volume = target;
            if (e.channel_id > 0) audio.channel_fade_volume(e.channel_id, target, dur_ms);
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicSetVolume", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            e.volume = static_cast<f32>(std::clamp(ctx.real(1, 1.0), 0.0, 1.0));
            if (e.channel_id > 0) audio.channel_set_volume(e.channel_id, e.volume);
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicGetVolume", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                return static_cast<f64>(audio.channel_get_volume(e.channel_id));
            }
            return static_cast<f64>(e.volume);
        });
        bind_stream_op(reg, "mmMusicSetPan", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            e.pan = static_cast<f32>(std::clamp(ctx.real(1, 0.0), -1.0, 1.0));
            if (e.channel_id > 0) audio.channel_set_pan(e.channel_id, e.pan);
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicGetPan", [](AudioEngine&, i32, MmMusicEntry& e, const CallContext&) {
            return static_cast<f64>(e.pan);
        });
        bind_stream_op(reg, "mmMusicGetLength", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            return audio.ext_sound_length_ms(e.ext_sound_id) * 0.001;
        });
        bind_stream_op(reg, "mmMusicGetPosition", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            if (e.channel_id <= 0 || !audio.channel_is_playing(e.channel_id)) return 0.0;
            return audio.channel_get_position_ms(e.channel_id) * 0.001;
        });
        bind_stream_op(reg, "mmMusicSetPosition", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            const f64 sec = std::max(0.0, ctx.real(1, 0.0));
            if (e.channel_id > 0) audio.channel_set_position_ms(e.channel_id, sec * 1000.0);
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicSetTempo", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            e.tempo_pct = static_cast<f32>(std::clamp(ctx.real(1, 0.0), -95.0, 5000.0));
            if (e.channel_id > 0) audio.channel_set_pitch(e.channel_id, compute_effective_rate(e));
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicGetTempo", [](AudioEngine&, i32, MmMusicEntry& e, const CallContext&) {
            return static_cast<f64>(e.tempo_pct);
        });
        bind_stream_op(reg, "mmMusicSetPitch", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            e.pitch_semi = static_cast<f32>(std::clamp(ctx.real(1, 0.0), -60.0, 60.0));
            if (e.channel_id > 0) audio.channel_set_pitch(e.channel_id, compute_effective_rate(e));
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicGetPitch", [](AudioEngine&, i32, MmMusicEntry& e, const CallContext&) {
            return static_cast<f64>(e.pitch_semi);
        });
        bind_stream_op(reg, "mmMusicSetSpeed", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            const f64 spd = ctx.real(1, 1.0);
            e.speed_mult = static_cast<f32>(spd > 10.0 ? (spd / 44100.0) : std::clamp(spd, 0.05, 8.0));
            if (e.channel_id > 0) audio.channel_set_pitch(e.channel_id, compute_effective_rate(e));
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicGetSpeed", [](AudioEngine&, i32, MmMusicEntry& e, const CallContext&) {
            return static_cast<f64>(e.speed_mult);
        });
        bind_stream_op(reg, "mmMusicSetLoopSection", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            e.has_loop_sec = true;
            e.loop_start_sec = std::max(0.0, ctx.real(1, 0.0));
            e.loop_end_sec = std::max(e.loop_start_sec + 0.001, ctx.real(2, 0.0));
            audio.ext_sound_set_loop_points_ms(e.ext_sound_id, e.loop_start_sec * 1000.0, e.loop_end_sec * 1000.0);
            if (e.channel_id > 0) {
                audio.channel_set_loop_points_ms(e.channel_id, e.loop_start_sec * 1000.0, e.loop_end_sec * 1000.0);
            }
            return 1.0;
        });
        bind_stream_op(reg, "mmMusicRemoveLoopSection", [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext&) {
            e.has_loop_sec = false;
            audio.ext_sound_clear_loop_points(e.ext_sound_id);
            if (e.channel_id > 0) audio.channel_clear_loop_points(e.channel_id);
            return 1.0;
        });
        bind_stream_op(reg, {"mmMusicGetLeftLevel", "mmMusciGetRightLevel", "mmMusicGetRightLevel"},
                       [](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            f32 l = 0.0f, r = 0.0f;
            if (e.channel_id > 0) audio.channel_get_levels(e.channel_id, l, r);
            const f32 val = (ctx.func_name == "mmMusicGetLeftLevel") ? l : r;
            return static_cast<f64>(std::clamp(val, 0.0f, 1.0f) * 32768.0f);
        });
        bind_stream_op(reg, "mmMusicGetData", [this](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            const u32 sz = static_cast<u32>(std::clamp(ctx.i32_arg(1, 256), 16, 4096));
            if (e.channel_id > 0) {
                audio.channel_get_spectrum_snapshot(e.channel_id, 0, sz, 3, data_buffer_);
                return 1.0;
            }
            data_buffer_.assign(sz, 0.0f);
            return 0.0;
        });
        bind_stream_op(reg, "mmMusicGetWaveData", [this](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            const u32 sz = static_cast<u32>(std::clamp(ctx.i32_arg(1, 256), 16, 4096));
            if (e.channel_id > 0) {
                audio.channel_get_wave_snapshot(e.channel_id, 0, sz, data_buffer_);
                return 1.0;
            }
            data_buffer_.assign(sz, 0.0f);
            return 0.0;
        });
        bind_stream_op(reg, "mmMusicSetFX", [this](AudioEngine& audio, i32 h, MmMusicEntry& e, const CallContext& ctx) {
            const DspEffectType t = mm_fx_type_to_dsp(ctx.i32_arg(1, -1));
            if (t == DspEffectType::None || e.channel_id <= 0) return 0.0;
            const i32 fx_h = audio.dsp_add_to_channel(e.channel_id, t);
            if (fx_h > 0) {
                e.fx_handles.push_back(fx_h);
                fx_map_[fx_h] = h;
            }
            return static_cast<f64>(fx_h);
        });
        bind_stream_op(reg, "mmMusicRemoveFX", [this](AudioEngine& audio, i32, MmMusicEntry& e, const CallContext& ctx) {
            const i32 fx_h = ctx.i32_arg(1, 0);
            if (e.channel_id > 0) audio.dsp_remove_from_channel(e.channel_id, fx_h);
            fx_map_.erase(fx_h);
            return 1.0;
        });
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

private:
    template <typename Op>
    void bind_stream_op(ExtensionRegistrar& reg, std::string_view name, Op&& op) {
        reg.bind(name, [this, fn = std::forward<Op>(op)](const CallContext& ctx) -> RValue {
            const i32 h = ctx.i32_arg(0, 0);
            auto it = streams_.find(h);
            if (it == streams_.end()) return RValue(0.0);
            return RValue(fn(ctx.host.ext_audio(), h, it->second, ctx));
        });
    }

    template <typename Op>
    void bind_stream_op(ExtensionRegistrar& reg, std::initializer_list<std::string_view> names, Op&& op) {
        reg.bind(names, [this, fn = std::forward<Op>(op)](const CallContext& ctx) -> RValue {
            const i32 h = ctx.i32_arg(0, 0);
            auto it = streams_.find(h);
            if (it == streams_.end()) return RValue(0.0);
            return RValue(fn(ctx.host.ext_audio(), h, it->second, ctx));
        });
    }

    f64 load_music(const CallContext& ctx) {
        auto& audio = ctx.host.ext_audio();
        const std::string path = ctx.str(0);
        const bool loop = ctx.boolean(1, false);
        const bool is_mem = (ctx.func_name == "mmLoadMusicMem" || ctx.func_name == "mmLoadMusicMemExt");
        const bool is_ext = (ctx.func_name == "mmLoadMusicExt" || ctx.func_name == "mmLoadMusicMemExt");

        auto bytes_res = ctx.host.ext_vfs().read_all_bytes(path);
        if (!bytes_res) {
            GM_LOG_DEBUG("ext", "MaizeMusic %.*s: file not found '%s'",
                         static_cast<int>(ctx.func_name.size()), ctx.func_name.data(), path.c_str());
            return 0.0;
        }

        std::span<const u8> slice = bytes_res.value();
        if (is_ext && ctx.args.size() >= 4) {
            const usize offset = static_cast<usize>(std::max(0.0, ctx.real(2, 0.0)));
            const usize length = static_cast<usize>(std::max(0.0, ctx.real(3, 0.0)));
            if (offset < slice.size()) {
                const usize avail = slice.size() - offset;
                slice = slice.subspan(offset, length > 0 ? std::min(length, avail) : avail);
            }
        }

        const i32 ext_id = audio.ext_load_from_memory(
            slice, false, !is_mem, static_cast<i32>(AudioGroupId::MaizeMusic), path);
        if (ext_id <= 0) return 0.0;

        MmMusicEntry e{};
        e.ext_sound_id = ext_id;
        e.looping = loop;
        const i32 h = next_handle_++;
        streams_[h] = std::move(e);
        return static_cast<f64>(h);
    }

    f64 load_music_res(const CallContext& ctx) {
        auto& audio = ctx.host.ext_audio();
        const i32 gm_snd_id = ctx.i32_arg(0, -1);
        const bool loop = ctx.boolean(1, false);
        const auto& game = ctx.host.ext_game();
        if (gm_snd_id < 0 || static_cast<usize>(gm_snd_id) >= game.sounds.size() ||
            !game.sounds[static_cast<usize>(gm_snd_id)].exists ||
            game.sounds[static_cast<usize>(gm_snd_id)].data.empty()) {
            return 0.0;
        }
        const auto& s = game.sounds[static_cast<usize>(gm_snd_id)];
        const i32 ext_id = audio.ext_load_from_memory(
            s.data, false, false, static_cast<i32>(AudioGroupId::MaizeMusic), s.extension);
        if (ext_id <= 0) return 0.0;
        MmMusicEntry e{};
        e.ext_sound_id = ext_id;
        e.looping = loop;
        const i32 h = next_handle_++;
        streams_[h] = std::move(e);
        return static_cast<f64>(h);
    }

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

std::unique_ptr<IExtension> create_maizemusic_extension() {
    return std::make_unique<MaizeMusicExtension>();
}

} // namespace gm
