#include "extensions/core/extension.h"

#include "audio/audio_crypto.h"
#include "base/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gm {

namespace {

constexpr u32 kFmodOk = 0;
constexpr u32 kFmodErrFileBad = 18;
constexpr u32 kFmodErrFileNotFound = 23;
constexpr u32 kFmodErrInitialization = 31;
constexpr u32 kFmodErrInvalidHandle = 36;
constexpr u32 kFmodErrInvalidParam = 37;

enum FmodExportFlags : u32 {
    kNone          = 0,
    kPreserveError = 1u << 0, // Do not reset last_error_ before call
    kRequireInit   = 1u << 1, // Require inited_ == true (else kFmodErrInitialization)
    kAutoInit      = 1u << 2, // Tolerantly set inited_ = true if not initialized
    kRequireGroup  = 1u << 3, // Validate is_fmod_group(arg0) (else kFmodErrInvalidParam)
};

DspEffectType fmod_dsp_type_from_id(i32 id) noexcept {
    if (id >= 1 && id <= 19) {
        return static_cast<DspEffectType>(id);
    }
    return DspEffectType::None;
}

class GmFmodExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "GMFMODSimple"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_aliases({"gmfmodsimple", "gmfmodsimplegrix", "gm82snd", "fmodex", "fmod"});
        reg.set_custom_dispatch(true);

        // -------------------------------------------------------------------
        // Error & debug queries (preserve last_error_, do not require init)
        // -------------------------------------------------------------------
        reg.with_flags(kPreserveError, [&](ExportTable& e) {
            e.bind_const("FMOD_Debug_GetLevel", 0.0);
            e.bind("FMODGetLastError", [this]() {
                return static_cast<f64>(last_error_);
            });
            e.bind("FMODGetErrorString", [this](const CallContext& ctx) {
                const i32 err = ctx.i32_arg(0, static_cast<i32>(last_error_));
                return RValue(std::string(err == 0 ? "No errors." : "FMOD error"));
            });
        });

        // -------------------------------------------------------------------
        // Lifecycle & global settings (clear last_error_, no init required)
        // -------------------------------------------------------------------
        reg.bind("FMODinit", [this]() {
            inited_ = true;
            return 1.0;
        });
        reg.bind("FMODfree", [this](IExtensionHost& host) {
            on_unload(host);
            return 1.0;
        });
        reg.bind({"FMODUpdate", "FMODUpdateTakeOverWhileLocked", "FMODUpdateTakeOverDone"}, [this]() {
            return inited_ ? 1.0 : 0.0;
        });
        reg.bind("FMODSetPassword", [this](const CallContext& ctx) {
            password_ = ctx.str(0);
            return RValue(1.0);
        });
        reg.bind_const("FMODSetResampler", 1.0);
        reg.bind("FMODGetNumInstances", [this](IExtensionHost& host) {
            if (!inited_) {
                last_error_ = kFmodErrInitialization;
                return 0.0;
            }
            return static_cast<f64>(host.ext_audio().active_fmod_channel_count());
        });
        reg.bind("FMODEncryptFile", [](const CallContext& ctx) {
            const std::string src = ctx.str(0);
            const std::string dst = ctx.str(1);
            const std::string pw = ctx.str(2);
            auto raw_res = ctx.host.ext_vfs().read_all_bytes(src);
            if (!raw_res) return RValue(0.0);
            auto enc_res = encrypt_fmodsimple(raw_res.value(), pw);
            if (!enc_res) return RValue(0.0);
            auto wr = ctx.host.ext_vfs().write_all_bytes(dst, enc_res.value());
            return RValue(wr ? 1.0 : 0.0);
        });

        // -------------------------------------------------------------------
        // Sound loading (auto-initializes tolerantly if called before FMODinit)
        // -------------------------------------------------------------------
        reg.with_flags(kAutoInit, [&](ExportTable& e) {
            e.bind<&GmFmodExtension::sound_add>(this, {"FMODSoundAdd", "FMODSoundAddAsyncStream"});
        });

