#include "audio/audio_engine.h"

#include "base/encoding.h"
#include "base/log.h"

#include <miniaudio.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include <tsf.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>

namespace gm {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 6.28318530717958647692f;
constexpr usize kStreamThresholdOgg = 192u * 1024u;
constexpr usize kStreamThresholdGeneric = 512u * 1024u;

void ma_data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    (void)input;
    auto* engine = static_cast<AudioEngine*>(device->pUserData);
    auto* out = static_cast<f32*>(output);
    if (engine) {
        engine->mix_frames(out, frame_count);
        if (engine->is_output_muted()) {
            std::memset(out, 0, static_cast<usize>(frame_count) * 2u * sizeof(f32));
        }
    } else {
        std::memset(out, 0, static_cast<usize>(frame_count) * 2u * sizeof(f32));
    }
}

int read_vorbis_stereo(stb_vorbis* decoder, std::span<f32> output) noexcept {
    const int frames = stb_vorbis_get_samples_float_interleaved(
        decoder, 2, output.data(), static_cast<int>(output.size()));
    // stb_vorbis zero-fills the missing right channel for mono input.
    if (stb_vorbis_get_info(decoder).channels == 1) {
        for (int i = 0; i < frames; ++i) {
            output[2 * i + 1] = output[2 * i];
        }
    }
    return frames;
}

void resample_stereo_linear(
    const f32* src,
    usize src_frames,
    u32 src_rate,
    std::vector<f32>& dst,
    u32 dst_rate) {
    if (src_frames == 0 || src_rate == 0) {
        dst.clear();
        return;
    }
    if (src_rate == dst_rate) {
        dst.assign(src, src + src_frames * 2u);
        return;
    }
    const f64 ratio = static_cast<f64>(src_rate) / static_cast<f64>(dst_rate);
    const usize dst_frames = static_cast<usize>(static_cast<f64>(src_frames) / ratio);
    dst.resize(dst_frames * 2u);
    for (usize i = 0; i < dst_frames; ++i) {
        const f64 pos = static_cast<f64>(i) * ratio;
        const usize i0 = static_cast<usize>(pos);
        const usize i1 = std::min(i0 + 1, src_frames - 1);
        const f32 frac = static_cast<f32>(pos - static_cast<f64>(i0));
        dst[i * 2u + 0] = src[i0 * 2u + 0] * (1.0f - frac) + src[i1 * 2u + 0] * frac;
        dst[i * 2u + 1] = src[i0 * 2u + 1] * (1.0f - frac) + src[i1 * 2u + 1] * frac;
    }
}

void parse_vorbis_comments(stb_vorbis* vorb, u32 sample_rate, SoundAsset& asset) {
    const stb_vorbis_comment cmt = stb_vorbis_get_comment(vorb);
    i64 loop_start_samples = -1;
    i64 loop_length_samples = -1;
    i64 loop_end_samples = -1;

    for (int i = 0; i < cmt.comment_list_length; ++i) {
        if (!cmt.comment_list[i]) continue;
        const std::string_view sv(cmt.comment_list[i]);
        const usize eq = sv.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = sv.substr(0, eq);
        const std::string_view val = sv.substr(eq + 1);
        asset.tags.push_back(AudioTag{std::string(key), std::string(val)});

        const std::string key_lo = ascii_to_lower(key);
        if (key_lo == "loopstart") {
            loop_start_samples = std::strtoll(std::string(val).c_str(), nullptr, 10);
        } else if (key_lo == "looplength") {
            loop_length_samples = std::strtoll(std::string(val).c_str(), nullptr, 10);
        } else if (key_lo == "loopend") {
            loop_end_samples = std::strtoll(std::string(val).c_str(), nullptr, 10);
        }
    }

    if (sample_rate > 0 && loop_start_samples >= 0) {
        i64 end_s = -1;
        if (loop_length_samples > 0) end_s = loop_start_samples + loop_length_samples;
        else if (loop_end_samples > loop_start_samples) end_s = loop_end_samples;
        if (end_s > loop_start_samples) {
            asset.has_loop_points = true;
            asset.loop_start_ms = static_cast<f64>(loop_start_samples) * 1000.0 / static_cast<f64>(sample_rate);
            asset.loop_end_ms = static_cast<f64>(end_s) * 1000.0 / static_cast<f64>(sample_rate);
        }
    }
}

f32 window_coeff(i32 window_type, usize i, usize n) noexcept {
    if (n <= 1) return 1.0f;
    const f32 x = static_cast<f32>(i) / static_cast<f32>(n - 1);
    switch (window_type) {
        case 1: // Triangle
            return 1.0f - std::fabs(2.0f * x - 1.0f);
        case 2: // Hamming
            return 0.54f - 0.46f * std::cos(kTwoPi * x);
        case 3: // Hanning
            return 0.5f * (1.0f - std::cos(kTwoPi * x));
        case 4: // Blackman
            return 0.42f - 0.5f * std::cos(kTwoPi * x) + 0.08f * std::cos(2.0f * kTwoPi * x);
        case 5: // Blackman-Harris
            return 0.35875f - 0.48829f * std::cos(kTwoPi * x) +
                   0.14128f * std::cos(2.0f * kTwoPi * x) - 0.01168f * std::cos(3.0f * kTwoPi * x);
        default: // Rectangular
            return 1.0f;
    }
}

void read_ring_samples(
    const std::vector<f32>& ring_l,
    const std::vector<f32>& ring_r,
    usize ring_write,
    i32 channel_lr,
    u32 count,
    std::vector<f32>& out) {
    const usize cap = ring_l.size();
    out.assign(count, 0.0f);
    if (cap == 0 || count == 0) return;
    const usize n = std::min<usize>(count, cap);
    const usize start = (ring_write + cap - n) % cap;
    const auto& src = (channel_lr == 1) ? ring_r : ring_l;
    for (usize i = 0; i < n; ++i) {
        out[i] = src[(start + i) % cap];
    }
}

} // namespace

struct AudioChannel {
    i32 id = 0;
    bool is_vanilla = false;
    i32 sound_id = -1; // Vanilla index or External sound handle
    i32 group_id = 0;
    AudioSourceKind source_kind = AudioSourceKind::Empty;

    bool active = false;
    bool paused = false;
    bool muted = false;
    bool looping = false;
    i32 loop_count = -1; // -1 = infinite, 0 = no more loops, >0 = remaining loops

    u32 orig_sample_rate = AudioEngine::kOutputSampleRate;
    u64 total_frames = 0;
    f64 length_ms = 0.0;

    // Playback position (in output-rate frames for PcmStereo, or source frames for streams)
    f64 frame_pos = 0.0;
    // DirectSound reports its physical PCM buffer cursor even across file loops.
    f64 buffer_frame_pos = 0.0;
    u32 source_bytes_per_frame = 0;
    bool has_loop_points = false;
    f64 loop_start_ms = 0.0;
    f64 loop_end_ms = 0.0;

    // Backing media handles (safe shared ownership so sound free can't dangle)
    std::shared_ptr<const std::vector<f32>> owned_pcm;
    const std::vector<f32>* pcm_ref = nullptr;
    std::shared_ptr<std::vector<u8>> stream_raw;
    stb_vorbis* ogg_decoder = nullptr;
    ma_decoder ma_dec{};
    bool ma_dec_init = false;
    std::unique_ptr<TrackerPlayer> tracker_player;
    std::unique_ptr<MidiPlayer> midi_player;
    std::array<f32, 4> synth_tail{}; // At most two stereo frames of interpolation lookahead.
    u32 synth_tail_frames = 0;
    f64 synth_fraction = 0.0;

    // Stream resampler state (when pitch/frequency != 1.0 or source rate != output rate)
    std::vector<f32> stream_chunk;
    usize stream_chunk_frames = 0;
    f64 stream_chunk_pos = 0.0;

    // Mix parameters
    f32 volume = 1.0f;
    f32 max_volume = 1.0f;
    f32 pan = 0.0f;
    f32 frequency = static_cast<f32>(AudioEngine::kOutputSampleRate);
    f32 pitch = 1.0f;
    f32 fade_target = 1.0f;
    f32 fade_rate_per_sec = 0.0f;
    std::array<f32, 8> speaker_mix = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    bool custom_speaker_mix = false;

    // 3D state
    bool is_3d = false;
    f32 pos[3] = {0.0f, 0.0f, 0.0f};
    f32 prev_pos[3] = {0.0f, 0.0f, 0.0f};
    f32 vel[3] = {0.0f, 0.0f, 0.0f};
    f32 min_dist = 1.0f;
    f32 max_dist = 10000.0f;
    f32 doppler_level = 1.0f;
    f32 cone_inside = 360.0f;
    f32 cone_outside = 360.0f;
    f32 cone_outside_vol = 1.0f;
    f32 cone_dir[3] = {0.0f, 0.0f, 1.0f};
    f32 pan_level_3d = 1.0f;
    f32 spread_3d = 0.0f;
    f32 last_audibility = 1.0f;

    // Attached DSP effect handles
    std::vector<i32> dsp_ids;

    // Recent stereo output ring buffer & peak metering
    static constexpr usize kRingFrames = 4096;
    std::vector<f32> ring_l;
    std::vector<f32> ring_r;
    usize ring_write = 0;
    f32 peak_l = 0.0f;
    f32 peak_r = 0.0f;

    AudioChannel() : ring_l(kRingFrames, 0.0f), ring_r(kRingFrames, 0.0f) {}

    ~AudioChannel() {
        close_decoders();
    }

    void close_decoders() noexcept {
        if (ogg_decoder) {
            stb_vorbis_close(ogg_decoder);
            ogg_decoder = nullptr;
        }
        if (ma_dec_init) {
            ma_decoder_uninit(&ma_dec);
            ma_dec_init = false;
        }
        tracker_player.reset();
        midi_player.reset();
    }

