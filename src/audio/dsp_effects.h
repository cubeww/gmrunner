#pragma once

#include "base/types.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

enum class DspEffectType : i32 {
    None = 0,
    Mixer = 1,
    Oscillator = 2,
    Lowpass = 3,
    ItLowpass = 4,
    Highpass = 5,
    Echo = 6,
    Flange = 7,
    Distortion = 8,
    Normalize = 9,
    ParamEq = 10,
    PitchShift = 11,
    Chorus = 12,
    Reverb = 13,
    VstPlugin = 14,
    WinampPlugin = 15,
    ItEcho = 16,
    Compressor = 17,
    SfxReverb = 18,
    LowpassSimple = 19,
    Gargle = 20, // GM8 / DirectSound DX8 gargle (amplitude modulation)
};

struct DspParamDesc {
    std::string_view name;
    std::string_view label;
    std::string_view description;
    f32 min_val = 0.0f;
    f32 max_val = 1.0f;
    f32 default_val = 0.0f;
};

class DspEffect {
public:
    static constexpr usize kMaxParams = 16;

    explicit DspEffect(DspEffectType type = DspEffectType::None, u32 sample_rate = 44100);

    [[nodiscard]] DspEffectType type() const noexcept { return type_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    void set_active(bool active) noexcept { active_ = active; }
    [[nodiscard]] bool bypass() const noexcept { return bypass_; }
    void set_bypass(bool bypass) noexcept { bypass_ = bypass; }

    [[nodiscard]] f32 default_freq() const noexcept { return default_freq_; }
    void set_default_freq(f32 hz) noexcept { default_freq_ = hz; }
    [[nodiscard]] f32 default_vol() const noexcept { return default_vol_; }
    void set_default_vol(f32 vol) noexcept { default_vol_ = vol; }
    [[nodiscard]] f32 default_pan() const noexcept { return default_pan_; }
    void set_default_pan(f32 pan) noexcept { default_pan_ = pan; }

    [[nodiscard]] bool speaker_active(i32 speaker) const noexcept;
    void set_speaker_active(i32 speaker, bool enabled) noexcept;

    [[nodiscard]] i32 param_count() const noexcept;
    [[nodiscard]] DspParamDesc param_desc(i32 idx) const noexcept;
    [[nodiscard]] f32 param_value(i32 idx) const noexcept;
    [[nodiscard]] std::string param_value_str(i32 idx) const;
    bool set_param(i32 idx, f32 val) noexcept;

    void reset() noexcept;

    // Processes interleaved stereo f32 audio in-place.
    void process(f32* stereo, u32 frame_count) noexcept;

private:
    void init_defaults();
    void update_biquad() noexcept;

    DspEffectType type_ = DspEffectType::None;
    u32 sample_rate_ = 44100;
    bool active_ = true;
    bool bypass_ = false;
    f32 default_freq_ = 44100.0f;
    f32 default_vol_ = 1.0f;
    f32 default_pan_ = 0.0f;
    std::array<bool, 16> speakers_{};
    std::array<f32, kMaxParams> params_{};
    i32 param_count_ = 0;

    // Filter state (biquad / 1-pole)
    f32 b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    f32 z1_l_ = 0.0f, z2_l_ = 0.0f, z1_r_ = 0.0f, z2_r_ = 0.0f;

    // Delay / Echo / Chorus / Flanger / Reverb buffers
    std::vector<f32> delay_l_;
    std::vector<f32> delay_r_;
    usize delay_write_ = 0;
    f32 lfo_phase_ = 0.0f;
    f32 env_gain_ = 1.0f;

    // Schroeder / Freeverb comb + allpass state for Reverb / SfxReverb
    struct CombFilter {
        std::vector<f32> buf;
        usize idx = 0;
        f32 store = 0.0f;
    };
    struct AllpassFilter {
        std::vector<f32> buf;
        usize idx = 0;
    };
    std::array<CombFilter, 4> combs_l_{};
    std::array<CombFilter, 4> combs_r_{};
    std::array<AllpassFilter, 2> allpass_l_{};
    std::array<AllpassFilter, 2> allpass_r_{};
};

} // namespace gm
