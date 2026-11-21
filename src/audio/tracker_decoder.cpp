#include "audio/tracker_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gm {

namespace {

constexpr f64 kAmigaClock = 3546895.0; // PAL Paula clock / 2

constexpr std::array<u16, 60> kModPeriods = {
    1712, 1616, 1525, 1440, 1357, 1281, 1209, 1141, 1077, 1017,  961,  907,
     856,  808,  762,  720,  678,  640,  604,  570,  538,  508,  480,  453,
     428,  404,  381,  360,  339,  320,  302,  285,  269,  254,  240,  226,
     214,  202,  190,  180,  170,  160,  151,  143,  135,  127,  120,  113,
     107,  101,   95,   90,   85,   80,   76,   71,   67,   64,   60,   57,
};

constexpr std::array<u8, 32> kSineTable = {
    0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24
};

u16 read_u16_le(std::span<const u8> d, usize off) noexcept {
    if (off + 2 > d.size()) return 0;
    return static_cast<u16>(d[off]) | (static_cast<u16>(d[off + 1]) << 8u);
}

u32 read_u32_le(std::span<const u8> d, usize off) noexcept {
    if (off + 4 > d.size()) return 0;
    return static_cast<u32>(d[off]) |
           (static_cast<u32>(d[off + 1]) << 8u) |
           (static_cast<u32>(d[off + 2]) << 16u) |
           (static_cast<u32>(d[off + 3]) << 24u);
}

u16 read_u16_be(std::span<const u8> d, usize off) noexcept {
    if (off + 2 > d.size()) return 0;
    return (static_cast<u16>(d[off]) << 8u) | static_cast<u16>(d[off + 1]);
}

std::string read_ascii_fixed(std::span<const u8> d, usize off, usize len) {
    if (off >= d.size()) return {};
    const usize avail = std::min(len, d.size() - off);
    usize end = avail;
    for (usize i = 0; i < avail; ++i) {
        if (d[off + i] == 0) { end = i; break; }
    }
    return std::string(reinterpret_cast<const char*>(d.data() + off), end);
}

i32 detect_mod_channels(std::span<const u8> data) noexcept {
    if (data.size() < 1084) return 0;
    const char tag[5] = {
        static_cast<char>(data[1080]),
        static_cast<char>(data[1081]),
        static_cast<char>(data[1082]),
        static_cast<char>(data[1083]),
        '\0'
    };
    if (std::strcmp(tag, "M.K.") == 0 || std::strcmp(tag, "M!K!") == 0 ||
        std::strcmp(tag, "FLT4") == 0 || std::strcmp(tag, "4CHN") == 0 ||
        std::strcmp(tag, "M&K!") == 0) {
        return 4;
    }
    if (std::strcmp(tag, "FLT8") == 0 || std::strcmp(tag, "CD81") == 0 ||
        std::strcmp(tag, "OKTA") == 0 || std::strcmp(tag, "OCTA") == 0) {
        return 8;
    }
    if (tag[1] == 'C' && tag[2] == 'H' && tag[3] == 'N' && tag[0] >= '1' && tag[0] <= '9') {
        return tag[0] - '0';
    }
    if (tag[2] == 'C' && tag[3] == 'H' &&
        tag[0] >= '0' && tag[0] <= '9' && tag[1] >= '0' && tag[1] <= '9') {
        const i32 ch = (tag[0] - '0') * 10 + (tag[1] - '0');
        if (ch >= 1 && ch <= 32) return ch;
    }
    if (tag[0] == 'T' && tag[1] == 'D' && tag[2] == 'Z' && tag[3] >= '1' && tag[3] <= '9') {
        return tag[3] - '0';
    }
    return 0;
}

f64 note_to_period(i32 note) noexcept {
    // Unified note: 1 = C-0, 49 = C-4 -> Amiga period 428
    if (note <= 0) return 0.0;
    return 428.0 * std::pow(2.0, (49.0 - static_cast<f64>(note)) / 12.0);
}

i32 period_to_note(u16 period) noexcept {
    if (period == 0) return 0;
    // Closest note where C-4 (49) = 428
    const f64 n = 49.0 - 12.0 * std::log2(static_cast<f64>(period) / 428.0);
    return std::clamp(static_cast<i32>(std::round(n)), 1, 96);
}

f64 estimate_module_duration_ms(const TrackerModule& mod) noexcept {
    u32 spd = mod.initial_speed > 0 ? mod.initial_speed : 6u;
    u32 bpm = mod.initial_bpm > 0 ? mod.initial_bpm : 125u;
    f64 total_ms = 0.0;
    for (usize oi = 0; oi < mod.orders.size(); ++oi) {
        const u8 pat_idx = mod.orders[oi];
        if (pat_idx >= mod.patterns.size()) continue;
        const auto& pat = mod.patterns[pat_idx];
        bool broke = false;
        for (u32 r = 0; r < pat.rows; ++r) {
            for (u32 c = 0; c < pat.channels; ++c) {
                const auto& cell = pat.cells[r * pat.channels + c];
                if (cell.effect == 0x0F && cell.param > 0) {
                    if (cell.param < 0x20) spd = cell.param;
                    else bpm = cell.param;
                } else if (cell.effect == 0x1F && cell.param > 0) {
                    spd = cell.param;
                } else if (cell.effect == 0x1E && cell.param >= 0x20) {
                    bpm = cell.param;
                } else if (cell.effect == 0x0D) {
                    broke = true;
                }
            }
            const f64 tick_ms = 2500.0 / static_cast<f64>(std::max<u32>(bpm, 32u));
            total_ms += tick_ms * static_cast<f64>(spd);
            if (broke) break;
        }
    }
    return std::max(total_ms, 1000.0);
}

Result<std::shared_ptr<TrackerModule>> parse_mod(std::span<const u8> data) {
    const i32 nch = detect_mod_channels(data);
    if (nch <= 0) return Error{"Unsupported or invalid MOD format"};

    auto mod = std::make_shared<TrackerModule>();
    mod->format = TrackerFormat::Mod;
    mod->title = read_ascii_fixed(data, 0, 20);
    mod->num_channels = static_cast<u32>(nch);
    mod->initial_speed = 6;
    mod->initial_bpm = 125;
    mod->initial_pan.resize(mod->num_channels, 0.0f);
    for (u32 c = 0; c < mod->num_channels; ++c) {
        // Amiga hard-stereo softened slightly for headphones
        mod->initial_pan[c] = ((c & 3u) == 0u || (c & 3u) == 3u) ? -0.55f : 0.55f;
    }

    struct ModSampleHeader {
        u32 len_bytes = 0;
        u32 loop_start = 0;
        u32 loop_len = 0;
        i8 finetune = 0;
        u8 vol = 64;
        std::string name;
    };
    std::array<ModSampleHeader, 31> sh{};
    for (usize i = 0; i < 31; ++i) {
        const usize off = 20 + i * 30;
        sh[i].name = read_ascii_fixed(data, off, 22);
        sh[i].len_bytes = static_cast<u32>(read_u16_be(data, off + 22)) * 2u;
        const u8 ft = data[off + 24] & 0x0Fu;
        sh[i].finetune = (ft >= 8u) ? static_cast<i8>(ft - 16) : static_cast<i8>(ft);
        sh[i].vol = std::min<u8>(data[off + 25], 64u);
        sh[i].loop_start = static_cast<u32>(read_u16_be(data, off + 26)) * 2u;
        sh[i].loop_len = static_cast<u32>(read_u16_be(data, off + 28)) * 2u;
    }

    const u8 song_len = std::clamp<u8>(data[950], 1u, 128u);
    u8 max_pat = 0;
    mod->orders.reserve(song_len);
    for (u32 i = 0; i < 128; ++i) {
        const u8 p = data[952 + i];
        if (i < song_len) mod->orders.push_back(p);
        if (p > max_pat && p < 128) max_pat = p;
    }

    const u32 num_patterns = static_cast<u32>(max_pat) + 1u;
    usize pos = 1084;
    mod->patterns.resize(num_patterns);
    for (u32 p = 0; p < num_patterns; ++p) {
        auto& pat = mod->patterns[p];
        pat.rows = 64;
        pat.channels = mod->num_channels;
        pat.cells.resize(64u * mod->num_channels);
        for (u32 r = 0; r < 64; ++r) {
            for (u32 c = 0; c < mod->num_channels; ++c) {
                if (pos + 4 > data.size()) break;
                const u8 b0 = data[pos + 0];
                const u8 b1 = data[pos + 1];
                const u8 b2 = data[pos + 2];
                const u8 b3 = data[pos + 3];
                pos += 4;

                const u8 smp = static_cast<u8>((b0 & 0xF0u) | (b2 >> 4u));
                const u16 per = static_cast<u16>(((b0 & 0x0Fu) << 8u) | b1);
                const u8 eff = b2 & 0x0Fu;
                const u8 prm = b3;

                auto& cell = pat.cells[r * mod->num_channels + c];
                cell.note = static_cast<u8>(period_to_note(per));
                cell.sample = smp;
                cell.effect = eff;
                cell.param = prm;
            }
        }
    }

    mod->samples.resize(31);
    for (usize i = 0; i < 31; ++i) {
        auto& dst = mod->samples[i];
        dst.name = sh[i].name;
        dst.finetune = sh[i].finetune;
        dst.default_volume = static_cast<f32>(sh[i].vol) / 64.0f;
        dst.c4_rate = 8287.14 * std::pow(2.0, static_cast<f64>(sh[i].finetune) / (12.0 * 8.0));
        const usize avail = (pos < data.size()) ? std::min<usize>(sh[i].len_bytes, data.size() - pos) : 0u;
        dst.pcm.resize(avail);
        for (usize s = 0; s < avail; ++s) {
            dst.pcm[s] = static_cast<f32>(static_cast<i8>(data[pos + s])) / 128.0f;
        }
        pos += sh[i].len_bytes;

        if (sh[i].loop_len > 2u && sh[i].loop_start < dst.pcm.size()) {
            dst.loop_enabled = true;
            dst.loop_start = sh[i].loop_start;
            dst.loop_end = std::min<u32>(sh[i].loop_start + sh[i].loop_len, static_cast<u32>(dst.pcm.size()));
        }
    }

    mod->estimated_duration_ms = estimate_module_duration_ms(*mod);
    return mod;
}

Result<std::shared_ptr<TrackerModule>> parse_s3m(std::span<const u8> data) {
    if (data.size() < 96 || std::memcmp(data.data() + 44, "SCRM", 4) != 0) {
        return Error{"Invalid S3M header"};
    }

    auto mod = std::make_shared<TrackerModule>();
    mod->format = TrackerFormat::S3m;
    mod->title = read_ascii_fixed(data, 0, 28);

    const u16 ord_num = read_u16_le(data, 32);
    const u16 ins_num = read_u16_le(data, 34);
    const u16 pat_num = read_u16_le(data, 36);
    const u16 ffi = read_u16_le(data, 42); // 1 = signed samples, 2 = unsigned samples
    const bool unsigned_pcm = (ffi == 2);

    mod->global_volume = static_cast<f32>(std::min<u8>(data[48], 64u)) / 64.0f;
    mod->initial_speed = data[49] > 0 ? data[49] : 6u;
    mod->initial_bpm = data[50] >= 32 ? data[50] : 125u;
    const u8 default_pan_flag = data[53];

    // Map enabled S3M channels (up to 32)
    std::array<i32, 32> ch_remap{};
    ch_remap.fill(-1);
    u32 active_ch = 0;
    for (u32 c = 0; c < 32; ++c) {
        const u8 setting = data[64 + c];
        if (setting < 16) {
            ch_remap[c] = static_cast<i32>(active_ch++);
            mod->initial_pan.push_back(setting < 8 ? -0.5f : 0.5f);
        }
    }
    if (active_ch == 0) {
        active_ch = 4;
        mod->initial_pan.assign(4, 0.0f);
        for (u32 c = 0; c < 4; ++c) ch_remap[c] = static_cast<i32>(c);
    }
    mod->num_channels = active_ch;

    usize cursor = 96;
    for (u32 i = 0; i < ord_num && cursor < data.size(); ++i, ++cursor) {
        const u8 o = data[cursor];
        if (o == 255) break;
        if (o == 254) continue;
        if (o < pat_num) mod->orders.push_back(o);
    }
    cursor = 96u + ord_num;

    std::vector<u32> ins_ptrs(ins_num, 0);
    for (u32 i = 0; i < ins_num; ++i) {
        ins_ptrs[i] = static_cast<u32>(read_u16_le(data, cursor + i * 2u)) * 16u;
    }
    cursor += static_cast<usize>(ins_num) * 2u;

    std::vector<u32> pat_ptrs(pat_num, 0);
    for (u32 i = 0; i < pat_num; ++i) {
        pat_ptrs[i] = static_cast<u32>(read_u16_le(data, cursor + i * 2u)) * 16u;
    }
    cursor += static_cast<usize>(pat_num) * 2u;

    if (default_pan_flag == 252 && cursor + 32 <= data.size()) {
        for (u32 c = 0; c < 32; ++c) {
            if (ch_remap[c] < 0) continue;
            const u8 p = data[cursor + c];
            if (p & 0x20u) {
                mod->initial_pan[static_cast<usize>(ch_remap[c])] =
                    (static_cast<f32>(p & 0x0Fu) / 7.5f) - 1.0f;
            }
        }
    }

    // Parse S3M instruments (PCM samples)
    mod->samples.resize(ins_num);
    for (u32 i = 0; i < ins_num; ++i) {
        const usize off = ins_ptrs[i];
        if (off == 0 || off + 80 > data.size()) continue;
        const u8 type = data[off];
        if (type != 1) continue; // Only PCM samples (skip AdLib)
        auto& dst = mod->samples[i];
        dst.name = read_ascii_fixed(data, off + 48, 28);
        const u32 memseg = (static_cast<u32>(data[off + 13]) << 16u) | read_u16_le(data, off + 14);
        const usize smp_off = static_cast<usize>(memseg) * 16u;
        const u32 length = read_u32_le(data, off + 16);
        const u32 loop_beg = read_u32_le(data, off + 20);
        const u32 loop_end = read_u32_le(data, off + 24);
        const u8 vol = std::min<u8>(data[off + 28], 64u);
        const u8 pack = data[off + 30];
        const u8 flags = data[off + 31];
        const u32 c2spd = read_u32_le(data, off + 32);

        dst.default_volume = static_cast<f32>(vol) / 64.0f;
        dst.c4_rate = c2spd > 0 ? static_cast<f64>(c2spd) : 8363.0;
        if (pack != 0 || smp_off >= data.size() || length == 0) continue;

        const bool is_stereo = (flags & 2u) != 0;
        const bool is_16bit = (flags & 4u) != 0;
        const u32 bytes_per_smp = is_16bit ? 2u : 1u;
        const usize avail_frames = std::min<usize>(length, (data.size() - smp_off) / bytes_per_smp);
        dst.pcm.resize(avail_frames);
        for (usize s = 0; s < avail_frames; ++s) {
            if (is_16bit) {
                const u16 raw = read_u16_le(data, smp_off + s * 2u);
                const i32 sv = unsigned_pcm ? (static_cast<i32>(raw) - 32768)
                                            : static_cast<i32>(static_cast<i16>(raw));
                dst.pcm[s] = static_cast<f32>(sv) / 32768.0f;
            } else {
                const u8 raw = data[smp_off + s];
                const i32 sv = unsigned_pcm ? (static_cast<i32>(raw) - 128)
                                            : static_cast<i32>(static_cast<i8>(raw));
                dst.pcm[s] = static_cast<f32>(sv) / 128.0f;
            }
        }
        (void)is_stereo;
        if ((flags & 1u) && loop_end > loop_beg && loop_beg < dst.pcm.size()) {
            dst.loop_enabled = true;
            dst.loop_start = loop_beg;
            dst.loop_end = std::min<u32>(loop_end, static_cast<u32>(dst.pcm.size()));
        }
    }

    // Parse S3M packed patterns
    mod->patterns.resize(pat_num);
    for (u32 p = 0; p < pat_num; ++p) {
        auto& pat = mod->patterns[p];
        pat.rows = 64;
        pat.channels = mod->num_channels;
        pat.cells.resize(64u * mod->num_channels);
        const usize off = pat_ptrs[p];
        if (off == 0 || off + 2 > data.size()) continue;
        const u16 packed_len = read_u16_le(data, off);
        usize pos = off + 2;
        const usize end_pos = std::min(data.size(), off + static_cast<usize>(packed_len) + 2u);

        u32 row = 0;
        while (row < 64 && pos < end_pos) {
            const u8 what = data[pos++];
            if (what == 0) {
                ++row;
                continue;
            }
            const u32 raw_ch = what & 31u;
            u8 note = 0, ins = 0, vol = 255, cmd = 0, info = 0;
            if (what & 32u) {
                if (pos + 2 > end_pos) break;
                const u8 nb = data[pos++];
                ins = data[pos++];
                if (nb == 254) {
                    note = 97; // note cut / keyoff
                } else if (nb < 254) {
                    const u32 oct = nb >> 4u;
                    const u32 semi = nb & 0x0Fu;
                    if (semi < 12) {
                        note = static_cast<u8>(std::clamp<u32>(oct * 12u + semi + 1u, 1u, 96u));
                    }
                }
            }
            if (what & 64u) {
                if (pos >= end_pos) break;
                vol = data[pos++];
            }
            if (what & 128u) {
                if (pos + 2 > end_pos) break;
                cmd = data[pos++];
                info = data[pos++];
            }

            const i32 mapped_ch = ch_remap[raw_ch];
            if (mapped_ch < 0) continue;
            auto& cell = pat.cells[row * mod->num_channels + static_cast<u32>(mapped_ch)];
            cell.note = note;
            cell.sample = ins;
            if (vol <= 64) {
                cell.vol_cmd = 1;
                cell.vol_val = vol;
            }
            // Translate key S3M commands (1='A' .. 26='Z') into unified effect IDs
            switch (cmd) {
                case 1:  cell.effect = 0x1F; cell.param = info; break; // A: set speed
                case 2:  cell.effect = 0x0B; cell.param = info; break; // B: position jump
                case 3:  cell.effect = 0x0D; cell.param = info; break; // C: pattern break
                case 4:  cell.effect = 0x0A; cell.param = info; break; // D: volume slide
                case 5:  cell.effect = 0x02; cell.param = info; break; // E: porta down
                case 6:  cell.effect = 0x01; cell.param = info; break; // F: porta up
                case 7:  cell.effect = 0x03; cell.param = info; break; // G: tone porta
                case 8:  cell.effect = 0x04; cell.param = info; break; // H: vibrato
                case 10: cell.effect = 0x00; cell.param = info; break; // J: arpeggio
                case 15: cell.effect = 0x09; cell.param = info; break; // O: sample offset
                case 20: cell.effect = 0x1E; cell.param = info; break; // T: set tempo
                case 22: cell.effect = 0x1D; cell.param = info; break; // V: global volume
                case 24: cell.effect = 0x08; cell.param = info; break; // X: set panning
                default: break;
            }
        }
    }

    mod->estimated_duration_ms = estimate_module_duration_ms(*mod);
    return mod;
}

Result<std::shared_ptr<TrackerModule>> parse_xm(std::span<const u8> data) {
    if (data.size() < 80 || std::memcmp(data.data(), "Extended Module: ", 17) != 0) {
        return Error{"Invalid XM header"};
    }

    auto mod = std::make_shared<TrackerModule>();
    mod->format = TrackerFormat::Xm;
    mod->title = read_ascii_fixed(data, 17, 20);

    const u32 hdr_size = read_u32_le(data, 60);
    const u16 song_len = std::min<u16>(read_u16_le(data, 64), 256u);
    const u16 num_ch = std::clamp<u16>(read_u16_le(data, 68), 1u, 32u);
    const u16 num_pat = read_u16_le(data, 70);
    const u16 num_ins = read_u16_le(data, 72);
    mod->num_channels = num_ch;
    mod->initial_speed = std::max<u16>(read_u16_le(data, 76), 1u);
    mod->initial_bpm = std::max<u16>(read_u16_le(data, 78), 32u);
    mod->initial_pan.assign(num_ch, 0.0f);

    for (u32 i = 0; i < song_len && (80u + i) < data.size(); ++i) {
        mod->orders.push_back(data[80u + i]);
    }

    usize pos = 60u + hdr_size;
    mod->patterns.resize(num_pat);
    for (u32 p = 0; p < num_pat && pos + 9 <= data.size(); ++p) {
        const u32 p_hdr_len = read_u32_le(data, pos);
        const u16 rows = std::clamp<u16>(read_u16_le(data, pos + 5), 1u, 256u);
        const u16 packed_sz = read_u16_le(data, pos + 7);
        pos += p_hdr_len;
        const usize end_pos = std::min(data.size(), pos + packed_sz);

        auto& pat = mod->patterns[p];
        pat.rows = rows;
        pat.channels = num_ch;
        pat.cells.resize(static_cast<usize>(rows) * num_ch);

        for (u32 r = 0; r < rows && pos < end_pos; ++r) {
            for (u32 c = 0; c < num_ch && pos < end_pos; ++c) {
                auto& cell = pat.cells[r * num_ch + c];
                const u8 b = data[pos++];
                u8 note = 0, ins = 0, vol = 0, eff = 0, prm = 0;
                if (b & 0x80u) {
                    if ((b & 0x01u) && pos < end_pos) note = data[pos++];
                    if ((b & 0x02u) && pos < end_pos) ins = data[pos++];
                    if ((b & 0x04u) && pos < end_pos) vol = data[pos++];
                    if ((b & 0x08u) && pos < end_pos) eff = data[pos++];
                    if ((b & 0x10u) && pos < end_pos) prm = data[pos++];
                } else {
                    note = b;
                    if (pos < end_pos) ins = data[pos++];
                    if (pos < end_pos) vol = data[pos++];
                    if (pos < end_pos) eff = data[pos++];
                    if (pos < end_pos) prm = data[pos++];
                }
                cell.note = note;
                cell.sample = ins;
                if (vol >= 0x10 && vol <= 0x50) {
                    cell.vol_cmd = 1;
                    cell.vol_val = static_cast<u8>(vol - 0x10);
                }
                cell.effect = eff;
                cell.param = prm;
            }
        }
        pos = end_pos;
    }

    // Parse XM instruments (mapping first sample per instrument cleanly)
    mod->samples.resize(num_ins);
    for (u32 i = 0; i < num_ins && pos + 29 <= data.size(); ++i) {
        const u32 ins_hdr_size = read_u32_le(data, pos);
        const std::string ins_name = read_ascii_fixed(data, pos + 4, 22);
        const u16 n_smp = read_u16_le(data, pos + 27);
        u32 smp_hdr_size = 40;
        if (n_smp > 0 && pos + 33 <= data.size()) {
            smp_hdr_size = read_u32_le(data, pos + 29);
            if (smp_hdr_size == 0) smp_hdr_size = 40;
        }
        pos += ins_hdr_size;

        struct XmSmpMeta {
            u32 len_bytes = 0;
            u32 loop_start = 0;
            u32 loop_len = 0;
            u8 vol = 64;
            i8 finetune = 0;
            u8 type = 0;
            u8 pan = 128;
            i8 rel_note = 0;
        };
        std::vector<XmSmpMeta> metas(n_smp);
        for (u32 s = 0; s < n_smp && pos + smp_hdr_size <= data.size(); ++s) {
            metas[s].len_bytes = read_u32_le(data, pos + 0);
            metas[s].loop_start = read_u32_le(data, pos + 4);
            metas[s].loop_len = read_u32_le(data, pos + 8);
            metas[s].vol = std::min<u8>(data[pos + 12], 64u);
            metas[s].finetune = static_cast<i8>(data[pos + 13]);
            metas[s].type = data[pos + 14];
            metas[s].pan = data[pos + 15];
            metas[s].rel_note = static_cast<i8>(data[pos + 16]);
            pos += smp_hdr_size;
        }
        for (u32 s = 0; s < n_smp; ++s) {
            const usize avail = (pos < data.size()) ? std::min<usize>(metas[s].len_bytes, data.size() - pos) : 0u;
            if (s == 0 && avail > 0) {
                auto& dst = mod->samples[i];
                dst.name = ins_name;
                dst.default_volume = static_cast<f32>(metas[s].vol) / 64.0f;
                dst.default_pan = (static_cast<f32>(metas[s].pan) / 127.5f) - 1.0f;
                dst.finetune = metas[s].finetune;
                dst.relative_note = metas[s].rel_note;
                dst.c4_rate = 8363.0 * std::pow(2.0, (static_cast<f64>(dst.relative_note) + static_cast<f64>(dst.finetune) / 128.0) / 12.0);
                const bool is_16 = (metas[s].type & 0x10u) != 0;
                if (is_16) {
                    const usize n = avail / 2u;
                    dst.pcm.resize(n);
                    i16 acc = 0;
                    for (usize k = 0; k < n; ++k) {
                        acc = static_cast<i16>(acc + static_cast<i16>(read_u16_le(data, pos + k * 2u)));
                        dst.pcm[k] = static_cast<f32>(acc) / 32768.0f;
                    }
                    if ((metas[s].type & 3u) != 0 && metas[s].loop_len > 0) {
                        dst.loop_enabled = true;
                        dst.ping_pong = (metas[s].type & 3u) == 2u;
                        dst.loop_start = metas[s].loop_start / 2u;
                        dst.loop_end = std::min<u32>((metas[s].loop_start + metas[s].loop_len) / 2u, static_cast<u32>(n));
                    }
                } else {
                    dst.pcm.resize(avail);
                    i8 acc = 0;
                    for (usize k = 0; k < avail; ++k) {
                        acc = static_cast<i8>(acc + static_cast<i8>(data[pos + k]));
                        dst.pcm[k] = static_cast<f32>(acc) / 128.0f;
                    }
                    if ((metas[s].type & 3u) != 0 && metas[s].loop_len > 0) {
                        dst.loop_enabled = true;
                        dst.ping_pong = (metas[s].type & 3u) == 2u;
                        dst.loop_start = metas[s].loop_start;
                        dst.loop_end = std::min<u32>(metas[s].loop_start + metas[s].loop_len, static_cast<u32>(avail));
                    }
                }
            }
            pos += metas[s].len_bytes;
        }
    }

    mod->estimated_duration_ms = estimate_module_duration_ms(*mod);
    return mod;
}

Result<std::shared_ptr<TrackerModule>> parse_it(std::span<const u8> data) {
    if (data.size() < 192 || std::memcmp(data.data(), "IMPM", 4) != 0) {
        return Error{"Invalid IT header"};
    }

    auto mod = std::make_shared<TrackerModule>();
    mod->format = TrackerFormat::It;
    mod->title = read_ascii_fixed(data, 4, 26);

    const u16 ord_num = read_u16_le(data, 32);
    const u16 ins_num = read_u16_le(data, 34);
    const u16 smp_num = read_u16_le(data, 36);
    const u16 pat_num = read_u16_le(data, 38);
    mod->global_volume = static_cast<f32>(std::min<u8>(data[48], 128u)) / 128.0f;
    mod->initial_speed = data[50] > 0 ? data[50] : 6u;
    mod->initial_bpm = data[51] >= 32 ? data[51] : 125u;
    mod->num_channels = 32;
    mod->initial_pan.resize(32, 0.0f);
    for (u32 c = 0; c < 32; ++c) {
        const u8 pan = data[64 + c] & 0x7Fu;
        if (pan <= 64) mod->initial_pan[c] = (static_cast<f32>(pan) / 32.0f) - 1.0f;
    }

    usize cursor = 192;
    for (u32 i = 0; i < ord_num && cursor < data.size(); ++i, ++cursor) {
        const u8 o = data[cursor];
        if (o == 255) break;
        if (o == 254) continue;
        if (o < pat_num) mod->orders.push_back(o);
    }
    cursor = 192u + ord_num + static_cast<usize>(ins_num) * 4u;

    std::vector<u32> smp_ptrs(smp_num, 0);
    for (u32 i = 0; i < smp_num; ++i) {
        smp_ptrs[i] = read_u32_le(data, cursor + i * 4u);
    }
    cursor += static_cast<usize>(smp_num) * 4u;

    std::vector<u32> pat_ptrs(pat_num, 0);
    for (u32 i = 0; i < pat_num; ++i) {
        pat_ptrs[i] = read_u32_le(data, cursor + i * 4u);
    }

    mod->samples.resize(smp_num);
    for (u32 i = 0; i < smp_num; ++i) {
        const usize off = smp_ptrs[i];
        if (off == 0 || off + 80 > data.size() || std::memcmp(data.data() + off, "IMPS", 4) != 0) continue;
        auto& dst = mod->samples[i];
        dst.name = read_ascii_fixed(data, off + 20, 26);
        const u8 flags = data[off + 18];
        const u8 vol = std::min<u8>(data[off + 19], 64u);
        const u8 cvt = data[off + 46];
        const u32 len = read_u32_le(data, off + 48);
        const u32 lb = read_u32_le(data, off + 52);
        const u32 le = read_u32_le(data, off + 56);
        const u32 c5spd = read_u32_le(data, off + 60);
        const u32 smp_ptr = read_u32_le(data, off + 72);

        dst.default_volume = static_cast<f32>(vol) / 64.0f;
        dst.c4_rate = c5spd > 0 ? static_cast<f64>(c5spd) : 8363.0;
        if (!(flags & 1u) || (flags & 8u) || smp_ptr >= data.size() || len == 0) continue;

        const bool is_16 = (flags & 2u) != 0;
        const bool is_signed = (cvt & 1u) != 0;
        const usize avail = std::min<usize>(len, (data.size() - smp_ptr) / (is_16 ? 2u : 1u));
        dst.pcm.resize(avail);
        for (usize k = 0; k < avail; ++k) {
            if (is_16) {
                const u16 raw = read_u16_le(data, smp_ptr + k * 2u);
                const i32 sv = is_signed ? static_cast<i32>(static_cast<i16>(raw)) : (static_cast<i32>(raw) - 32768);
                dst.pcm[k] = static_cast<f32>(sv) / 32768.0f;
            } else {
                const u8 raw = data[smp_ptr + k];
                const i32 sv = is_signed ? static_cast<i32>(static_cast<i8>(raw)) : (static_cast<i32>(raw) - 128);
                dst.pcm[k] = static_cast<f32>(sv) / 128.0f;
            }
        }
        if ((flags & 16u) && le > lb && lb < dst.pcm.size()) {
            dst.loop_enabled = true;
            dst.ping_pong = (flags & 64u) != 0;
            dst.loop_start = lb;
            dst.loop_end = std::min<u32>(le, static_cast<u32>(dst.pcm.size()));
        }
    }

    mod->patterns.resize(pat_num);
    for (u32 p = 0; p < pat_num; ++p) {
        auto& pat = mod->patterns[p];
        pat.rows = 64;
        pat.channels = mod->num_channels;
        const usize off = pat_ptrs[p];
        if (off == 0 || off + 8 > data.size()) {
            pat.cells.resize(64u * mod->num_channels);
            continue;
        }
        const u16 len = read_u16_le(data, off);
        const u16 rows = std::clamp<u16>(read_u16_le(data, off + 2), 1u, 256u);
        pat.rows = rows;
        pat.cells.resize(static_cast<usize>(rows) * mod->num_channels);

        usize pos = off + 8;
        const usize end_pos = std::min(data.size(), pos + len);
        std::array<u8, 64> last_mask{};
        std::array<TrackerCell, 64> last_cell{};
        u32 row = 0;
        while (row < rows && pos < end_pos) {
            const u8 chvar = data[pos++];
            if (chvar == 0) {
                ++row;
                continue;
            }
            const u32 ch = (chvar - 1u) & 63u;
            u8 mask = last_mask[ch];
            if (chvar & 128u) {
                if (pos >= end_pos) break;
                mask = data[pos++];
                last_mask[ch] = mask;
            }
            TrackerCell c{};
            if (mask & 1u) {
                if (pos >= end_pos) break;
                const u8 n = data[pos++];
                c.note = (n >= 254) ? 97 : static_cast<u8>(std::clamp<i32>(static_cast<i32>(n) - 11, 1, 96));
                last_cell[ch].note = c.note;
            } else if (mask & 16u) {
                c.note = last_cell[ch].note;
            }
            if (mask & 2u) {
                if (pos >= end_pos) break;
                c.sample = data[pos++];
                last_cell[ch].sample = c.sample;
            } else if (mask & 32u) {
                c.sample = last_cell[ch].sample;
            }
            if (mask & 4u) {
                if (pos >= end_pos) break;
                const u8 v = data[pos++];
                if (v <= 64) { c.vol_cmd = 1; c.vol_val = v; }
                last_cell[ch].vol_cmd = c.vol_cmd;
                last_cell[ch].vol_val = c.vol_val;
            } else if (mask & 64u) {
                c.vol_cmd = last_cell[ch].vol_cmd;
                c.vol_val = last_cell[ch].vol_val;
            }
            if (mask & 8u) {
                if (pos + 2 > end_pos) break;
                c.effect = data[pos++];
                c.param = data[pos++];
                last_cell[ch].effect = c.effect;
                last_cell[ch].param = c.param;
            } else if (mask & 128u) {
                c.effect = last_cell[ch].effect;
                c.param = last_cell[ch].param;
            }
            if (ch < mod->num_channels) {
                pat.cells[row * mod->num_channels + ch] = c;
            }
        }
    }

    mod->estimated_duration_ms = estimate_module_duration_ms(*mod);
    return mod;
}

} // namespace