    bool open_stream_decoder(const SoundAsset& asset, const tsf* soundfont) {
        close_decoders();
        source_kind = asset.source_kind;
        orig_sample_rate = asset.orig_sample_rate > 0 ? asset.orig_sample_rate : AudioEngine::kOutputSampleRate;
        total_frames = asset.total_frames;
        source_bytes_per_frame = asset.source_bytes_per_frame;
        length_ms = asset.length_ms;
        frequency = static_cast<f32>(orig_sample_rate);
        frame_pos = 0.0;
        stream_chunk_frames = 0;
        stream_chunk_pos = 0.0;

        switch (asset.source_kind) {
            case AudioSourceKind::PcmStereo:
                if (!asset.pcm_stereo || asset.pcm_stereo->empty()) return false;
                owned_pcm = asset.pcm_stereo;
                pcm_ref = owned_pcm.get();
                total_frames = owned_pcm->size() / 2u;
                return true;

            case AudioSourceKind::OggStream: {
                if (!asset.stream_raw || asset.stream_raw->empty()) return false;
                stream_raw = asset.stream_raw;
                int err = 0;
                ogg_decoder = stb_vorbis_open_memory(
                    stream_raw->data(), static_cast<int>(stream_raw->size()), &err, nullptr);
                if (!ogg_decoder) return false;
                stream_chunk.resize(2048u * 2u);
                return true;
            }

            case AudioSourceKind::MaStream: {
                if (!asset.stream_raw || asset.stream_raw->empty()) return false;
                stream_raw = asset.stream_raw;
                ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, AudioEngine::kOutputSampleRate);
                if (ma_decoder_init_memory(stream_raw->data(), stream_raw->size(), &cfg, &ma_dec) != MA_SUCCESS) {
                    return false;
                }
                ma_dec_init = true;
                stream_chunk.resize(2048u * 2u);
                return true;
            }

            case AudioSourceKind::Tracker:
                if (!asset.tracker_mod) return false;
                tracker_player = std::make_unique<TrackerPlayer>(asset.tracker_mod, AudioEngine::kOutputSampleRate);
                for (usize c = 0; c < asset.tracker_channel_vols.size(); ++c) {
                    tracker_player->set_channel_volume(static_cast<u32>(c), asset.tracker_channel_vols[c]);
                }
                orig_sample_rate = AudioEngine::kOutputSampleRate;
                frequency = static_cast<f32>(orig_sample_rate);
                return true;

            case AudioSourceKind::Midi:
                if (!asset.midi_seq) return false;
                midi_player = std::make_unique<MidiPlayer>(asset.midi_seq, soundfont, AudioEngine::kOutputSampleRate);
                orig_sample_rate = AudioEngine::kOutputSampleRate;
                frequency = static_cast<f32>(orig_sample_rate);
                return true;

            default:
                return false;
        }
    }

    void seek_to_ms(f64 ms) noexcept {
        const f64 clamped_ms = std::clamp(ms, 0.0, std::max(length_ms, 0.0));
        stream_chunk_frames = 0;
        stream_chunk_pos = 0.0;
        synth_tail_frames = 0;
        synth_fraction = 0.0;

        switch (source_kind) {
            case AudioSourceKind::PcmStereo:
                frame_pos = clamped_ms * 0.001 * static_cast<f64>(AudioEngine::kOutputSampleRate);
                if (total_frames > 0 && frame_pos >= static_cast<f64>(total_frames)) {
                    frame_pos = static_cast<f64>(total_frames - 1u);
                }
                break;
            case AudioSourceKind::OggStream:
                if (ogg_decoder) {
                    const u32 target_s = static_cast<u32>(clamped_ms * 0.001 * static_cast<f64>(orig_sample_rate));
                    stb_vorbis_seek(ogg_decoder, target_s);
                    frame_pos = static_cast<f64>(target_s);
                }
                break;
            case AudioSourceKind::MaStream:
                if (ma_dec_init) {
                    const ma_uint64 target_f = static_cast<ma_uint64>(clamped_ms * 0.001 * static_cast<f64>(AudioEngine::kOutputSampleRate));
                    ma_decoder_seek_to_pcm_frame(&ma_dec, target_f);
                    frame_pos = static_cast<f64>(target_f);
                }
                break;
            case AudioSourceKind::Tracker:
                if (tracker_player) {
                    tracker_player->seek_ms(clamped_ms);
                    frame_pos = clamped_ms * 0.001 * static_cast<f64>(AudioEngine::kOutputSampleRate);
                }
                break;
            case AudioSourceKind::Midi:
                if (midi_player) {
                    midi_player->seek_ms(clamped_ms);
                    frame_pos = clamped_ms * 0.001 * static_cast<f64>(AudioEngine::kOutputSampleRate);
                }
                break;
            default:
                break;
        }
    }

    f64 current_position_ms() const noexcept {
        switch (source_kind) {
            case AudioSourceKind::PcmStereo:
            case AudioSourceKind::MaStream:
                return frame_pos * 1000.0 / static_cast<f64>(AudioEngine::kOutputSampleRate);
            case AudioSourceKind::OggStream:
                return orig_sample_rate > 0 ? (frame_pos * 1000.0 / static_cast<f64>(orig_sample_rate)) : 0.0;
            case AudioSourceKind::Tracker:
            case AudioSourceKind::Midi:
                if (tracker_player || midi_player) {
                    const f64 generated = tracker_player ? tracker_player->position_ms() : midi_player->position_ms();
                    const f64 ahead = (synth_tail_frames - synth_fraction) * 1000.0 / AudioEngine::kOutputSampleRate;
                    return std::max(0.0, generated - ahead);
                }
                return 0.0;
            default:
                return 0.0;
        }
    }

    // FMOD's channel-group pitch resamples the synthesized output, including
    // the note pitch, envelopes and tempo. Generate only the frames needed
    // for this mix so live tracker volume changes do not sit behind a buffer.
    u32 pull_pitched_synth(f32* out, u32 frames, f64 rate, bool loop) noexcept {
        auto render = [&](f32* dst, u32 count) {
            return tracker_player ? tracker_player->render(dst, count, loop) : midi_player->render(dst, count, loop);
        };
        auto finished = [&]() { return tracker_player ? tracker_player->finished() : midi_player->finished(); };
        if (rate == 1.0 && synth_fraction == 0.0) {
            const u32 cached = std::min(frames, synth_tail_frames);
            std::copy_n(synth_tail.begin(), 2 * cached, out);
            synth_tail_frames -= cached;
            std::copy_n(synth_tail.data() + 2 * cached, 2 * synth_tail_frames, synth_tail.begin());
            const u32 got = render(out + 2 * cached, frames - cached);
            if (synth_tail_frames == 0 && finished()) active = false;
            return cached + got;
        }
        constexpr u32 block_size = 64;
        std::array<f32, (8 * block_size + 2) * 2> source;
        u32 written = 0;
        while (written < frames) {
            const u32 block = std::min(block_size, frames - written);
            const u32 advance = static_cast<u32>(synth_fraction + block * rate);
            const u32 last = static_cast<u32>(synth_fraction + (block - 1) * rate);
            const u32 needed = std::max(advance, last + 2);
            std::copy_n(synth_tail.begin(), 2 * synth_tail_frames, source.begin());
            const u32 request = needed - synth_tail_frames;
            f32* next = source.data() + 2 * synth_tail_frames;
            const u32 got = render(next, request);
            const u32 available = synth_tail_frames + got;
            u32 f = 0;
            for (; f < block; ++f) {
                const f64 pos = synth_fraction + f * rate;
                const u32 a = static_cast<u32>(pos);
                if (a >= available) break;
                const u32 b = std::min(a + 1, available - 1);
                const f32 frac = static_cast<f32>(pos - a);
                for (u32 c = 0; c < 2; ++c) {
                    out[2 * (written + f) + c] = source[2 * a + c] + frac * (source[2 * b + c] - source[2 * a + c]);
                }
            }
            written += f;
            const f64 consumed = synth_fraction + f * rate;
            const u32 whole = std::min(static_cast<u32>(consumed), available);
            synth_tail_frames = available - whole;
            std::copy_n(source.data() + 2 * whole, 2 * synth_tail_frames, synth_tail.begin());
            synth_fraction = consumed - std::floor(consumed);
            if (f < block || (synth_tail_frames == 0 && finished())) {
                active = false;
                break;
            }
        }
        return written;
    }

    // Pulls decoded stereo f32 frames at output sample rate, applying pitch/rate & looping.
    u32 pull_frames(f32* out_stereo, u32 frame_count, f32 effective_rate_mult) noexcept {
        const f64 rate_mult = std::clamp<f64>(static_cast<f64>(effective_rate_mult), 0.05, 8.0);
        const bool do_loop = looping && (loop_count != 0);

        auto consume_loop_iteration = [&]() -> bool {
            if (!looping || loop_count == 0) return false;
            if (loop_count > 0) --loop_count;
            return true;
        };

        switch (source_kind) {
            case AudioSourceKind::PcmStereo: {
                if (!pcm_ref || pcm_ref->empty() || total_frames == 0) {
                    active = false;
                    return 0;
                }
                const f32* pcm = pcm_ref->data();
                const f64 lp_start = has_loop_points
                                         ? std::clamp(loop_start_ms * 0.001 * static_cast<f64>(AudioEngine::kOutputSampleRate), 0.0, static_cast<f64>(total_frames - 1u))
                                         : 0.0;
                const f64 lp_end = (has_loop_points && loop_end_ms > loop_start_ms)
                                       ? std::clamp(loop_end_ms * 0.001 * static_cast<f64>(AudioEngine::kOutputSampleRate), lp_start + 1.0, static_cast<f64>(total_frames))
                                       : static_cast<f64>(total_frames);

                u32 written = 0;
                for (u32 f = 0; f < frame_count; ++f) {
                    if (frame_pos >= lp_end || frame_pos >= static_cast<f64>(total_frames)) {
                        if (consume_loop_iteration()) {
                            frame_pos = lp_start + std::fmod(std::max(0.0, frame_pos - lp_start), std::max(1.0, lp_end - lp_start));
                        } else if (has_loop_points && frame_pos < static_cast<f64>(total_frames)) {
                            // Continue past loop end on final pass
                        } else {
                            active = false;
                            break;
                        }
                    }
                    const usize i0 = std::min<usize>(static_cast<usize>(frame_pos), static_cast<usize>(total_frames - 1u));
                    const usize i1 = std::min<usize>(i0 + 1u, static_cast<usize>(total_frames - 1u));
                    const f32 frac = static_cast<f32>(frame_pos - static_cast<f64>(i0));
                    out_stereo[f * 2u + 0] = pcm[i0 * 2u + 0] * (1.0f - frac) + pcm[i1 * 2u + 0] * frac;
                    out_stereo[f * 2u + 1] = pcm[i0 * 2u + 1] * (1.0f - frac) + pcm[i1 * 2u + 1] * frac;
                    frame_pos += rate_mult;
                    ++written;
                }
                return written;
            }

            case AudioSourceKind::OggStream: {
                if (!ogg_decoder) { active = false; return 0; }
                const f64 src_step = (static_cast<f64>(orig_sample_rate) / static_cast<f64>(AudioEngine::kOutputSampleRate)) * rate_mult;
                const f64 lp_start = has_loop_points
                                         ? std::max(0.0, loop_start_ms * 0.001 * static_cast<f64>(orig_sample_rate))
                                         : 0.0;
                const f64 lp_end = (has_loop_points && loop_end_ms > loop_start_ms)
                                       ? (loop_end_ms * 0.001 * static_cast<f64>(orig_sample_rate))
                                       : static_cast<f64>(total_frames);

                u32 written = 0;
                while (written < frame_count) {
                    if (stream_chunk_pos >= static_cast<f64>(stream_chunk_frames)) {
                        const int got = read_vorbis_stereo(ogg_decoder, stream_chunk);
                        if (got <= 0) {
                            if (consume_loop_iteration()) {
                                stb_vorbis_seek(ogg_decoder, static_cast<unsigned int>(lp_start));
                                frame_pos = lp_start;
                                stream_chunk_frames = 0;
                                stream_chunk_pos = 0.0;
                                continue;
                            } else {
                                active = false;
                                break;
                            }
                        }
                        stream_chunk_frames = static_cast<usize>(got);
                        stream_chunk_pos = 0.0;
                    }

                    if (has_loop_points && lp_end > lp_start && frame_pos >= lp_end && do_loop) {
                        if (consume_loop_iteration()) {
                            stb_vorbis_seek(ogg_decoder, static_cast<unsigned int>(lp_start));
                            frame_pos = lp_start;
                            stream_chunk_frames = 0;
                            stream_chunk_pos = 0.0;
                            continue;
                        }
                    }

                    const usize i0 = std::min<usize>(static_cast<usize>(stream_chunk_pos), stream_chunk_frames - 1u);
                    const usize i1 = std::min<usize>(i0 + 1u, stream_chunk_frames - 1u);
                    const f32 frac = static_cast<f32>(stream_chunk_pos - static_cast<f64>(i0));
                    out_stereo[written * 2u + 0] = stream_chunk[i0 * 2u + 0] * (1.0f - frac) + stream_chunk[i1 * 2u + 0] * frac;
                    out_stereo[written * 2u + 1] = stream_chunk[i0 * 2u + 1] * (1.0f - frac) + stream_chunk[i1 * 2u + 1] * frac;
                    stream_chunk_pos += src_step;
                    frame_pos += src_step;
                    ++written;
                }
                return written;
            }

            case AudioSourceKind::MaStream: {
                if (!ma_dec_init) { active = false; return 0; }
                const f64 lp_start = has_loop_points
                                         ? std::max(0.0, loop_start_ms * 0.001 * static_cast<f64>(AudioEngine::kOutputSampleRate))
                                         : 0.0;
                const f64 lp_end = (has_loop_points && loop_end_ms > loop_start_ms)
                                       ? (loop_end_ms * 0.001 * static_cast<f64>(AudioEngine::kOutputSampleRate))
                                       : static_cast<f64>(total_frames);

                u32 written = 0;
                while (written < frame_count) {
                    if (stream_chunk_pos >= static_cast<f64>(stream_chunk_frames)) {
                        ma_uint64 got = 0;
                        const ma_uint64 req = stream_chunk.size() / 2u;
                        const ma_result res = ma_decoder_read_pcm_frames(&ma_dec, stream_chunk.data(), req, &got);
                        if (got == 0 || (res != MA_SUCCESS && res != MA_AT_END)) {
                            if (consume_loop_iteration()) {
                                ma_decoder_seek_to_pcm_frame(&ma_dec, static_cast<ma_uint64>(lp_start));
                                frame_pos = lp_start;
                                stream_chunk_frames = 0;
                                stream_chunk_pos = 0.0;
                                continue;
                            } else {
                                active = false;
                                break;
                            }
                        }
                        stream_chunk_frames = static_cast<usize>(got);
                        stream_chunk_pos = 0.0;
                    }

                    if (has_loop_points && lp_end > lp_start && frame_pos >= lp_end && do_loop) {
                        if (consume_loop_iteration()) {
                            ma_decoder_seek_to_pcm_frame(&ma_dec, static_cast<ma_uint64>(lp_start));
                            frame_pos = lp_start;
                            stream_chunk_frames = 0;
                            stream_chunk_pos = 0.0;
                            continue;
                        }
                    }

                    const usize i0 = std::min<usize>(static_cast<usize>(stream_chunk_pos), stream_chunk_frames - 1u);
                    const usize i1 = std::min<usize>(i0 + 1u, stream_chunk_frames - 1u);
                    const f32 frac = static_cast<f32>(stream_chunk_pos - static_cast<f64>(i0));
                    out_stereo[written * 2u + 0] = stream_chunk[i0 * 2u + 0] * (1.0f - frac) + stream_chunk[i1 * 2u + 0] * frac;
                    out_stereo[written * 2u + 1] = stream_chunk[i0 * 2u + 1] * (1.0f - frac) + stream_chunk[i1 * 2u + 1] * frac;
                    stream_chunk_pos += rate_mult;
                    frame_pos += rate_mult;
                    ++written;
                }
                return written;
            }

            case AudioSourceKind::Tracker: {
                if (!tracker_player) { active = false; return 0; }
                if (!is_vanilla && (rate_mult != 1.0 || synth_tail_frames || synth_fraction != 0.0)) {
                    return pull_pitched_synth(out_stereo, frame_count, rate_mult, do_loop);
                }
                const u32 got = tracker_player->render(out_stereo, frame_count, do_loop);
                if (tracker_player->finished()) {
                    if (consume_loop_iteration()) {
                        tracker_player->reset();
                    } else {
                        active = false;
                    }
                }
                return got;
            }

            case AudioSourceKind::Midi: {
                if (!midi_player) { active = false; return 0; }
                if (!is_vanilla && (rate_mult != 1.0 || synth_tail_frames || synth_fraction != 0.0)) {
                    return pull_pitched_synth(out_stereo, frame_count, rate_mult, do_loop);
                }
                midi_player->set_tempo_scale(rate_mult);
                const u32 got = midi_player->render(out_stereo, frame_count, do_loop);
                if (midi_player->finished()) {
                    if (consume_loop_iteration()) {
                        midi_player->reset();
                    } else {
                        active = false;
                    }
                }
                return got;
            }

            default:
                active = false;
                return 0;
        }
    }
};

