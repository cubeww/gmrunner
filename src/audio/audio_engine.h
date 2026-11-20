#pragma once

#include "base/types.h"
#include "loader/game_data.h"

#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

namespace gm {

struct DecodedSound {
    bool loaded = false;
    bool exists = false;
    i32 kind = 0;
    u32 sample_rate = 44100;
    std::vector<f32> pcm_stereo; // Interleaved stereo f32 @ 44100 Hz
    f32 volume = 1.0f;
    f32 pan = 0.0f;
};

struct ActiveVoice {
    i32 sound_id = -1;
    f64 frame_pos = 0.0;
    f64 pitch = 1.0;
    f32 volume = 1.0f;
    f32 pan = 0.0f;
    f32 fade_target = 1.0f;
    f32 fade_rate_per_sec = 0.0f;
    bool looping = false;
    bool active = false;
};

class AudioEngine : public NonCopyable {
public:
    static constexpr u32 kOutputSampleRate = 44100;

    AudioEngine();
    ~AudioEngine();

    [[nodiscard]] Result<void> init();
    void shutdown() noexcept;
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    void load_game_sounds(const GameData& game);
    [[nodiscard]] i32 add_sound_from_memory(std::span<const u8> encoded, i32 kind, std::string_view ext_hint = {});
    bool replace_sound_from_memory(i32 sound_id, std::span<const u8> encoded, i32 kind, std::string_view ext_hint = {});
    void delete_sound(i32 sound_id);
    [[nodiscard]] bool sound_exists(i32 sound_id) const noexcept;

    void sound_play(i32 sound_id);
    void sound_loop(i32 sound_id);
    void sound_stop(i32 sound_id);
    void sound_stop_all();
    [[nodiscard]] bool sound_isplaying(i32 sound_id) const;

    void sound_volume(i32 sound_id, f32 vol);
    void sound_pan(i32 sound_id, f32 pan);
    void sound_fade(i32 sound_id, f32 target_vol, f32 duration_ms);
    void sound_global_volume(f32 vol);

    // Called from audio callback thread to fill interleaved stereo f32 frames.
    void mix_frames(f32* out_stereo, u32 frame_count) noexcept;

private:
    static bool decode_audio_to_stereo_f32(std::span<const u8> encoded, std::vector<f32>& out_pcm);
    void play_internal(i32 sound_id, bool loop);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool initialized_ = false;

    mutable std::mutex mutex_;
    std::vector<DecodedSound> sounds_;
    std::vector<ActiveVoice> voices_;
    f32 global_volume_ = 1.0f;
};

} // namespace gm