TrackerFormat detect_tracker_format(std::span<const u8> data) noexcept {
    if (data.size() >= 17 && std::memcmp(data.data(), "Extended Module: ", 17) == 0) {
        return TrackerFormat::Xm;
    }
    if (data.size() >= 4 && std::memcmp(data.data(), "IMPM", 4) == 0) {
        return TrackerFormat::It;
    }
    if (data.size() >= 48 && std::memcmp(data.data() + 44, "SCRM", 4) == 0) {
        return TrackerFormat::S3m;
    }
    if (detect_mod_channels(data) > 0) {
        return TrackerFormat::Mod;
    }
    return TrackerFormat::Unknown;
}

Result<std::shared_ptr<TrackerModule>> load_tracker_module(std::span<const u8> data) {
    switch (detect_tracker_format(data)) {
        case TrackerFormat::Mod: return parse_mod(data);
        case TrackerFormat::S3m: return parse_s3m(data);
        case TrackerFormat::Xm:  return parse_xm(data);
        case TrackerFormat::It:  return parse_it(data);
        default: return Error{"Unrecognized tracker module format"};
    }
}

TrackerPlayer::TrackerPlayer(std::shared_ptr<const TrackerModule> mod, u32 output_sample_rate)
    : mod_(std::move(mod)), sample_rate_(output_sample_rate > 0 ? output_sample_rate : 44100u) {
    reset();
}

