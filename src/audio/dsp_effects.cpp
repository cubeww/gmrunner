#include "audio/dsp_effects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gm {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 6.28318530717958647692f;

constexpr std::array<DspParamDesc, 2> kOscParams = {{
    {"Type", "", "Waveform type (0=sine,1=square,2=sawup,3=sawdown,4=tri,5=noise)", 0.0f, 5.0f, 0.0f},
    {"Rate", "Hz", "Frequency in Hz", 1.0f, 22000.0f, 220.0f},
}};
constexpr std::array<DspParamDesc, 2> kLowpassParams = {{
    {"Cutoff", "Hz", "Lowpass cutoff frequency", 10.0f, 22000.0f, 5000.0f},
    {"Resonance", "", "Lowpass resonance Q", 1.0f, 10.0f, 1.0f},
}};
constexpr std::array<DspParamDesc, 2> kHighpassParams = {{
    {"Cutoff", "Hz", "Highpass cutoff frequency", 10.0f, 22000.0f, 5000.0f},
    {"Resonance", "", "Highpass resonance Q", 1.0f, 10.0f, 1.0f},
}};
constexpr std::array<DspParamDesc, 5> kEchoParams = {{
    {"Delay", "ms", "Echo delay in milliseconds", 1.0f, 5000.0f, 500.0f},
    {"Decay", "", "Echo decay ratio", 0.0f, 1.0f, 0.5f},
    {"Max Channels", "", "Max channels", 0.0f, 16.0f, 0.0f},
    {"Dry Mix", "", "Dry mix level", 0.0f, 1.0f, 1.0f},
    {"Wet Mix", "", "Wet mix level", 0.0f, 1.0f, 1.0f},
}};
constexpr std::array<DspParamDesc, 4> kFlangeParams = {{
    {"Dry Mix", "", "Dry mix level", 0.0f, 1.0f, 0.45f},
    {"Wet Mix", "", "Wet mix level", 0.0f, 1.0f, 0.55f},
    {"Depth", "", "Flange depth", 0.01f, 1.0f, 1.0f},
    {"Rate", "Hz", "Flange modulation rate", 0.0f, 20.0f, 0.1f},
}};
constexpr std::array<DspParamDesc, 1> kDistortionParams = {{
    {"Level", "", "Distortion drive level", 0.0f, 1.0f, 0.5f},
}};
constexpr std::array<DspParamDesc, 3> kNormalizeParams = {{
    {"Fade Time", "ms", "Time to ramp gain", 0.0f, 20000.0f, 5000.0f},
    {"Threshold", "", "Lower threshold", 0.0f, 1.0f, 0.1f},
    {"Max Amp", "", "Maximum amplification", 1.0f, 100000.0f, 20.0f},
}};
constexpr std::array<DspParamDesc, 3> kParamEqParams = {{
    {"Center", "Hz", "Center frequency", 20.0f, 22000.0f, 8000.0f},
    {"Bandwidth", "oct", "Octave bandwidth", 0.2f, 5.0f, 1.0f},
    {"Gain", "x", "Linear gain", 0.05f, 3.0f, 1.0f},
}};
constexpr std::array<DspParamDesc, 2> kPitchShiftParams = {{
    {"Pitch", "x", "Pitch multiplier", 0.5f, 2.0f, 1.0f},
    {"FFT Size", "", "FFT window size", 256.0f, 4096.0f, 1024.0f},
}};
constexpr std::array<DspParamDesc, 6> kChorusParams = {{
    {"Dry Mix", "", "Dry mix level", 0.0f, 1.0f, 0.5f},
    {"Wet Mix 1", "", "Wet mix tap 1", 0.0f, 1.0f, 0.5f},
    {"Wet Mix 2", "", "Wet mix tap 2", 0.0f, 1.0f, 0.5f},
    {"Wet Mix 3", "", "Wet mix tap 3", 0.0f, 1.0f, 0.5f},
    {"Delay", "ms", "Chorus delay", 0.1f, 100.0f, 40.0f},
    {"Rate", "Hz", "Chorus rate", 0.0f, 20.0f, 0.8f},
}};
constexpr std::array<DspParamDesc, 4> kCompressorParams = {{
    {"Threshold", "dB", "Compression threshold", -60.0f, 0.0f, 0.0f},
    {"Attack", "ms", "Attack time", 10.0f, 200.0f, 50.0f},
    {"Release", "ms", "Release time", 20.0f, 1000.0f, 50.0f},
    {"Gain Make Up", "dB", "Make-up gain", 0.0f, 30.0f, 0.0f},
}};
constexpr std::array<DspParamDesc, 5> kReverbParams = {{
    {"Dry Level", "dB", "Dry level", -80.0f, 20.0f, 0.0f},
    {"Room", "dB", "Room wet level", -100.0f, 0.0f, -10.0f},
    {"Decay Time", "s", "Reverb decay time", 0.1f, 20.0f, 1.5f},
    {"Damping", "", "High-frequency damping", 0.0f, 1.0f, 0.5f},
    {"Mix", "", "Wet mix ratio", 0.0f, 1.0f, 0.35f},
}};
constexpr std::array<DspParamDesc, 1> kLowpassSimpleParams = {{
    {"Cutoff", "Hz", "Cutoff frequency", 10.0f, 22000.0f, 5000.0f},
}};
constexpr std::array<DspParamDesc, 2> kGargleParams = {{
    {"Rate", "Hz", "Modulation rate", 1.0f, 1000.0f, 20.0f},
    {"Wave Shape", "", "0=Triangle, 1=Square", 0.0f, 1.0f, 0.0f},
}};

