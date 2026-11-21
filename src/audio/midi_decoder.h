#pragma once

#include "base/types.h"

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

struct tsf;
struct tml_message;

namespace gm {

struct MidiSequence {
    std::vector<u8> raw_bytes;
    tml_message* messages = nullptr;
    f64 duration_ms = 0.0;

    MidiSequence() = default;
    ~MidiSequence();
    MidiSequence(const MidiSequence&) = delete;
    MidiSequence& operator=(const MidiSequence&) = delete;
};

[[nodiscard]] bool is_midi_data(std::span<const u8> data) noexcept;
[[nodiscard]] Result<std::shared_ptr<MidiSequence>> load_midi_sequence(std::span<const u8> data);

class MidiPlayer {
public:
    MidiPlayer(std::shared_ptr<const MidiSequence> seq, const tsf* shared_soundfont, u32 output_sample_rate = 44100);
    ~MidiPlayer();

    void reset() noexcept;
    void seek_ms(f64 ms) noexcept;
    void set_tempo_scale(f64 scale) noexcept { tempo_scale_ = scale > 0.05 ? scale : 0.05; }
    [[nodiscard]] f64 position_ms() const noexcept { return current_ms_; }
    [[nodiscard]] f64 duration_ms() const noexcept { return seq_ ? seq_->duration_ms : 0.0; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    // Synthesizes stereo interleaved f32 frames. Returns number of frames produced.
    u32 render(f32* out_stereo, u32 frame_count, bool loop) noexcept;

private:
    struct FallbackVoice {
        bool active = false;
        bool releasing = false;
        u8 channel = 0;
        u8 key = 0;
        f32 phase = 0.0f;
        f32 freq_hz = 440.0f;
        f32 amp = 0.0f;
        f32 env = 0.0f;
        bool is_drum = false;
        u32 noise_state = 0x12345678u;
    };

    void dispatch_event(const tml_message& msg) noexcept;
    void render_fallback(f32* out_stereo, u32 frame_count) noexcept;

    std::shared_ptr<const MidiSequence> seq_;
    tsf* synth_ = nullptr; // Cloned TinySoundFont instance if SF2 is available
    u32 sample_rate_ = 44100;
    const tml_message* cursor_ = nullptr;
    f64 current_ms_ = 0.0;
    f64 tempo_scale_ = 1.0;
    bool finished_ = false;

    // Built-in GM fallback synth state when no SF2 SoundFont is present
    std::array<f32, 16> ch_vol_{};
    std::array<f32, 16> ch_pan_{};
    std::array<u8, 16> ch_prog_{};
    std::array<FallbackVoice, 32> voices_{};
};

} // namespace gm