void TrackerPlayer::reset() noexcept {
    order_idx_ = 0;
    row_idx_ = 0;
    tick_ = 0;
    pending_jump_order_ = -1;
    pending_break_row_ = -1;
    played_frames_ = 0;
    finished_ = false;
    tick_sample_accum_ = 0.0;

    if (!mod_) {
        channels_.clear();
        return;
    }

    speed_ = std::max<u32>(mod_->initial_speed, 1u);
    bpm_ = std::max<u32>(mod_->initial_bpm, 32u);
    global_vol_ = mod_->global_volume;
    samples_per_tick_ = static_cast<f64>(sample_rate_) * 2.5 / static_cast<f64>(bpm_);

    std::vector<f32> prev_user_vols;
    prev_user_vols.reserve(channels_.size());
    for (const auto& c : channels_) prev_user_vols.push_back(c.user_volume);

    channels_.assign(mod_->num_channels, ChannelState{});
    for (u32 c = 0; c < mod_->num_channels; ++c) {
        if (c < mod_->initial_pan.size()) channels_[c].pan = mod_->initial_pan[c];
        if (c < prev_user_vols.size()) channels_[c].user_volume = prev_user_vols[c];
    }
}

void TrackerPlayer::seek_ms(f64 ms) noexcept {
    if (!mod_ || mod_->orders.empty()) return;
    reset();
    if (ms <= 0.0) return;

    const u64 target_frames = static_cast<u64>(ms * 0.001 * static_cast<f64>(sample_rate_));
    u64 sim_frames = 0;
    while (sim_frames < target_frames && !finished_) {
        advance_tick(true);
        sim_frames += static_cast<u64>(samples_per_tick_);
    }
    played_frames_ = target_frames;
}

