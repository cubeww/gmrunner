#pragma once

#include "audio/dsp_effects.h"
#include "audio/midi_decoder.h"
#include "audio/tracker_decoder.h"
#include "base/types.h"
#include "loader/game_data.h"

#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct tsf;

namespace gm {

enum class AudioGroupId : i32 {
    Master = 0,
    Priority = 1,
    Effects = 2,
    AmbientMusic = 3,
    AmbientEffects = 4,
    Vanilla = 5,
    MaizeMusic = 6,
    SuperSound = 7,
    Count = 8,
};

inline bool is_fmod_group(i32 group) noexcept {
    return group >= static_cast<i32>(AudioGroupId::Master) &&
           group <= static_cast<i32>(AudioGroupId::AmbientEffects);
}

enum class AudioSourceKind : u8 {
    Empty = 0,
    PcmStereo,
    OggStream,
    MaStream,
    Tracker,
    Midi,
};

struct AudioTag {
    std::string name;
    std::string value;
};

struct SoundAsset {
    bool exists = false;
    bool loaded = false;
    bool is_3d = false;
    bool is_streamed = false;
    i32 gm_kind = 0;       // Vanilla GM8 kind: 0=normal, 1=background, 2=3D, 3=multimedia
    i32 group_id = 5;      // Default AudioGroupId::Vanilla (or 0 for FMOD)

    AudioSourceKind source_kind = AudioSourceKind::Empty;
    u32 orig_sample_rate = 44100;
    u32 orig_channels = 2;
    u32 source_bytes_per_frame = 4;
    u64 source_frames = 0;
    u64 total_frames = 0;  // Mixer frames: native rate for Vorbis streams, output rate otherwise.
    f64 length_ms = 0.0;

    // Backing media payload (only one populated according to source_kind)
    std::shared_ptr<std::vector<f32>> pcm_stereo; // Interleaved stereo f32 @ kOutputSampleRate (shared zero-copy)
    std::shared_ptr<std::vector<u8>> stream_raw;  // Compressed OGG/MP3/WAV/FLAC bytes for streaming
    std::shared_ptr<TrackerModule> tracker_mod;
    std::shared_ptr<MidiSequence> midi_seq;

    // Default properties inherited by spawned channels
    f32 volume = 1.0f;
    f32 max_volume = 1.0f;
    f32 pan = 0.0f;
    i32 loop_count = -1; // -1 = infinite when looped
    bool has_loop_points = false;
    f64 loop_start_ms = 0.0;
    f64 loop_end_ms = 0.0;

    // 3D defaults
    f32 min_dist = 1.0f;
    f32 max_dist = 10000.0f;
    f32 doppler_level = 1.0f;
    f32 cone_inside = 360.0f;
    f32 cone_outside = 360.0f;
    f32 cone_outside_vol = 1.0f;
    f32 pos3d[3] = {0.0f, 0.0f, 0.0f};
    f32 vel3d[3] = {0.0f, 0.0f, 0.0f};
    f32 cone_dir[3] = {0.0f, 0.0f, 1.0f};

    // Effects attached at sound level (vanilla se_* bitmask + FMODSoundAddEffect slots)
    u32 effects_mask = 0;
    std::array<DspEffectType, 10> extra_effects{};

    // Per-channel volume overrides for tracker modules
    std::vector<f32> tracker_channel_vols;

    // Metadata tags (Vorbis comments / tracker title)
    std::vector<AudioTag> tags;
};

struct AudioChannel;

struct AudioGroupState {
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    f32 pan = 0.0f;
    bool paused = false;
    bool muted = false;
    std::vector<i32> dsp_ids;

    // Ring buffer of most recent stereo output frames for wave/spectrum snapshots
    static constexpr usize kRingFrames = 8192;
    std::vector<f32> ring_l;
    std::vector<f32> ring_r;
    usize ring_write = 0;