const DspParamDesc* lookup_param_table(DspEffectType type, i32& out_count) noexcept {
    switch (type) {
        case DspEffectType::Oscillator: out_count = static_cast<i32>(kOscParams.size()); return kOscParams.data();
        case DspEffectType::Lowpass:
        case DspEffectType::ItLowpass: out_count = static_cast<i32>(kLowpassParams.size()); return kLowpassParams.data();
        case DspEffectType::Highpass: out_count = static_cast<i32>(kHighpassParams.size()); return kHighpassParams.data();
        case DspEffectType::Echo:
        case DspEffectType::ItEcho: out_count = static_cast<i32>(kEchoParams.size()); return kEchoParams.data();
        case DspEffectType::Flange: out_count = static_cast<i32>(kFlangeParams.size()); return kFlangeParams.data();
        case DspEffectType::Distortion: out_count = static_cast<i32>(kDistortionParams.size()); return kDistortionParams.data();
        case DspEffectType::Normalize: out_count = static_cast<i32>(kNormalizeParams.size()); return kNormalizeParams.data();
        case DspEffectType::ParamEq: out_count = static_cast<i32>(kParamEqParams.size()); return kParamEqParams.data();
        case DspEffectType::PitchShift: out_count = static_cast<i32>(kPitchShiftParams.size()); return kPitchShiftParams.data();
        case DspEffectType::Chorus: out_count = static_cast<i32>(kChorusParams.size()); return kChorusParams.data();
        case DspEffectType::Compressor: out_count = static_cast<i32>(kCompressorParams.size()); return kCompressorParams.data();
        case DspEffectType::Reverb:
        case DspEffectType::SfxReverb: out_count = static_cast<i32>(kReverbParams.size()); return kReverbParams.data();
        case DspEffectType::LowpassSimple: out_count = static_cast<i32>(kLowpassSimpleParams.size()); return kLowpassSimpleParams.data();
        case DspEffectType::Gargle: out_count = static_cast<i32>(kGargleParams.size()); return kGargleParams.data();
        default: out_count = 0; return nullptr;
    }
}

} // namespace

DspEffect::DspEffect(DspEffectType type, u32 sample_rate)
    : type_(type), sample_rate_(sample_rate > 0 ? sample_rate : 44100u) {
    speakers_.fill(true);
    default_freq_ = static_cast<f32>(sample_rate_);
    init_defaults();
}

