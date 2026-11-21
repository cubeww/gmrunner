#include "audio/midi_decoder.h"

#include <tml.h>
#include <tsf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gm {

namespace {

constexpr f32 kTwoPi = 6.28318530717958647692f;

f32 midi_note_to_hz(u8 key) noexcept {
    return 440.0f * std::pow(2.0f, (static_cast<f32>(key) - 69.0f) / 12.0f);
}

} // namespace

MidiSequence::~MidiSequence() {
    if (messages) {
        tml_free(messages);
        messages = nullptr;
    }
}

bool is_midi_data(std::span<const u8> data) noexcept {
    if (data.size() < 4) return false;
    if (std::memcmp(data.data(), "MThd", 4) == 0) return true;
    // RIFF RMID container ("RIFF....RMID")
    if (data.size() >= 12 &&
        std::memcmp(data.data(), "RIFF", 4) == 0 &&
        std::memcmp(data.data() + 8, "RMID", 4) == 0) {
        return true;
    }
    return false;
}

Result<std::shared_ptr<MidiSequence>> load_midi_sequence(std::span<const u8> data) {
    if (!is_midi_data(data)) {
        return Error{"Not a valid MIDI stream"};
    }

    // Extract inner MThd if wrapped in RIFF RMID
    std::span<const u8> midi_span = data;
    if (data.size() >= 20 && std::memcmp(data.data(), "RIFF", 4) == 0) {
        for (usize i = 12; i + 4 <= data.size(); ++i) {
            if (std::memcmp(data.data() + i, "MThd", 4) == 0) {
                midi_span = data.subspan(i);
                break;
            }
        }
    }

    tml_message* msgs = tml_load_memory(midi_span.data(), static_cast<int>(midi_span.size()));
    if (!msgs) {
        return Error{"TinyMidiLoader failed to parse MIDI stream"};
    }

    auto seq = std::make_shared<MidiSequence>();
    seq->messages = msgs;
    unsigned int total_ms = 0;
    tml_get_info(msgs, nullptr, nullptr, nullptr, nullptr, &total_ms);
    seq->duration_ms = std::max<f64>(static_cast<f64>(total_ms), 100.0);
    return seq;
}

MidiPlayer::MidiPlayer(std::shared_ptr<const MidiSequence> seq, const tsf* shared_soundfont, u32 output_sample_rate)
    : seq_(std::move(seq)), sample_rate_(output_sample_rate > 0 ? output_sample_rate : 44100u) {
    if (shared_soundfont) {
        synth_ = tsf_copy(const_cast<tsf*>(shared_soundfont));
        if (synth_) {
            tsf_set_output(synth_, TSF_STEREO_INTERLEAVED, static_cast<int>(sample_rate_), 0.0f);
            tsf_channel_set_bank_preset(synth_, 9, 128, 0);
        }
    }
    reset();
}

MidiPlayer::~MidiPlayer() {
    if (synth_) {
        tsf_close(synth_);
        synth_ = nullptr;
    }
}

void MidiPlayer::reset() noexcept {
    cursor_ = seq_ ? seq_->messages : nullptr;
    current_ms_ = 0.0;
    finished_ = (cursor_ == nullptr);
    ch_vol_.fill(0.8f);
    ch_pan_.fill(0.0f);
    ch_prog_.fill(0);
    for (auto& v : voices_) v.active = false;
    if (synth_) {
        tsf_reset(synth_);
        tsf_channel_set_bank_preset(synth_, 9, 128, 0);
    }
}

void MidiPlayer::seek_ms(f64 ms) noexcept {
    reset();
    if (!seq_ || !cursor_ || ms <= 0.0) return;
    current_ms_ = std::min(ms, seq_->duration_ms);
    while (cursor_ && static_cast<f64>(cursor_->time) < current_ms_) {
        if (cursor_->type == TML_PROGRAM_CHANGE || cursor_->type == TML_CONTROL_CHANGE ||
            cursor_->type == TML_PITCH_BEND) {
            dispatch_event(*cursor_);
        }
        cursor_ = cursor_->next;
    }
    if (!cursor_) finished_ = true;
}