    AudioGroupState() : ring_l(kRingFrames, 0.0f), ring_r(kRingFrames, 0.0f) {}
};

struct AudioListener3D {
    f32 pos[3] = {0.0f, 0.0f, 0.0f};
    f32 prev_pos[3] = {0.0f, 0.0f, 0.0f};
    f32 vel[3] = {0.0f, 0.0f, 0.0f};
    f32 forward[3] = {0.0f, 0.0f, 1.0f};
    f32 up[3] = {0.0f, 1.0f, 0.0f};
    bool distance_only = false;
};

struct AudioBlocker3D {
    bool active = false;
    bool enabled = true;
    f32 pos[3] = {0.0f, 0.0f, 0.0f};
    f32 size[3] = {1.0f, 1.0f, 1.0f};
    f32 scale[3] = {1.0f, 1.0f, 1.0f};
    f32 forward[3] = {0.0f, 0.0f, 1.0f};
    f32 up[3] = {0.0f, 1.0f, 0.0f};
    f32 strength = 1.0f;
};

class AudioEngine : public NonCopyable {
public:
    static constexpr u32 kOutputSampleRate = 44100;

    AudioEngine();
    ~AudioEngine();

    [[nodiscard]] Result<void> init();
    void shutdown() noexcept;
    void suspend() noexcept;
    void resume() noexcept;
    void set_output_muted(bool muted) noexcept;
    [[nodiscard]] bool is_output_muted() const noexcept;
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    // Optional SoundFont loading for TinySoundFont MIDI playback.
    bool load_soundfont(std::span<const u8> sf2_bytes);

    // -----------------------------------------------------------------------
    // Vanilla GM8 Sound Resource API (0-based sound indices matching GameData)
    // -----------------------------------------------------------------------
    void load_game_sounds(const GameData& game);
    [[nodiscard]] i32 add_sound_from_memory(std::span<const u8> encoded, i32 kind, std::string_view ext_hint = {});
    bool replace_sound_from_memory(i32 sound_id, std::span<const u8> encoded, i32 kind, std::string_view ext_hint = {});
    void delete_sound(i32 sound_id);
    [[nodiscard]] bool sound_exists(i32 sound_id) const noexcept;
    void sound_discard(i32 sound_id);
    void sound_restore(i32 sound_id);

    void sound_play(i32 sound_id);
    void sound_loop(i32 sound_id);
    void sound_stop(i32 sound_id);
    void sound_stop_all();
    [[nodiscard]] bool sound_isplaying(i32 sound_id) const;

    void sound_volume(i32 sound_id, f32 vol);
    void sound_pan(i32 sound_id, f32 pan);
    void sound_fade(i32 sound_id, f32 target_vol, f32 duration_ms);
    void sound_global_volume(f32 vol);
    void sound_background_tempo(f32 factor);
    void sound_set_search_directory(std::string_view dir);
    [[nodiscard]] std::string sound_search_directory() const;

    // Vanilla GM8 sound_effect_* & sound_3d_*
    void sound_effect_set(i32 sound_id, u32 effects_mask);
    void sound_effect_chorus(i32 sound_id, f32 wetdry, f32 depth, f32 feedback, f32 freq, i32 wave, f32 delay, i32 phase);
    void sound_effect_compressor(i32 sound_id, f32 gain, f32 attack, f32 release, f32 thresh, f32 ratio, f32 delay);
    void sound_effect_echo(i32 sound_id, f32 wetdry, f32 feedback, f32 leftdelay, f32 rightdelay, i32 pandelay);
    void sound_effect_flanger(i32 sound_id, f32 wetdry, f32 depth, f32 feedback, f32 freq, i32 wave, f32 delay, i32 phase);
    void sound_effect_gargle(i32 sound_id, f32 rate, i32 wave);
    void sound_effect_equalizer(i32 sound_id, f32 center, f32 bandwidth, f32 gain);
    void sound_effect_reverb(i32 sound_id, f32 gain, f32 mix, f32 time, f32 ratio);

    void sound_3d_set_position(i32 sound_id, f32 x, f32 y, f32 z);
    void sound_3d_set_velocity(i32 sound_id, f32 vx, f32 vy, f32 vz);
    void sound_3d_set_distance(i32 sound_id, f32 mindist, f32 maxdist);
    void sound_3d_set_cone(i32 sound_id, f32 x, f32 y, f32 z, f32 anglein, f32 angleout, f32 voloutside);