struct AudioEngine::Impl {
    ma_device device{};
    bool device_initialized = false;
    bool device_started = false;
    std::atomic<bool> output_muted{false};
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {
    mix_ch_buf_.resize(4096u * 2u, 0.0f);
    for (auto& gb : mix_group_bufs_) {
        gb.resize(4096u * 2u, 0.0f);
    }
}

AudioEngine::~AudioEngine() {
    shutdown();
}

Result<void> AudioEngine::init() {
    if (initialized_) {
        return Result<void>::success();
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = kOutputSampleRate;
#if defined(__EMSCRIPTEN__)
    // WebAudio mixes on the browser's main thread. Allow about one 50 Hz game frame per callback.
    cfg.periodSizeInFrames = 1024;
#endif
    cfg.dataCallback = &ma_data_callback;
    cfg.pUserData = this;

    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        GM_LOG_WARN("audio", "Failed to initialize miniaudio device; continuing in silent mode");
        initialized_ = true;
        return Result<void>::success();
    }
    impl_->device_initialized = true;

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
#if defined(__EMSCRIPTEN__)
        // Browsers suspend WebAudio until the first user gesture; resume() will start it.
        GM_LOG_INFO("audio", "Audio device waiting for browser user gesture to resume");
        initialized_ = true;
        return Result<void>::success();
#else
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
        GM_LOG_WARN("audio", "Failed to start miniaudio device; continuing in silent mode");
        initialized_ = true;
        return Result<void>::success();
#endif
    }

    impl_->device_started = true;
    initialized_ = true;
    GM_LOG_INFO("audio", "Initialized unified software audio mixer (%u Hz stereo f32)", kOutputSampleRate);
    return Result<void>::success();
}

void AudioEngine::suspend() noexcept {
    if (!initialized_ || !impl_ || !impl_->device_initialized || !impl_->device_started) return;
    if (ma_device_stop(&impl_->device) == MA_SUCCESS) {
        impl_->device_started = false;
    }
}

void AudioEngine::resume() noexcept {
    if (!initialized_ || !impl_ || !impl_->device_initialized) return;
    if (!impl_->device_started) {
        if (ma_device_start(&impl_->device) == MA_SUCCESS) {
            impl_->device_started = true;
        }
    }
}

void AudioEngine::set_output_muted(bool muted) noexcept {
    if (impl_) {
        impl_->output_muted.store(muted, std::memory_order_relaxed);
    }
}

bool AudioEngine::is_output_muted() const noexcept {
    return impl_ && impl_->output_muted.load(std::memory_order_relaxed);
}

void AudioEngine::shutdown() noexcept {
    if (!initialized_) return;
    if (impl_ && impl_->device_initialized) {
        ma_device_uninit(&impl_->device);
        impl_->device_started = false;
        impl_->device_initialized = false;
    }
    std::scoped_lock lock(mutex_);
    channels_.clear();
    dsps_.clear();
    ext_sounds_.clear();
    vanilla_sounds_.clear();
    blockers_.clear();
    if (soundfont_) {
        tsf_close(soundfont_);
        soundfont_ = nullptr;
    }
    initialized_ = false;
}

bool AudioEngine::load_soundfont(std::span<const u8> sf2_bytes) {
    if (sf2_bytes.empty()) return false;
    tsf* sf = tsf_load_memory(sf2_bytes.data(), static_cast<int>(sf2_bytes.size()));
    if (!sf) return false;
    std::scoped_lock lock(mutex_);
    if (soundfont_) tsf_close(soundfont_);
    soundfont_ = sf;
    GM_LOG_INFO("audio", "Loaded SF2 SoundFont (%zu bytes, %d presets)", sf2_bytes.size(), tsf_get_presetcount(sf));
    return true;
}

bool AudioEngine::decode_asset_from_memory(
    std::span<const u8> encoded,
    bool is_3d,
    bool streamed,
    SoundAsset& out_asset,
    std::string_view /*hint*/) {
    out_asset = SoundAsset{};
    out_asset.exists = true;
    out_asset.is_3d = is_3d;
    out_asset.is_streamed = streamed;
    if (encoded.size() < 4) return false;

    // 1. MIDI ("MThd" or RIFF RMID)
    if (is_midi_data(encoded)) {
        auto seq = load_midi_sequence(encoded);
        if (!seq) return false;
        out_asset.source_kind = AudioSourceKind::Midi;
        out_asset.midi_seq = std::move(seq.value());
        out_asset.orig_sample_rate = kOutputSampleRate;
        out_asset.orig_channels = 2;
        out_asset.length_ms = out_asset.midi_seq->duration_ms;
        out_asset.total_frames = static_cast<u64>(out_asset.length_ms * 0.001 * kOutputSampleRate);
        out_asset.source_frames = out_asset.total_frames;
        out_asset.loaded = true;
        return true;
    }

    // 2. OGG Vorbis ("OggS")
    if (encoded[0] == 'O' && encoded[1] == 'g' && encoded[2] == 'g' && encoded[3] == 'S') {
        int err = 0;
        stb_vorbis* vorb = stb_vorbis_open_memory(encoded.data(), static_cast<int>(encoded.size()), &err, nullptr);
        if (!vorb) return false;
        const stb_vorbis_info info = stb_vorbis_get_info(vorb);
        const u32 total_s = stb_vorbis_stream_length_in_samples(vorb);
        out_asset.orig_sample_rate = info.sample_rate > 0 ? info.sample_rate : kOutputSampleRate;
        out_asset.orig_channels = info.channels > 0 ? static_cast<u32>(info.channels) : 2u;
        // SuperSound decodes Vorbis to signed 16-bit samples in its DirectSound buffer.
        out_asset.source_bytes_per_frame = out_asset.orig_channels * sizeof(i16);
        out_asset.source_frames = total_s;
        out_asset.length_ms = static_cast<f64>(total_s) * 1000.0 / out_asset.orig_sample_rate;
        parse_vorbis_comments(vorb, out_asset.orig_sample_rate, out_asset);

        if (streamed || encoded.size() > kStreamThresholdOgg) {
            out_asset.source_kind = AudioSourceKind::OggStream;
            out_asset.stream_raw = std::make_shared<std::vector<u8>>(encoded.begin(), encoded.end());
            out_asset.total_frames = total_s;
            stb_vorbis_close(vorb);
            out_asset.loaded = true;
            return true;
        }

        // Eager decode short OGG to stereo f32 @ kOutputSampleRate
        std::vector<f32> raw_stereo;
        if (total_s > 0) raw_stereo.reserve(static_cast<usize>(total_s) * 2u);
        std::array<f32, 4096 * 2> chunk{};
        for (;;) {
            const int got = read_vorbis_stereo(vorb, chunk);
            if (got <= 0) break;
            raw_stereo.insert(raw_stereo.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got * 2));
        }
        stb_vorbis_close(vorb);
        if (raw_stereo.empty()) return false;

        auto pcm = std::make_shared<std::vector<f32>>();
        resample_stereo_linear(raw_stereo.data(), raw_stereo.size() / 2u, out_asset.orig_sample_rate, *pcm, kOutputSampleRate);
        out_asset.source_kind = AudioSourceKind::PcmStereo;
        out_asset.total_frames = pcm->size() / 2u;
        out_asset.loaded = !pcm->empty();
        out_asset.pcm_stereo = std::move(pcm);
        return out_asset.loaded;
    }

    // 3. Tracker modules (MOD / S3M / XM / IT)
    if (detect_tracker_format(encoded) != TrackerFormat::Unknown) {
        auto mod_res = load_tracker_module(encoded);
        if (mod_res) {
            out_asset.source_kind = AudioSourceKind::Tracker;
            out_asset.tracker_mod = std::move(mod_res.value());
            out_asset.orig_sample_rate = kOutputSampleRate;
            out_asset.orig_channels = 2;
            out_asset.length_ms = out_asset.tracker_mod->estimated_duration_ms;
            out_asset.total_frames = static_cast<u64>(out_asset.length_ms * 0.001 * kOutputSampleRate);
            out_asset.source_frames = out_asset.total_frames;
            out_asset.tracker_channel_vols.assign(out_asset.tracker_mod->num_channels, 1.0f);
            if (!out_asset.tracker_mod->title.empty()) {
                out_asset.tags.push_back(AudioTag{"TITLE", out_asset.tracker_mod->title});
            }
            out_asset.loaded = true;
            return true;
        }
    }

    // 4. miniaudio built-in decoders (WAV, MP3, FLAC)
    // Ask the decoder for native PCM metadata before configuring the stereo
    // mixer output. The converted format cannot supply extension byte offsets.
    ma_decoder_config dec_cfg = ma_decoder_config_init(ma_format_unknown, 0, 0);
    ma_decoder decoder{};
    if (ma_decoder_init_memory(encoded.data(), encoded.size(), &dec_cfg, &decoder) != MA_SUCCESS) {
        return false;
    }

    ma_format orig_fmt = ma_format_unknown;
    ma_uint32 orig_ch = 2;
    ma_uint32 orig_sr = kOutputSampleRate;
    if (ma_decoder_get_data_format(&decoder, &orig_fmt, &orig_ch, &orig_sr, nullptr, 0) != MA_SUCCESS ||
        orig_sr == 0 || orig_ch == 0) {
        ma_decoder_uninit(&decoder);
        return false;
    }
    ma_uint64 len_frames = 0;
    (void)ma_decoder_get_length_in_pcm_frames(&decoder, &len_frames);
    ma_decoder_uninit(&decoder);

    out_asset.orig_sample_rate = orig_sr;
    out_asset.orig_channels = orig_ch;
    out_asset.source_bytes_per_frame = ma_get_bytes_per_frame(orig_fmt, orig_ch);
    out_asset.source_frames = len_frames;
    out_asset.length_ms = static_cast<f64>(len_frames) * 1000.0 / orig_sr;

    if (streamed || encoded.size() > kStreamThresholdGeneric) {
        out_asset.source_kind = AudioSourceKind::MaStream;
        out_asset.stream_raw = std::make_shared<std::vector<u8>>(encoded.begin(), encoded.end());
        out_asset.total_frames = static_cast<u64>(std::ceil(static_cast<f64>(len_frames) * kOutputSampleRate / orig_sr));
        out_asset.loaded = true;
        return true;
    }

    dec_cfg = ma_decoder_config_init(ma_format_f32, 2, kOutputSampleRate);
    if (ma_decoder_init_memory(encoded.data(), encoded.size(), &dec_cfg, &decoder) != MA_SUCCESS) return false;
    auto pcm = std::make_shared<std::vector<f32>>();
    constexpr usize kChunkFrames = 4096;
    std::vector<f32> chunk(kChunkFrames * 2u);
    for (;;) {
        ma_uint64 frames_read = 0;
        const ma_result res = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &frames_read);
        if (frames_read > 0) {
            pcm->insert(pcm->end(),
                        chunk.begin(),
                        chunk.begin() + static_cast<std::ptrdiff_t>(frames_read * 2u));
        }
        if (res != MA_SUCCESS || frames_read < kChunkFrames) break;
    }
    ma_decoder_uninit(&decoder);

    out_asset.source_kind = AudioSourceKind::PcmStereo;
    out_asset.total_frames = pcm->size() / 2u;
    if (len_frames == 0) {
        out_asset.source_frames = static_cast<u64>(std::llround(static_cast<f64>(out_asset.total_frames) * orig_sr / kOutputSampleRate));
        out_asset.length_ms = static_cast<f64>(out_asset.total_frames) * 1000.0 / kOutputSampleRate;
    }
    out_asset.loaded = !pcm->empty();
    out_asset.pcm_stereo = std::move(pcm);
    return out_asset.loaded;
}

// ---------------------------------------------------------------------------
// Vanilla GM8 Sound API
// ---------------------------------------------------------------------------