f32 TrackerPlayer::channel_volume(u32 ch) const noexcept {
    if (ch >= channels_.size()) return 0.0f;
    return channels_[ch].user_volume;
}

void TrackerPlayer::set_channel_volume(u32 ch, f32 vol) noexcept {
    if (ch < channels_.size()) {
        channels_[ch].user_volume = std::clamp(vol, 0.0f, 1.0f);
    }
}

void TrackerPlayer::update_channel_step(ChannelState& ch, f64 period_override) noexcept {
    const f64 p = (period_override > 0.0) ? period_override : ch.period;
    if (p <= 1.0 || ch.sample_idx < 0 || !mod_ ||
        static_cast<usize>(ch.sample_idx) >= mod_->samples.size()) {
        ch.sample_step = 0.0;
        return;
    }
    const auto& smp = mod_->samples[static_cast<usize>(ch.sample_idx)];
    const f64 hz = (mod_->format == TrackerFormat::Mod)
                       ? (kAmigaClock / p) * (smp.c4_rate / 8287.14)
                       : smp.c4_rate * (428.0 / p);
    ch.sample_step = hz / static_cast<f64>(sample_rate_);
}

void TrackerPlayer::process_row(bool loop) noexcept {
    if (!mod_ || mod_->orders.empty()) {
        finished_ = true;
        return;
    }
    if (order_idx_ >= mod_->orders.size()) {
        if (loop) {
            order_idx_ = 0;
            row_idx_ = 0;
        } else {
            finished_ = true;
            return;
        }
    }

    const u8 pat_idx = mod_->orders[order_idx_];
    if (pat_idx >= mod_->patterns.size()) {
        finished_ = true;
        return;
    }
    const auto& pat = mod_->patterns[pat_idx];
    if (row_idx_ >= pat.rows) {
        row_idx_ = 0;
    }

    pending_jump_order_ = -1;
    pending_break_row_ = -1;

    for (u32 c = 0; c < mod_->num_channels && c < pat.channels; ++c) {
        const auto& cell = pat.cells[row_idx_ * pat.channels + c];
        auto& ch = channels_[c];

        ch.effect = cell.effect;
        ch.param = cell.param;

        if (cell.sample > 0 && static_cast<usize>(cell.sample - 1) < mod_->samples.size()) {
            ch.sample_idx = static_cast<i32>(cell.sample - 1);
            ch.volume = mod_->samples[static_cast<usize>(ch.sample_idx)].default_volume;
        }

        if (cell.note == 97) {
            ch.playing = false;
        } else if (cell.note > 0) {
            const f64 new_period = note_to_period(cell.note);
            if (cell.effect == 0x03 || cell.effect == 0x05) {
                ch.target_period = new_period;
            } else {
                ch.note = cell.note;
                ch.period = new_period;
                ch.sample_pos = 0.0;
                ch.reverse_dir = false;
                ch.playing = (ch.sample_idx >= 0 &&
                              !mod_->samples[static_cast<usize>(ch.sample_idx)].pcm.empty());
                update_channel_step(ch);
            }
        }

        if (cell.vol_cmd == 1) {
            ch.volume = static_cast<f32>(std::min<u8>(cell.vol_val, 64u)) / 64.0f;
        }

        // Tick-0 effects
        switch (cell.effect) {
            case 0x03: // Tone portamento
                if (cell.param > 0) ch.porta_speed = cell.param;
                break;
            case 0x04: // Vibrato
                if (cell.param >> 4u) ch.vib_speed = cell.param >> 4u;
                if (cell.param & 0x0Fu) ch.vib_depth = cell.param & 0x0Fu;
                break;
            case 0x08: // Panning
                ch.pan = (static_cast<f32>(cell.param) / 127.5f) - 1.0f;
                break;
            case 0x09: // Sample offset
                ch.sample_pos = static_cast<f64>(cell.param) * 256.0;
                break;
            case 0x0B: // Position jump
                pending_jump_order_ = static_cast<i32>(cell.param);
                break;
            case 0x0C: // Set volume
                ch.volume = static_cast<f32>(std::min<u8>(cell.param, 64u)) / 64.0f;
                break;
            case 0x0D: // Pattern break (BCD)
                pending_break_row_ = static_cast<i32>((cell.param >> 4u) * 10u + (cell.param & 0x0Fu));
                break;
            case 0x0E: { // Extended MOD effects
                const u8 sub = cell.param >> 4u;
                const u8 val = cell.param & 0x0Fu;
                if (sub == 0x1) { ch.period = std::max(28.0, ch.period - val); update_channel_step(ch); }
                else if (sub == 0x2) { ch.period = std::min(6848.0, ch.period + val); update_channel_step(ch); }
                else if (sub == 0x8) { ch.pan = (static_cast<f32>(val) / 7.5f) - 1.0f; }
                else if (sub == 0xA) { ch.volume = std::min(1.0f, ch.volume + static_cast<f32>(val) / 64.0f); }
                else if (sub == 0xB) { ch.volume = std::max(0.0f, ch.volume - static_cast<f32>(val) / 64.0f); }
                break;
            }
            case 0x0F: // Set speed/BPM
                if (cell.param > 0) {
                    if (cell.param < 0x20) speed_ = cell.param;
                    else {
                        bpm_ = cell.param;
                        samples_per_tick_ = static_cast<f64>(sample_rate_) * 2.5 / static_cast<f64>(bpm_);
                    }
                }
                break;
            case 0x1D: // S3M global volume
                global_vol_ = static_cast<f32>(std::min<u8>(cell.param, 64u)) / 64.0f;
                break;
            case 0x1E: // S3M set tempo
                if (cell.param >= 32) {
                    bpm_ = cell.param;
                    samples_per_tick_ = static_cast<f64>(sample_rate_) * 2.5 / static_cast<f64>(bpm_);
                }
                break;
            case 0x1F: // S3M set speed
                if (cell.param > 0) speed_ = cell.param;
                break;
            default:
                break;
        }
    }
}