    // -----------------------------------------------------------------------
    // Unified External Audio API (GMFMODSimple, SuperSound, MaizeMusic, gm82snd)
    // -----------------------------------------------------------------------
    [[nodiscard]] i32 ext_load_from_memory(
        std::span<const u8> encoded,
        bool is_3d,
        bool streamed,
        i32 default_group_id = 0,
        std::string_view hint = {});
    bool ext_free_sound(i32 ext_id);
    [[nodiscard]] bool ext_sound_exists(i32 ext_id) const;
    [[nodiscard]] f64 ext_sound_length_ms(i32 ext_id) const;
    [[nodiscard]] u32 ext_sound_sample_rate(i32 ext_id) const;
    [[nodiscard]] u32 ext_sound_channels(i32 ext_id) const;
    [[nodiscard]] u32 ext_sound_bytes_per_frame(i32 ext_id) const;
    [[nodiscard]] u64 ext_sound_length_bytes(i32 ext_id) const;
    [[nodiscard]] bool ext_sound_is_3d(i32 ext_id) const;
    [[nodiscard]] bool ext_sound_is_streamed(i32 ext_id) const;
    bool ext_sound_set_group(i32 ext_id, i32 group_id);
    bool ext_sound_set_max_volume(i32 ext_id, f32 vol);
    [[nodiscard]] f32 ext_sound_get_max_volume(i32 ext_id) const;
    bool ext_sound_set_loop_count(i32 ext_id, i32 count);
    [[nodiscard]] i32 ext_sound_get_loop_count(i32 ext_id) const;
    bool ext_sound_set_loop_points_ms(i32 ext_id, f64 start_ms, f64 end_ms);
    bool ext_sound_clear_loop_points(i32 ext_id);
    bool ext_sound_set_3d_min_max(i32 ext_id, f32 min_d, f32 max_d);
    [[nodiscard]] f32 ext_sound_get_max_dist(i32 ext_id) const;
    bool ext_sound_set_3d_doppler(i32 ext_id, f32 doppler);
    bool ext_sound_set_3d_cone(i32 ext_id, f32 inside_deg, f32 outside_deg, f32 outside_vol);
    bool ext_sound_set_effects_mask(i32 ext_id, u32 mask);
    bool ext_sound_add_effect(i32 ext_id, DspEffectType type, i32 slot_pos);
    [[nodiscard]] i32 ext_sound_get_music_num_channels(i32 ext_id) const;
    [[nodiscard]] f32 ext_sound_get_music_channel_volume(i32 ext_id, i32 ch) const;
    bool ext_sound_set_music_channel_volume(i32 ext_id, i32 ch, f32 vol);
    [[nodiscard]] std::vector<AudioTag> ext_sound_get_tags(i32 ext_id) const;

    // Channel control
    [[nodiscard]] i32 channel_create(
        i32 ext_id,
        bool looping,
        bool start_paused,
        bool is_3d = false,
        f32 x = 0.0f,
        f32 y = 0.0f,
        f32 z = 0.0f);
    bool channel_stop(i32 ch_id);
    [[nodiscard]] bool channel_exists(i32 ch_id) const;
    [[nodiscard]] bool channel_is_playing(i32 ch_id) const;
    [[nodiscard]] bool channel_is_paused(i32 ch_id) const;
    [[nodiscard]] bool channel_is_looping(i32 ch_id) const;
    [[nodiscard]] i32 channel_get_sound(i32 ch_id) const;
    bool channel_set_paused(i32 ch_id, bool paused);
    bool channel_set_muted(i32 ch_id, bool muted);
    [[nodiscard]] bool channel_get_muted(i32 ch_id) const;
    bool channel_set_volume(i32 ch_id, f32 vol);
    [[nodiscard]] f32 channel_get_volume(i32 ch_id) const;
    bool channel_fade_volume(i32 ch_id, f32 target_vol, f32 duration_ms);
    bool channel_set_pan(i32 ch_id, f32 pan);
    [[nodiscard]] f32 channel_get_pan(i32 ch_id) const;
    bool channel_set_frequency(i32 ch_id, f32 freq_hz);
    [[nodiscard]] f32 channel_get_frequency(i32 ch_id) const;
    bool channel_set_pitch(i32 ch_id, f32 pitch);
    [[nodiscard]] f32 channel_get_pitch(i32 ch_id) const;
    bool channel_set_position_ms(i32 ch_id, f64 ms);
    [[nodiscard]] f64 channel_get_position_ms(i32 ch_id) const;
    [[nodiscard]] u64 channel_get_buffer_cursor(i32 ch_id, u64 buffer_bytes) const;
    void channel_set_buffer_cursor(i32 ch_id, u64 buffer_bytes, u64 byte_offset);
    bool channel_set_loop_count(i32 ch_id, i32 count);
    [[nodiscard]] i32 channel_get_loop_count(i32 ch_id) const;
    bool channel_set_loop_points_ms(i32 ch_id, f64 start_ms, f64 end_ms);
    bool channel_clear_loop_points(i32 ch_id);
    bool channel_set_speaker_mix(i32 ch_id, const std::array<f32, 8>& mix);
    [[nodiscard]] f32 channel_get_audibility(i32 ch_id) const;
    void channel_get_levels(i32 ch_id, f32& out_left, f32& out_right) const;