void AudioEngine::load_game_sounds(const GameData& game) {
    std::scoped_lock lock(mutex_);
    vanilla_sounds_.clear();
    vanilla_sounds_.resize(game.sounds.size());

    usize decoded_ok = 0;
    usize active_with_data = 0;
    for (usize i = 0; i < game.sounds.size(); ++i) {
        const auto& src = game.sounds[i];
        if (!src.exists) continue;
        auto& dst = vanilla_sounds_[i];
        dst.exists = true;
        dst.gm_kind = src.kind;
        dst.group_id = static_cast<i32>(AudioGroupId::Vanilla);
        dst.volume = static_cast<f32>(std::clamp(src.volume, 0.0, 1.0));
        dst.max_volume = dst.volume;
        dst.pan = static_cast<f32>(std::clamp(src.pan, -1.0, 1.0));
        dst.effects_mask = static_cast<u32>(src.effects);
        dst.is_3d = (src.kind == 2);

        if (!src.data.empty()) {
            ++active_with_data;
            SoundAsset decoded{};
            if (decode_asset_from_memory(src.data, src.kind == 2, src.kind == 1 || src.kind == 3, decoded, src.extension)) {
                decoded.gm_kind = src.kind;
                decoded.group_id = static_cast<i32>(AudioGroupId::Vanilla);
                decoded.volume = static_cast<f32>(std::clamp(src.volume, 0.0, 1.0));
                decoded.max_volume = decoded.volume;
                decoded.pan = static_cast<f32>(std::clamp(src.pan, -1.0, 1.0));
                decoded.effects_mask = static_cast<u32>(src.effects);
                dst = std::move(decoded);
                ++decoded_ok;
            } else {
                GM_LOG_DEBUG("audio", "Sound[%zu] '%s' (%s, %zu bytes) not decoded",
                             i, src.name.c_str(), src.extension.c_str(), src.data.size());
            }
        }
    }
    GM_LOG_INFO("audio", "Loaded %zu / %zu embedded game sounds", decoded_ok, active_with_data);
}

i32 AudioEngine::add_sound_from_memory(std::span<const u8> encoded, i32 kind, std::string_view ext_hint) {
    SoundAsset asset{};
    (void)decode_asset_from_memory(encoded, kind == 2, kind == 1 || kind == 3, asset, ext_hint);
    asset.exists = true;
    asset.gm_kind = kind;
    asset.group_id = static_cast<i32>(AudioGroupId::Vanilla);
    std::scoped_lock lock(mutex_);
    const i32 id = static_cast<i32>(vanilla_sounds_.size());
    vanilla_sounds_.push_back(std::move(asset));
    return id;
}

bool AudioEngine::replace_sound_from_memory(i32 sound_id, std::span<const u8> encoded, i32 kind, std::string_view ext_hint) {
    if (sound_id < 0) return false;
    SoundAsset asset{};
    const bool ok = decode_asset_from_memory(encoded, kind == 2, kind == 1 || kind == 3, asset, ext_hint);
    asset.exists = true;
    asset.gm_kind = kind;
    asset.group_id = static_cast<i32>(AudioGroupId::Vanilla);
    std::scoped_lock lock(mutex_);
    if (static_cast<usize>(sound_id) >= vanilla_sounds_.size()) return false;
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second->is_vanilla && it->second->sound_id == sound_id) {
            for (i32 d : it->second->dsp_ids) dsps_.erase(d);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
    vanilla_sounds_[static_cast<usize>(sound_id)] = std::move(asset);
    return ok;
}

void AudioEngine::delete_sound(i32 sound_id) {
    std::scoped_lock lock(mutex_);
    if (sound_id < 0 || static_cast<usize>(sound_id) >= vanilla_sounds_.size()) return;
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second->is_vanilla && it->second->sound_id == sound_id) {
            for (i32 d : it->second->dsp_ids) dsps_.erase(d);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
    vanilla_sounds_[static_cast<usize>(sound_id)] = SoundAsset{};
}

bool AudioEngine::sound_exists(i32 sound_id) const noexcept {
    std::scoped_lock lock(mutex_);
    return sound_id >= 0 && static_cast<usize>(sound_id) < vanilla_sounds_.size() &&
           vanilla_sounds_[static_cast<usize>(sound_id)].exists;
}

void AudioEngine::sound_discard(i32 /*sound_id*/) {}
void AudioEngine::sound_restore(i32 /*sound_id*/) {}

void AudioEngine::attach_asset_effects_to_channel(const SoundAsset& asset, AudioChannel& ch) {
    auto add_fx = [&](DspEffectType t) {
        if (t == DspEffectType::None) return;
        const i32 did = next_dsp_id_++;
        dsps_[did] = std::make_unique<DspEffect>(t, kOutputSampleRate);
        ch.dsp_ids.push_back(did);
    };
    if (asset.effects_mask != 0) {
        if (asset.effects_mask & 1u) add_fx(DspEffectType::Chorus);
        if (asset.effects_mask & 2u) add_fx(DspEffectType::Echo);
        if (asset.effects_mask & 4u) add_fx(DspEffectType::Flange);
        if (asset.effects_mask & 8u) add_fx(DspEffectType::Gargle);
        if (asset.effects_mask & 16u) add_fx(DspEffectType::SfxReverb);
        if (asset.effects_mask & 32u) add_fx(DspEffectType::Compressor);
        if (asset.effects_mask & 64u) add_fx(DspEffectType::ParamEq);
    }
    for (DspEffectType t : asset.extra_effects) {
        if (t != DspEffectType::None) add_fx(t);
    }
}

void AudioEngine::play_vanilla_internal(i32 sound_id, bool loop) {
    std::scoped_lock lock(mutex_);
    if (sound_id < 0 || static_cast<usize>(sound_id) >= vanilla_sounds_.size()) return;
    const auto& snd = vanilla_sounds_[static_cast<usize>(sound_id)];
    if (!snd.exists || !snd.loaded) return;

    // GM8 kind 1 (background music): stop other kind-1 music first.
    if (snd.gm_kind == 1) {
        for (auto it = channels_.begin(); it != channels_.end();) {
            if (it->second->is_vanilla &&
                it->second->sound_id >= 0 &&
                static_cast<usize>(it->second->sound_id) < vanilla_sounds_.size() &&
                vanilla_sounds_[static_cast<usize>(it->second->sound_id)].gm_kind == 1) {
                for (i32 d : it->second->dsp_ids) dsps_.erase(d);
                it = channels_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto ch = std::make_unique<AudioChannel>();
    if (!ch->open_stream_decoder(snd, soundfont_)) return;
    ch->id = next_channel_id_++;
    ch->is_vanilla = true;
    ch->sound_id = sound_id;
    ch->group_id = static_cast<i32>(AudioGroupId::Vanilla);
    ch->active = true;
    ch->paused = false;
    ch->looping = loop;
    ch->loop_count = loop ? -1 : 0;
    ch->volume = snd.volume;
    ch->max_volume = 1.0f;
    ch->fade_target = snd.volume;
    ch->pan = snd.pan;
    ch->pitch = (snd.gm_kind == 1) ? vanilla_bg_tempo_ : 1.0f;
    ch->is_3d = snd.is_3d;
    std::memcpy(ch->pos, snd.pos3d, sizeof(ch->pos));
    std::memcpy(ch->vel, snd.vel3d, sizeof(ch->vel));
    ch->min_dist = snd.min_dist;
    ch->max_dist = snd.max_dist;
    ch->cone_inside = snd.cone_inside;
    ch->cone_outside = snd.cone_outside;
    ch->cone_outside_vol = snd.cone_outside_vol;
    std::memcpy(ch->cone_dir, snd.cone_dir, sizeof(ch->cone_dir));
    ch->has_loop_points = snd.has_loop_points;
    ch->loop_start_ms = snd.loop_start_ms;
    ch->loop_end_ms = snd.loop_end_ms;
    attach_asset_effects_to_channel(snd, *ch);

    const i32 cid = ch->id;
    channels_[cid] = std::move(ch);
}

void AudioEngine::sound_play(i32 sound_id) { play_vanilla_internal(sound_id, false); }
void AudioEngine::sound_loop(i32 sound_id) { play_vanilla_internal(sound_id, true); }

void AudioEngine::sound_stop(i32 sound_id) {
    std::scoped_lock lock(mutex_);
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second->is_vanilla && it->second->sound_id == sound_id) {
            for (i32 d : it->second->dsp_ids) dsps_.erase(d);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioEngine::sound_stop_all() {
    std::scoped_lock lock(mutex_);
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second->is_vanilla) {
            for (i32 d : it->second->dsp_ids) dsps_.erase(d);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
}

bool AudioEngine::sound_isplaying(i32 sound_id) const {
    std::scoped_lock lock(mutex_);
    for (const auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->active && ch->sound_id == sound_id) return true;
    }
    return false;
}

void AudioEngine::sound_volume(i32 sound_id, f32 vol) {
    const f32 clamped = std::clamp(vol, 0.0f, 1.0f);
    std::scoped_lock lock(mutex_);
    if (sound_id >= 0 && static_cast<usize>(sound_id) < vanilla_sounds_.size()) {
        vanilla_sounds_[static_cast<usize>(sound_id)].volume = clamped;
    }
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->active && ch->sound_id == sound_id) {
            ch->volume = clamped;
            ch->fade_target = clamped;
            ch->fade_rate_per_sec = 0.0f;
        }
    }
}

void AudioEngine::sound_pan(i32 sound_id, f32 pan) {
    const f32 clamped = std::clamp(pan, -1.0f, 1.0f);
    std::scoped_lock lock(mutex_);
    if (sound_id >= 0 && static_cast<usize>(sound_id) < vanilla_sounds_.size()) {
        vanilla_sounds_[static_cast<usize>(sound_id)].pan = clamped;
    }
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->active && ch->sound_id == sound_id) {
            ch->pan = clamped;
        }
    }
}

void AudioEngine::sound_fade(i32 sound_id, f32 target_vol, f32 duration_ms) {
    const f32 target = std::clamp(target_vol, 0.0f, 1.0f);
    std::scoped_lock lock(mutex_);
    if (duration_ms <= 0.0f) {
        if (sound_id >= 0 && static_cast<usize>(sound_id) < vanilla_sounds_.size()) {
            vanilla_sounds_[static_cast<usize>(sound_id)].volume = target;
        }
        for (auto& [cid, ch] : channels_) {
            if (ch->is_vanilla && ch->active && ch->sound_id == sound_id) {
                ch->volume = target;
                ch->fade_target = target;
                ch->fade_rate_per_sec = 0.0f;
            }
        }
        return;
    }
    const f32 dur_sec = duration_ms * 0.001f;
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->active && ch->sound_id == sound_id) {
            ch->fade_target = target;
            ch->fade_rate_per_sec = (target - ch->volume) / dur_sec;
        }
    }
}

void AudioEngine::sound_global_volume(f32 vol) {
    std::scoped_lock lock(mutex_);
    vanilla_global_volume_ = std::clamp(vol, 0.0f, 1.0f);
    groups_[static_cast<usize>(AudioGroupId::Vanilla)].volume = vanilla_global_volume_;
}

void AudioEngine::sound_background_tempo(f32 factor) {
    std::scoped_lock lock(mutex_);
    vanilla_bg_tempo_ = std::clamp(factor, 0.05f, 8.0f);
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->sound_id >= 0 &&
            static_cast<usize>(ch->sound_id) < vanilla_sounds_.size() &&
            vanilla_sounds_[static_cast<usize>(ch->sound_id)].gm_kind == 1) {
            ch->pitch = vanilla_bg_tempo_;
        }
    }
}

void AudioEngine::sound_set_search_directory(std::string_view dir) {
    std::scoped_lock lock(mutex_);
    search_directory_ = std::string(dir);
}

std::string AudioEngine::sound_search_directory() const {
    std::scoped_lock lock(mutex_);
    return search_directory_;
}

void AudioEngine::sound_effect_set(i32 sound_id, u32 effects_mask) {
    std::scoped_lock lock(mutex_);
    if (sound_id < 0 || static_cast<usize>(sound_id) >= vanilla_sounds_.size()) return;
    vanilla_sounds_[static_cast<usize>(sound_id)].effects_mask = effects_mask;
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->sound_id == sound_id) {
            for (i32 d : ch->dsp_ids) dsps_.erase(d);
            ch->dsp_ids.clear();
            attach_asset_effects_to_channel(vanilla_sounds_[static_cast<usize>(sound_id)], *ch);
        }
    }
}

void AudioEngine::sound_effect_chorus(i32 sound_id, f32 wetdry, f32 /*depth*/, f32 /*feedback*/, f32 freq, i32 /*wave*/, f32 delay, i32 /*phase*/) {
    std::scoped_lock lock(mutex_);
    for (auto& [cid, ch] : channels_) {
        if (!ch->is_vanilla || ch->sound_id != sound_id) continue;
        for (i32 did : ch->dsp_ids) {
            if (auto it = dsps_.find(did); it != dsps_.end() && it->second->type() == DspEffectType::Chorus) {
                const f32 wet = std::clamp(wetdry * 0.01f, 0.0f, 1.0f);
                it->second->set_param(0, 1.0f - wet * 0.5f);
                it->second->set_param(1, wet);
                it->second->set_param(4, delay);
                it->second->set_param(5, freq);
            }
        }
    }
}