void TrackerPlayer::advance_tick(bool loop) noexcept {
    if (finished_ || !mod_) return;

    if (tick_ == 0) {
        process_row(loop);
    } else {
        // Non-zero tick continuous effects
        for (auto& ch : channels_) {
            switch (ch.effect) {
                case 0x00: // Arpeggio
                    if (ch.param != 0 && ch.note > 0) {
                        const u32 phase = tick_ % 3u;
                        const i32 offset = (phase == 1) ? static_cast<i32>(ch.param >> 4u)
                                         : (phase == 2) ? static_cast<i32>(ch.param & 0x0Fu)
                                                        : 0;
                        update_channel_step(ch, note_to_period(ch.note + offset));
                    }
                    break;
                case 0x01: // Porta up
                    ch.period = std::max(28.0, ch.period - static_cast<f64>(ch.param));
                    update_channel_step(ch);
                    break;
                case 0x02: // Porta down
                    ch.period = std::min(6848.0, ch.period + static_cast<f64>(ch.param));
                    update_channel_step(ch);
                    break;
                case 0x03: // Tone portamento
                    if (ch.target_period > 0.0) {
                        if (ch.period < ch.target_period) {
                            ch.period = std::min(ch.target_period, ch.period + ch.porta_speed);
                        } else if (ch.period > ch.target_period) {
                            ch.period = std::max(ch.target_period, ch.period - ch.porta_speed);
                        }
                        update_channel_step(ch);
                    }
                    break;
                case 0x04: { // Vibrato
                    const i32 sin_val = kSineTable[ch.vib_pos & 31u];
                    const f64 delta = (static_cast<f64>(sin_val) * static_cast<f64>(ch.vib_depth)) / 128.0;
                    const f64 mod_p = (ch.vib_pos & 32u) ? (ch.period - delta) : (ch.period + delta);
                    update_channel_step(ch, std::max(28.0, mod_p));
                    ch.vib_pos = (ch.vib_pos + ch.vib_speed) & 63u;
                    break;
                }
                case 0x0A: // Volume slide
                case 0x05:
                case 0x06: {
                    const u8 p = ch.param ? ch.param : ch.vol_slide_mem;
                    ch.vol_slide_mem = p;
                    const u8 up = p >> 4u;
                    const u8 dn = p & 0x0Fu;
                    if (up > 0) ch.volume = std::min(1.0f, ch.volume + static_cast<f32>(up) / 64.0f);
                    else if (dn > 0) ch.volume = std::max(0.0f, ch.volume - static_cast<f32>(dn) / 64.0f);
                    break;
                }
                case 0x0E: {
                    const u8 sub = ch.param >> 4u;
                    const u8 val = ch.param & 0x0Fu;
                    if (sub == 0xC && tick_ == val) ch.volume = 0.0f; // Note cut
                    break;
                }
                default:
                    break;
            }
        }
    }

    ++tick_;
    if (tick_ >= speed_) {
        tick_ = 0;
        if (pending_jump_order_ >= 0) {
            const u32 target = static_cast<u32>(pending_jump_order_);
            if (target <= order_idx_ && !loop) {
                finished_ = true;
            } else {
                order_idx_ = target % static_cast<u32>(std::max<usize>(mod_->orders.size(), 1u));
                row_idx_ = pending_break_row_ >= 0 ? static_cast<u32>(pending_break_row_) : 0u;
            }
        } else if (pending_break_row_ >= 0) {
            ++order_idx_;
            row_idx_ = static_cast<u32>(pending_break_row_);
        } else {
            ++row_idx_;
            const u32 pat_rows = (order_idx_ < mod_->orders.size() &&
                                  mod_->orders[order_idx_] < mod_->patterns.size())
                                     ? mod_->patterns[mod_->orders[order_idx_]].rows
                                     : 64u;
            if (row_idx_ >= pat_rows) {
                row_idx_ = 0;
                ++order_idx_;
            }
        }

        if (order_idx_ >= mod_->orders.size()) {
            if (loop) {
                order_idx_ = 0;
                row_idx_ = 0;
            } else {
                finished_ = true;
            }
        }
    }
}