    bool channel_set_3d_position(i32 ch_id, f32 x, f32 y, f32 z);
    bool channel_set_3d_min_max(i32 ch_id, f32 min_d, f32 max_d);
    [[nodiscard]] f32 channel_get_max_dist(i32 ch_id) const;
    bool channel_set_3d_doppler(i32 ch_id, f32 doppler);
    bool channel_set_3d_cone(i32 ch_id, f32 inside_deg, f32 outside_deg, f32 outside_vol);
    bool channel_set_3d_cone_orientation(i32 ch_id, f32 dx, f32 dy, f32 dz);
    bool channel_set_3d_pan_level(i32 ch_id, f32 level);
    [[nodiscard]] f32 channel_get_3d_pan_level(i32 ch_id) const;
    bool channel_set_3d_spread(i32 ch_id, f32 spread_deg);
    [[nodiscard]] f32 channel_get_3d_spread(i32 ch_id) const;

    // Group & global controls
    [[nodiscard]] i32 active_fmod_channel_count() const;
    [[nodiscard]] bool fmod_owns_sound(i32 sound_id) const;
    void stop_group_channels(i32 group_id);

    bool group_set_volume(i32 group_id, f32 vol);
    [[nodiscard]] f32 group_get_volume(i32 group_id) const;
    bool group_set_pitch(i32 group_id, f32 pitch);
    [[nodiscard]] f32 group_get_pitch(i32 group_id) const;
    bool group_set_frequency(i32 group_id, f32 freq_hz);
    bool group_set_pan(i32 group_id, f32 pan);
    bool group_set_paused(i32 group_id, bool paused);
    [[nodiscard]] bool group_get_paused(i32 group_id) const;
    bool group_set_muted(i32 group_id, bool muted);
    [[nodiscard]] bool group_get_muted(i32 group_id) const;

    // DSP effects management
    [[nodiscard]] i32 dsp_add_to_channel(i32 ch_id, DspEffectType type);
    [[nodiscard]] i32 dsp_add_to_group(i32 group_id, DspEffectType type);
    bool dsp_remove_from_channel(i32 ch_id, i32 dsp_id);
    bool dsp_free(i32 dsp_id);
    bool dsp_set_active(i32 dsp_id, bool active);
    [[nodiscard]] bool dsp_get_active(i32 dsp_id) const;
    bool dsp_set_bypass(i32 dsp_id, bool bypass);
    [[nodiscard]] bool dsp_get_bypass(i32 dsp_id) const;
    bool dsp_set_default_freq(i32 dsp_id, f32 hz);
    [[nodiscard]] f32 dsp_get_default_freq(i32 dsp_id) const;
    bool dsp_set_default_vol(i32 dsp_id, f32 vol);
    [[nodiscard]] f32 dsp_get_default_vol(i32 dsp_id) const;
    bool dsp_set_default_pan(i32 dsp_id, f32 pan);
    [[nodiscard]] f32 dsp_get_default_pan(i32 dsp_id) const;
    bool dsp_set_speaker_active(i32 dsp_id, i32 speaker, bool active);
    [[nodiscard]] bool dsp_get_speaker_active(i32 dsp_id, i32 speaker) const;
    [[nodiscard]] i32 dsp_get_num_params(i32 dsp_id) const;
    [[nodiscard]] DspParamDesc dsp_get_param_desc(i32 dsp_id, i32 idx) const;
    [[nodiscard]] f32 dsp_get_param_value(i32 dsp_id, i32 idx) const;
    [[nodiscard]] std::string dsp_get_param_value_str(i32 dsp_id, i32 idx) const;
    bool dsp_set_param_value(i32 dsp_id, i32 idx, f32 val);
    bool dsp_reset(i32 dsp_id);