void AudioEngine::sound_effect_compressor(i32 sound_id, f32 gain, f32 attack, f32 release, f32 thresh, f32 /*ratio*/, f32 /*delay*/) {
    std::scoped_lock lock(mutex_);
    for (auto& [cid, ch] : channels_) {
        if (!ch->is_vanilla || ch->sound_id != sound_id) continue;
        for (i32 did : ch->dsp_ids) {
            if (auto it = dsps_.find(did); it != dsps_.end() && it->second->type() == DspEffectType::Compressor) {
                it->second->set_param(0, thresh);
                it->second->set_param(1, attack);
                it->second->set_param(2, release);
                it->second->set_param(3, gain);
            }
        }
    }
}

void AudioEngine::sound_effect_echo(i32 sound_id, f32 wetdry, f32 feedback, f32 leftdelay, f32 /*rightdelay*/, i32 /*pandelay*/) {
    std::scoped_lock lock(mutex_);
    for (auto& [cid, ch] : channels_) {
        if (!ch->is_vanilla || ch->sound_id != sound_id) continue;
        for (i32 did : ch->dsp_ids) {
            if (auto it = dsps_.find(did); it != dsps_.end() && it->second->type() == DspEffectType::Echo) {
                const f32 wet = std::clamp(wetdry * 0.01f, 0.0f, 1.0f);
                it->second->set_param(0, leftdelay);
                it->second->set_param(1, std::clamp(feedback * 0.01f, 0.0f, 0.95f));
                it->second->set_param(3, 1.0f - wet * 0.5f);
                it->second->set_param(4, wet);
            }
        }
    }
}

void AudioEngine::sound_effect_flanger(i32 sound_id, f32 wetdry, f32 depth, f32 /*feedback*/, f32 freq, i32 /*wave*/, f32 /*delay*/, i32 /*phase*/) {
    std::scoped_lock lock(mutex_);
    for (auto& [cid, ch] : channels_) {
        if (!ch->is_vanilla || ch->sound_id != sound_id) continue;
        for (i32 did : ch->dsp_ids) {
            if (auto it = dsps_.find(did); it != dsps_.end() && it->second->type() == DspEffectType::Flange) {
                const f32 wet = std::clamp(wetdry * 0.01f, 0.0f, 1.0f);
                it->second->set_param(0, 1.0f - wet * 0.5f);
                it->second->set_param(1, wet);
                it->second->set_param(2, std::clamp(depth * 0.01f, 0.01f, 1.0f));
                it->second->set_param(3, freq);
            }
        }
    }
}

void AudioEngine::sound_effect_gargle(i32 sound_id, f32 rate, i32 wave) {
    std::scoped_lock lock(mutex_);
    for (auto& [cid, ch] : channels_) {
        if (!ch->is_vanilla || ch->sound_id != sound_id) continue;
        for (i32 did : ch->dsp_ids) {
            if (auto it = dsps_.find(did); it != dsps_.end() && it->second->type() == DspEffectType::Gargle) {
                it->second->set_param(0, rate);
                it->second->set_param(1, static_cast<f32>(wave != 0 ? 1 : 0));
            }
        }
    }
}

void AudioEngine::sound_effect_equalizer(i32 sound_id, f32 center, f32 bandwidth, f32 gain) {
    std::scoped_lock lock(mutex_);
    for (auto& [cid, ch] : channels_) {
        if (!ch->is_vanilla || ch->sound_id != sound_id) continue;
        for (i32 did : ch->dsp_ids) {
            if (auto it = dsps_.find(did); it != dsps_.end() && it->second->type() == DspEffectType::ParamEq) {
                it->second->set_param(0, center);
                it->second->set_param(1, std::clamp(bandwidth / 12.0f, 0.2f, 5.0f));
                it->second->set_param(2, std::pow(10.0f, gain / 20.0f));
            }
        }
    }
}

void AudioEngine::sound_effect_reverb(i32 sound_id, f32 gain, f32 mix, f32 time, f32 ratio) {
    std::scoped_lock lock(mutex_);
    for (auto& [cid, ch] : channels_) {
        if (!ch->is_vanilla || ch->sound_id != sound_id) continue;
        for (i32 did : ch->dsp_ids) {
            if (auto it = dsps_.find(did); it != dsps_.end() &&
                (it->second->type() == DspEffectType::SfxReverb || it->second->type() == DspEffectType::Reverb)) {
                it->second->set_param(0, gain);
                it->second->set_param(1, mix);
                it->second->set_param(2, time * 0.001f);
                it->second->set_param(3, std::clamp(ratio, 0.0f, 1.0f));
            }
        }
    }
}

void AudioEngine::sound_3d_set_position(i32 sound_id, f32 x, f32 y, f32 z) {
    std::scoped_lock lock(mutex_);
    if (sound_id >= 0 && static_cast<usize>(sound_id) < vanilla_sounds_.size()) {
        auto& s = vanilla_sounds_[static_cast<usize>(sound_id)];
        s.pos3d[0] = x; s.pos3d[1] = y; s.pos3d[2] = z;
    }
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->sound_id == sound_id) {
            ch->pos[0] = x; ch->pos[1] = y; ch->pos[2] = z;
        }
    }
}

void AudioEngine::sound_3d_set_velocity(i32 sound_id, f32 vx, f32 vy, f32 vz) {
    std::scoped_lock lock(mutex_);
    if (sound_id >= 0 && static_cast<usize>(sound_id) < vanilla_sounds_.size()) {
        auto& s = vanilla_sounds_[static_cast<usize>(sound_id)];
        s.vel3d[0] = vx; s.vel3d[1] = vy; s.vel3d[2] = vz;
    }
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->sound_id == sound_id) {
            ch->vel[0] = vx; ch->vel[1] = vy; ch->vel[2] = vz;
        }
    }
}

void AudioEngine::sound_3d_set_distance(i32 sound_id, f32 mindist, f32 maxdist) {
    std::scoped_lock lock(mutex_);
    if (sound_id >= 0 && static_cast<usize>(sound_id) < vanilla_sounds_.size()) {
        auto& s = vanilla_sounds_[static_cast<usize>(sound_id)];
        s.min_dist = std::max(0.01f, mindist);
        s.max_dist = std::max(s.min_dist, maxdist);
    }
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->sound_id == sound_id) {
            ch->min_dist = std::max(0.01f, mindist);
            ch->max_dist = std::max(ch->min_dist, maxdist);
        }
    }
}

void AudioEngine::sound_3d_set_cone(i32 sound_id, f32 x, f32 y, f32 z, f32 anglein, f32 angleout, f32 voloutside) {
    std::scoped_lock lock(mutex_);
    if (sound_id >= 0 && static_cast<usize>(sound_id) < vanilla_sounds_.size()) {
        auto& s = vanilla_sounds_[static_cast<usize>(sound_id)];
        s.cone_dir[0] = x; s.cone_dir[1] = y; s.cone_dir[2] = z;
        s.cone_inside = anglein; s.cone_outside = angleout; s.cone_outside_vol = voloutside;
    }
    for (auto& [cid, ch] : channels_) {
        if (ch->is_vanilla && ch->sound_id == sound_id) {
            ch->cone_dir[0] = x; ch->cone_dir[1] = y; ch->cone_dir[2] = z;
            ch->cone_inside = anglein; ch->cone_outside = angleout; ch->cone_outside_vol = voloutside;
        }
    }
}

// ---------------------------------------------------------------------------
// Unified External Audio API
// ---------------------------------------------------------------------------

i32 AudioEngine::ext_load_from_memory(
    std::span<const u8> encoded,
    bool is_3d,
    bool streamed,
    i32 default_group_id,
    std::string_view hint) {
    SoundAsset asset{};
    if (!decode_asset_from_memory(encoded, is_3d, streamed, asset, hint)) {
        return 0;
    }
    asset.group_id = std::clamp(default_group_id, 0, static_cast<i32>(AudioGroupId::Count) - 1);
    std::scoped_lock lock(mutex_);
    const i32 id = next_ext_sound_id_++;
    ext_sounds_[id] = std::move(asset);
    return id;
}

bool AudioEngine::ext_free_sound(i32 ext_id) {
    std::scoped_lock lock(mutex_);
    auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    for (auto cit = channels_.begin(); cit != channels_.end();) {
        if (!cit->second->is_vanilla && cit->second->sound_id == ext_id) {
            for (i32 d : cit->second->dsp_ids) dsps_.erase(d);
            cit = channels_.erase(cit);
        } else {
            ++cit;
        }
    }
    ext_sounds_.erase(it);
    return true;
}

bool AudioEngine::ext_sound_exists(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    return ext_sounds_.contains(ext_id);
}

f64 AudioEngine::ext_sound_length_ms(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() ? it->second.length_ms : 0.0;
}

u32 AudioEngine::ext_sound_sample_rate(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() ? it->second.orig_sample_rate : kOutputSampleRate;
}

u32 AudioEngine::ext_sound_channels(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() ? it->second.orig_channels : 0u;
}

u32 AudioEngine::ext_sound_bytes_per_frame(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() ? it->second.source_bytes_per_frame : 0;
}

u64 AudioEngine::ext_sound_length_bytes(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() ? it->second.source_frames * it->second.source_bytes_per_frame : 0;
}

bool AudioEngine::ext_sound_is_3d(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() && it->second.is_3d;
}

bool AudioEngine::ext_sound_is_streamed(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() && it->second.is_streamed;
}

bool AudioEngine::ext_sound_set_group(i32 ext_id, i32 group_id) {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.group_id = group_id;
    return true;
}

bool AudioEngine::ext_sound_set_max_volume(i32 ext_id, f32 vol) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.max_volume = std::clamp(vol, 0.0f, 1.0f);
    return true;
}

f32 AudioEngine::ext_sound_get_max_volume(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() ? it->second.max_volume : 0.0f;
}

bool AudioEngine::ext_sound_set_loop_count(i32 ext_id, i32 count) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.loop_count = count;
    return true;
}

i32 AudioEngine::ext_sound_get_loop_count(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() ? it->second.loop_count : 0;
}

bool AudioEngine::ext_sound_set_loop_points_ms(i32 ext_id, f64 start_ms, f64 end_ms) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.has_loop_points = true;
    it->second.loop_start_ms = std::max(0.0, start_ms);
    it->second.loop_end_ms = std::max(start_ms + 1.0, end_ms);
    return true;
}

bool AudioEngine::ext_sound_clear_loop_points(i32 ext_id) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.has_loop_points = false;
    return true;
}

bool AudioEngine::ext_sound_set_3d_min_max(i32 ext_id, f32 min_d, f32 max_d) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.min_dist = std::max(0.01f, min_d);
    it->second.max_dist = std::max(it->second.min_dist, max_d);
    return true;
}

f32 AudioEngine::ext_sound_get_max_dist(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    return it != ext_sounds_.end() ? it->second.max_dist : 0.0f;
}

bool AudioEngine::ext_sound_set_3d_doppler(i32 ext_id, f32 doppler) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.doppler_level = std::clamp(doppler, 0.0f, 5.0f);
    return true;
}

bool AudioEngine::ext_sound_set_3d_cone(i32 ext_id, f32 inside_deg, f32 outside_deg, f32 outside_vol) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.cone_inside = inside_deg;
    it->second.cone_outside = outside_deg;
    it->second.cone_outside_vol = std::clamp(outside_vol, 0.0f, 1.0f);
    return true;
}

bool AudioEngine::ext_sound_set_effects_mask(i32 ext_id, u32 mask) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.effects_mask = mask;
    return true;
}

bool AudioEngine::ext_sound_add_effect(i32 ext_id, DspEffectType type, i32 slot_pos) {
    if (slot_pos < 0 || slot_pos >= 10) return false;
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return false;
    it->second.extra_effects[static_cast<usize>(slot_pos)] = type;
    return true;
}

i32 AudioEngine::ext_sound_get_music_num_channels(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end() || !it->second.tracker_mod) return 0;
    return static_cast<i32>(it->second.tracker_mod->num_channels);
}

f32 AudioEngine::ext_sound_get_music_channel_volume(i32 ext_id, i32 ch) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end() || ch < 0 || static_cast<usize>(ch) >= it->second.tracker_channel_vols.size()) return 0.0f;
    return it->second.tracker_channel_vols[static_cast<usize>(ch)];
}

bool AudioEngine::ext_sound_set_music_channel_volume(i32 ext_id, i32 ch, f32 vol) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end() || ch < 0 || static_cast<usize>(ch) >= it->second.tracker_channel_vols.size()) return false;
    const f32 clamped = std::clamp(vol, 0.0f, 1.0f);
    it->second.tracker_channel_vols[static_cast<usize>(ch)] = clamped;
    for (auto& [cid, chan] : channels_) {
        if (!chan->is_vanilla && chan->sound_id == ext_id && chan->tracker_player) {
            chan->tracker_player->set_channel_volume(static_cast<u32>(ch), clamped);
        }
    }
    return true;
}

std::vector<AudioTag> AudioEngine::ext_sound_get_tags(i32 ext_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end()) return {};
    return it->second.tags;
}

// ---------------------------------------------------------------------------
// Channel Control
// ---------------------------------------------------------------------------

