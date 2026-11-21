#include "extensions/shim_supersound.h"

#include "base/encoding.h"
#include "base/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace gm {

namespace {

// DirectSound millibel conversion: 0 = max gain (1.0), -10000 = silent (0.0).
f32 ds_millibels_to_gain(f64 mb) noexcept {
    if (mb <= -10000.0) return 0.0f;
    if (mb >= 0.0) return 1.0f;
    return static_cast<f32>(std::pow(10.0, mb / 2000.0));
}

struct SsSoundEntry {
    i32 ext_sound_id = 0;
    i32 channel_id = 0;
    f64 ds_vol_mb = 0.0;       // DirectSound millibels [-10000..0]
    f64 ds_pan = 0.0;          // DirectSound pan [-10000..+10000]
    f32 freq_hz = 44100.0f;
    u32 bytes_per_sec = 0;
    u32 bytes_per_frame = 0;
    u64 length_bytes = 0;
    u64 stopped_cursor = 0;
    bool looping = false;
};

std::string int_to_str(i64 v) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return std::string(buf);
}

class SuperSoundShim final : public IDllShim {
public:
    std::string_view canonical_name() const noexcept override { return "SuperSound"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "supersound" || normalized_dll == "ss";
    }

    bool has_function(std::string_view func_name) const noexcept override {
        return func_name.starts_with("SS_") || func_name.starts_with("ss_");
    }

    void on_unload(IExtensionHost& host) override {
        auto& audio = host.ext_audio();
        for (auto& [handle, entry] : sounds_) {
            if (entry.channel_id > 0) audio.channel_stop(entry.channel_id);
            if (entry.ext_sound_id > 0) audio.ext_free_sound(entry.ext_sound_id);
        }
        sounds_.clear();
    }