void DspEffect::init_defaults() {
    const DspParamDesc* table = lookup_param_table(type_, param_count_);
    params_.fill(0.0f);
    for (i32 i = 0; i < param_count_ && i < static_cast<i32>(kMaxParams); ++i) {
        params_[static_cast<usize>(i)] = table[i].default_val;
    }

    if (type_ == DspEffectType::Echo || type_ == DspEffectType::ItEcho) {
        const usize max_samples = static_cast<usize>(sample_rate_) * 2u; // up to 2s echo
        delay_l_.assign(max_samples, 0.0f);
        delay_r_.assign(max_samples, 0.0f);
    } else if (type_ == DspEffectType::Chorus || type_ == DspEffectType::Flange) {
        const usize max_samples = static_cast<usize>(sample_rate_) / 4u; // up to 250ms
        delay_l_.assign(max_samples, 0.0f);
        delay_r_.assign(max_samples, 0.0f);
    } else if (type_ == DspEffectType::Reverb || type_ == DspEffectType::SfxReverb) {
        constexpr std::array<usize, 4> kCombLensL = {1116, 1188, 1277, 1356};
        constexpr std::array<usize, 4> kCombLensR = {1139, 1211, 1300, 1379};
        constexpr std::array<usize, 2> kApLensL = {556, 441};
        constexpr std::array<usize, 2> kApLensR = {579, 464};
        for (usize i = 0; i < 4; ++i) {
            combs_l_[i].buf.assign(kCombLensL[i], 0.0f);
            combs_r_[i].buf.assign(kCombLensR[i], 0.0f);
        }
        for (usize i = 0; i < 2; ++i) {
            allpass_l_[i].buf.assign(kApLensL[i], 0.0f);
            allpass_r_[i].buf.assign(kApLensR[i], 0.0f);
        }
    }

    update_biquad();
}

bool DspEffect::speaker_active(i32 speaker) const noexcept {
    if (speaker < 0 || speaker >= static_cast<i32>(speakers_.size())) return false;
    return speakers_[static_cast<usize>(speaker)];
}

void DspEffect::set_speaker_active(i32 speaker, bool enabled) noexcept {
    if (speaker >= 0 && speaker < static_cast<i32>(speakers_.size())) {
        speakers_[static_cast<usize>(speaker)] = enabled;
    }
}

i32 DspEffect::param_count() const noexcept {
    return param_count_;
}

DspParamDesc DspEffect::param_desc(i32 idx) const noexcept {
    i32 count = 0;
    const DspParamDesc* table = lookup_param_table(type_, count);
    if (!table || idx < 0 || idx >= count) {
        return DspParamDesc{"", "", "", 0.0f, 0.0f, 0.0f};
    }
    return table[idx];
}

f32 DspEffect::param_value(i32 idx) const noexcept {
    if (idx < 0 || idx >= param_count_) return 0.0f;
    return params_[static_cast<usize>(idx)];
}

std::string DspEffect::param_value_str(i32 idx) const {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(param_value(idx)));
    return std::string(buf);
}

bool DspEffect::set_param(i32 idx, f32 val) noexcept {
    if (idx < 0 || idx >= param_count_) return false;
    const DspParamDesc d = param_desc(idx);
    params_[static_cast<usize>(idx)] = std::clamp(val, d.min_val, d.max_val);
    update_biquad();
    return true;
}

void DspEffect::reset() noexcept {
    z1_l_ = z2_l_ = z1_r_ = z2_r_ = 0.0f;
    delay_write_ = 0;
    lfo_phase_ = 0.0f;
    env_gain_ = 1.0f;
    std::fill(delay_l_.begin(), delay_l_.end(), 0.0f);
    std::fill(delay_r_.begin(), delay_r_.end(), 0.0f);
    for (auto& c : combs_l_) { std::fill(c.buf.begin(), c.buf.end(), 0.0f); c.idx = 0; c.store = 0.0f; }
    for (auto& c : combs_r_) { std::fill(c.buf.begin(), c.buf.end(), 0.0f); c.idx = 0; c.store = 0.0f; }
    for (auto& a : allpass_l_) { std::fill(a.buf.begin(), a.buf.end(), 0.0f); a.idx = 0; }
    for (auto& a : allpass_r_) { std::fill(a.buf.begin(), a.buf.end(), 0.0f); a.idx = 0; }
}