i32 AudioEngine::channel_create(
    i32 ext_id,
    bool looping,
    bool start_paused,
    bool is_3d,
    f32 x,
    f32 y,
    f32 z) {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(ext_id);
    if (it == ext_sounds_.end() || !it->second.loaded) return 0;
    const auto& snd = it->second;

    auto ch = std::make_unique<AudioChannel>();
    if (!ch->open_stream_decoder(snd, soundfont_)) return 0;

    ch->id = next_channel_id_++;
    ch->is_vanilla = false;
    ch->sound_id = ext_id;
    ch->group_id = snd.group_id;
    ch->active = true;
    ch->paused = start_paused;
    ch->looping = looping;
    ch->loop_count = looping ? (snd.loop_count == 0 ? -1 : snd.loop_count) : 0;
    ch->volume = snd.max_volume;
    ch->max_volume = snd.max_volume;
    ch->fade_target = ch->volume;
    ch->pan = snd.pan;
    ch->has_loop_points = snd.has_loop_points;
    ch->loop_start_ms = snd.loop_start_ms;
    ch->loop_end_ms = snd.loop_end_ms;
    ch->is_3d = is_3d || snd.is_3d;
    ch->pos[0] = ch->prev_pos[0] = x;
    ch->pos[1] = ch->prev_pos[1] = y;
    ch->pos[2] = ch->prev_pos[2] = z;
    ch->min_dist = snd.min_dist;
    ch->max_dist = snd.max_dist;
    ch->doppler_level = snd.doppler_level;
    ch->cone_inside = snd.cone_inside;
    ch->cone_outside = snd.cone_outside;
    ch->cone_outside_vol = snd.cone_outside_vol;
    attach_asset_effects_to_channel(snd, *ch);

    const i32 cid = ch->id;
    channels_[cid] = std::move(ch);
    return cid;
}

void AudioEngine::remove_channel_locked(i32 ch_id) {
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return;
    for (i32 d : it->second->dsp_ids) dsps_.erase(d);
    channels_.erase(it);
}

bool AudioEngine::channel_stop(i32 ch_id) {
    std::scoped_lock lock(mutex_);
    if (!channels_.contains(ch_id)) return false;
    remove_channel_locked(ch_id);
    return true;
}

bool AudioEngine::channel_exists(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    return channels_.contains(ch_id);
}

bool AudioEngine::channel_is_playing(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() && it->second->active;
}

bool AudioEngine::channel_is_paused(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() && it->second->paused;
}

bool AudioEngine::channel_is_looping(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() && it->second->looping;
}

i32 AudioEngine::channel_get_sound(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->sound_id : 0;
}

bool AudioEngine::channel_set_paused(i32 ch_id, bool paused) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->paused = paused;
    return true;
}

bool AudioEngine::channel_set_muted(i32 ch_id, bool muted) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->muted = muted;
    return true;
}

bool AudioEngine::channel_get_muted(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() && it->second->muted;
}

bool AudioEngine::channel_set_volume(i32 ch_id, f32 vol) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    const f32 v = std::clamp(vol, 0.0f, 1.0f);
    it->second->volume = v;
    it->second->fade_target = v;
    it->second->fade_rate_per_sec = 0.0f;
    return true;
}

f32 AudioEngine::channel_get_volume(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->volume : 0.0f;
}

bool AudioEngine::channel_fade_volume(i32 ch_id, f32 target_vol, f32 duration_ms) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    const f32 target = std::clamp(target_vol, 0.0f, 1.0f);
    if (duration_ms <= 0.0f) {
        it->second->volume = target;
        it->second->fade_target = target;
        it->second->fade_rate_per_sec = 0.0f;
    } else {
        it->second->fade_target = target;
        it->second->fade_rate_per_sec = (target - it->second->volume) / (duration_ms * 0.001f);
    }
    return true;
}

bool AudioEngine::channel_set_pan(i32 ch_id, f32 pan) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->pan = std::clamp(pan, -1.0f, 1.0f);
    return true;
}

f32 AudioEngine::channel_get_pan(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->pan : 0.0f;
}

bool AudioEngine::channel_set_frequency(i32 ch_id, f32 freq_hz) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    const f32 base = static_cast<f32>(it->second->orig_sample_rate > 0 ? it->second->orig_sample_rate : kOutputSampleRate);
    const f32 f = (freq_hz <= 0.0f) ? base : std::clamp(freq_hz, 100.0f, 384000.0f);
    it->second->frequency = f;
    it->second->pitch = f / base;
    return true;
}

f32 AudioEngine::channel_get_frequency(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->frequency : 0.0f;
}

bool AudioEngine::channel_set_pitch(i32 ch_id, f32 pitch) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    const f32 p = std::clamp(pitch, 0.05f, 8.0f);
    it->second->pitch = p;
    it->second->frequency = static_cast<f32>(it->second->orig_sample_rate) * p;
    return true;
}

f32 AudioEngine::channel_get_pitch(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->pitch : 1.0f;
}

bool AudioEngine::channel_set_position_ms(i32 ch_id, f64 ms) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->seek_to_ms(ms);
    return true;
}

f64 AudioEngine::channel_get_position_ms(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->current_position_ms() : 0.0;
}

u64 AudioEngine::channel_get_buffer_cursor(i32 ch_id, u64 buffer_bytes) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return 0;
    const auto& ch = *it->second;
    if (ch.source_bytes_per_frame == 0 || buffer_bytes < ch.source_bytes_per_frame) return 0;
    const u64 buffer_frames = buffer_bytes / ch.source_bytes_per_frame;
    return (static_cast<u64>(ch.buffer_frame_pos) % buffer_frames) * ch.source_bytes_per_frame;
}

void AudioEngine::channel_set_buffer_cursor(i32 ch_id, u64 buffer_bytes, u64 byte_offset) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end() || byte_offset >= buffer_bytes) return;
    auto& ch = *it->second;
    if (ch.source_bytes_per_frame == 0 || buffer_bytes < ch.source_bytes_per_frame) return;
    const u64 buffer_frames = buffer_bytes / ch.source_bytes_per_frame;
    const f64 old_frame = std::fmod(ch.buffer_frame_pos, static_cast<f64>(buffer_frames));
    const f64 new_frame = static_cast<f64>(byte_offset / ch.source_bytes_per_frame);
    f64 media_ms = ch.current_position_ms() + (new_frame - old_frame) * 1000.0 / ch.orig_sample_rate;
    if (ch.looping && ch.length_ms > 0.0) {
        media_ms = std::fmod(media_ms, ch.length_ms);
        if (media_ms < 0.0) media_ms += ch.length_ms;
    }
    ch.seek_to_ms(media_ms);
    ch.buffer_frame_pos = new_frame;
}

bool AudioEngine::channel_set_loop_count(i32 ch_id, i32 count) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->loop_count = count;
    it->second->looping = (count != 0);
    return true;
}

i32 AudioEngine::channel_get_loop_count(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->loop_count : 0;
}

bool AudioEngine::channel_set_loop_points_ms(i32 ch_id, f64 start_ms, f64 end_ms) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->has_loop_points = true;
    it->second->loop_start_ms = std::max(0.0, start_ms);
    it->second->loop_end_ms = std::max(start_ms + 1.0, end_ms);
    return true;
}

bool AudioEngine::channel_clear_loop_points(i32 ch_id) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->has_loop_points = false;
    return true;
}

bool AudioEngine::channel_set_speaker_mix(i32 ch_id, const std::array<f32, 8>& mix) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->speaker_mix = mix;
    it->second->custom_speaker_mix = true;
    return true;
}

f32 AudioEngine::channel_get_audibility(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->last_audibility : 0.0f;
}

void AudioEngine::channel_get_levels(i32 ch_id, f32& out_left, f32& out_right) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) {
        out_left = 0.0f;
        out_right = 0.0f;
        return;
    }
    out_left = it->second->peak_l;
    out_right = it->second->peak_r;
}

bool AudioEngine::channel_set_3d_position(i32 ch_id, f32 x, f32 y, f32 z) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    auto& ch = *it->second;
    if (doppler_fps_ > 0.0f) {
        ch.vel[0] = (x - ch.pos[0]) * doppler_fps_;
        ch.vel[1] = (y - ch.pos[1]) * doppler_fps_;
        ch.vel[2] = (z - ch.pos[2]) * doppler_fps_;
    }
    ch.prev_pos[0] = ch.pos[0]; ch.prev_pos[1] = ch.pos[1]; ch.prev_pos[2] = ch.pos[2];
    ch.pos[0] = x; ch.pos[1] = y; ch.pos[2] = z;
    return true;
}

bool AudioEngine::channel_set_3d_min_max(i32 ch_id, f32 min_d, f32 max_d) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->min_dist = std::max(0.01f, min_d);
    it->second->max_dist = std::max(it->second->min_dist, max_d);
    return true;
}

f32 AudioEngine::channel_get_max_dist(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->max_dist : 0.0f;
}

bool AudioEngine::channel_set_3d_doppler(i32 ch_id, f32 doppler) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->doppler_level = std::clamp(doppler, 0.0f, 5.0f);
    return true;
}

bool AudioEngine::channel_set_3d_cone(i32 ch_id, f32 inside_deg, f32 outside_deg, f32 outside_vol) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->cone_inside = inside_deg;
    it->second->cone_outside = outside_deg;
    it->second->cone_outside_vol = std::clamp(outside_vol, 0.0f, 1.0f);
    return true;
}

bool AudioEngine::channel_set_3d_cone_orientation(i32 ch_id, f32 dx, f32 dy, f32 dz) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->cone_dir[0] = dx;
    it->second->cone_dir[1] = dy;
    it->second->cone_dir[2] = dz;
    return true;
}

bool AudioEngine::channel_set_3d_pan_level(i32 ch_id, f32 level) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->pan_level_3d = std::clamp(level, 0.0f, 1.0f);
    return true;
}

f32 AudioEngine::channel_get_3d_pan_level(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->pan_level_3d : 0.0f;
}

bool AudioEngine::channel_set_3d_spread(i32 ch_id, f32 spread_deg) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    it->second->spread_3d = std::clamp(spread_deg, 0.0f, 360.0f);
    return true;
}

f32 AudioEngine::channel_get_3d_spread(i32 ch_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    return it != channels_.end() ? it->second->spread_3d : 0.0f;
}

// ---------------------------------------------------------------------------
// Group Controls
// ---------------------------------------------------------------------------

i32 AudioEngine::active_fmod_channel_count() const {
    std::scoped_lock lock(mutex_);
    i32 count = 0;
    for (const auto& [cid, ch] : channels_) {
        if (ch->active && is_fmod_group(ch->group_id)) ++count;
    }
    return count;
}

bool AudioEngine::fmod_owns_sound(i32 sound_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = ext_sounds_.find(sound_id);
    return it != ext_sounds_.end() && is_fmod_group(it->second.group_id);
}

void AudioEngine::stop_group_channels(i32 group_id) {
    std::scoped_lock lock(mutex_);
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second->group_id == group_id || (group_id == 0 && is_fmod_group(it->second->group_id))) {
            for (i32 d : it->second->dsp_ids) dsps_.erase(d);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
}

bool AudioEngine::group_set_volume(i32 group_id, f32 vol) {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    groups_[static_cast<usize>(group_id)].volume = std::clamp(vol, 0.0f, 1.0f);
    return true;
}

f32 AudioEngine::group_get_volume(i32 group_id) const {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return 0.0f;
    std::scoped_lock lock(mutex_);
    return groups_[static_cast<usize>(group_id)].volume;
}

bool AudioEngine::group_set_pitch(i32 group_id, f32 pitch) {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    groups_[static_cast<usize>(group_id)].pitch = std::clamp(pitch, 0.05f, 8.0f);
    return true;
}

f32 AudioEngine::group_get_pitch(i32 group_id) const {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return 1.0f;
    std::scoped_lock lock(mutex_);
    return groups_[static_cast<usize>(group_id)].pitch;
}

bool AudioEngine::group_set_frequency(i32 group_id, f32 freq_hz) {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    for (auto& [cid, ch] : channels_) {
        if (ch->group_id == group_id || (group_id == 0 && is_fmod_group(ch->group_id))) {
            const f32 base = static_cast<f32>(ch->orig_sample_rate > 0 ? ch->orig_sample_rate : kOutputSampleRate);
            const f32 f = std::clamp(freq_hz, 100.0f, 384000.0f);
            ch->frequency = f;
            ch->pitch = f / base;
        }
    }
    return true;
}

bool AudioEngine::group_set_pan(i32 group_id, f32 pan) {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    groups_[static_cast<usize>(group_id)].pan = std::clamp(pan, -1.0f, 1.0f);
    return true;
}

bool AudioEngine::group_set_paused(i32 group_id, bool paused) {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    groups_[static_cast<usize>(group_id)].paused = paused;
    return true;
}

bool AudioEngine::group_get_paused(i32 group_id) const {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    return groups_[static_cast<usize>(group_id)].paused;
}

bool AudioEngine::group_set_muted(i32 group_id, bool muted) {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    groups_[static_cast<usize>(group_id)].muted = muted;
    return true;
}