        // -------------------------------------------------------------------
        // Operational calls (require initialization)
        // -------------------------------------------------------------------
        reg.with_flags(kRequireInit, [&](ExportTable& e) {
            // Sound asset queries & controls
            e.bind("FMODSoundAsyncReady", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_exists(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceAsyncOK", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_exists(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundFree", [this](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 snd = ctx.i32_arg(0, 0);
                // Other libraries return numeric handles too. They cannot transfer a
                // sound into FMOD's ownership or release it through this DLL.
                if (!audio.fmod_owns_sound(snd)) {
                    last_error_ = kFmodErrInvalidHandle;
                    return RValue(0.0);
                }
                const bool ok = audio.ext_free_sound(snd);
                last_error_ = ok ? kFmodOk : kFmodErrInvalidHandle;
                return RValue(ok ? 1.0 : 0.0);
            });
            e.bind("FMODSoundSetGroup", [this](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 snd = ctx.i32_arg(0, 0);
                if (!audio.fmod_owns_sound(snd)) {
                    last_error_ = kFmodErrInvalidHandle;
                    return RValue(0.0);
                }
                const i32 grp = ctx.i32_arg(1, 0);
                if (grp < 1 || grp > 4) {
                    last_error_ = kFmodErrInvalidParam;
                    return RValue(0.0);
                }
                const bool ok = audio.ext_sound_set_group(snd, grp);
                last_error_ = ok ? kFmodOk : kFmodErrInvalidHandle;
                return RValue(ok ? 1.0 : 0.0);
            });
            e.bind("FMODSoundSetMaxVolume", [](const CallContext& ctx) {
                const f32 vol = static_cast<f32>(std::clamp(ctx.real(1, 1.0), 0.0, 1.0));
                return RValue(ctx.host.ext_audio().ext_sound_set_max_volume(ctx.i32_arg(0, 0), vol) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundGetMaxVolume", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().ext_sound_get_max_volume(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODSoundGetLength", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_length_ms(ctx.i32_arg(0, 0)));
            });
            e.bind("FMODSoundGetNumChannels", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().ext_sound_channels(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODSoundIsStreamed", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_is_streamed(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundIs3d", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_is_3d(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundGetMaxDist", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().ext_sound_get_max_dist(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODSoundSet3dMinMaxDistance", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_set_3d_min_max(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 1.0)),
                    static_cast<f32>(ctx.real(2, 10000.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundSet3dDopplerMax", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_set_3d_doppler(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundSet3dCone", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_set_3d_cone(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 360.0)),
                    static_cast<f32>(ctx.real(2, 360.0)),
                    static_cast<f32>(ctx.real(3, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundSetEffects", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_set_effects_mask(
                    ctx.i32_arg(0, 0), static_cast<u32>(ctx.i32_arg(1, 0))) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundAddEffect", [this](const CallContext& ctx) {
                const i32 snd = ctx.i32_arg(0, 0);
                const DspEffectType t = fmod_dsp_type_from_id(ctx.i32_arg(1, 0));
                const i32 pos = ctx.i32_arg(2, 0);
                if (t == DspEffectType::None || pos < 0 || pos > 9) {
                    last_error_ = kFmodErrInvalidParam;
                    return RValue(0.0);
                }
                return RValue(ctx.host.ext_audio().ext_sound_add_effect(snd, t, pos) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundSetLoopCount", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_set_loop_count(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, -1)) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundGetLoopCount", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().ext_sound_get_loop_count(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODSoundSetLoopPoints", [this](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 snd = ctx.i32_arg(0, 0);
                const f64 s = ctx.real(1, 0.0);
                const f64 end = ctx.real(2, 1.0);
                if (s < 0.0 || s > 1.0 || end <= s || end > 1.0) {
                    last_error_ = kFmodErrInvalidParam;
                    return RValue(0.0);
                }
                const f64 len_ms = audio.ext_sound_length_ms(snd);
                return RValue(audio.ext_sound_set_loop_points_ms(snd, s * len_ms, end * len_ms) ? 1.0 : 0.0);
            });
            e.bind("FMODSoundGetMusicNumChannels", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().ext_sound_get_music_num_channels(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODSoundGetMusicChannelVolume", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().ext_sound_get_music_channel_volume(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0))));
            });
            e.bind("FMODSoundSetMusicChannelVolume", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().ext_sound_set_music_channel_volume(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0), static_cast<f32>(ctx.real(2, 1.0))) ? 1.0 : 0.0);
            });

            // Playback / channel instantiation
            e.bind("FMODSoundInstanciate", [this](const CallContext& ctx) {
                const i32 ch = ctx.host.ext_audio().channel_create(ctx.i32_arg(0, 0), false, true);
                last_error_ = ch > 0 ? kFmodOk : kFmodErrInvalidHandle;
                return RValue(static_cast<f64>(ch));
            });
            e.bind({"FMODSoundPlay", "FMODSoundLoop"}, [this](const CallContext& ctx) {
                const bool loop = (ctx.func_name == "FMODSoundLoop");
                const i32 ch = ctx.host.ext_audio().channel_create(ctx.i32_arg(0, 0), loop, ctx.boolean(1, false));
                last_error_ = ch > 0 ? kFmodOk : kFmodErrInvalidHandle;
                return RValue(static_cast<f64>(ch));
            });
            e.bind({"FMODSoundPlay3d", "FMODSoundLoop3d"}, [this](const CallContext& ctx) {
                const bool loop = (ctx.func_name == "FMODSoundLoop3d");
                const i32 ch = ctx.host.ext_audio().channel_create(
                    ctx.i32_arg(0, 0), loop, ctx.boolean(4, false), true,
                    static_cast<f32>(ctx.real(1, 0.0)),
                    static_cast<f32>(ctx.real(2, 0.0)),
                    static_cast<f32>(ctx.real(3, 0.0)));
                last_error_ = ch > 0 ? kFmodOk : kFmodErrInvalidHandle;
                return RValue(static_cast<f64>(ch));
            });

            // Instance controls
            e.bind("FMODInstanceStop", [this](const CallContext& ctx) {
                const i32 ch = ctx.i32_arg(0, 0);
                tag_cursor_.erase(ch);
                return RValue(ctx.host.ext_audio().channel_stop(ch) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceIsPlaying", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_is_playing(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetSound", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_sound(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODInstanceSoundGetLength", [](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 snd = audio.channel_get_sound(ctx.i32_arg(0, 0));
                return RValue(snd > 0 ? audio.ext_sound_length_ms(snd) : 0.0);
            });
            e.bind("FMODInstanceSetVolume", [](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 ch = ctx.i32_arg(0, 0);
                const i32 snd = audio.channel_get_sound(ch);
                const f32 max_v = snd > 0 ? audio.ext_sound_get_max_volume(snd) : 1.0f;
                const f32 vol = static_cast<f32>(std::clamp(ctx.real(1, 1.0), 0.0, 1.0)) * max_v;
                return RValue(audio.channel_set_volume(ch, vol) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetVolume", [](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 ch = ctx.i32_arg(0, 0);
                const i32 snd = audio.channel_get_sound(ch);
                const f32 max_v = snd > 0 ? audio.ext_sound_get_max_volume(snd) : 1.0f;
                const f32 v = audio.channel_get_volume(ch);
                return RValue(static_cast<f64>(max_v > 1e-6f ? (v / max_v) : 0.0f));
            });
            e.bind("FMODInstanceSetFrequency", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_frequency(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 44100.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetFrequency", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_frequency(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODInstanceSetPitch", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_pitch(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetPitch", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_pitch(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODInstanceSetMuted", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_muted(
                    ctx.i32_arg(0, 0), ctx.boolean(1, false)) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetMuted", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_get_muted(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceSetPaused", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_paused(
                    ctx.i32_arg(0, 0), ctx.boolean(1, false)) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetPaused", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_is_paused(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetPosition", [](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 ch = ctx.i32_arg(0, 0);
                const i32 snd = audio.channel_get_sound(ch);
                if (snd <= 0) return RValue(0.0);
                const f64 len_ms = std::max(audio.ext_sound_length_ms(snd), 1.0);
                return RValue(std::clamp(audio.channel_get_position_ms(ch) / len_ms, 0.0, 1.0));
            });
            e.bind("FMODInstanceSetPosition", [this](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 ch = ctx.i32_arg(0, 0);
                const f64 p = ctx.real(1, 0.0);
                if (p < 0.0 || p > 1.0) {
                    last_error_ = kFmodErrInvalidParam;
                    return RValue(0.0);
                }
                const i32 snd = audio.channel_get_sound(ch);
                if (snd <= 0) return RValue(0.0);
                const f64 len_ms = audio.ext_sound_length_ms(snd);
                return RValue(audio.channel_set_position_ms(ch, p * len_ms) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceSetPan", [this](const CallContext& ctx) {
                const f64 p = ctx.real(1, 0.0);
                if (p < -1.0 || p > 1.0) {
                    last_error_ = kFmodErrInvalidParam;
                    return RValue(0.0);
                }
                return RValue(ctx.host.ext_audio().channel_set_pan(ctx.i32_arg(0, 0), static_cast<f32>(p)) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetPan", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_pan(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODInstanceSetLoopCount", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_loop_count(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, -1)) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetLoopCount", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_loop_count(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODInstanceSetLoopPoints", [this](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 ch = ctx.i32_arg(0, 0);
                const f64 s = ctx.real(1, 0.0);
                const f64 end = ctx.real(2, 1.0);
                if (s < 0.0 || s > 1.0 || end <= s || end > 1.0) {
                    last_error_ = kFmodErrInvalidParam;
                    return RValue(0.0);
                }
                const i32 snd = audio.channel_get_sound(ch);
                if (snd <= 0) return RValue(0.0);
                const f64 len_ms = audio.ext_sound_length_ms(snd);
                return RValue(audio.channel_set_loop_points_ms(ch, s * len_ms, end * len_ms) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetAudibility", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_audibility(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODInstanceSetSpeakerMix", [](const CallContext& ctx) {
                std::array<f32, 8> mix{};
                for (usize i = 0; i < 8; ++i) {
                    mix[i] = static_cast<f32>(std::clamp(ctx.real(i + 1, 1.0), 0.0, 5.0));
                }
                return RValue(ctx.host.ext_audio().channel_set_speaker_mix(ctx.i32_arg(0, 0), mix) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceSet3dPosition", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_3d_position(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 0.0)),
                    static_cast<f32>(ctx.real(2, 0.0)),
                    static_cast<f32>(ctx.real(3, 0.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceSet3dMinMaxDistance", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_3d_min_max(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 1.0)),
                    static_cast<f32>(ctx.real(2, 10000.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGetMaxDist", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_max_dist(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODInstanceSet3dDopplerMax", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_3d_doppler(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceSet3dCone", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_3d_cone(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 360.0)),
                    static_cast<f32>(ctx.real(2, 360.0)),
                    static_cast<f32>(ctx.real(3, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceSet3dConeOrientation", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_3d_cone_orientation(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 0.0)),
                    static_cast<f32>(ctx.real(2, 0.0)),
                    static_cast<f32>(ctx.real(3, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceSet3DPanLevel", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_3d_pan_level(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGet3DPanLevel", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_3d_pan_level(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODInstanceSet3DSpread", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().channel_set_3d_spread(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 0.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODInstanceGet3DSpread", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().channel_get_3d_spread(ctx.i32_arg(0, 0))));
            });

            // Master controls
            e.bind("FMODAllStop", [](IExtensionHost& host) {
                host.ext_audio().stop_group_channels(0);
                return 1.0;
            });
            e.bind("FMODAllSetMuted", [](const CallContext& ctx) {
                ctx.host.ext_audio().group_set_muted(0, ctx.boolean(0, false));
                return RValue(1.0);
            });
            e.bind("FMODAllSetPaused", [](const CallContext& ctx) {
                ctx.host.ext_audio().group_set_paused(0, ctx.boolean(0, false));
                return RValue(1.0);
            });
            e.bind("FMODMasterSetVolume", [](const CallContext& ctx) {
                ctx.host.ext_audio().group_set_volume(0, static_cast<f32>(std::clamp(ctx.real(0, 1.0), 0.0, 1.0)));
                return RValue(1.0);
            });

            // DSP effects (non-group)
            e.bind("FMODInstanceAddEffect", [](const CallContext& ctx) {
                const DspEffectType t = fmod_dsp_type_from_id(ctx.i32_arg(1, 0));
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_add_to_channel(ctx.i32_arg(0, 0), t)));
            });
            e.bind("FMODEffectFree", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_free(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectGetActive", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_get_active(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectSetActive", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_set_active(ctx.i32_arg(0, 0), ctx.boolean(1, true)) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectGetBypass", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_get_bypass(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectSetBypass", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_set_bypass(ctx.i32_arg(0, 0), ctx.boolean(1, false)) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectGetDefaultFr", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_get_default_freq(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODEffectSetDefaultFr", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_set_default_freq(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 44100.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectGetDefaultVol", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_get_default_vol(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODEffectSetDefaultVol", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_set_default_vol(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectGetDefaultPan", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_get_default_pan(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODEffectSetDefaultPan", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_set_default_pan(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 0.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectGetSpeakerActive", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_get_speaker_active(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectSetSpeakerActive", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_set_speaker_active(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0), ctx.boolean(2, true)) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectGetNumParams", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_get_num_params(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODEffectGetParamValue", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_get_param_value(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0))));
            });
            e.bind("FMODEffectSetParamValue", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_set_param_value(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0), static_cast<f32>(ctx.real(2, 0.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODEffectGetParamMin", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_get_param_desc(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0)).min_val));
            });
            e.bind("FMODEffectGetParamMax", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_get_param_desc(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0)).max_val));
            });
            e.bind_const({"FMODEffectGetParamValueStr", "FMODEffectGetParamName",
                          "FMODEffectGetParamLabel", "FMODEffectGetParamDesc"}, 1.0);
            e.bind("FMODEffectReset", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().dsp_reset(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });

            // 3D Listeners & Blockers
            e.bind("FMODListenerSetNumber", [](const CallContext& ctx) {
                ctx.host.ext_audio().listener_set_number(ctx.i32_arg(0, 1));
                return RValue(1.0);
            });
            e.bind("FMODListenerSet3dPosition", [](const CallContext& ctx) {
                ctx.host.ext_audio().listener_set_3d_pos(
                    ctx.i32_arg(0, 1),
                    static_cast<f32>(ctx.real(1, 0.0)),
                    static_cast<f32>(ctx.real(2, 0.0)),
                    static_cast<f32>(ctx.real(3, 0.0)));
                return RValue(1.0);
            });
            e.bind("FMODListenerSet3dPositionEx", [](const CallContext& ctx) {
                ctx.host.ext_audio().listener_set_3d_pos(
                    ctx.i32_arg(0, 1),
                    static_cast<f32>(ctx.real(1, 0.0)),
                    static_cast<f32>(ctx.real(2, 0.0)),
                    static_cast<f32>(ctx.real(3, 0.0)),
                    static_cast<f32>(ctx.real(4, 0.0)),
                    static_cast<f32>(ctx.real(5, 0.0)),
                    static_cast<f32>(ctx.real(6, 1.0)),
                    static_cast<f32>(ctx.real(7, 0.0)),
                    static_cast<f32>(ctx.real(8, 1.0)),
                    static_cast<f32>(ctx.real(9, 0.0)),
                    true);
                return RValue(1.0);
            });
            e.bind("FMODListenerHearsDistanceOnly", [](const CallContext& ctx) {
                ctx.host.ext_audio().listener_set_distance_only(ctx.i32_arg(0, 1), ctx.boolean(1, false));
                return RValue(1.0);
            });
            e.bind("FMODSetWorldScale", [](const CallContext& ctx) {
                ctx.host.ext_audio().set_world_scale(static_cast<f32>(ctx.real(0, 1.0)));
                return RValue(1.0);
            });
            e.bind("FMODSetDopplerFPS", [](const CallContext& ctx) {
                ctx.host.ext_audio().set_doppler_fps(static_cast<f32>(ctx.real(0, 0.0)));
                return RValue(1.0);
            });
            e.bind("FMODBlockersInit", [](const CallContext& ctx) {
                ctx.host.ext_audio().blockers_init(ctx.i32_arg(0, 64));
                return RValue(1.0);
            });
            e.bind("FMODBlockersFree", [](IExtensionHost& host) {
                host.ext_audio().blockers_free();
                return 1.0;
            });
            e.bind("FMODBlockerAdd", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().blocker_add(
                    static_cast<f32>(ctx.real(0, 0.0)),
                    static_cast<f32>(ctx.real(1, 0.0)),
                    static_cast<f32>(ctx.real(2, 0.0)),
                    static_cast<f32>(ctx.real(3, 1.0)),
                    static_cast<f32>(ctx.real(4, 1.0)),
                    static_cast<f32>(ctx.real(5, 1.0)))));
            });
            e.bind("FMODBlockerSet3dPosition", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().blocker_set_pos(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 0.0)),
                    static_cast<f32>(ctx.real(2, 0.0)),
                    static_cast<f32>(ctx.real(3, 0.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODBlockerSet3dOrientation", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().blocker_set_orient(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 0.0)),
                    static_cast<f32>(ctx.real(2, 0.0)),
                    static_cast<f32>(ctx.real(3, 1.0)),
                    static_cast<f32>(ctx.real(4, 0.0)),
                    static_cast<f32>(ctx.real(5, 1.0)),
                    static_cast<f32>(ctx.real(6, 0.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODBlockerSet3dScale", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().blocker_set_scale(
                    ctx.i32_arg(0, 0),
                    static_cast<f32>(ctx.real(1, 1.0)),
                    static_cast<f32>(ctx.real(2, 1.0)),
                    static_cast<f32>(ctx.real(3, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODBlockerSetEnabled", [](const CallContext& ctx) {
                // Official LoadFMOD.gml has a known typo binding FMODBlockerGetEnabled (1 arg) to FMODBlockerSetEnabled.
                if (ctx.args.size() == 1) {
                    return RValue(ctx.host.ext_audio().blocker_get_enabled(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
                }
                return RValue(ctx.host.ext_audio().blocker_set_enabled(
                    ctx.i32_arg(0, 0), ctx.boolean(1, true)) ? 1.0 : 0.0);
            });
            e.bind("FMODBlockerGetEnabled", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().blocker_get_enabled(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODBlockerSetStrength", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().blocker_set_strength(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODBlockerGetStrength", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().blocker_get_strength(ctx.i32_arg(0, 0))));
            });

            // Snapshots (Wave & FFT Spectrum)
            e.bind("FMODSpectrumSetSnapshotType", [this](const CallContext& ctx) {
                spectrum_window_ = std::clamp(ctx.i32_arg(0, 0), 0, 5);
                return RValue(1.0);
            });
            e.bind({"FMODInstanceGetWaveSnapshot", "FMODInstanceGetWaveSnapshot2"}, [this](const CallContext& ctx) {
                const u32 sz = static_cast<u32>(std::clamp(ctx.i32_arg(2, 256), 1, 8192));
                return RValue(ctx.host.ext_audio().channel_get_wave_snapshot(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0), sz, snapshot_buf_) ? 1.0 : 0.0);
            });
            e.bind({"FMODInstanceGetSpectrumSnapshot", "FMODInstanceGetSpectrumSnapshot2"}, [this](const CallContext& ctx) {
                const u32 sz = static_cast<u32>(std::clamp(ctx.i32_arg(2, 256), 1, 8192));
                return RValue(ctx.host.ext_audio().channel_get_spectrum_snapshot(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0), sz, spectrum_window_, snapshot_buf_) ? 1.0 : 0.0);
            });
            e.bind({"FMODNormalizeSpectrumData", "FMODNormalizeWaveData"}, [this](const CallContext& ctx) {
                const usize start = static_cast<usize>(std::max(0, ctx.i32_arg(0, 0)));
                const usize count = static_cast<usize>(std::max(0, ctx.i32_arg(1, 0)));
                if (start >= snapshot_buf_.size() || count == 0) return RValue(0.0);
                const usize end = std::min(snapshot_buf_.size(), start + count);
                f32 max_v = 0.0f;
                for (usize i = start; i < end; ++i) max_v = std::max(max_v, std::fabs(snapshot_buf_[i]));
                if (max_v > 1e-6f) {
                    for (usize i = start; i < end; ++i) snapshot_buf_[i] /= max_v;
                }
                return RValue(static_cast<f64>(max_v));
            });
            e.bind("FMODGetSnapshotEntry", [this](const CallContext& ctx) {
                const i32 idx = ctx.i32_arg(0, 0);
                if (idx < 0 || static_cast<usize>(idx) >= snapshot_buf_.size()) return RValue(0.0);
                return RValue(static_cast<f64>(snapshot_buf_[static_cast<usize>(idx)]));
            });
            e.bind("FMODSnapShotToDsList", [this](const CallContext& ctx) {
                const usize start = static_cast<usize>(std::max(0, ctx.i32_arg(0, 0)));
                const usize count = static_cast<usize>(std::max(0, ctx.i32_arg(1, 0)));
                const i32 list_id = ctx.i32_arg(2, -1);
                const usize end = std::min(snapshot_buf_.size(), start + count);
                for (usize i = start; i < end; ++i) {
                    ctx.host.ext_ds_list_add_real(list_id, static_cast<f64>(snapshot_buf_[i]));
                }
                return RValue(1.0);
            });
            e.bind_const({"FMODGetWaveBuffer", "FMODGetSpectrumBuffer"}, 1.0);

            // Tags & Microphone stubs
            e.bind("FMODInstanceGetNextTag", [this](const CallContext& ctx) {
                auto& audio = ctx.host.ext_audio();
                const i32 ch = ctx.i32_arg(0, 0);
                const i32 snd = audio.channel_get_sound(ch);
                if (snd <= 0) return RValue(0.0);
                const auto tags = audio.ext_sound_get_tags(snd);
                usize& cur = tag_cursor_[ch];
                if (cur >= tags.size()) {
                    cur = 0;
                    return RValue(0.0);
                }
                last_tag_name_ = tags[cur].name;
                last_tag_data_ = tags[cur].value;
                ++cur;
                return RValue(1.0);
            });
            e.bind_const({"FMODGetTagName", "FMODGetTagData", "FMODGetTagPicture"}, 1.0);
            e.bind_stub({"FMODCreateSoundFromMicInput", "FMODRecordStart", "FMODRecordStop"}, 0.0);
        });

        // -------------------------------------------------------------------
        // Group controls (require init AND valid FMOD group id 1..4 in arg 0)
        // -------------------------------------------------------------------
        reg.with_flags(kRequireInit | kRequireGroup, [&](ExportTable& e) {
            e.bind("FMODGroupStop", [](const CallContext& ctx) {
                ctx.host.ext_audio().stop_group_channels(ctx.i32_arg(0, 0));
                return RValue(1.0);
            });
            e.bind("FMODGroupSetVolume", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().group_set_volume(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODGroupGetVolume", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().group_get_volume(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODGroupSetPitch", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().group_set_pitch(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 1.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODGroupGetPitch", [](const CallContext& ctx) {
                return RValue(static_cast<f64>(ctx.host.ext_audio().group_get_pitch(ctx.i32_arg(0, 0))));
            });
            e.bind("FMODGroupSetFrequency", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().group_set_frequency(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 44100.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODGroupSetPan", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().group_set_pan(
                    ctx.i32_arg(0, 0), static_cast<f32>(ctx.real(1, 0.0))) ? 1.0 : 0.0);
            });
            e.bind("FMODGroupSetPaused", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().group_set_paused(
                    ctx.i32_arg(0, 0), ctx.boolean(1, false)) ? 1.0 : 0.0);
            });
            e.bind("FMODGroupGetPaused", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().group_get_paused(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODGroupSetMuted", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().group_set_muted(
                    ctx.i32_arg(0, 0), ctx.boolean(1, false)) ? 1.0 : 0.0);
            });
            e.bind("FMODGroupGetMuted", [](const CallContext& ctx) {
                return RValue(ctx.host.ext_audio().group_get_muted(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
            });
            e.bind("FMODGroupAddEffect", [](const CallContext& ctx) {
                const DspEffectType t = fmod_dsp_type_from_id(ctx.i32_arg(1, 0));
                return RValue(static_cast<f64>(ctx.host.ext_audio().dsp_add_to_group(ctx.i32_arg(0, 0), t)));
            });
            e.bind({"FMODGroupGetWaveSnapshot", "FMODGroupGetWaveSnapshot2"}, [this](const CallContext& ctx) {
                const u32 sz = static_cast<u32>(std::clamp(ctx.i32_arg(2, 256), 1, 8192));
                return RValue(ctx.host.ext_audio().group_get_wave_snapshot(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0), sz, snapshot_buf_) ? 1.0 : 0.0);
            });
            e.bind({"FMODGroupGetSpectrumSnapshot", "FMODGroupGetSpectrumSnapshot2"}, [this](const CallContext& ctx) {
                const u32 sz = static_cast<u32>(std::clamp(ctx.i32_arg(2, 256), 1, 8192));
                return RValue(ctx.host.ext_audio().group_get_spectrum_snapshot(
                    ctx.i32_arg(0, 0), ctx.i32_arg(1, 0), sz, spectrum_window_, snapshot_buf_) ? 1.0 : 0.0);
            });
        });
    }

    RValue dispatch(const ExportedFunction& fn, const CallContext& ctx) override {
        if ((fn.flags & kPreserveError) != 0) {
            return fn.invoke(fn.instance, ctx);
        }
        // Clear last_error_ before every operational call so gm82snd's
        // __gm82snd_call() wrapper never observes a stale error code.
        last_error_ = kFmodOk;

        if ((fn.flags & kAutoInit) != 0) {
            inited_ = true;
        } else if ((fn.flags & kRequireInit) != 0 && !inited_) {
            last_error_ = kFmodErrInitialization;
            return RValue(0.0);
        }

        if ((fn.flags & kRequireGroup) != 0 && !is_fmod_group(ctx.i32_arg(0, 0))) {
            last_error_ = kFmodErrInvalidParam;
            return RValue(0.0);
        }

        return fn.invoke(fn.instance, ctx);
    }

    void on_unload(IExtensionHost& host) override {
        host.ext_audio().clear_fmod_audio();
        inited_ = false;
        password_.clear();
        snapshot_buf_.clear();
        tag_cursor_.clear();
        last_tag_name_.clear();
        last_tag_data_.clear();
        last_error_ = kFmodOk;
    }

private:
    RValue sound_add(const CallContext& ctx) {
        auto& audio = ctx.host.ext_audio();
        const std::string path = ctx.str(0);
        const bool is_3d = ctx.boolean(1, false);
        // gm82snd passes mode=2 for FMOD_CREATECOMPRESSEDSAMPLE (in-memory); only mode=1 is streamed.
        const bool streamed = (ctx.func_name == "FMODSoundAddAsyncStream") ? true : (ctx.i32_arg(2, 0) == 1);

        auto raw_res = ctx.host.ext_vfs().read_all_bytes(path);
        if (!raw_res) {
            last_error_ = kFmodErrFileNotFound;
            GM_LOG_DEBUG("ext", "FMODSoundAdd: file not found '%s'", path.c_str());
            return RValue(0.0);
        }

        std::vector<u8> audio_bytes;
        if (is_fmodsimple_encrypted(raw_res.value())) {
            auto dec = decrypt_fmodsimple(raw_res.value(), password_);
            if (!dec) {
                last_error_ = kFmodErrFileBad;
                GM_LOG_WARN("ext", "FMODSoundAdd: decryption failed for '%s'", path.c_str());
                return RValue(0.0);
            }
            audio_bytes = std::move(dec.value());
        } else {
            audio_bytes = std::move(raw_res.value());
        }

        const i32 snd_id = audio.ext_load_from_memory(audio_bytes, is_3d, streamed, 0, path);
        if (snd_id <= 0) {
            last_error_ = kFmodErrFileBad;
            GM_LOG_WARN("ext", "FMODSoundAdd: decode failed for '%s'", path.c_str());
            return RValue(0.0);
        }
        last_error_ = kFmodOk;
        return RValue(static_cast<f64>(snd_id));
    }

    bool inited_ = false;
    u32 last_error_ = kFmodOk;
    std::string password_;
    i32 spectrum_window_ = 0;
    std::vector<f32> snapshot_buf_;
    std::unordered_map<i32, usize> tag_cursor_;
    std::string last_tag_name_;
    std::string last_tag_data_;
};

} // namespace

std::unique_ptr<IExtension> create_gmfmod_extension() {
    return std::make_unique<GmFmodExtension>();
}

} // namespace gm