    RValue invoke(
        IExtensionHost& host,
        std::string_view func_name,
        std::span<const RValue> args,
        ExternalValueType) override {
        auto& audio = host.ext_audio();

        if (func_name == "SS_Init") {
            // Note: Many SuperSound GML scripts accidentally bind SS_Unload to "SS_Init".
            // Keep SS_Init idempotent and always return "Yes" (not "No").
            initialized_ = true;
            return RValue(std::string("Yes"));
        }
        if (func_name == "SS_Unload") {
            on_unload(host);
            initialized_ = false;
            return RValue(std::string(""));
        }
        if (func_name == "SS_LoadSound") {
            const std::string path = shim_str(args, 0);
            const bool streamed = shim_real(args, 1, 0.0) >= 0.5;
            auto bytes_res = host.ext_vfs().read_all_bytes(path);
            if (!bytes_res) {
                GM_LOG_WARN("ext", "SS_LoadSound failed to read '%s'", path.c_str());
                return RValue(std::string("0"));
            }
            const i32 ext_id = audio.ext_load_from_memory(
                bytes_res.value(), false, streamed, static_cast<i32>(AudioGroupId::SuperSound), path);
            if (ext_id <= 0) {
                GM_LOG_WARN("ext", "SS_LoadSound failed to decode '%s'", path.c_str());
                return RValue(std::string("0"));
            }
            const u32 sr = audio.ext_sound_sample_rate(ext_id);
            const auto& data = bytes_res.value();
            const bool vorbis_stream = streamed && data.size() >= 4 &&
                data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S';

            SsSoundEntry e{};
            e.ext_sound_id = ext_id;
            e.freq_hz = static_cast<f32>(sr > 0 ? sr : AudioEngine::kOutputSampleRate);
            e.bytes_per_frame = audio.ext_sound_bytes_per_frame(ext_id);
            e.bytes_per_sec = sr * e.bytes_per_frame;
            // SS_GetSoundLength returns DirectSound's buffer size. Streamed
            // Vorbis uses a 64 KiB circular buffer independent of track length.
            e.length_bytes = vorbis_stream ? 65536u : audio.ext_sound_length_bytes(ext_id);

            const i32 handle = next_handle_++;
            sounds_[handle] = e;
            return RValue(int_to_str(handle));
        }

        // All remaining SS_* calls take handle string in arg 0.
        const i32 handle = parse_handle(args, 0);
        auto it = sounds_.find(handle);

        if (func_name == "SS_IsSoundPlaying") {
            if (it == sounds_.end()) return RValue(std::string("-1"));
            const bool playing = it->second.channel_id > 0 &&
                                 audio.channel_is_playing(it->second.channel_id) &&
                                 !audio.channel_is_paused(it->second.channel_id);
            return RValue(std::string(playing ? "1" : "0"));
        }
        if (func_name == "SS_IsSoundPaused") {
            if (it == sounds_.end()) return RValue(std::string("-1"));
            const bool paused = it->second.channel_id > 0 &&
                                audio.channel_is_playing(it->second.channel_id) &&
                                audio.channel_is_paused(it->second.channel_id);
            return RValue(std::string(paused ? "1" : "0"));
        }
        if (func_name == "SS_IsSoundLooping") {
            if (it == sounds_.end()) return RValue(std::string("-1"));
            const bool lp = it->second.channel_id > 0 &&
                            audio.channel_is_playing(it->second.channel_id) &&
                            it->second.looping;
            return RValue(std::string(lp ? "1" : "0"));
        }

        if (it == sounds_.end()) {
            return RValue(std::string("-1"));
        }
        auto& e = it->second;

        if (func_name == "SS_FreeSound") {
            if (e.channel_id > 0) audio.channel_stop(e.channel_id);
            audio.ext_free_sound(e.ext_sound_id);
            sounds_.erase(it);
            return RValue(std::string("1"));
        }
        if (func_name == "SS_PlaySound" || func_name == "SS_LoopSound") {
            const bool loop = (func_name == "SS_LoopSound");
            if (e.channel_id > 0) {
                audio.channel_stop(e.channel_id);
                e.channel_id = 0;
            }
            e.looping = loop;
            e.stopped_cursor = 0;
            e.channel_id = audio.channel_create(e.ext_sound_id, loop, false);
            if (e.channel_id > 0) {
                audio.channel_set_volume(e.channel_id, ds_millibels_to_gain(e.ds_vol_mb));
                audio.channel_set_pan(e.channel_id, static_cast<f32>(std::clamp(e.ds_pan / 10000.0, -1.0, 1.0)));
                audio.channel_set_frequency(e.channel_id, e.freq_hz);
            }
            return RValue(std::string("0"));
        }
        if (func_name == "SS_StopSound") {
            if (e.channel_id > 0) {
                audio.channel_stop(e.channel_id);
                e.channel_id = 0;
            }
            e.looping = false;
            e.stopped_cursor = 0;
            return RValue(std::string("0"));
        }
        if (func_name == "SS_PauseSound") {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                audio.channel_set_paused(e.channel_id, true);
                return RValue(std::string("1"));
            }
            return RValue(std::string("0"));
        }
        if (func_name == "SS_ResumeSound") {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                audio.channel_set_paused(e.channel_id, false);
                return RValue(std::string("1"));
            }
            return RValue(std::string("0"));
        }
        if (func_name == "SS_SetSoundVol") {
            e.ds_vol_mb = std::clamp(shim_real(args, 1, 0.0), -10000.0, 0.0);
            if (e.channel_id > 0) {
                audio.channel_set_volume(e.channel_id, ds_millibels_to_gain(e.ds_vol_mb));
            }
            return RValue(std::string("1"));
        }
        if (func_name == "SS_GetSoundVol") {
            return RValue(int_to_str(static_cast<i64>(std::llround(e.ds_vol_mb))));
        }
        if (func_name == "SS_SetSoundPan") {
            e.ds_pan = std::clamp(shim_real(args, 1, 0.0), -10000.0, 10000.0);
            if (e.channel_id > 0) {
                audio.channel_set_pan(e.channel_id, static_cast<f32>(e.ds_pan / 10000.0));
            }
            return RValue(std::string("1"));
        }
        if (func_name == "SS_GetSoundPan") {
            return RValue(int_to_str(static_cast<i64>(std::llround(e.ds_pan))));
        }
        if (func_name == "SS_SetSoundFreq") {
            const f64 req = shim_real(args, 1, 0.0);
            const u32 orig = audio.ext_sound_sample_rate(e.ext_sound_id);
            e.freq_hz = (req <= 0.0) ? static_cast<f32>(orig) : static_cast<f32>(std::clamp(req, 100.0, 200000.0));
            if (e.channel_id > 0) {
                audio.channel_set_frequency(e.channel_id, e.freq_hz);
            }
            return RValue(std::string("1"));
        }
        if (func_name == "SS_GetSoundFreq") {
            return RValue(int_to_str(static_cast<i64>(std::llround(e.freq_hz))));
        }
        if (func_name == "SS_GetSoundBytesPerSecond") {
            return RValue(int_to_str(static_cast<i64>(e.bytes_per_sec)));
        }
        if (func_name == "SS_GetSoundLength") {
            return RValue(int_to_str(static_cast<i64>(e.length_bytes)));
        }
        if (func_name == "SS_GetSoundPosition") {
            const u64 bytes = e.channel_id > 0 ? audio.channel_get_buffer_cursor(e.channel_id, e.length_bytes) : e.stopped_cursor;
            return RValue(int_to_str(static_cast<i64>(bytes)));
        }
        if (func_name == "SS_SetSoundPosition") {
            const f64 requested = shim_real(args, 1, 0.0);
            if (requested >= 0.0 && requested < static_cast<f64>(e.length_bytes) && e.bytes_per_frame > 0) {
                const u64 offset = static_cast<u64>(requested) / e.bytes_per_frame * e.bytes_per_frame;
                if (e.channel_id <= 0 || !audio.channel_is_playing(e.channel_id)) {
                    e.channel_id = 0;
                    e.stopped_cursor = offset;
                } else if (!audio.channel_is_paused(e.channel_id)) {
                    // Pause saves the cursor used by GetSoundPosition and Resume.
                    audio.channel_set_buffer_cursor(e.channel_id, e.length_bytes, offset);
                }
            }
            return RValue(std::string("1"));
        }

        return RValue(std::string("0"));
    }

private:
    static i32 parse_handle(std::span<const RValue> args, usize idx) noexcept {
        if (idx >= args.size()) return 0;
        const f64 v = shim_real(args, idx, 0.0);
        return static_cast<i32>(v);
    }

    bool initialized_ = false;
    i32 next_handle_ = 1;
    std::unordered_map<i32, SsSoundEntry> sounds_;
};

} // namespace

std::unique_ptr<IDllShim> create_supersound_shim() {
    return std::make_unique<SuperSoundShim>();
}

} // namespace gm
