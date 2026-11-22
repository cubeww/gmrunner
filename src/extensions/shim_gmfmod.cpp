#include "extensions/shim_gmfmod.h"

#include "audio/audio_crypto.h"
#include "base/encoding.h"
#include "base/log.h"

#include <algorithm>
#include <array>
#include <cmath>
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

DspEffectType fmod_dsp_type_from_id(i32 id) noexcept {
    if (id >= 1 && id <= 19) {
        return static_cast<DspEffectType>(id);
    }
    return DspEffectType::None;
}

class GmFmodShim final : public IDllShim {
public:
    std::string_view canonical_name() const noexcept override { return "GMFMODSimple"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "gmfmodsimple" ||
               normalized_dll == "gmfmodsimplegrix" ||
               normalized_dll == "gm82snd" ||
               normalized_dll == "fmodex" ||
               normalized_dll == "fmod";
    }

    bool has_function(std::string_view func_name) const noexcept override {
        return func_name.starts_with("FMOD");
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

    RValue invoke(
        IExtensionHost& host,
        std::string_view fn,
        std::span<const RValue> args,
        ExternalValueType) override {
        auto& audio = host.ext_audio();

        // -------------------------------------------------------------------
        // System lifecycle, errors & password
        // -------------------------------------------------------------------
        if (fn == "FMOD_Debug_GetLevel") {
            return RValue(0.0);
        }
        if (fn == "FMODGetLastError") {
            return RValue(static_cast<f64>(last_error_));
        }
        if (fn == "FMODGetErrorString") {
            const i32 err = shim_i32(args, 0, static_cast<i32>(last_error_));
            return RValue(std::string(err == 0 ? "No errors." : "FMOD error"));
        }

        // Clear last_error_ before every operational call so gm82snd's
        // __gm82snd_call() wrapper never observes a stale error code.
        last_error_ = kFmodOk;

        if (fn == "FMODinit") {
            inited_ = true;
            return RValue(1.0);
        }
        if (fn == "FMODfree") {
            on_unload(host);
            return RValue(1.0);
        }
        if (fn == "FMODUpdate" || fn == "FMODUpdateTakeOverWhileLocked" || fn == "FMODUpdateTakeOverDone") {
            return RValue(inited_ ? 1.0 : 0.0);
        }
        if (fn == "FMODSetPassword") {
            password_ = shim_str(args, 0);
            return RValue(1.0);
        }
        if (fn == "FMODSetResampler") {
            return RValue(1.0);
        }
        if (fn == "FMODGetNumInstances") {
            if (!inited_) { last_error_ = kFmodErrInitialization; return RValue(0.0); }
            return RValue(static_cast<f64>(audio.active_fmod_channel_count()));
        }
        if (fn == "FMODEncryptFile") {
            const std::string src = shim_str(args, 0);
            const std::string dst = shim_str(args, 1);
            const std::string pw = shim_str(args, 2);
            auto raw_res = host.ext_vfs().read_all_bytes(src);
            if (!raw_res) return RValue(0.0);
            auto enc_res = encrypt_fmodsimple(raw_res.value(), pw);
            if (!enc_res) return RValue(0.0);
            auto wr = host.ext_vfs().write_all_bytes(dst, enc_res.value());
            return RValue(wr ? 1.0 : 0.0);
        }

        if (!inited_) {
            // Auto-initialize tolerantly if a game calls FMOD before FMODinit, or flag error
            if (fn != "FMODSoundAdd" && fn != "FMODSoundAddAsyncStream") {
                last_error_ = kFmodErrInitialization;
                return RValue(0.0);
            }
            inited_ = true;
        }

        // -------------------------------------------------------------------
        // Sound loading & asset properties
        // -------------------------------------------------------------------
        if (fn == "FMODSoundAdd" || fn == "FMODSoundAddAsyncStream") {
            const std::string path = shim_str(args, 0);
            const bool is_3d = shim_bool(args, 1, false);
            // gm82snd passes mode=2 for FMOD_CREATECOMPRESSEDSAMPLE (in-memory); only mode=1 is streamed.
            const bool streamed = (fn == "FMODSoundAddAsyncStream") ? true : (shim_i32(args, 2, 0) == 1);

            auto raw_res = host.ext_vfs().read_all_bytes(path);
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
        // Other libraries return numeric handles too. They cannot transfer a
        // sound into FMOD's ownership or release it through this DLL.
        if ((fn == "FMODSoundFree" || fn == "FMODSoundSetGroup") && !audio.fmod_owns_sound(shim_i32(args, 0))) {
            last_error_ = kFmodErrInvalidHandle;
            return RValue(0.0);
        }
        if (fn == "FMODSoundAsyncReady") {
            const i32 snd = shim_i32(args, 0, 0);
            return RValue(audio.ext_sound_exists(snd) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceAsyncOK") {
            const i32 ch = shim_i32(args, 0, 0);
            return RValue(audio.channel_exists(ch) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundFree") {
            const i32 snd = shim_i32(args, 0, 0);
            const bool ok = audio.ext_free_sound(snd);
            last_error_ = ok ? kFmodOk : kFmodErrInvalidHandle;
            return RValue(ok ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundSetGroup") {
            const i32 snd = shim_i32(args, 0, 0);
            const i32 grp = shim_i32(args, 1, 0);
            if (grp < 1 || grp > 4) { last_error_ = kFmodErrInvalidParam; return RValue(0.0); }
            const bool ok = audio.ext_sound_set_group(snd, grp);
            last_error_ = ok ? kFmodOk : kFmodErrInvalidHandle;
            return RValue(ok ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundSetMaxVolume") {
            const i32 snd = shim_i32(args, 0, 0);
            const f32 vol = static_cast<f32>(std::clamp(shim_real(args, 1, 1.0), 0.0, 1.0));
            return RValue(audio.ext_sound_set_max_volume(snd, vol) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundGetMaxVolume") {
            return RValue(static_cast<f64>(audio.ext_sound_get_max_volume(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODSoundGetLength") {
            return RValue(audio.ext_sound_length_ms(shim_i32(args, 0, 0)));
        }
        if (fn == "FMODSoundGetNumChannels") {
            return RValue(static_cast<f64>(audio.ext_sound_channels(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODSoundIsStreamed") {
            return RValue(audio.ext_sound_is_streamed(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundIs3d") {
            return RValue(audio.ext_sound_is_3d(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundGetMaxDist") {
            return RValue(static_cast<f64>(audio.ext_sound_get_max_dist(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODSoundSet3dMinMaxDistance") {
            return RValue(audio.ext_sound_set_3d_min_max(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 1.0)),
                static_cast<f32>(shim_real(args, 2, 10000.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundSet3dDopplerMax") {
            return RValue(audio.ext_sound_set_3d_doppler(
                shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundSet3dCone") {
            return RValue(audio.ext_sound_set_3d_cone(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 360.0)),
                static_cast<f32>(shim_real(args, 2, 360.0)),
                static_cast<f32>(shim_real(args, 3, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundSetEffects") {
            return RValue(audio.ext_sound_set_effects_mask(
                shim_i32(args, 0, 0), static_cast<u32>(shim_i32(args, 1, 0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundAddEffect") {
            const i32 snd = shim_i32(args, 0, 0);
            const DspEffectType t = fmod_dsp_type_from_id(shim_i32(args, 1, 0));
            const i32 pos = shim_i32(args, 2, 0);
            if (t == DspEffectType::None || pos < 0 || pos > 9) {
                last_error_ = kFmodErrInvalidParam;
                return RValue(0.0);
            }
            return RValue(audio.ext_sound_add_effect(snd, t, pos) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundSetLoopCount") {
            return RValue(audio.ext_sound_set_loop_count(shim_i32(args, 0, 0), shim_i32(args, 1, -1)) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundGetLoopCount") {
            return RValue(static_cast<f64>(audio.ext_sound_get_loop_count(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODSoundSetLoopPoints") {
            const i32 snd = shim_i32(args, 0, 0);
            const f64 s = shim_real(args, 1, 0.0);
            const f64 e = shim_real(args, 2, 1.0);
            if (s < 0.0 || s > 1.0 || e <= s || e > 1.0) { last_error_ = kFmodErrInvalidParam; return RValue(0.0); }
            const f64 len_ms = audio.ext_sound_length_ms(snd);
            return RValue(audio.ext_sound_set_loop_points_ms(snd, s * len_ms, e * len_ms) ? 1.0 : 0.0);
        }
        if (fn == "FMODSoundGetMusicNumChannels") {
            return RValue(static_cast<f64>(audio.ext_sound_get_music_num_channels(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODSoundGetMusicChannelVolume") {
            return RValue(static_cast<f64>(audio.ext_sound_get_music_channel_volume(
                shim_i32(args, 0, 0), shim_i32(args, 1, 0))));
        }
        if (fn == "FMODSoundSetMusicChannelVolume") {
            return RValue(audio.ext_sound_set_music_channel_volume(
                shim_i32(args, 0, 0), shim_i32(args, 1, 0), static_cast<f32>(shim_real(args, 2, 1.0))) ? 1.0 : 0.0);
        }

        // -------------------------------------------------------------------
        // Playback / channel instantiation
        // -------------------------------------------------------------------
        if (fn == "FMODSoundInstanciate") {
            const i32 snd = shim_i32(args, 0, 0);
            const i32 ch = audio.channel_create(snd, false, true);
            last_error_ = ch > 0 ? kFmodOk : kFmodErrInvalidHandle;
            return RValue(static_cast<f64>(ch));
        }
        if (fn == "FMODSoundPlay" || fn == "FMODSoundLoop") {
            const i32 snd = shim_i32(args, 0, 0);
            const bool paused = shim_bool(args, 1, false);
            const bool loop = (fn == "FMODSoundLoop");
            const i32 ch = audio.channel_create(snd, loop, paused);
            last_error_ = ch > 0 ? kFmodOk : kFmodErrInvalidHandle;
            return RValue(static_cast<f64>(ch));
        }
        if (fn == "FMODSoundPlay3d" || fn == "FMODSoundLoop3d") {
            const i32 snd = shim_i32(args, 0, 0);
            const f32 x = static_cast<f32>(shim_real(args, 1, 0.0));
            const f32 y = static_cast<f32>(shim_real(args, 2, 0.0));
            const f32 z = static_cast<f32>(shim_real(args, 3, 0.0));
            const bool paused = shim_bool(args, 4, false);
            const bool loop = (fn == "FMODSoundLoop3d");
            const i32 ch = audio.channel_create(snd, loop, paused, true, x, y, z);
            last_error_ = ch > 0 ? kFmodOk : kFmodErrInvalidHandle;
            return RValue(static_cast<f64>(ch));
        }

        // -------------------------------------------------------------------
        // Instance controls
        // -------------------------------------------------------------------
        if (fn == "FMODInstanceStop") {
            const i32 ch = shim_i32(args, 0, 0);
            tag_cursor_.erase(ch);
            return RValue(audio.channel_stop(ch) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceIsPlaying") {
            return RValue(audio.channel_is_playing(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetSound") {
            return RValue(static_cast<f64>(audio.channel_get_sound(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODInstanceSoundGetLength") {
            const i32 snd = audio.channel_get_sound(shim_i32(args, 0, 0));
            return RValue(snd > 0 ? audio.ext_sound_length_ms(snd) : 0.0);
        }
        if (fn == "FMODInstanceSetVolume") {
            const i32 ch = shim_i32(args, 0, 0);
            const i32 snd = audio.channel_get_sound(ch);
            const f32 max_v = snd > 0 ? audio.ext_sound_get_max_volume(snd) : 1.0f;
            const f32 vol = static_cast<f32>(std::clamp(shim_real(args, 1, 1.0), 0.0, 1.0)) * max_v;
            return RValue(audio.channel_set_volume(ch, vol) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetVolume") {
            const i32 ch = shim_i32(args, 0, 0);
            const i32 snd = audio.channel_get_sound(ch);
            const f32 max_v = snd > 0 ? audio.ext_sound_get_max_volume(snd) : 1.0f;
            const f32 v = audio.channel_get_volume(ch);
            return RValue(static_cast<f64>(max_v > 1e-6f ? (v / max_v) : 0.0f));
        }
        if (fn == "FMODInstanceSetFrequency") {
            return RValue(audio.channel_set_frequency(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 44100.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetFrequency") {
            return RValue(static_cast<f64>(audio.channel_get_frequency(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODInstanceSetPitch") {
            return RValue(audio.channel_set_pitch(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetPitch") {
            return RValue(static_cast<f64>(audio.channel_get_pitch(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODInstanceSetMuted") {
            return RValue(audio.channel_set_muted(shim_i32(args, 0, 0), shim_bool(args, 1, false)) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetMuted") {
            return RValue(audio.channel_get_muted(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceSetPaused") {
            return RValue(audio.channel_set_paused(shim_i32(args, 0, 0), shim_bool(args, 1, false)) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetPaused") {
            return RValue(audio.channel_is_paused(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetPosition") {
            const i32 ch = shim_i32(args, 0, 0);
            const i32 snd = audio.channel_get_sound(ch);
            if (snd <= 0) return RValue(0.0);
            const f64 len_ms = std::max(audio.ext_sound_length_ms(snd), 1.0);
            return RValue(std::clamp(audio.channel_get_position_ms(ch) / len_ms, 0.0, 1.0));
        }
        if (fn == "FMODInstanceSetPosition") {
            const i32 ch = shim_i32(args, 0, 0);
            const f64 p = shim_real(args, 1, 0.0);
            if (p < 0.0 || p > 1.0) { last_error_ = kFmodErrInvalidParam; return RValue(0.0); }
            const i32 snd = audio.channel_get_sound(ch);
            if (snd <= 0) return RValue(0.0);
            const f64 len_ms = audio.ext_sound_length_ms(snd);
            return RValue(audio.channel_set_position_ms(ch, p * len_ms) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceSetPan") {
            const f64 p = shim_real(args, 1, 0.0);
            if (p < -1.0 || p > 1.0) { last_error_ = kFmodErrInvalidParam; return RValue(0.0); }
            return RValue(audio.channel_set_pan(shim_i32(args, 0, 0), static_cast<f32>(p)) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetPan") {
            return RValue(static_cast<f64>(audio.channel_get_pan(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODInstanceSetLoopCount") {
            return RValue(audio.channel_set_loop_count(shim_i32(args, 0, 0), shim_i32(args, 1, -1)) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetLoopCount") {
            return RValue(static_cast<f64>(audio.channel_get_loop_count(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODInstanceSetLoopPoints") {
            const i32 ch = shim_i32(args, 0, 0);
            const f64 s = shim_real(args, 1, 0.0);
            const f64 e = shim_real(args, 2, 1.0);
            if (s < 0.0 || s > 1.0 || e <= s || e > 1.0) { last_error_ = kFmodErrInvalidParam; return RValue(0.0); }
            const i32 snd = audio.channel_get_sound(ch);
            if (snd <= 0) return RValue(0.0);
            const f64 len_ms = audio.ext_sound_length_ms(snd);
            return RValue(audio.channel_set_loop_points_ms(ch, s * len_ms, e * len_ms) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetAudibility") {
            return RValue(static_cast<f64>(audio.channel_get_audibility(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODInstanceSetSpeakerMix") {
            std::array<f32, 8> mix{};
            for (usize i = 0; i < 8; ++i) {
                mix[i] = static_cast<f32>(std::clamp(shim_real(args, i + 1, 1.0), 0.0, 5.0));
            }
            return RValue(audio.channel_set_speaker_mix(shim_i32(args, 0, 0), mix) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceSet3dPosition") {
            return RValue(audio.channel_set_3d_position(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 0.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceSet3dMinMaxDistance") {
            return RValue(audio.channel_set_3d_min_max(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 1.0)),
                static_cast<f32>(shim_real(args, 2, 10000.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetMaxDist") {
            return RValue(static_cast<f64>(audio.channel_get_max_dist(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODInstanceSet3dDopplerMax") {
            return RValue(audio.channel_set_3d_doppler(
                shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceSet3dCone") {
            return RValue(audio.channel_set_3d_cone(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 360.0)),
                static_cast<f32>(shim_real(args, 2, 360.0)),
                static_cast<f32>(shim_real(args, 3, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceSet3dConeOrientation") {
            return RValue(audio.channel_set_3d_cone_orientation(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceSet3DPanLevel") {
            return RValue(audio.channel_set_3d_pan_level(
                shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGet3DPanLevel") {
            return RValue(static_cast<f64>(audio.channel_get_3d_pan_level(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODInstanceSet3DSpread") {
            return RValue(audio.channel_set_3d_spread(
                shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 0.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGet3DSpread") {
            return RValue(static_cast<f64>(audio.channel_get_3d_spread(shim_i32(args, 0, 0))));
        }

        // -------------------------------------------------------------------
        // Groups & Master controls
        // -------------------------------------------------------------------
        if (fn == "FMODAllStop") {
            audio.stop_group_channels(0);
            return RValue(1.0);
        }
        if (fn == "FMODAllSetMuted") {
            audio.group_set_muted(0, shim_bool(args, 0, false));
            return RValue(1.0);
        }
        if (fn == "FMODAllSetPaused") {
            audio.group_set_paused(0, shim_bool(args, 0, false));
            return RValue(1.0);
        }
        if (fn == "FMODMasterSetVolume") {
            audio.group_set_volume(0, static_cast<f32>(std::clamp(shim_real(args, 0, 1.0), 0.0, 1.0)));
            return RValue(1.0);
        }
        if (fn.starts_with("FMODGroup") && !is_fmod_group(shim_i32(args, 0, 0))) {
            last_error_ = kFmodErrInvalidParam;
            return RValue(0.0);
        }
        if (fn == "FMODGroupStop") {
            audio.stop_group_channels(shim_i32(args, 0, 0));
            return RValue(1.0);
        }
        if (fn == "FMODGroupSetVolume") {
            return RValue(audio.group_set_volume(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupGetVolume") {
            return RValue(static_cast<f64>(audio.group_get_volume(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODGroupSetPitch") {
            return RValue(audio.group_set_pitch(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupGetPitch") {
            return RValue(static_cast<f64>(audio.group_get_pitch(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODGroupSetFrequency") {
            return RValue(audio.group_set_frequency(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 44100.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupSetPan") {
            return RValue(audio.group_set_pan(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 0.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupSetPaused") {
            return RValue(audio.group_set_paused(shim_i32(args, 0, 0), shim_bool(args, 1, false)) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupGetPaused") {
            return RValue(audio.group_get_paused(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupSetMuted") {
            return RValue(audio.group_set_muted(shim_i32(args, 0, 0), shim_bool(args, 1, false)) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupGetMuted") {
            return RValue(audio.group_get_muted(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }

        // -------------------------------------------------------------------
        // DSP effects
        // -------------------------------------------------------------------
        if (fn == "FMODInstanceAddEffect") {
            const DspEffectType t = fmod_dsp_type_from_id(shim_i32(args, 1, 0));
            return RValue(static_cast<f64>(audio.dsp_add_to_channel(shim_i32(args, 0, 0), t)));
        }
        if (fn == "FMODGroupAddEffect") {
            const DspEffectType t = fmod_dsp_type_from_id(shim_i32(args, 1, 0));
            return RValue(static_cast<f64>(audio.dsp_add_to_group(shim_i32(args, 0, 0), t)));
        }
        if (fn == "FMODEffectFree") {
            return RValue(audio.dsp_free(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectGetActive") {
            return RValue(audio.dsp_get_active(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectSetActive") {
            return RValue(audio.dsp_set_active(shim_i32(args, 0, 0), shim_bool(args, 1, true)) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectGetBypass") {
            return RValue(audio.dsp_get_bypass(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectSetBypass") {
            return RValue(audio.dsp_set_bypass(shim_i32(args, 0, 0), shim_bool(args, 1, false)) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectGetDefaultFr") {
            return RValue(static_cast<f64>(audio.dsp_get_default_freq(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODEffectSetDefaultFr") {
            return RValue(audio.dsp_set_default_freq(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 44100.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectGetDefaultVol") {
            return RValue(static_cast<f64>(audio.dsp_get_default_vol(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODEffectSetDefaultVol") {
            return RValue(audio.dsp_set_default_vol(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectGetDefaultPan") {
            return RValue(static_cast<f64>(audio.dsp_get_default_pan(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODEffectSetDefaultPan") {
            return RValue(audio.dsp_set_default_pan(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 0.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectGetSpeakerActive") {
            return RValue(audio.dsp_get_speaker_active(shim_i32(args, 0, 0), shim_i32(args, 1, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectSetSpeakerActive") {
            return RValue(audio.dsp_set_speaker_active(shim_i32(args, 0, 0), shim_i32(args, 1, 0), shim_bool(args, 2, true)) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectGetNumParams") {
            return RValue(static_cast<f64>(audio.dsp_get_num_params(shim_i32(args, 0, 0))));
        }
        if (fn == "FMODEffectGetParamValue") {
            return RValue(static_cast<f64>(audio.dsp_get_param_value(shim_i32(args, 0, 0), shim_i32(args, 1, 0))));
        }
        if (fn == "FMODEffectSetParamValue") {
            return RValue(audio.dsp_set_param_value(
                shim_i32(args, 0, 0), shim_i32(args, 1, 0), static_cast<f32>(shim_real(args, 2, 0.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODEffectGetParamMin") {
            return RValue(static_cast<f64>(audio.dsp_get_param_desc(shim_i32(args, 0, 0), shim_i32(args, 1, 0)).min_val));
        }
        if (fn == "FMODEffectGetParamMax") {
            return RValue(static_cast<f64>(audio.dsp_get_param_desc(shim_i32(args, 0, 0), shim_i32(args, 1, 0)).max_val));
        }
        if (fn == "FMODEffectGetParamValueStr" || fn == "FMODEffectGetParamName" ||
            fn == "FMODEffectGetParamLabel" || fn == "FMODEffectGetParamDesc") {
            return RValue(1.0);
        }
        if (fn == "FMODEffectReset") {
            return RValue(audio.dsp_reset(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }

        // -------------------------------------------------------------------
        // 3D Listeners & Blockers
        // -------------------------------------------------------------------
        if (fn == "FMODListenerSetNumber") {
            audio.listener_set_number(shim_i32(args, 0, 1));
            return RValue(1.0);
        }
        if (fn == "FMODListenerSet3dPosition") {
            audio.listener_set_3d_pos(
                shim_i32(args, 0, 1),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 0.0)));
            return RValue(1.0);
        }
        if (fn == "FMODListenerSet3dPositionEx") {
            audio.listener_set_3d_pos(
                shim_i32(args, 0, 1),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 0.0)),
                static_cast<f32>(shim_real(args, 4, 0.0)),
                static_cast<f32>(shim_real(args, 5, 0.0)),
                static_cast<f32>(shim_real(args, 6, 1.0)),
                static_cast<f32>(shim_real(args, 7, 0.0)),
                static_cast<f32>(shim_real(args, 8, 1.0)),
                static_cast<f32>(shim_real(args, 9, 0.0)),
                true);
            return RValue(1.0);
        }
        if (fn == "FMODListenerHearsDistanceOnly") {
            audio.listener_set_distance_only(shim_i32(args, 0, 1), shim_bool(args, 1, false));
            return RValue(1.0);
        }
        if (fn == "FMODSetWorldScale") {
            audio.set_world_scale(static_cast<f32>(shim_real(args, 0, 1.0)));
            return RValue(1.0);
        }
        if (fn == "FMODSetDopplerFPS") {
            audio.set_doppler_fps(static_cast<f32>(shim_real(args, 0, 0.0)));
            return RValue(1.0);
        }
        if (fn == "FMODBlockersInit") {
            audio.blockers_init(shim_i32(args, 0, 64));
            return RValue(1.0);
        }
        if (fn == "FMODBlockersFree") {
            audio.blockers_free();
            return RValue(1.0);
        }
        if (fn == "FMODBlockerAdd") {
            const i32 bid = audio.blocker_add(
                static_cast<f32>(shim_real(args, 0, 0.0)),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 1.0)),
                static_cast<f32>(shim_real(args, 4, 1.0)),
                static_cast<f32>(shim_real(args, 5, 1.0)));
            return RValue(static_cast<f64>(bid));
        }
        if (fn == "FMODBlockerSet3dPosition") {
            return RValue(audio.blocker_set_pos(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 0.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODBlockerSet3dOrientation") {
            return RValue(audio.blocker_set_orient(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 1.0)),
                static_cast<f32>(shim_real(args, 4, 0.0)),
                static_cast<f32>(shim_real(args, 5, 1.0)),
                static_cast<f32>(shim_real(args, 6, 0.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODBlockerSet3dScale") {
            return RValue(audio.blocker_set_scale(
                shim_i32(args, 0, 0),
                static_cast<f32>(shim_real(args, 1, 1.0)),
                static_cast<f32>(shim_real(args, 2, 1.0)),
                static_cast<f32>(shim_real(args, 3, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODBlockerSetEnabled") {
            // Official LoadFMOD.gml has a known typo binding FMODBlockerGetEnabled (1 arg) to FMODBlockerSetEnabled.
            if (args.size() == 1) {
                return RValue(audio.blocker_get_enabled(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
            }
            return RValue(audio.blocker_set_enabled(shim_i32(args, 0, 0), shim_bool(args, 1, true)) ? 1.0 : 0.0);
        }
        if (fn == "FMODBlockerGetEnabled") {
            return RValue(audio.blocker_get_enabled(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "FMODBlockerSetStrength") {
            return RValue(audio.blocker_set_strength(shim_i32(args, 0, 0), static_cast<f32>(shim_real(args, 1, 1.0))) ? 1.0 : 0.0);
        }
        if (fn == "FMODBlockerGetStrength") {
            return RValue(static_cast<f64>(audio.blocker_get_strength(shim_i32(args, 0, 0))));
        }

        // -------------------------------------------------------------------
        // Snapshots (Wave & FFT Spectrum)
        // -------------------------------------------------------------------
        if (fn == "FMODSpectrumSetSnapshotType") {
            spectrum_window_ = std::clamp(shim_i32(args, 0, 0), 0, 5);
            return RValue(1.0);
        }
        if (fn == "FMODInstanceGetWaveSnapshot" || fn == "FMODInstanceGetWaveSnapshot2") {
            const i32 ch = shim_i32(args, 0, 0);
            const i32 lr = shim_i32(args, 1, 0);
            const u32 sz = static_cast<u32>(std::clamp(shim_i32(args, 2, 256), 1, 8192));
            return RValue(audio.channel_get_wave_snapshot(ch, lr, sz, snapshot_buf_) ? 1.0 : 0.0);
        }
        if (fn == "FMODInstanceGetSpectrumSnapshot" || fn == "FMODInstanceGetSpectrumSnapshot2") {
            const i32 ch = shim_i32(args, 0, 0);
            const i32 lr = shim_i32(args, 1, 0);
            const u32 sz = static_cast<u32>(std::clamp(shim_i32(args, 2, 256), 1, 8192));
            return RValue(audio.channel_get_spectrum_snapshot(ch, lr, sz, spectrum_window_, snapshot_buf_) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupGetWaveSnapshot" || fn == "FMODGroupGetWaveSnapshot2") {
            const i32 grp = shim_i32(args, 0, 0);
            const i32 lr = shim_i32(args, 1, 0);
            const u32 sz = static_cast<u32>(std::clamp(shim_i32(args, 2, 256), 1, 8192));
            return RValue(audio.group_get_wave_snapshot(grp, lr, sz, snapshot_buf_) ? 1.0 : 0.0);
        }
        if (fn == "FMODGroupGetSpectrumSnapshot" || fn == "FMODGroupGetSpectrumSnapshot2") {
            const i32 grp = shim_i32(args, 0, 0);
            const i32 lr = shim_i32(args, 1, 0);
            const u32 sz = static_cast<u32>(std::clamp(shim_i32(args, 2, 256), 1, 8192));
            return RValue(audio.group_get_spectrum_snapshot(grp, lr, sz, spectrum_window_, snapshot_buf_) ? 1.0 : 0.0);
        }
        if (fn == "FMODNormalizeSpectrumData" || fn == "FMODNormalizeWaveData") {
            const usize start = static_cast<usize>(std::max(0, shim_i32(args, 0, 0)));
            const usize count = static_cast<usize>(std::max(0, shim_i32(args, 1, 0)));
            if (start >= snapshot_buf_.size() || count == 0) return RValue(0.0);
            const usize end = std::min(snapshot_buf_.size(), start + count);
            f32 max_v = 0.0f;
            for (usize i = start; i < end; ++i) max_v = std::max(max_v, std::fabs(snapshot_buf_[i]));
            if (max_v > 1e-6f) {
                for (usize i = start; i < end; ++i) snapshot_buf_[i] /= max_v;
            }
            return RValue(static_cast<f64>(max_v));
        }
        if (fn == "FMODGetSnapshotEntry") {
            const i32 idx = shim_i32(args, 0, 0);
            if (idx < 0 || static_cast<usize>(idx) >= snapshot_buf_.size()) return RValue(0.0);
            return RValue(static_cast<f64>(snapshot_buf_[static_cast<usize>(idx)]));
        }
        if (fn == "FMODSnapShotToDsList") {
            const usize start = static_cast<usize>(std::max(0, shim_i32(args, 0, 0)));
            const usize count = static_cast<usize>(std::max(0, shim_i32(args, 1, 0)));
            const i32 list_id = shim_i32(args, 2, -1);
            const usize end = std::min(snapshot_buf_.size(), start + count);
            for (usize i = start; i < end; ++i) {
                host.ext_ds_list_add_real(list_id, static_cast<f64>(snapshot_buf_[i]));
            }
            return RValue(1.0);
        }
        if (fn == "FMODGetWaveBuffer" || fn == "FMODGetSpectrumBuffer") {
            return RValue(1.0);
        }

        // -------------------------------------------------------------------
        // Tags & Microphone stubs
        // -------------------------------------------------------------------
        if (fn == "FMODInstanceGetNextTag") {
            const i32 ch = shim_i32(args, 0, 0);
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
        }
        if (fn == "FMODGetTagName" || fn == "FMODGetTagData" || fn == "FMODGetTagPicture") {
            return RValue(1.0);
        }
        if (fn == "FMODCreateSoundFromMicInput" || fn == "FMODRecordStart" || fn == "FMODRecordStop") {
            return RValue(0.0);
        }

        return RValue(0.0);
    }

private:
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

std::unique_ptr<IDllShim> create_gmfmod_shim() {
    return std::make_unique<GmFmodShim>();
}

} // namespace gm