void DspEffect::update_biquad() noexcept {
    const f32 sr = static_cast<f32>(sample_rate_);
    const f32 max_fc = sr * 0.45f;

    if (type_ == DspEffectType::Lowpass || type_ == DspEffectType::ItLowpass || type_ == DspEffectType::LowpassSimple) {
        const f32 fc = std::clamp(params_[0], 10.0f, max_fc);
        const f32 q = (type_ == DspEffectType::LowpassSimple) ? 0.7071f : std::max(0.5f, params_[1] * 0.7071f);
        const f32 w0 = kTwoPi * fc / sr;
        const f32 cosw = std::cos(w0);
        const f32 alpha = std::sin(w0) / (2.0f * q);
        const f32 a0 = 1.0f + alpha;
        b0_ = ((1.0f - cosw) * 0.5f) / a0;
        b1_ = (1.0f - cosw) / a0;
        b2_ = b0_;
        a1_ = (-2.0f * cosw) / a0;
        a2_ = (1.0f - alpha) / a0;
    } else if (type_ == DspEffectType::Highpass) {
        const f32 fc = std::clamp(params_[0], 10.0f, max_fc);
        const f32 q = std::max(0.5f, params_[1] * 0.7071f);
        const f32 w0 = kTwoPi * fc / sr;
        const f32 cosw = std::cos(w0);
        const f32 alpha = std::sin(w0) / (2.0f * q);
        const f32 a0 = 1.0f + alpha;
        b0_ = ((1.0f + cosw) * 0.5f) / a0;
        b1_ = -(1.0f + cosw) / a0;
        b2_ = b0_;
        a1_ = (-2.0f * cosw) / a0;
        a2_ = (1.0f - alpha) / a0;
    } else if (type_ == DspEffectType::ParamEq) {
        const f32 fc = std::clamp(params_[0], 20.0f, max_fc);
        const f32 bw = std::clamp(params_[1], 0.2f, 5.0f);
        const f32 gain_lin = std::clamp(params_[2], 0.05f, 3.0f);
        const f32 a = std::sqrt(gain_lin);
        const f32 w0 = kTwoPi * fc / sr;
        const f32 cosw = std::cos(w0);
        const f32 sinw = std::sin(w0);
        const f32 alpha = sinw * std::sinh(std::log(2.0f) * 0.5f * bw * (w0 / std::max(sinw, 1e-4f)));
        const f32 a0 = 1.0f + alpha / a;
        b0_ = (1.0f + alpha * a) / a0;
        b1_ = (-2.0f * cosw) / a0;
        b2_ = (1.0f - alpha * a) / a0;
        a1_ = (-2.0f * cosw) / a0;
        a2_ = (1.0f - alpha / a) / a0;
    }
}