bool AudioEngine::group_get_muted(i32 group_id) const {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return false;
    std::scoped_lock lock(mutex_);
    return groups_[static_cast<usize>(group_id)].muted;
}

// ---------------------------------------------------------------------------
// DSP Effect Management
// ---------------------------------------------------------------------------

i32 AudioEngine::dsp_add_to_channel(i32 ch_id, DspEffectType type) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return 0;
    const i32 did = next_dsp_id_++;
    dsps_[did] = std::make_unique<DspEffect>(type, kOutputSampleRate);
    it->second->dsp_ids.push_back(did);
    return did;
}

i32 AudioEngine::dsp_add_to_group(i32 group_id, DspEffectType type) {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count)) return 0;
    std::scoped_lock lock(mutex_);
    const i32 did = next_dsp_id_++;
    dsps_[did] = std::make_unique<DspEffect>(type, kOutputSampleRate);
    groups_[static_cast<usize>(group_id)].dsp_ids.push_back(did);
    return did;
}

bool AudioEngine::dsp_remove_from_channel(i32 ch_id, i32 dsp_id) {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end()) return false;
    auto& list = it->second->dsp_ids;
    list.erase(std::remove(list.begin(), list.end(), dsp_id), list.end());
    dsps_.erase(dsp_id);
    return true;
}

bool AudioEngine::dsp_free(i32 dsp_id) {
    std::scoped_lock lock(mutex_);
    if (!dsps_.contains(dsp_id)) return false;
    for (auto& [cid, ch] : channels_) {
        auto& list = ch->dsp_ids;
        list.erase(std::remove(list.begin(), list.end(), dsp_id), list.end());
    }
    for (auto& g : groups_) {
        g.dsp_ids.erase(std::remove(g.dsp_ids.begin(), g.dsp_ids.end(), dsp_id), g.dsp_ids.end());
    }
    dsps_.erase(dsp_id);
    return true;
}

bool AudioEngine::dsp_set_active(i32 dsp_id, bool active) {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    if (it == dsps_.end()) return false;
    it->second->set_active(active);
    return true;
}

bool AudioEngine::dsp_get_active(i32 dsp_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() && it->second->active();
}

bool AudioEngine::dsp_set_bypass(i32 dsp_id, bool bypass) {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    if (it == dsps_.end()) return false;
    it->second->set_bypass(bypass);
    return true;
}

bool AudioEngine::dsp_get_bypass(i32 dsp_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() && it->second->bypass();
}

bool AudioEngine::dsp_set_default_freq(i32 dsp_id, f32 hz) {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    if (it == dsps_.end()) return false;
    it->second->set_default_freq(hz);
    return true;
}

f32 AudioEngine::dsp_get_default_freq(i32 dsp_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() ? it->second->default_freq() : 0.0f;
}

bool AudioEngine::dsp_set_default_vol(i32 dsp_id, f32 vol) {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    if (it == dsps_.end()) return false;
    it->second->set_default_vol(vol);
    return true;
}

f32 AudioEngine::dsp_get_default_vol(i32 dsp_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() ? it->second->default_vol() : 0.0f;
}

bool AudioEngine::dsp_set_default_pan(i32 dsp_id, f32 pan) {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    if (it == dsps_.end()) return false;
    it->second->set_default_pan(pan);
    return true;
}

f32 AudioEngine::dsp_get_default_pan(i32 dsp_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() ? it->second->default_pan() : 0.0f;
}

bool AudioEngine::dsp_set_speaker_active(i32 dsp_id, i32 speaker, bool active) {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    if (it == dsps_.end()) return false;
    it->second->set_speaker_active(speaker, active);
    return true;
}

bool AudioEngine::dsp_get_speaker_active(i32 dsp_id, i32 speaker) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() && it->second->speaker_active(speaker);
}

i32 AudioEngine::dsp_get_num_params(i32 dsp_id) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() ? it->second->param_count() : 0;
}

DspParamDesc AudioEngine::dsp_get_param_desc(i32 dsp_id, i32 idx) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() ? it->second->param_desc(idx) : DspParamDesc{};
}

f32 AudioEngine::dsp_get_param_value(i32 dsp_id, i32 idx) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() ? it->second->param_value(idx) : 0.0f;
}

std::string AudioEngine::dsp_get_param_value_str(i32 dsp_id, i32 idx) const {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    return it != dsps_.end() ? it->second->param_value_str(idx) : std::string{};
}

bool AudioEngine::dsp_set_param_value(i32 dsp_id, i32 idx, f32 val) {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    if (it == dsps_.end()) return false;
    return it->second->set_param(idx, val);
}

bool AudioEngine::dsp_reset(i32 dsp_id) {
    std::scoped_lock lock(mutex_);
    const auto it = dsps_.find(dsp_id);
    if (it == dsps_.end()) return false;
    it->second->reset();
    return true;
}

// ---------------------------------------------------------------------------
// 3D Listeners & Blockers
// ---------------------------------------------------------------------------

void AudioEngine::listener_set_number(i32 count) {
    std::scoped_lock lock(mutex_);
    listener_count_ = std::clamp(count, 1, 4);
}

void AudioEngine::listener_set_3d_pos(
    i32 idx, f32 x, f32 y, f32 z,
    f32 fx, f32 fy, f32 fz,
    f32 ux, f32 uy, f32 uz,
    bool has_orient) {
    std::scoped_lock lock(mutex_);
    const i32 slot = std::clamp(idx - 1, 0, 3);
    auto& l = listeners_[static_cast<usize>(slot)];
    if (doppler_fps_ > 0.0f) {
        l.vel[0] = (x - l.pos[0]) * doppler_fps_;
        l.vel[1] = (y - l.pos[1]) * doppler_fps_;
        l.vel[2] = (z - l.pos[2]) * doppler_fps_;
    }
    l.prev_pos[0] = l.pos[0]; l.prev_pos[1] = l.pos[1]; l.prev_pos[2] = l.pos[2];
    l.pos[0] = x; l.pos[1] = y; l.pos[2] = z;
    if (has_orient) {
        l.forward[0] = fx; l.forward[1] = fy; l.forward[2] = fz;
        l.up[0] = ux; l.up[1] = uy; l.up[2] = uz;
    }
}

void AudioEngine::listener_set_distance_only(i32 idx, bool dist_only) {
    std::scoped_lock lock(mutex_);
    const i32 slot = std::clamp(idx - 1, 0, 3);
    listeners_[static_cast<usize>(slot)].distance_only = dist_only;
}

void AudioEngine::set_world_scale(f32 scale) {
    std::scoped_lock lock(mutex_);
    world_scale_ = scale > 0.0f ? scale : 1.0f;
}

void AudioEngine::set_doppler_fps(f32 fps) {
    std::scoped_lock lock(mutex_);
    doppler_fps_ = std::max(0.0f, fps);
}

void AudioEngine::blockers_init(i32 max_count) {
    std::scoped_lock lock(mutex_);
    blockers_.clear();
    blockers_.reserve(static_cast<usize>(std::clamp(max_count, 0, 4096)));
}

void AudioEngine::blockers_free() {
    std::scoped_lock lock(mutex_);
    blockers_.clear();
}

i32 AudioEngine::blocker_add(f32 x, f32 y, f32 z, f32 xs, f32 ys, f32 zs) {
    std::scoped_lock lock(mutex_);
    AudioBlocker3D b{};
    b.active = true;
    b.enabled = true;
    b.pos[0] = x; b.pos[1] = y; b.pos[2] = z;
    b.size[0] = xs; b.size[1] = ys; b.size[2] = zs;
    blockers_.push_back(b);
    return static_cast<i32>(blockers_.size()); // 1-based handle
}

bool AudioEngine::blocker_set_pos(i32 id, f32 x, f32 y, f32 z) {
    std::scoped_lock lock(mutex_);
    if (id <= 0 || static_cast<usize>(id) > blockers_.size()) return false;
    auto& b = blockers_[static_cast<usize>(id - 1)];
    b.pos[0] = x; b.pos[1] = y; b.pos[2] = z;
    return true;
}

bool AudioEngine::blocker_set_orient(i32 id, f32 fx, f32 fy, f32 fz, f32 ux, f32 uy, f32 uz) {
    std::scoped_lock lock(mutex_);
    if (id <= 0 || static_cast<usize>(id) > blockers_.size()) return false;
    auto& b = blockers_[static_cast<usize>(id - 1)];
    b.forward[0] = fx; b.forward[1] = fy; b.forward[2] = fz;
    b.up[0] = ux; b.up[1] = uy; b.up[2] = uz;
    return true;
}

bool AudioEngine::blocker_set_scale(i32 id, f32 sx, f32 sy, f32 sz) {
    std::scoped_lock lock(mutex_);
    if (id <= 0 || static_cast<usize>(id) > blockers_.size()) return false;
    auto& b = blockers_[static_cast<usize>(id - 1)];
    b.scale[0] = sx; b.scale[1] = sy; b.scale[2] = sz;
    return true;
}

bool AudioEngine::blocker_set_enabled(i32 id, bool enabled) {
    std::scoped_lock lock(mutex_);
    if (id <= 0 || static_cast<usize>(id) > blockers_.size()) return false;
    blockers_[static_cast<usize>(id - 1)].enabled = enabled;
    return true;
}

bool AudioEngine::blocker_get_enabled(i32 id) const {
    std::scoped_lock lock(mutex_);
    if (id <= 0 || static_cast<usize>(id) > blockers_.size()) return false;
    return blockers_[static_cast<usize>(id - 1)].enabled;
}

bool AudioEngine::blocker_set_strength(i32 id, f32 strength) {
    std::scoped_lock lock(mutex_);
    if (id <= 0 || static_cast<usize>(id) > blockers_.size()) return false;
    blockers_[static_cast<usize>(id - 1)].strength = std::clamp(strength, 0.0f, 1.0f);
    return true;
}

f32 AudioEngine::blocker_get_strength(i32 id) const {
    std::scoped_lock lock(mutex_);
    if (id <= 0 || static_cast<usize>(id) > blockers_.size()) return 0.0f;
    return blockers_[static_cast<usize>(id - 1)].strength;
}

// ---------------------------------------------------------------------------
// Wave & Spectrum Snapshots + Radix-2 FFT
// ---------------------------------------------------------------------------

void AudioEngine::compute_fft_magnitude(std::span<const f32> samples, i32 window_type, std::vector<f32>& out) {
    usize n = 1;
    while (n < samples.size() && n < 8192) n <<= 1u;
    if (n < 64) n = 64;

    std::vector<std::complex<f32>> buf(n, {0.0f, 0.0f});
    const usize copy_n = std::min(samples.size(), n);
    for (usize i = 0; i < copy_n; ++i) {
        buf[i] = {samples[i] * window_coeff(window_type, i, copy_n), 0.0f};
    }

    // Bit-reversal permutation
    for (usize i = 1, j = 0; i < n; ++i) {
        usize bit = n >> 1u;
        for (; j & bit; bit >>= 1u) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(buf[i], buf[j]);
    }

    // Cooley-Tukey iterative radix-2 FFT
    for (usize len = 2; len <= n; len <<= 1u) {
        const f32 ang = -kTwoPi / static_cast<f32>(len);
        const std::complex<f32> wlen(std::cos(ang), std::sin(ang));
        for (usize i = 0; i < n; i += len) {
            std::complex<f32> w(1.0f, 0.0f);
            for (usize j = 0; j < len / 2u; ++j) {
                const auto u = buf[i + j];
                const auto v = buf[i + j + len / 2u] * w;
                buf[i + j] = u + v;
                buf[i + j + len / 2u] = u - v;
                w *= wlen;
            }
        }
    }

    const usize out_n = samples.size();
    out.resize(out_n, 0.0f);
    const f32 inv_n = 2.0f / static_cast<f32>(n);
    for (usize i = 0; i < out_n && i < n / 2u; ++i) {
        out[i] = std::abs(buf[i]) * inv_n;
    }
}

bool AudioEngine::channel_get_wave_snapshot(i32 ch_id, i32 channel_lr, u32 size, std::vector<f32>& out) const {
    std::scoped_lock lock(mutex_);
    const auto it = channels_.find(ch_id);
    if (it == channels_.end() || size == 0) return false;
    read_ring_samples(it->second->ring_l, it->second->ring_r, it->second->ring_write, channel_lr, size, out);
    return true;
}

bool AudioEngine::channel_get_spectrum_snapshot(i32 ch_id, i32 channel_lr, u32 size, i32 window_type, std::vector<f32>& out) const {
    std::vector<f32> wave;
    if (!channel_get_wave_snapshot(ch_id, channel_lr, size * 2u, wave)) return false;
    std::vector<f32> mag;
    compute_fft_magnitude(wave, window_type, mag);
    out.assign(size, 0.0f);
    const usize n = std::min<usize>(size, mag.size());
    std::copy_n(mag.begin(), n, out.begin());
    return true;
}

