#include "extensions/core/extension.h"

#include "base/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
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

class SuperSoundExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "SuperSound"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_aliases({"supersound", "ss"});

        reg.bind("SS_Init", [this]() {
            // Note: Many SuperSound GML scripts accidentally bind SS_Unload to "SS_Init".
            // Keep SS_Init idempotent and always return "Yes" (not "No").
            initialized_ = true;
            return std::string("Yes");
        });
        reg.bind("SS_Unload", [this](IExtensionHost& host) {
            on_unload(host);
            initialized_ = false;
            return std::string{};
        });
        reg.bind<&SuperSoundExtension::load_sound>(this, "SS_LoadSound");

        reg.bind("SS_IsSoundPlaying", [this](const CallContext& ctx) {
            auto* e = find_sound(ctx);
            if (!e) return RValue(std::string("-1"));
            auto& audio = ctx.host.ext_audio();
            const bool playing = e->channel_id > 0 &&
                                 audio.channel_is_playing(e->channel_id) &&
                                 !audio.channel_is_paused(e->channel_id);
            return RValue(std::string(playing ? "1" : "0"));
        });
        reg.bind("SS_IsSoundPaused", [this](const CallContext& ctx) {
            auto* e = find_sound(ctx);
            if (!e) return RValue(std::string("-1"));
            auto& audio = ctx.host.ext_audio();
            const bool paused = e->channel_id > 0 &&
                                audio.channel_is_playing(e->channel_id) &&
                                audio.channel_is_paused(e->channel_id);
            return RValue(std::string(paused ? "1" : "0"));
        });
        reg.bind("SS_IsSoundLooping", [this](const CallContext& ctx) {
            auto* e = find_sound(ctx);
            if (!e) return RValue(std::string("-1"));
            auto& audio = ctx.host.ext_audio();
            const bool lp = e->channel_id > 0 &&
                            audio.channel_is_playing(e->channel_id) &&
                            e->looping;
            return RValue(std::string(lp ? "1" : "0"));
        });

        reg.bind("SS_FreeSound", [this](const CallContext& ctx) {
            const i32 handle = parse_handle(ctx);
            auto it = sounds_.find(handle);
            if (it == sounds_.end()) return RValue(std::string("-1"));
            auto& audio = ctx.host.ext_audio();
            if (it->second.channel_id > 0) audio.channel_stop(it->second.channel_id);
            audio.ext_free_sound(it->second.ext_sound_id);
            sounds_.erase(it);
            return RValue(std::string("1"));
        });

        bind_sound_op(reg, {"SS_PlaySound", "SS_LoopSound"}, [](AudioEngine& audio, SsSoundEntry& e, const CallContext& ctx) {
            const bool loop = (ctx.func_name == "SS_LoopSound");
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
            return std::string("0");
        });
        bind_sound_op(reg, "SS_StopSound", [](AudioEngine& audio, SsSoundEntry& e, const CallContext&) {
            if (e.channel_id > 0) {
                audio.channel_stop(e.channel_id);
                e.channel_id = 0;
            }
            e.looping = false;
            e.stopped_cursor = 0;
            return std::string("0");
        });
        bind_sound_op(reg, "SS_PauseSound", [](AudioEngine& audio, SsSoundEntry& e, const CallContext&) {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                audio.channel_set_paused(e.channel_id, true);
                return std::string("1");
            }
            return std::string("0");
        });
        bind_sound_op(reg, "SS_ResumeSound", [](AudioEngine& audio, SsSoundEntry& e, const CallContext&) {
            if (e.channel_id > 0 && audio.channel_is_playing(e.channel_id)) {
                audio.channel_set_paused(e.channel_id, false);
                return std::string("1");
            }
            return std::string("0");
        });
        bind_sound_op(reg, "SS_SetSoundVol", [](AudioEngine& audio, SsSoundEntry& e, const CallContext& ctx) {
            e.ds_vol_mb = std::clamp(ctx.real(1, 0.0), -10000.0, 0.0);
            if (e.channel_id > 0) {
                audio.channel_set_volume(e.channel_id, ds_millibels_to_gain(e.ds_vol_mb));
            }
            return std::string("1");
        });
        bind_sound_op(reg, "SS_GetSoundVol", [](AudioEngine&, SsSoundEntry& e, const CallContext&) {
            return int_to_str(static_cast<i64>(std::llround(e.ds_vol_mb)));
        });
        bind_sound_op(reg, "SS_SetSoundPan", [](AudioEngine& audio, SsSoundEntry& e, const CallContext& ctx) {
            e.ds_pan = std::clamp(ctx.real(1, 0.0), -10000.0, 10000.0);
            if (e.channel_id > 0) {
                audio.channel_set_pan(e.channel_id, static_cast<f32>(e.ds_pan / 10000.0));
            }
            return std::string("1");
        });
        bind_sound_op(reg, "SS_GetSoundPan", [](AudioEngine&, SsSoundEntry& e, const CallContext&) {
            return int_to_str(static_cast<i64>(std::llround(e.ds_pan)));
        });
        bind_sound_op(reg, "SS_SetSoundFreq", [](AudioEngine& audio, SsSoundEntry& e, const CallContext& ctx) {
            const f64 req = ctx.real(1, 0.0);
            const u32 orig = audio.ext_sound_sample_rate(e.ext_sound_id);
            e.freq_hz = (req <= 0.0) ? static_cast<f32>(orig) : static_cast<f32>(std::clamp(req, 100.0, 200000.0));
            if (e.channel_id > 0) {
                audio.channel_set_frequency(e.channel_id, e.freq_hz);
            }
            return std::string("1");
        });
        bind_sound_op(reg, "SS_GetSoundFreq", [](AudioEngine&, SsSoundEntry& e, const CallContext&) {
            return int_to_str(static_cast<i64>(std::llround(e.freq_hz)));
        });
        bind_sound_op(reg, "SS_GetSoundBytesPerSecond", [](AudioEngine&, SsSoundEntry& e, const CallContext&) {
            return int_to_str(static_cast<i64>(e.bytes_per_sec));
        });
        bind_sound_op(reg, "SS_GetSoundLength", [](AudioEngine&, SsSoundEntry& e, const CallContext&) {
            return int_to_str(static_cast<i64>(e.length_bytes));
        });
        bind_sound_op(reg, "SS_GetSoundPosition", [](AudioEngine& audio, SsSoundEntry& e, const CallContext&) {
            const u64 bytes = e.channel_id > 0 ? audio.channel_get_buffer_cursor(e.channel_id, e.length_bytes) : e.stopped_cursor;
            return int_to_str(static_cast<i64>(bytes));
        });
        bind_sound_op(reg, "SS_SetSoundPosition", [](AudioEngine& audio, SsSoundEntry& e, const CallContext& ctx) {
            const f64 requested = ctx.real(1, 0.0);
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
            return std::string("1");
        });
    }

    void on_unload(IExtensionHost& host) override {
        auto& audio = host.ext_audio();
        for (auto& [handle, entry] : sounds_) {
            if (entry.channel_id > 0) audio.channel_stop(entry.channel_id);
            if (entry.ext_sound_id > 0) audio.ext_free_sound(entry.ext_sound_id);
        }
        sounds_.clear();
    }