void DspEffect::process(f32* stereo, u32 frame_count) noexcept {
    if (!active_ || bypass_ || frame_count == 0 || type_ == DspEffectType::None || type_ == DspEffectType::Mixer) {
        return;
    }

    const f32 sr = static_cast<f32>(sample_rate_);

    switch (type_) {
        case DspEffectType::Lowpass:
        case DspEffectType::ItLowpass:
        case DspEffectType::LowpassSimple:
        case DspEffectType::Highpass:
        case DspEffectType::ParamEq: {
            for (u32 i = 0; i < frame_count; ++i) {
                const f32 xl = stereo[i * 2u + 0];
                const f32 xr = stereo[i * 2u + 1];
                const f32 yl = b0_ * xl + z1_l_;
                z1_l_ = b1_ * xl - a1_ * yl + z2_l_;
                z2_l_ = b2_ * xl - a2_ * yl;
                const f32 yr = b0_ * xr + z1_r_;
                z1_r_ = b1_ * xr - a1_ * yr + z2_r_;
                z2_r_ = b2_ * xr - a2_ * yr;
                stereo[i * 2u + 0] = yl;
                stereo[i * 2u + 1] = yr;
            }
            break;
        }
        case DspEffectType::Distortion: {
            const f32 level = std::clamp(params_[0], 0.0f, 0.99f);
            const f32 k = 2.0f * level / (1.0f - level);
            for (u32 i = 0; i < frame_count * 2u; ++i) {
                const f32 x = stereo[i];
                stereo[i] = (1.0f + k) * x / (1.0f + k * std::fabs(x));
            }
            break;
        }
        case DspEffectType::Echo:
        case DspEffectType::ItEcho: {
            if (delay_l_.empty()) break;
            const usize buf_sz = delay_l_.size();
            const usize delay_samps = std::clamp<usize>(
                static_cast<usize>(params_[0] * 0.001f * sr), 1u, buf_sz - 1u);
            const f32 decay = std::clamp(params_[1], 0.0f, 0.95f);
            const f32 dry = params_[3];
            const f32 wet = params_[4];
            for (u32 i = 0; i < frame_count; ++i) {
                const usize read_idx = (delay_write_ + buf_sz - delay_samps) % buf_sz;
                const f32 dl = delay_l_[read_idx];
                const f32 dr = delay_r_[read_idx];
                const f32 in_l = stereo[i * 2u + 0];
                const f32 in_r = stereo[i * 2u + 1];
                delay_l_[delay_write_] = in_l + dl * decay;
                delay_r_[delay_write_] = in_r + dr * decay;
                delay_write_ = (delay_write_ + 1u) % buf_sz;
                stereo[i * 2u + 0] = in_l * dry + dl * wet;
                stereo[i * 2u + 1] = in_r * dry + dr * wet;
            }
            break;
        }
        case DspEffectType::Flange:
        case DspEffectType::Chorus: {
            if (delay_l_.empty()) break;
            const usize buf_sz = delay_l_.size();
            const bool is_chorus = (type_ == DspEffectType::Chorus);
            const f32 dry = params_[0];
            const f32 wet = params_[1];
            const f32 base_ms = is_chorus ? params_[4] : 2.0f;
            const f32 depth_ms = is_chorus ? (base_ms * 0.25f) : (params_[2] * 6.0f);
            const f32 rate_hz = is_chorus ? params_[5] : params_[3];
            const f32 phase_inc = kTwoPi * rate_hz / sr;
            for (u32 i = 0; i < frame_count; ++i) {
                const f32 in_l = stereo[i * 2u + 0];
                const f32 in_r = stereo[i * 2u + 1];
                delay_l_[delay_write_] = in_l;
                delay_r_[delay_write_] = in_r;

                const f32 lfo_l = 0.5f * (1.0f + std::sin(lfo_phase_));
                const f32 lfo_r = 0.5f * (1.0f + std::sin(lfo_phase_ + kPi * 0.5f));
                lfo_phase_ += phase_inc;
                if (lfo_phase_ >= kTwoPi) lfo_phase_ -= kTwoPi;

                const f32 d_l = std::clamp((base_ms + depth_ms * lfo_l) * 0.001f * sr, 1.0f, static_cast<f32>(buf_sz - 2u));
                const f32 d_r = std::clamp((base_ms + depth_ms * lfo_r) * 0.001f * sr, 1.0f, static_cast<f32>(buf_sz - 2u));

                const usize i0_l = (delay_write_ + buf_sz - static_cast<usize>(d_l)) % buf_sz;
                const usize i1_l = (i0_l + buf_sz - 1u) % buf_sz;
                const f32 f_l = d_l - std::floor(d_l);
                const f32 tap_l = delay_l_[i0_l] * (1.0f - f_l) + delay_l_[i1_l] * f_l;

                const usize i0_r = (delay_write_ + buf_sz - static_cast<usize>(d_r)) % buf_sz;
                const usize i1_r = (i0_r + buf_sz - 1u) % buf_sz;
                const f32 f_r = d_r - std::floor(d_r);
                const f32 tap_r = delay_r_[i0_r] * (1.0f - f_r) + delay_r_[i1_r] * f_r;

                delay_write_ = (delay_write_ + 1u) % buf_sz;
                stereo[i * 2u + 0] = in_l * dry + tap_l * wet;
                stereo[i * 2u + 1] = in_r * dry + tap_r * wet;
            }
            break;
        }
        case DspEffectType::Reverb:
        case DspEffectType::SfxReverb: {
            const f32 feedback = std::clamp(0.7f + 0.25f * std::min(params_[2] / 5.0f, 1.0f), 0.3f, 0.94f);
            const f32 damp = std::clamp(params_[3], 0.05f, 0.95f);
            const f32 mix = std::clamp(params_[4], 0.0f, 1.0f);
            const f32 dry = 1.0f - mix * 0.5f;
            for (u32 i = 0; i < frame_count; ++i) {
                const f32 in_l = stereo[i * 2u + 0];
                const f32 in_r = stereo[i * 2u + 1];
                const f32 mono_in = (in_l + in_r) * 0.015f;
                f32 out_l = 0.0f;
                f32 out_r = 0.0f;
                for (usize c = 0; c < 4; ++c) {
                    auto& cl = combs_l_[c];
                    const f32 y_l = cl.buf[cl.idx];
                    cl.store = y_l * (1.0f - damp) + cl.store * damp;
                    cl.buf[cl.idx] = mono_in + cl.store * feedback;
                    cl.idx = (cl.idx + 1u) % cl.buf.size();
                    out_l += y_l;

                    auto& cr = combs_r_[c];
                    const f32 y_r = cr.buf[cr.idx];
                    cr.store = y_r * (1.0f - damp) + cr.store * damp;
                    cr.buf[cr.idx] = mono_in + cr.store * feedback;
                    cr.idx = (cr.idx + 1u) % cr.buf.size();
                    out_r += y_r;
                }
                for (usize a = 0; a < 2; ++a) {
                    auto& al = allpass_l_[a];
                    const f32 b_l = al.buf[al.idx];
                    const f32 y_l = -out_l + b_l;
                    al.buf[al.idx] = out_l + b_l * 0.5f;
                    al.idx = (al.idx + 1u) % al.buf.size();
                    out_l = y_l;

                    auto& ar = allpass_r_[a];
                    const f32 b_r = ar.buf[ar.idx];
                    const f32 y_r = -out_r + b_r;
                    ar.buf[ar.idx] = out_r + b_r * 0.5f;
                    ar.idx = (ar.idx + 1u) % ar.buf.size();
                    out_r = y_r;
                }
                stereo[i * 2u + 0] = in_l * dry + out_l * mix;
                stereo[i * 2u + 1] = in_r * dry + out_r * mix;
            }
            break;
        }
        case DspEffectType::Compressor: {
            const f32 thresh_lin = std::pow(10.0f, params_[0] / 20.0f);
            const f32 attack_coeff = std::exp(-1.0f / std::max(1.0f, params_[1] * 0.001f * sr));
            const f32 release_coeff = std::exp(-1.0f / std::max(1.0f, params_[2] * 0.001f * sr));
            const f32 makeup = std::pow(10.0f, params_[3] / 20.0f);
            for (u32 i = 0; i < frame_count; ++i) {
                const f32 peak = std::max(std::fabs(stereo[i * 2u + 0]), std::fabs(stereo[i * 2u + 1]));
                f32 target_gain = 1.0f;
                if (peak > thresh_lin && peak > 1e-6f) {
                    const f32 over = peak / thresh_lin;
                    target_gain = std::pow(over, -0.75f); // 4:1 ratio
                }
                const f32 coeff = (target_gain < env_gain_) ? attack_coeff : release_coeff;
                env_gain_ = coeff * env_gain_ + (1.0f - coeff) * target_gain;
                const f32 g = env_gain_ * makeup;
                stereo[i * 2u + 0] *= g;
                stereo[i * 2u + 1] *= g;
            }
            break;
        }
        case DspEffectType::Normalize: {
            const f32 thresh = std::max(params_[1], 0.01f);
            const f32 max_amp = std::max(params_[2], 1.0f);
            const f32 alpha = 1.0f / std::max(1.0f, params_[0] * 0.001f * sr);
            for (u32 i = 0; i < frame_count; ++i) {
                const f32 peak = std::max(std::fabs(stereo[i * 2u + 0]), std::fabs(stereo[i * 2u + 1]));
                const f32 desired = (peak > thresh) ? std::min(max_amp, 0.9f / peak) : 1.0f;
                env_gain_ += (desired - env_gain_) * alpha;
                stereo[i * 2u + 0] *= env_gain_;
                stereo[i * 2u + 1] *= env_gain_;
            }
            break;
        }
        case DspEffectType::Gargle: {
            const f32 rate_hz = std::max(1.0f, params_[0]);
            const bool square = params_[1] >= 0.5f;
            const f32 inc = rate_hz / sr;
            for (u32 i = 0; i < frame_count; ++i) {
                lfo_phase_ += inc;
                if (lfo_phase_ >= 1.0f) lfo_phase_ -= 1.0f;
                const f32 mod = square ? (lfo_phase_ < 0.5f ? 1.0f : 0.0f)
                                       : (lfo_phase_ < 0.5f ? (lfo_phase_ * 2.0f) : ((1.0f - lfo_phase_) * 2.0f));
                stereo[i * 2u + 0] *= mod;
                stereo[i * 2u + 1] *= mod;
            }
            break;
        }
        default:
            break;
    }
}

} // namespace gm
