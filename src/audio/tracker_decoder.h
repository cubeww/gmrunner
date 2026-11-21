#pragma once

#include "base/types.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gm {

enum class TrackerFormat : u8 {
    Unknown = 0,
    Mod,
    S3m,
    Xm,
    It,
};

struct TrackerSample {
    std::string name;
    std::vector<f32> pcm; // Mono normalized [-1, 1]
    u32 loop_start = 0;
    u32 loop_end = 0;
    bool loop_enabled = false;
    bool ping_pong = false;
    f32 default_volume = 1.0f; // 0..1
    f32 default_pan = 0.0f;    // -1..1
    f64 c4_rate = 8363.0;      // Hz for middle-C (C-4 / Amiga period 428)
    i8 finetune = 0;           // -8..+7 (MOD) or -128..+127 (XM)
    i8 relative_note = 0;      // semitones (XM)
};

struct TrackerCell {
    u8 note = 0;       // 0=none, 1..96=C-0..B-7 (MOD/S3M/XM/IT mapped uniformly), 97=keyoff
    u8 sample = 0;     // 0=none, 1..N=1-based sample index
    u8 vol_cmd = 0;    // 0=none, 1=set volume (0..64)
    u8 vol_val = 0;
    u8 effect = 0;     // unified effect code (MOD 0x0..0xF, plus extended S3M/XM)
    u8 param = 0;
};

struct TrackerPattern {
    u32 rows = 64;
    u32 channels = 4;
    std::vector<TrackerCell> cells; // row * channels + ch
};

struct TrackerModule {
    TrackerFormat format = TrackerFormat::Unknown;
    std::string title;
    u32 num_channels = 4;
    u32 initial_speed = 6;
    u32 initial_bpm = 125;
    f32 global_volume = 1.0f;
    std::vector<f32> initial_pan; // per-channel pan [-1, 1]
    std::vector<u8> orders;
    std::vector<TrackerPattern> patterns;
    std::vector<TrackerSample> samples;
    f64 estimated_duration_ms = 0.0;
};

// Detects whether raw bytes represent a MOD, S3M, XM, or IT tracker module.
[[nodiscard]] TrackerFormat detect_tracker_format(std::span<const u8> data) noexcept;

// Parses MOD / S3M / XM / IT module bytes into a playable TrackerModule.
[[nodiscard]] Result<std::shared_ptr<TrackerModule>> load_tracker_module(std::span<const u8> data);

class TrackerPlayer {
public:
    explicit TrackerPlayer(std::shared_ptr<const TrackerModule> mod, u32 output_sample_rate = 44100);

    void reset() noexcept;
    void seek_ms(f64 ms) noexcept;

    [[nodiscard]] u32 channel_count() const noexcept { return mod_ ? mod_->num_channels : 0; }
    [[nodiscard]] f32 channel_volume(u32 ch) const noexcept;
    void set_channel_volume(u32 ch, f32 vol) noexcept;

    [[nodiscard]] f64 position_ms() const noexcept { return played_frames_ * 1000.0 / static_cast<f64>(sample_rate_); }
    [[nodiscard]] f64 duration_ms() const noexcept { return mod_ ? mod_->estimated_duration_ms : 0.0; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    // Synthesizes stereo interleaved f32 frames. Returns number of frames produced.
    u32 render(f32* out_stereo, u32 frame_count, bool loop) noexcept;

private:
    struct ChannelState {
        i32 sample_idx = -1;
        f64 sample_pos = 0.0;
        f64 sample_step = 0.0;
        bool reverse_dir = false;
        bool playing = false;

        f64 period = 0.0;
        f64 target_period = 0.0;
        f32 volume = 0.0f;
        f32 pan = 0.0f;
        f32 user_volume = 1.0f; // Per-channel FMOD music volume multiplier

        u8 effect = 0;
        u8 param = 0;
        u8 porta_speed = 0;
        u8 vib_speed = 0;
        u8 vib_depth = 0;
        u8 vib_pos = 0;
        u8 vol_slide_mem = 0;
        i32 note = 0;
    };

    void advance_tick(bool loop) noexcept;
    void process_row(bool loop) noexcept;
    void update_channel_step(ChannelState& ch, f64 period_override = -1.0) noexcept;

    std::shared_ptr<const TrackerModule> mod_;
    u32 sample_rate_ = 44100;
    std::vector<ChannelState> channels_;

    u32 order_idx_ = 0;
    u32 row_idx_ = 0;
    u32 tick_ = 0;
    u32 speed_ = 6;
    u32 bpm_ = 125;
    f32 global_vol_ = 1.0f;
    f64 samples_per_tick_ = 882.0;
    f64 tick_sample_accum_ = 0.0;
    i32 pending_jump_order_ = -1;
    i32 pending_break_row_ = -1;
    u64 played_frames_ = 0;
    bool finished_ = false;
};

} // namespace gm