void MidiPlayer::dispatch_event(const tml_message& msg) noexcept {
    const u8 ch = msg.channel & 15u;
    if (synth_) {
        switch (msg.type) {
            case TML_PROGRAM_CHANGE:
                tsf_channel_set_presetnumber(synth_, ch, static_cast<unsigned char>(msg.program), ch == 9 ? 1 : 0);
                break;
            case TML_NOTE_ON:
                if (static_cast<unsigned char>(msg.velocity) > 0) {
                    tsf_channel_note_on(synth_, ch, static_cast<unsigned char>(msg.key),
                                        static_cast<f32>(static_cast<unsigned char>(msg.velocity)) / 127.0f);
                } else {
                    tsf_channel_note_off(synth_, ch, static_cast<unsigned char>(msg.key));
                }
                break;
            case TML_NOTE_OFF:
                tsf_channel_note_off(synth_, ch, static_cast<unsigned char>(msg.key));
                break;
            case TML_PITCH_BEND:
                tsf_channel_set_pitchwheel(synth_, ch, msg.pitch_bend);
                break;
            case TML_CONTROL_CHANGE:
                tsf_channel_midi_control(synth_, ch, static_cast<unsigned char>(msg.control),
                                         static_cast<unsigned char>(msg.control_value));
                break;
            default:
                break;
        }
        return;
    }

    // Fallback built-in GM synthesizer
    switch (msg.type) {
        case TML_PROGRAM_CHANGE:
            ch_prog_[ch] = static_cast<u8>(msg.program);
            break;
        case TML_CONTROL_CHANGE:
            if (static_cast<u8>(msg.control) == TML_VOLUME_MSB) {
                ch_vol_[ch] = static_cast<f32>(static_cast<u8>(msg.control_value)) / 127.0f;
            } else if (static_cast<u8>(msg.control) == TML_PAN_MSB) {
                ch_pan_[ch] = (static_cast<f32>(static_cast<u8>(msg.control_value)) / 63.5f) - 1.0f;
            } else if (static_cast<u8>(msg.control) == TML_ALL_NOTES_OFF ||
                       static_cast<u8>(msg.control) == TML_ALL_SOUND_OFF) {
                for (auto& v : voices_) {
                    if (v.active && v.channel == ch) v.releasing = true;
                }
            }
            break;
        case TML_NOTE_ON: {
            const u8 vel = static_cast<u8>(msg.velocity);
            const u8 key = static_cast<u8>(msg.key);
            if (vel == 0) {
                for (auto& v : voices_) {
                    if (v.active && v.channel == ch && v.key == key) v.releasing = true;
                }
                break;
            }
            FallbackVoice* slot = nullptr;
            for (auto& v : voices_) {
                if (!v.active) { slot = &v; break; }
            }
            if (!slot) slot = &voices_[0];
            slot->active = true;
            slot->releasing = false;
            slot->channel = ch;
            slot->key = key;
            slot->phase = 0.0f;
            slot->freq_hz = midi_note_to_hz(key);
            slot->amp = (static_cast<f32>(vel) / 127.0f) * 0.14f;
            slot->env = 1.0f;
            slot->is_drum = (ch == 9);
            slot->noise_state = 0x9E3779B9u ^ (static_cast<u32>(key) * 2654435761u);
            break;
        }
        case TML_NOTE_OFF: {
            const u8 key = static_cast<u8>(msg.key);
            for (auto& v : voices_) {
                if (v.active && v.channel == ch && v.key == key) v.releasing = true;
            }
            break;
        }
        default:
            break;
    }
}

void MidiPlayer::render_fallback(f32* out_stereo, u32 frame_count) noexcept {
    const f32 inv_sr = 1.0f / static_cast<f32>(sample_rate_);
    for (u32 f = 0; f < frame_count; ++f) {
        f32 l = 0.0f, r = 0.0f;
        for (auto& v : voices_) {
            if (!v.active) continue;
            f32 sample = 0.0f;
            if (v.is_drum) {
                v.noise_state = v.noise_state * 1664525u + 1013904223u;
                const f32 noise = static_cast<f32>(static_cast<i32>(v.noise_state >> 16u) - 32768) / 32768.0f;
                const f32 body = std::sin(v.phase * kTwoPi);
                v.phase += (v.key < 40 ? 90.0f : 180.0f) * inv_sr;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                sample = (v.key < 40 ? (body * 0.7f + noise * 0.3f) : noise * 0.6f);
                v.env *= 0.9992f;
                if (v.env < 0.005f) v.active = false;
            } else {
                const u8 prog = ch_prog_[v.channel];
                // Blend fundamental + gentle harmonics based on GM program family
                const f32 s1 = std::sin(v.phase * kTwoPi);
                const f32 s2 = std::sin(v.phase * 2.0f * kTwoPi) * 0.35f;
                const f32 tri = 4.0f * std::fabs(v.phase - 0.5f) - 1.0f;
                if (prog >= 80 && prog <= 95) {
                    sample = (v.phase < 0.5f ? 0.55f : -0.55f) * 0.6f + s1 * 0.4f; // synth lead
                } else if (prog >= 32 && prog <= 39) {
                    sample = tri * 0.65f + s1 * 0.35f; // bass
                } else {
                    sample = s1 * 0.75f + s2; // piano / strings / general melodic
                }
                v.phase += v.freq_hz * inv_sr;
                if (v.phase >= 1.0f) v.phase -= std::floor(v.phase);
                if (v.releasing) {
                    v.env -= 4.5f * inv_sr;
                    if (v.env <= 0.0f) { v.active = false; continue; }
                } else {
                    // Gentle natural decay for percussive/piano instruments
                    if (prog < 16) v.env *= 0.99992f;
                }
            }
            const f32 g = v.amp * v.env * ch_vol_[v.channel];
            const f32 pan = ch_pan_[v.channel];
            l += sample * g * std::clamp(1.0f - pan, 0.0f, 1.0f);
            r += sample * g * std::clamp(1.0f + pan, 0.0f, 1.0f);
        }
        out_stereo[f * 2u + 0] = l;
        out_stereo[f * 2u + 1] = r;
    }
}

u32 MidiPlayer::render(f32* out_stereo, u32 frame_count, bool loop) noexcept {
    std::memset(out_stereo, 0, static_cast<usize>(frame_count) * 2u * sizeof(f32));
    if (!seq_ || finished_ || frame_count == 0) return 0;

    constexpr u32 kBlock = 64;
    u32 offset = 0;
    while (offset < frame_count) {
        const u32 chunk = std::min(kBlock, frame_count - offset);
        const f64 dt_ms = (static_cast<f64>(chunk) * 1000.0 / static_cast<f64>(sample_rate_)) * tempo_scale_;
        current_ms_ += dt_ms;

        while (cursor_ && static_cast<f64>(cursor_->time) <= current_ms_) {
            dispatch_event(*cursor_);
            cursor_ = cursor_->next;
        }

        if (synth_) {
            tsf_render_float(synth_, out_stereo + offset * 2u, static_cast<int>(chunk), 0);
        } else {
            render_fallback(out_stereo + offset * 2u, chunk);
        }

        offset += chunk;

        if (!cursor_ && current_ms_ >= seq_->duration_ms) {
            if (loop) {
                reset();
            } else {
                finished_ = true;
                break;
            }
        }
    }
    return offset;
}

} // namespace gm