u32 TrackerPlayer::render(f32* out_stereo, u32 frame_count, bool loop) noexcept {
    std::memset(out_stereo, 0, static_cast<usize>(frame_count) * 2u * sizeof(f32));
    if (!mod_ || finished_ || frame_count == 0) return 0;

    // Gentle per-channel scaling to prevent clipping on multi-channel modules
    const f32 norm_scale = 0.65f / std::sqrt(static_cast<f32>(std::max<u32>(mod_->num_channels, 4u)) * 0.25f);

    u32 frames_written = 0;
    for (u32 f = 0; f < frame_count; ++f) {
        if (tick_sample_accum_ <= 0.0) {
            advance_tick(loop);
            if (finished_) break;
            tick_sample_accum_ += samples_per_tick_;
        }
        tick_sample_accum_ -= 1.0;

        f32 sum_l = 0.0f;
        f32 sum_r = 0.0f;

        for (auto& ch : channels_) {
            if (!ch.playing || ch.sample_idx < 0) continue;
            const auto& smp = mod_->samples[static_cast<usize>(ch.sample_idx)];
            const usize len = smp.pcm.size();
            if (len == 0) {
                ch.playing = false;
                continue;
            }

            const usize i0 = static_cast<usize>(ch.sample_pos);
            if (i0 >= len) {
                ch.playing = false;
                continue;
            }
            const usize i1 = (i0 + 1 < len) ? (i0 + 1) : (smp.loop_enabled ? smp.loop_start : i0);
            const f32 frac = static_cast<f32>(ch.sample_pos - static_cast<f64>(i0));
            const f32 sample_val = smp.pcm[i0] * (1.0f - frac) + smp.pcm[i1] * frac;

            const f32 g = ch.volume * ch.user_volume * global_vol_ * norm_scale;
            const f32 gl = g * std::clamp(1.0f - ch.pan, 0.0f, 1.0f);
            const f32 gr = g * std::clamp(1.0f + ch.pan, 0.0f, 1.0f);
            sum_l += sample_val * gl;
            sum_r += sample_val * gr;

            if (ch.reverse_dir) {
                ch.sample_pos -= ch.sample_step;
                if (ch.sample_pos <= static_cast<f64>(smp.loop_start)) {
                    ch.sample_pos = static_cast<f64>(smp.loop_start);
                    ch.reverse_dir = false;
                }
            } else {
                ch.sample_pos += ch.sample_step;
                if (smp.loop_enabled && ch.sample_pos >= static_cast<f64>(smp.loop_end)) {
                    if (smp.ping_pong) {
                        ch.sample_pos = static_cast<f64>(smp.loop_end - 1u);
                        ch.reverse_dir = true;
                    } else {
                        const f64 loop_len = static_cast<f64>(std::max<u32>(1u, smp.loop_end - smp.loop_start));
                        ch.sample_pos = static_cast<f64>(smp.loop_start) +
                                        std::fmod(ch.sample_pos - static_cast<f64>(smp.loop_start), loop_len);
                    }
                } else if (!smp.loop_enabled && ch.sample_pos >= static_cast<f64>(len)) {
                    ch.playing = false;
                }
            }
        }

        out_stereo[f * 2u + 0] = sum_l;
        out_stereo[f * 2u + 1] = sum_r;
        ++frames_written;
        ++played_frames_;
    }

    return frames_written;
}

} // namespace gm