bool AudioEngine::group_get_wave_snapshot(i32 group_id, i32 channel_lr, u32 size, std::vector<f32>& out) const {
    if (group_id < 0 || group_id >= static_cast<i32>(AudioGroupId::Count) || size == 0) return false;
    std::scoped_lock lock(mutex_);
    const auto& g = groups_[static_cast<usize>(group_id)];
    read_ring_samples(g.ring_l, g.ring_r, g.ring_write, channel_lr, size, out);
    return true;
}

bool AudioEngine::group_get_spectrum_snapshot(i32 group_id, i32 channel_lr, u32 size, i32 window_type, std::vector<f32>& out) const {
    std::vector<f32> wave;
    if (!group_get_wave_snapshot(group_id, channel_lr, size * 2u, wave)) return false;
    std::vector<f32> mag;
    compute_fft_magnitude(wave, window_type, mag);
    out.assign(size, 0.0f);
    const usize n = std::min<usize>(size, mag.size());
    std::copy_n(mag.begin(), n, out.begin());
    return true;
}

void AudioEngine::clear_fmod_audio() {
    std::scoped_lock lock(mutex_);
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (is_fmod_group(it->second->group_id)) {
            for (i32 d : it->second->dsp_ids) dsps_.erase(d);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(ext_sounds_, [](const auto& entry) { return is_fmod_group(entry.second.group_id); });
    blockers_.clear();
    for (usize i = 0; i < groups_.size(); ++i) {
        if (!is_fmod_group(static_cast<i32>(i))) continue;
        for (i32 d : groups_[i].dsp_ids) dsps_.erase(d);
        groups_[i].dsp_ids.clear();
        groups_[i].volume = 1.0f;
        groups_[i].pitch = 1.0f;
        groups_[i].pan = 0.0f;
        groups_[i].paused = false;
        groups_[i].muted = false;
    }
}

// ---------------------------------------------------------------------------
// 3D Spatialization & Main Software Mixer
// ---------------------------------------------------------------------------

void AudioEngine::compute_3d_spatial(AudioChannel& ch, f32& out_gain, f32& out_pan, f32& out_doppler) const noexcept {
    out_gain = 1.0f;
    out_pan = ch.pan;
    out_doppler = 1.0f;
    if (!ch.is_3d) return;

    // Pick closest listener among active listeners_[0..listener_count_-1]
    const AudioListener3D* best_l = &listeners_[0];
    f32 best_dist = 1e30f;
    for (i32 i = 0; i < listener_count_; ++i) {
        const auto& l = listeners_[static_cast<usize>(i)];
        const f32 dx = ch.pos[0] - l.pos[0];
        const f32 dy = ch.pos[1] - l.pos[1];
        const f32 dz = ch.pos[2] - l.pos[2];
        const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < best_dist) {
            best_dist = d;
            best_l = &l;
        }
    }

    const f32 dist = best_dist * world_scale_;
    const f32 min_d = std::max(0.01f, ch.min_dist * world_scale_);
    const f32 max_d = std::max(min_d, ch.max_dist * world_scale_);

    if (dist <= min_d) {
        out_gain = 1.0f;
    } else if (dist >= max_d) {
        out_gain = 0.0f;
    } else {
        // Linear/inverse rolloff matching GMFMODSimple's min/max distance model
        out_gain = std::clamp(1.0f - (dist - min_d) / std::max(0.001f, max_d - min_d), 0.0f, 1.0f);
    }

    if (!best_l->distance_only && best_dist > 1e-4f) {
        // Right vector = cross(up, forward) (LH/2D screen space: +X is right)
        const f32 fx = best_l->forward[0], fy = best_l->forward[1], fz = best_l->forward[2];
        const f32 ux = best_l->up[0], uy = best_l->up[1], uz = best_l->up[2];
        f32 rx = uy * fz - uz * fy;
        f32 ry = uz * fx - ux * fz;
        f32 rz = ux * fy - uy * fx;
        const f32 rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rlen > 1e-4f) {
            rx /= rlen; ry /= rlen; rz /= rlen;
        } else {
            rx = 1.0f; ry = 0.0f; rz = 0.0f;
        }
        const f32 dx = (ch.pos[0] - best_l->pos[0]) / best_dist;
        const f32 dy = (ch.pos[1] - best_l->pos[1]) / best_dist;
        const f32 dz = (ch.pos[2] - best_l->pos[2]) / best_dist;
        const f32 pan3d = std::clamp(dx * rx + dy * ry + dz * rz, -1.0f, 1.0f);
        out_pan = ch.pan * (1.0f - ch.pan_level_3d) + pan3d * ch.pan_level_3d;
    }

    // Simple AABB occlusion check against enabled blockers
    for (const auto& b : blockers_) {
        if (!b.active || !b.enabled || b.strength <= 0.0f) continue;
        const f32 hx = std::fabs(b.size[0] * b.scale[0]) * 0.5f;
        const f32 hy = std::fabs(b.size[1] * b.scale[1]) * 0.5f;
        const f32 hz = std::fabs(b.size[2] * b.scale[2]) * 0.5f;
        const f32 mx = (ch.pos[0] + best_l->pos[0]) * 0.5f;
        const f32 my = (ch.pos[1] + best_l->pos[1]) * 0.5f;
        const f32 mz = (ch.pos[2] + best_l->pos[2]) * 0.5f;
        if (std::fabs(mx - b.pos[0]) <= hx && std::fabs(my - b.pos[1]) <= hy && std::fabs(mz - b.pos[2]) <= hz) {
            out_gain *= (1.0f - b.strength);
        }
    }
}

void AudioEngine::apply_dsp_chain(const std::vector<i32>& dsp_ids, f32* stereo, u32 frame_count) noexcept {
    for (i32 did : dsp_ids) {
        const auto it = dsps_.find(did);
        if (it != dsps_.end()) {
            it->second->process(stereo, frame_count);
        }
    }
}

void AudioEngine::mix_frames(f32* out_stereo, u32 frame_count) noexcept {
    std::memset(out_stereo, 0, static_cast<usize>(frame_count) * 2u * sizeof(f32));
    if (frame_count == 0) return;

    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    // Process in chunks fitting our pre-allocated scratch buffers (zero allocs on audio thread)
    const u32 max_chunk = static_cast<u32>(mix_ch_buf_.size() / 2u);
    u32 done = 0;
    while (done < frame_count) {
        const u32 chunk = std::min(frame_count - done, max_chunk);
        f32* dst = out_stereo + static_cast<usize>(done) * 2u;
        const f32 dt_frame = 1.0f / static_cast<f32>(kOutputSampleRate);

        for (auto& gb : mix_group_bufs_) {
            std::memset(gb.data(), 0, static_cast<usize>(chunk) * 2u * sizeof(f32));
        }

        const auto& master_grp = groups_[0];
        std::array<bool, static_cast<usize>(AudioGroupId::Count)> group_used{};

        for (auto& [cid, ch_ptr] : channels_) {
            auto& ch = *ch_ptr;
            if (!ch.active) continue;

            const usize gidx = static_cast<usize>(std::clamp(ch.group_id, 0, static_cast<i32>(AudioGroupId::Count) - 1));
            const auto& grp = groups_[gidx];

            const bool fmod_child = gidx != 0 && is_fmod_group(ch.group_id);
            const bool master_paused = fmod_child && master_grp.paused;
            if (ch.paused || grp.paused || master_paused) {
                ch.peak_l *= 0.85f;
                ch.peak_r *= 0.85f;
                continue;
            }

            f32 spatial_gain = 1.0f;
            f32 effective_pan = ch.pan;
            f32 doppler_mult = 1.0f;
            compute_3d_spatial(ch, spatial_gain, effective_pan, doppler_mult);

            const f32 grp_pitch = grp.pitch * (fmod_child ? master_grp.pitch : 1.0f);
            const f32 rate_mult = ch.pitch * grp_pitch * doppler_mult;

            std::memset(mix_ch_buf_.data(), 0, static_cast<usize>(chunk) * 2u * sizeof(f32));
            const u32 got = ch.pull_frames(mix_ch_buf_.data(), chunk, rate_mult);
            if (ch.group_id == static_cast<i32>(AudioGroupId::SuperSound)) {
                ch.buffer_frame_pos += static_cast<f64>(got) * ch.orig_sample_rate / kOutputSampleRate *
                                       std::clamp<f64>(rate_mult, 0.05, 8.0);
            }

            // Apply per-channel DSP chain
            if (got > 0 && !ch.dsp_ids.empty()) {
                apply_dsp_chain(ch.dsp_ids, mix_ch_buf_.data(), got);
            }

            const bool is_muted = ch.muted || grp.muted || (fmod_child && master_grp.muted);
            const f32 total_pan = std::clamp(effective_pan + grp.pan, -1.0f, 1.0f);
            const f32 pan_l = (total_pan <= 0.0f) ? 1.0f : (1.0f - total_pan);
            const f32 pan_r = (total_pan >= 0.0f) ? 1.0f : (1.0f + total_pan);
            const f32 spk_l = ch.custom_speaker_mix ? ch.speaker_mix[0] : 1.0f;
            const f32 spk_r = ch.custom_speaker_mix ? ch.speaker_mix[1] : 1.0f;

            f32 peak_l = 0.0f, peak_r = 0.0f;
            f32* gbuf = mix_group_bufs_[gidx].data();
            group_used[gidx] = true;

            for (u32 f = 0; f < got; ++f) {
                if (ch.fade_rate_per_sec != 0.0f) {
                    ch.volume += ch.fade_rate_per_sec * dt_frame;
                    if ((ch.fade_rate_per_sec > 0.0f && ch.volume >= ch.fade_target) ||
                        (ch.fade_rate_per_sec < 0.0f && ch.volume <= ch.fade_target)) {
                        ch.volume = ch.fade_target;
                        ch.fade_rate_per_sec = 0.0f;
                    }
                }

                const f32 gain = is_muted ? 0.0f : (ch.volume * spatial_gain);
                const f32 sl = mix_ch_buf_[f * 2u + 0] * gain * pan_l * spk_l;
                const f32 sr = mix_ch_buf_[f * 2u + 1] * gain * pan_r * spk_r;

                gbuf[f * 2u + 0] += sl;
                gbuf[f * 2u + 1] += sr;

                ch.ring_l[ch.ring_write] = sl;
                ch.ring_r[ch.ring_write] = sr;
                ch.ring_write = (ch.ring_write + 1u) % AudioChannel::kRingFrames;

                peak_l = std::max(peak_l, std::fabs(sl));
                peak_r = std::max(peak_r, std::fabs(sr));
            }
            ch.peak_l = peak_l;
            ch.peak_r = peak_r;
            ch.last_audibility = is_muted ? 0.0f : (ch.volume * spatial_gain * grp.volume * (fmod_child ? master_grp.volume : 1.0f));
        }

        // Only FMOD's own subgroups pass through its master DSP, volume and mute.
        for (usize gidx = 1; gidx < groups_.size(); ++gidx) {
            if (!group_used[gidx]) continue;
            auto& grp = groups_[gidx];
            f32* gbuf = mix_group_bufs_[gidx].data();
            if (!grp.dsp_ids.empty()) {
                apply_dsp_chain(grp.dsp_ids, gbuf, chunk);
            }
            const f32 gvol = grp.muted ? 0.0f : grp.volume;
            const bool to_master = is_fmod_group(static_cast<i32>(gidx));
            f32* target = to_master ? mix_group_bufs_[0].data() : dst;
            if (to_master) group_used[0] = true;

            for (u32 f = 0; f < chunk; ++f) {
                const f32 sl = gbuf[f * 2u + 0] * gvol;
                const f32 sr = gbuf[f * 2u + 1] * gvol;
                grp.ring_l[grp.ring_write] = sl;
                grp.ring_r[grp.ring_write] = sr;
                grp.ring_write = (grp.ring_write + 1u) % AudioGroupState::kRingFrames;
                target[f * 2u + 0] += sl;
                target[f * 2u + 1] += sr;
            }
        }

        // Apply Master (0) DSP & volume into output
        {
            auto& mgrp = groups_[0];
            f32* mbuf = mix_group_bufs_[0].data();
            if (group_used[0] && !mgrp.dsp_ids.empty()) {
                apply_dsp_chain(mgrp.dsp_ids, mbuf, chunk);
            }
            const f32 mvol = mgrp.muted ? 0.0f : mgrp.volume;
            for (u32 f = 0; f < chunk; ++f) {
                const f32 sl = mbuf[f * 2u + 0] * mvol;
                const f32 sr = mbuf[f * 2u + 1] * mvol;
                mgrp.ring_l[mgrp.ring_write] = sl;
                mgrp.ring_r[mgrp.ring_write] = sr;
                mgrp.ring_write = (mgrp.ring_write + 1u) % AudioGroupState::kRingFrames;
                dst[f * 2u + 0] = std::clamp(dst[f * 2u + 0] + sl, -1.0f, 1.0f);
                dst[f * 2u + 1] = std::clamp(dst[f * 2u + 1] + sr, -1.0f, 1.0f);
            }
        }

        done += chunk;
    }

    // Reap channels that have finished playing (paused channels remain active=true).
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (!it->second->active) {
            for (i32 d : it->second->dsp_ids) dsps_.erase(d);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace gm