    // 3D Listener & Blockers
    void listener_set_number(i32 count);
    void listener_set_3d_pos(i32 idx, f32 x, f32 y, f32 z,
                             f32 fx = 0.0f, f32 fy = 0.0f, f32 fz = 1.0f,
                             f32 ux = 0.0f, f32 uy = 1.0f, f32 uz = 0.0f,
                             bool has_orient = false);
    void listener_set_distance_only(i32 idx, bool dist_only);
    void set_world_scale(f32 scale);
    void set_doppler_fps(f32 fps);

    void blockers_init(i32 max_count);
    void blockers_free();
    [[nodiscard]] i32 blocker_add(f32 x, f32 y, f32 z, f32 xs, f32 ys, f32 zs);
    bool blocker_set_pos(i32 id, f32 x, f32 y, f32 z);
    bool blocker_set_orient(i32 id, f32 fx, f32 fy, f32 fz, f32 ux, f32 uy, f32 uz);
    bool blocker_set_scale(i32 id, f32 sx, f32 sy, f32 sz);
    bool blocker_set_enabled(i32 id, bool enabled);
    [[nodiscard]] bool blocker_get_enabled(i32 id) const;
    bool blocker_set_strength(i32 id, f32 strength);
    [[nodiscard]] f32 blocker_get_strength(i32 id) const;

    // Wave & FFT spectrum snapshots
    bool channel_get_wave_snapshot(i32 ch_id, i32 channel_lr, u32 size, std::vector<f32>& out) const;
    bool channel_get_spectrum_snapshot(i32 ch_id, i32 channel_lr, u32 size, i32 window_type, std::vector<f32>& out) const;
    bool group_get_wave_snapshot(i32 group_id, i32 channel_lr, u32 size, std::vector<f32>& out) const;
    bool group_get_spectrum_snapshot(i32 group_id, i32 channel_lr, u32 size, i32 window_type, std::vector<f32>& out) const;

    // Release the sounds, channels and effects belonging to the FMOD system.
    void clear_fmod_audio();

    // Audio device callback entrypoint
    void mix_frames(f32* out_stereo, u32 frame_count) noexcept;

    // Reusable FFT helper (power-of-two size, returns magnitude spectrum)
    static void compute_fft_magnitude(std::span<const f32> samples, i32 window_type, std::vector<f32>& out);

private:
    bool decode_asset_from_memory(
        std::span<const u8> encoded,
        bool is_3d,
        bool streamed,
        SoundAsset& out_asset,
        std::string_view hint);
    void play_vanilla_internal(i32 sound_id, bool loop);
    void compute_3d_spatial(AudioChannel& ch, f32& out_gain, f32& out_pan, f32& out_doppler) const noexcept;
    void apply_dsp_chain(const std::vector<i32>& dsp_ids, f32* stereo, u32 frame_count) noexcept;
    void attach_asset_effects_to_channel(const SoundAsset& asset, AudioChannel& ch);
    void remove_channel_locked(i32 ch_id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool initialized_ = false;
    tsf* soundfont_ = nullptr;

    mutable std::mutex mutex_;

    // Vanilla GM8 sounds (indexed 0..N-1)
    std::vector<SoundAsset> vanilla_sounds_;
    f32 vanilla_global_volume_ = 1.0f;
    f32 vanilla_bg_tempo_ = 1.0f;
    std::string search_directory_;

    // External sounds (monotonic handles >= 1)
    std::unordered_map<i32, SoundAsset> ext_sounds_;
    i32 next_ext_sound_id_ = 1;

    // Unified active channels (monotonic handles >= 1)
    std::unordered_map<i32, std::unique_ptr<AudioChannel>> channels_;
    i32 next_channel_id_ = 1;

    // Channel groups (0..7)
    std::array<AudioGroupState, static_cast<usize>(AudioGroupId::Count)> groups_{};

    // DSP effect registry (monotonic handles >= 1)
    std::unordered_map<i32, std::unique_ptr<DspEffect>> dsps_;
    i32 next_dsp_id_ = 1;

    // 3D Audio listener & blockers
    i32 listener_count_ = 1;
    std::array<AudioListener3D, 4> listeners_{};
    f32 world_scale_ = 1.0f;
    f32 doppler_fps_ = 0.0f;
    std::vector<AudioBlocker3D> blockers_;

    // Temporary scratch buffers used strictly inside mix_frames (no heap allocs on audio thread!)
    std::vector<f32> mix_ch_buf_;
    std::array<std::vector<f32>, static_cast<usize>(AudioGroupId::Count)> mix_group_bufs_;
};

} // namespace gm