private:
    static i32 parse_handle(const CallContext& ctx) noexcept {
        if (ctx.args.empty()) return 0;
        return static_cast<i32>(ctx.real(0, 0.0));
    }

    [[nodiscard]] SsSoundEntry* find_sound(const CallContext& ctx) noexcept {
        const auto it = sounds_.find(parse_handle(ctx));
        return it != sounds_.end() ? &it->second : nullptr;
    }

    template <typename Op>
    void bind_sound_op(ExtensionRegistrar& reg, std::string_view name, Op&& op) {
        reg.bind(name, [this, fn = std::forward<Op>(op)](const CallContext& ctx) -> RValue {
            auto* e = find_sound(ctx);
            if (!e) return RValue(std::string("-1"));
            return RValue(fn(ctx.host.ext_audio(), *e, ctx));
        });
    }

    template <typename Op>
    void bind_sound_op(ExtensionRegistrar& reg, std::initializer_list<std::string_view> names, Op&& op) {
        reg.bind(names, [this, fn = std::forward<Op>(op)](const CallContext& ctx) -> RValue {
            auto* e = find_sound(ctx);
            if (!e) return RValue(std::string("-1"));
            return RValue(fn(ctx.host.ext_audio(), *e, ctx));
        });
    }

    std::string load_sound(const CallContext& ctx) {
        auto& audio = ctx.host.ext_audio();
        const std::string path = ctx.str(0);
        const bool streamed = ctx.real(1, 0.0) >= 0.5;
        auto bytes_res = ctx.host.ext_vfs().read_all_bytes(path);
        if (!bytes_res) {
            GM_LOG_WARN("ext", "SS_LoadSound failed to read '%s'", path.c_str());
            return "0";
        }
        const i32 ext_id = audio.ext_load_from_memory(
            bytes_res.value(), false, streamed, static_cast<i32>(AudioGroupId::SuperSound), path);
        if (ext_id <= 0) {
            GM_LOG_WARN("ext", "SS_LoadSound failed to decode '%s'", path.c_str());
            return "0";
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
        return int_to_str(handle);
    }

    bool initialized_ = false;
    i32 next_handle_ = 1;
    std::unordered_map<i32, SsSoundEntry> sounds_;
};

} // namespace

std::unique_ptr<IExtension> create_supersound_extension() {
    return std::make_unique<SuperSoundExtension>();
}

} // namespace gm
