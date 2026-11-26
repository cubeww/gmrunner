#include "loader/exe_loader.h"

#include "base/binary_reader.h"
#include "base/encoding.h"
#include "base/hash.h"
#include "base/log.h"
#include "loader/antidecompiler.h"
#include "loader/codepage_detection.h"
#include "loader/crypto.h"
#include "loader/upx_unpack.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace gm {

namespace {

void read_particle_mask(std::span<const u8> data, Bitmap32& shape) {
    // Delphi TBitmap class/length, followed by a bottom-up, 24-bit BMP.
    constexpr usize side = 64;
    constexpr usize pixels = 12 + 54;
    if (data.size() != pixels + side * side * 3 || std::memcmp(data.data(), "\x07TBitmap", 8) != 0 ||
        std::memcmp(data.data() + 12, "BM", 2) != 0) return;
    BinaryReader bmp(data.subspan(12 + 10));
    if (bmp.read_u32().value() != 54 || bmp.read_u32().value() != 40 ||
        bmp.read_i32().value() != side || bmp.read_i32().value() != side ||
        bmp.read_u16().value() != 1 || bmp.read_u16().value() != 24 || bmp.read_u32().value() != 0) return;
    shape.width = shape.height = side;
    shape.bgra.assign(side * side * 4, 255);
    for (usize y = 0; y < side; ++y) {
        for (usize x = 0; x < side; ++x) {
            shape.bgra[(y * side + x) * 4 + 3] = data[pixels + ((side - 1 - y) * side + x) * 3];
        }
    }
}

// GM8.0's TLMDImageList stores the particle strip as independently compressed blocks.
bool unpack_particle_block(std::span<const u8> block, std::vector<u8>& out, usize max_size) {
    if (block.empty()) return false;
    if (block.front() == 0x80) {
        block = block.subspan(1);
        if (block.size() > max_size - out.size()) return false;
        out.insert(out.end(), block.begin(), block.end());
        return true;
    }
    const usize start = out.size();
    usize pos = 1;
    while (pos < block.size()) {
        if (block.size() - pos < 2) return false;
        const u16 flags = (block[pos] << 8) | block[pos + 1];
        pos += 2;
        for (u16 bit = 0x8000; bit && pos < block.size(); bit >>= 1) {
            if ((flags & bit) == 0) {
                if (out.size() == max_size) return false;
                out.push_back(block[pos++]);
                continue;
            }
            if (block.size() - pos < 2) return false;
            const usize distance = (block[pos] << 4) | (block[pos + 1] >> 4);
            if (distance) {
                const usize count = (block[pos + 1] & 15) + 3;
                if (distance > out.size() - start || count > max_size - out.size()) return false;
                for (usize i = 0; i < count; ++i) out.push_back(out[out.size() - distance]);
                pos += 2;
            } else {
                if (block.size() - pos < 4) return false;
                const usize count = (block[pos + 1] << 8) + block[pos + 2] + 16;
                if (count > max_size - out.size()) return false;
                out.insert(out.end(), count, block[pos + 3]);
                pos += 4;
            }
        }
    }
    return true;
}

void read_particle_strip(std::span<const u8> property, GameData& game) {
    constexpr usize side = 64, frames = 15, width = side * frames;
    constexpr usize bmp_size = 54 + width * side * 4;
    BinaryReader reader(property);
    auto data = reader.read_length_prefixed_span();
    if (!data || data.value().size() < 22) return;
    BinaryReader list(data.value());
    if (list.read_u8().value() != 1 || list.read_u32().value() != side || list.read_u32().value() != side ||
        list.read_u32().value() != 4 || list.read_u8().value() != 0 || list.read_u32().value() != frames) return;
    auto stream = list.read_length_prefixed_span();
    if (!stream) return;
    BinaryReader blocks(stream.value());
    std::vector<u8> bmp;
    bmp.reserve(bmp_size);
    while (!blocks.eof()) {
        auto block = blocks.read_length_prefixed_span();
        if (!block || !unpack_particle_block(block.value(), bmp, bmp_size)) return;
    }
    if (bmp.size() != bmp_size || std::memcmp(bmp.data(), "BM", 2) != 0) return;
    BinaryReader header(std::span<const u8>(bmp).subspan(10));
    if (header.read_u32().value() != 54 || header.read_u32().value() != 40 ||
        header.read_i32().value() != width || header.read_i32().value() != side ||
        header.read_u16().value() != 1 || header.read_u16().value() != 32 || header.read_u32().value() != 0) return;
    for (usize i = 0; i < game.particle_shapes.size(); ++i) {
        auto& shape = game.particle_shapes[i];
        shape.width = shape.height = side;
        shape.bgra.assign(side * side * 4, 255);
        for (usize y = 0; y < side; ++y) {
            const usize row = (side - 1 - y) * width + (i + 1) * side;
            for (usize x = 0; x < side; ++x) shape.bgra[(y * side + x) * 4 + 3] = bmp[54 + (row + x) * 4];
        }
    }
}

void read_particle_shapes(std::span<const u8> runner, GameData& game) {
    // GM8.1/8.2 store pt_shape_* as 64x64 grayscale bitmaps in the Delphi particle form.
    std::string_view bytes(reinterpret_cast<const char*>(runner.data()), runner.size());
    const usize form = bytes.find("TPF0\rTParticleForm\fParticleForm");
    if (form == bytes.npos) return;
    bytes.remove_prefix(form);
    constexpr std::string_view list_name = "\rTLMDImageList\x0bPartImages2";
    if (bytes.substr(0, 1024).find(list_name) != bytes.npos) {
        constexpr std::string_view property = "\x08Internal\x0a";
        const usize pos = bytes.substr(0, 1024).find(property);
        if (pos != bytes.npos) read_particle_strip(runner.subspan(form + pos + property.size()), game);
        return;
    }
    constexpr std::string_view image = "\x06TImage";
    usize pos = bytes.substr(0, 1024).find(image);
    if (pos == bytes.npos) return;
    bytes.remove_prefix(pos);
    while (bytes.starts_with(image)) {
        bytes.remove_prefix(image.size());
        if (bytes.empty()) return;
        const usize len = static_cast<u8>(bytes.front());
        bytes.remove_prefix(1);
        if (bytes.size() < len) return;
        auto name = bytes.substr(0, len);
        bytes.remove_prefix(len);
        constexpr std::string_view prefix = "ParticleBMP";
        if (!name.starts_with(prefix)) return;
        name.remove_prefix(prefix.size());
        i32 number = 0;
        const auto [end, ec] = std::from_chars(name.data(), name.data() + name.size(), number);
        if (ec != std::errc{} || end != name.data() + name.size() || number < 2 || number > 15) return;

        constexpr std::string_view picture = "\x0cPicture.Data\x0a";
        pos = bytes.substr(0, 128).find(picture);
        if (pos == bytes.npos) return;
        bytes.remove_prefix(pos + picture.size());
        BinaryReader property({reinterpret_cast<const u8*>(bytes.data()), bytes.size()});
        auto data = property.read_length_prefixed_span();
        if (!data) return;
        bytes.remove_prefix(property.position());
        // The two zeroes terminate the component's properties and children.
        if (!bytes.starts_with(std::string_view("\0\0", 2))) return;
        bytes.remove_prefix(2);

        read_particle_mask(data.value(), game.particle_shapes[static_cast<usize>(number - 2)]);
    }
}

void read_message_form_assets(std::span<const u8> runner, GameData& game) {
    // GM8.0/8.1/8.2 embed the default TMessageForm background JPEG and 3-state button
    // TImageList bitmap in the runner's Delphi DFM resource.
    std::string_view bytes(reinterpret_cast<const char*>(runner.data()), runner.size());
    constexpr std::string_view form_sig("TPF0\x0c" "TMessageForm\x0b" "MessageForm", 29);
    const usize form = bytes.find(form_sig);
    if (form == bytes.npos) return;

    const usize form_limit = std::min<usize>(runner.size() - form, 65536u);
    std::string_view chunk = bytes.substr(form, form_limit);

    // 1. BackgroundImage.Picture.Data -> Pascal class name + 4-byte length + JPEG/BMP bytes.
    constexpr std::string_view bg_obj("\x0f" "BackgroundImage", 16);
    if (const usize bg_pos = chunk.find(bg_obj); bg_pos != chunk.npos) {
        constexpr std::string_view pic_prop("\x0c" "Picture.Data\x0a", 14);
        const usize search_len = std::min<usize>(chunk.size() - bg_pos, 256u);
        if (const usize ppos = chunk.substr(bg_pos, search_len).find(pic_prop); ppos != std::string_view::npos) {
            BinaryReader prop_reader(runner.subspan(form + bg_pos + ppos + pic_prop.size()));
            if (auto data_res = prop_reader.read_length_prefixed_span(); data_res) {
                const auto blob = data_res.value();
                if (blob.size() >= 6) {
                    const usize cls_len = blob[0];
                    if (1 + cls_len + 4 <= blob.size()) {
                        BinaryReader inner(blob.subspan(1 + cls_len));
                        if (auto img_res = inner.read_length_prefixed_span(); img_res && !img_res.value().empty()) {
                            const auto img = img_res.value();
                            game.message_background_image.assign(img.begin(), img.end());
                        }
                    }
                }
            }
        }
    }

    // 2. TheImages.Bitmap -> Win32 ImageList stream ("IL") containing 3 button subimages (80x25).
    constexpr std::string_view il_obj("\x09" "TheImages", 10);
    if (const usize il_pos = chunk.find(il_obj); il_pos != chunk.npos) {
        constexpr std::string_view bmp_prop("\x06" "Bitmap\x0a", 8);
        const usize search_len = std::min<usize>(chunk.size() - il_pos, 256u);
        if (const usize bpos = chunk.substr(il_pos, search_len).find(bmp_prop); bpos != std::string_view::npos) {
            BinaryReader prop_reader(runner.subspan(form + il_pos + bpos + bmp_prop.size()));
            if (auto data_res = prop_reader.read_length_prefixed_span(); data_res) {
                const auto il = data_res.value();
                if (il.size() >= 28 + 54 && il[0] == 'I' && il[1] == 'L') {
                    BinaryReader hdr(il.subspan(4));
                    const auto cur_count = hdr.read_u16();
                    (void)hdr.read_u16(); // cMaxImage
                    (void)hdr.read_u16(); // cGrow
                    const auto btn_w = hdr.read_u16();
                    const auto btn_h = hdr.read_u16();
                    if (cur_count && btn_w && btn_h &&
                        cur_count.value() >= 3 && btn_w.value() > 0 && btn_h.value() > 0 &&
                        btn_w.value() <= 512 && btn_h.value() <= 512) {
                        const i32 bw = static_cast<i32>(btn_w.value());
                        const i32 bh = static_cast<i32>(btn_h.value());
                        // Locate embedded BMP ("BM") after the 28-byte ImageList header.
                        usize bm_off = 28;
                        while (bm_off + 54 <= il.size() && !(il[bm_off] == 'B' && il[bm_off + 1] == 'M')) {
                            ++bm_off;
                        }
                        if (bm_off + 54 <= il.size()) {
                            BinaryReader dib(il.subspan(bm_off + 10));
                            const auto px_off = dib.read_u32();
                            const auto dib_sz = dib.read_u32();
                            const auto img_w = dib.read_i32();
                            const auto img_h = dib.read_i32();
                            const auto planes = dib.read_u16();
                            const auto bpp = dib.read_u16();
                            const auto comp = dib.read_u32();
                            if (px_off && dib_sz && img_w && img_h && planes && bpp && comp &&
                                dib_sz.value() >= 40 && planes.value() == 1 && comp.value() == 0 &&
                                (bpp.value() == 24 || bpp.value() == 32) &&
                                img_w.value() >= bw * 3 && std::abs(img_h.value()) >= bh) {
                                const i32 src_w = img_w.value();
                                const i32 src_h = std::abs(img_h.value());
                                const bool top_down = img_h.value() < 0;
                                const usize bytes_per_px = bpp.value() / 8u;
                                const usize row_stride = ((static_cast<usize>(src_w) * bytes_per_px) + 3u) & ~usize{3u};
                                const usize pixel_base = bm_off + px_off.value();
                                if (pixel_base + row_stride * static_cast<usize>(src_h) <= il.size()) {
                                    for (usize s = 0; s < 3; ++s) {
                                        auto& dst = game.message_buttons[s];
                                        dst.width = bw;
                                        dst.height = bh;
                                        dst.bgra.resize(static_cast<usize>(bw) * static_cast<usize>(bh) * 4u);
                                        for (i32 y = 0; y < bh; ++y) {
                                            const i32 src_y = top_down ? y : (src_h - 1 - y);
                                            const usize src_row = pixel_base + static_cast<usize>(src_y) * row_stride;
                                            for (i32 x = 0; x < bw; ++x) {
                                                const usize sx = static_cast<usize>(static_cast<i32>(s) * bw + x);
                                                const usize sp = src_row + sx * bytes_per_px;
                                                const usize dp = (static_cast<usize>(y) * static_cast<usize>(bw) + static_cast<usize>(x)) * 4u;
                                                dst.bgra[dp + 0] = il[sp + 0];
                                                dst.bgra[dp + 1] = il[sp + 1];
                                                dst.bgra[dp + 2] = il[sp + 2];
                                                dst.bgra[dp + 3] = 255u; // TheImages has Masked=False
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

Result<std::vector<u8>> read_optional_bitmap(BinaryReader& r) {
    auto exists_res = r.read_bool32();
    if (!exists_res) return exists_res.error();
    if (!exists_res.value()) {
        return std::vector<u8>{};
    }
    return r.read_compressed_stream();
}

Result<GameOptions> parse_options(BinaryReader& outer) {
    auto ver_res = outer.read_i32();
    if (!ver_res) return Error{"Options: failed reading version: " + ver_res.error_message()};
    const i32 ver = ver_res.value();

    std::vector<u8> decompressed;
    BinaryReader* r = &outer;
    std::optional<BinaryReader> sub_reader;
    if (ver >= 800) {
        auto dec_res = outer.read_compressed_stream();
        if (!dec_res) return Error{"Options: zlib decompress failed: " + dec_res.error_message()};
        decompressed = std::move(dec_res.value());
        sub_reader.emplace(decompressed);
        r = &(*sub_reader);
    }

    GameOptions o{};
    o.version = ver;

    auto rb = [&](bool& dst) -> Result<void> {
        auto v = r->read_bool32();
        if (!v) return v.error();
        dst = v.value();
        return Result<void>::success();
    };
    auto ri = [&](i32& dst) -> Result<void> {
        auto v = r->read_i32();
        if (!v) return v.error();
        dst = v.value();
        return Result<void>::success();
    };
    auto ru = [&](u32& dst) -> Result<void> {
        auto v = r->read_u32();
        if (!v) return v.error();
        dst = v.value();
        return Result<void>::success();
    };

    if (auto e = rb(o.fullscreen); !e) return e.error();
    if (auto e = rb(o.interpolate_pixels); !e) return e.error();
    if (auto e = rb(o.no_border); !e) return e.error();
    if (auto e = rb(o.show_cursor); !e) return e.error();
    if (auto e = ri(o.scale); !e) return e.error();
    if (auto e = rb(o.sizeable); !e) return e.error();
    if (auto e = rb(o.stay_on_top); !e) return e.error();
    if (auto e = ru(o.window_color); !e) return e.error();
    if (auto e = rb(o.change_resolution); !e) return e.error();
    if (auto e = ri(o.color_depth); !e) return e.error();
    if (auto e = ri(o.resolution); !e) return e.error();
    if (auto e = ri(o.frequency); !e) return e.error();
    if (auto e = rb(o.no_buttons); !e) return e.error();
    if (auto e = ri(o.sync_vertex); !e) return e.error();
    if (ver >= 800) {
        if (auto e = rb(o.no_screensaver); !e) return e.error();
    }
    if (auto e = rb(o.screen_key); !e) return e.error();
    if (auto e = rb(o.help_key); !e) return e.error();
    if (auto e = rb(o.quit_key); !e) return e.error();
    if (auto e = rb(o.save_key); !e) return e.error();
    if (auto e = rb(o.screenshot_key); !e) return e.error();
    if (auto e = rb(o.close_sec); !e) return e.error();
    if (auto e = ri(o.priority); !e) return e.error();
    if (auto e = rb(o.freeze); !e) return e.error();
    if (auto e = rb(o.show_progress); !e) return e.error();

    if (o.show_progress) {
        auto back = read_optional_bitmap(*r);
        if (!back) return Error{"Options: progress back image: " + back.error_message()};
        o.progress_back_image = std::move(back.value());

        auto front = read_optional_bitmap(*r);
        if (!front) return Error{"Options: progress front image: " + front.error_message()};
        o.progress_front_image = std::move(front.value());
    }

    auto load_img = read_optional_bitmap(*r);
    if (!load_img) return Error{"Options: load image: " + load_img.error_message()};
    o.load_image = std::move(load_img.value());

    if (auto e = rb(o.load_transparent); !e) return e.error();
    if (auto e = ri(o.load_alpha); !e) return e.error();
    if (auto e = rb(o.scale_progress); !e) return e.error();
    if (auto e = rb(o.display_errors); !e) return e.error();
    if (auto e = rb(o.write_errors); !e) return e.error();
    if (auto e = rb(o.abort_errors); !e) return e.error();

    auto var_flags_res = r->read_u32();
    if (!var_flags_res) return var_flags_res.error();
    o.treat_uninitialized_as_zero = (var_flags_res.value() & 1u) != 0u;
    o.error_on_arg_count = (var_flags_res.value() & 2u) != 0u;

    if (ver >= 820 && r->remaining() >= 8) {
        if (auto e = ri(o.webgl_mode); !e) return e.error();
        if (auto e = rb(o.creation_event_order); !e) return e.error();
    }

    return o;
}

Result<Bitmap32> read_bitmap32_v800(BinaryReader& r) {
    Bitmap32 bmp{};
    auto w = r.read_i32();
    auto h = r.read_i32();
    if (!w || !h || w.value() < 0 || h.value() < 0) return Error{"Bitmap32 v800: invalid dimensions"};
    bmp.width = w.value();
    bmp.height = h.value();
    if (bmp.width > 0 && bmp.height > 0) {
        auto pixels = r.read_length_prefixed_span();
        const u64 size = static_cast<u64>(bmp.width) * bmp.height * 4u;
        if (!pixels || size > pixels.value().size()) return Error{"Bitmap32 v800: truncated pixels"};
        const auto image = pixels.value().first(static_cast<usize>(size));
        bmp.bgra.assign(image.begin(), image.end());
    }
    return bmp;
}

Result<Bitmap32> parse_bitmap32(BinaryReader& r) {
    auto ver_res = r.read_i32();
    if (!ver_res) return ver_res.error();
    const i32 ver = ver_res.value();
    if (ver == 800) return read_bitmap32_v800(r);

    Bitmap32 bmp{};
    if (ver == 540) {
        auto has_img = r.read_bool32();
        if (!has_img) return has_img.error();
        if (has_img.value()) {
            auto w = r.read_i32();
            auto h = r.read_i32();
            if (!w || !h) return Error{"Bitmap32 v540: failed reading dimensions"};
            bmp.width = w.value();
            bmp.height = h.value();
            auto dec = r.read_compressed_stream();
            if (!dec) return Error{"Bitmap32 v540: zlib failed: " + dec.error_message()};
            bmp.bgra = std::move(dec.value());
        }
    } else {
        return Error{"Bitmap32: unsupported version " + std::to_string(ver)};
    }
    return bmp;
}

Result<CollisionMask> parse_collision_mask(BinaryReader& r) {
    auto ver_res = r.read_i32();
    if (!ver_res) return ver_res.error();
    if (ver_res.value() != 800) {
        return Error{"CollisionMask: unsupported version " + std::to_string(ver_res.value())};
    }

    CollisionMask mask{};
    auto w = r.read_i32();
    auto h = r.read_i32();
    auto bl = r.read_i32();
    auto br = r.read_i32();
    auto bb = r.read_i32();
    auto bt = r.read_i32();
    if (!w || !h || !bl || !br || !bb || !bt) {
        return Error{"CollisionMask: failed reading header"};
    }
    mask.width = w.value();
    mask.height = h.value();
    mask.bbox_left = bl.value();
    mask.bbox_right = br.value();
    mask.bbox_bottom = bb.value();
    mask.bbox_top = bt.value();

    if (mask.width < 0 || mask.height < 0) {
        return Error{"CollisionMask: negative dimensions"};
    }
    const usize count = static_cast<usize>(mask.width) * static_cast<usize>(mask.height);
    auto raw_res = r.read_span(count * 4u);
    if (!raw_res) return Error{"CollisionMask: unexpected EOF in pixel grid"};
    const u8* p = raw_res.value().data();
    mask.pixels.resize(count);
    for (usize i = 0; i < count; ++i) {
        const u32 v = static_cast<u32>(p[0]) |
                      (static_cast<u32>(p[1]) << 8u) |
                      (static_cast<u32>(p[2]) << 16u) |
                      (static_cast<u32>(p[3]) << 24u);
        mask.pixels[i] = (v != 0u) ? 1u : 0u;
        p += 4;
    }
    return mask;
}

Result<EventData> parse_event(BinaryReader& r) {
    auto ev_ver = r.read_i32();
    if (!ev_ver) return ev_ver.error();
    auto act_count = r.read_i32();
    if (!act_count || act_count.value() < 0) return Error{"Event: invalid action count"};

    EventData ev{};
    ev.actions.reserve(static_cast<usize>(act_count.value()));

    for (i32 a = 0; a < act_count.value(); ++a) {
        auto act_ver = r.read_i32();
        auto lib_id = r.read_i32();
        auto id = r.read_i32();
        auto kind = r.read_i32();
        auto use_rel = r.read_bool32();
        auto is_q = r.read_bool32();
        auto use_apply = r.read_bool32();
        auto exe_type = r.read_i32();
        auto name = r.read_gm_string();
        auto code = r.read_gm_string();
        auto arg_count = r.read_i32();
        auto arg_type_cnt = r.read_i32();
        if (!act_ver || !lib_id || !id || !kind || !use_rel || !is_q ||
            !use_apply || !exe_type || !name || !code || !arg_count || !arg_type_cnt) {
            return Error{"Action: failed reading header"};
        }

        ActionData act{};
        act.lib_id = lib_id.value();
        act.id = id.value();
        act.kind = kind.value();
        act.use_relative = use_rel.value();
        act.is_question = is_q.value();
        act.use_apply_to = use_apply.value();
        act.exe_type = exe_type.value();
        act.name = std::move(name.value());
        act.code = std::move(code.value());
        act.arg_count = arg_count.value();

        if (arg_type_cnt.value() < 0) return Error{"Action: negative arg_type_cnt"};
        act.arg_types.reserve(static_cast<usize>(arg_type_cnt.value()));
        for (i32 t = 0; t < arg_type_cnt.value(); ++t) {
            auto at = r.read_i32();
            if (!at) return at.error();
            act.arg_types.push_back(at.value());
        }

        auto who = r.read_i32();
        auto rel = r.read_bool32();
        auto arg_val_cnt = r.read_i32();
        if (!who || !rel || !arg_val_cnt || arg_val_cnt.value() < 0) {
            return Error{"Action: failed reading target/args"};
        }
        act.who = who.value();
        act.relative = rel.value();

        act.args.reserve(static_cast<usize>(arg_val_cnt.value()));
        for (i32 v = 0; v < arg_val_cnt.value(); ++v) {
            auto av = r.read_gm_string();
            if (!av) return av.error();
            act.args.push_back(std::move(av.value()));
        }

        auto is_not = r.read_bool32();
        if (!is_not) return is_not.error();
        act.is_not = is_not.value();

        ev.actions.push_back(std::move(act));
    }

    return ev;
}

Result<void> parse_extensions(BinaryReader& r, std::vector<ExtensionData>& out) {
    auto blk_ver = r.read_i32();
    auto count = r.read_i32();
    if (!blk_ver || !count || count.value() < 0) {
        return Error{"Extensions: failed reading block header"};
    }

    out.reserve(static_cast<usize>(count.value()));
    for (i32 i = 0; i < count.value(); ++i) {
        auto ext_ver = r.read_i32();
        auto name = r.read_gm_string();
        auto folder = r.read_gm_string();
        auto inc_cnt = r.read_i32();
        if (!ext_ver || !name || !folder || !inc_cnt || inc_cnt.value() < 0) {
            return Error{"Extensions[" + std::to_string(i) + "]: failed reading header"};
        }

        ExtensionData ext{};
        ext.name = std::move(name.value());
        ext.folder = std::move(folder.value());
        ext.includes.reserve(static_cast<usize>(inc_cnt.value()));

        for (i32 j = 0; j < inc_cnt.value(); ++j) {
            auto inc_ver = r.read_i32();
            auto filename = r.read_gm_string();
            auto kind = r.read_i32();
            auto init_fn = r.read_gm_string();
            auto final_fn = r.read_gm_string();
            auto fn_cnt = r.read_i32();
            if (!inc_ver || !filename || !kind || !init_fn || !final_fn || !fn_cnt || fn_cnt.value() < 0) {
                return Error{"Extensions[" + std::to_string(i) + "].Include[" + std::to_string(j) + "]: header error"};
            }

            ExtensionInclude inc{};
            inc.filename = std::move(filename.value());
            inc.kind = kind.value();
            inc.init_func = std::move(init_fn.value());
            inc.final_func = std::move(final_fn.value());

            inc.functions.reserve(static_cast<usize>(fn_cnt.value()));
            for (i32 f = 0; f < fn_cnt.value(); ++f) {
                auto fn_ver = r.read_i32();
                auto fn_name = r.read_gm_string();
                auto ext_name = r.read_gm_string();
                auto call_type = r.read_i32();
                auto fn_id = r.read_i32();
                auto arg_cnt = r.read_i32();
                if (!fn_ver || !fn_name || !ext_name || !call_type || !fn_id || !arg_cnt) {
                    return Error{"ExtensionFunction: header error"};
                }

                ExtensionFunction fn{};
                fn.name = std::move(fn_name.value());
                fn.external_name = std::move(ext_name.value());
                fn.call_type = call_type.value();
                fn.id = fn_id.value();
                fn.arg_count = arg_cnt.value();

                // Always stores 17 arg-type int32 slots (0..16), keeping < arg_count.
                for (i32 a = 0; a <= 16; ++a) {
                    auto at = r.read_i32();
                    if (!at) return at.error();
                    if (a < fn.arg_count) {
                        fn.arg_types.push_back(at.value());
                    }
                }
                auto ret_type = r.read_i32();
                if (!ret_type) return ret_type.error();
                fn.return_type = ret_type.value();

                inc.functions.push_back(std::move(fn));
            }

            auto const_cnt = r.read_i32();
            if (!const_cnt || const_cnt.value() < 0) return Error{"ExtensionInclude: invalid constant count"};
            inc.constants.reserve(static_cast<usize>(const_cnt.value()));
            for (i32 c = 0; c < const_cnt.value(); ++c) {
                auto c_ver = r.read_i32();
                auto c_name = r.read_gm_string();
                auto c_val = r.read_gm_string();
                if (!c_ver || !c_name || !c_val) return Error{"ExtensionConstant: read error"};
                inc.constants.push_back(ExtensionConstant{std::move(c_name.value()), std::move(c_val.value())});
            }

            ext.includes.push_back(std::move(inc));
        }

        // Read encrypted package data blob (u32 len + key + swap-encrypted bytes starting at pos+1 = 5).
        auto blob_span = r.read_length_prefixed_span();
        if (!blob_span) {
            return Error{"Extensions[" + std::to_string(i) + "]: failed reading payload blob"};
        }
        if (blob_span.value().size() >= 4) {
            std::vector<u8> blob(blob_span.value().begin(), blob_span.value().end());
            BinaryReader blob_reader(blob);
            auto key_res = blob_reader.read_i32();
            if (!key_res) return key_res.error();
            decrypt_gm7_swap(key_res.value(), blob, blob_reader.position() + 1);

            for (auto& inc : ext.includes) {
                if (inc.kind != 3) { // kind 3 = Lib (no binary payload)
                    auto dec = blob_reader.read_compressed_stream();
                    if (!dec) {
                        return Error{"Extensions[" + ext.name + "]/" + inc.filename +
                                     ": payload decompress failed: " + dec.error_message()};
                    }
                    inc.payload = std::move(dec.value());
                }
            }
        }

        out.push_back(std::move(ext));
    }

    return Result<void>::success();
}

Result<void> parse_triggers(BinaryReader& r, std::vector<TriggerData>& out) {
    auto ver = r.read_i32();
    if (!ver) return ver.error();
    if (ver.value() != 800) return Result<void>::success();

    auto count = r.read_i32();
    if (!count || count.value() < 0) return Error{"Triggers: invalid count"};
    out.reserve(static_cast<usize>(count.value()));

    for (i32 i = 0; i < count.value(); ++i) {
        auto dec = r.read_compressed_stream();
        if (!dec) return Error{"Triggers[" + std::to_string(i) + "]: zlib failed"};
        BinaryReader sub(dec.value());

        auto exists = sub.read_bool32();
        if (!exists) return exists.error();
        TriggerData t{};
        t.exists = exists.value();
        if (t.exists) {
            auto tv = sub.read_i32();
            auto name = sub.read_gm_string();
            auto cond = sub.read_gm_string();
            auto moment = sub.read_i32();
            auto cname = sub.read_gm_string();
            if (!tv || !name || !cond || !moment || !cname) {
                return Error{"Triggers[" + std::to_string(i) + "]: field read error"};
            }
            t.name = std::move(name.value());
            t.condition = std::move(cond.value());
            t.moment = moment.value();
            t.const_name = std::move(cname.value());
        }
        out.push_back(std::move(t));
    }
    return Result<void>::success();
}

Result<void> parse_constants(BinaryReader& r, std::vector<ConstantData>& out) {
    auto ver = r.read_i32();
    if (!ver) return ver.error();
    if (ver.value() != 800) return Result<void>::success();

    auto count = r.read_i32();
    if (!count || count.value() < 0) return Error{"Constants: invalid count"};
    out.reserve(static_cast<usize>(count.value()));

    for (i32 i = 0; i < count.value(); ++i) {
        auto name = r.read_gm_string();
        auto val = r.read_gm_string();
        if (!name || !val) return Error{"Constants[" + std::to_string(i) + "]: read error"};
        out.push_back(ConstantData{std::move(name.value()), std::move(val.value())});
    }
    return Result<void>::success();
}

// Helper for asset blocks where blockVersion == 800 wraps each entry in ReadStreamC().
template <typename T, typename ParseFn>
Result<void> parse_asset_block(
    BinaryReader& r,
    std::string_view block_name,
    std::vector<T>& out,
    ParseFn&& parse_entry) {
    auto ver = r.read_i32();
    auto count = r.read_i32();
    if (!ver || !count || count.value() < 0) {
        return Error{std::string(block_name) + ": invalid block header"};
    }

    const bool compressed = (ver.value() == 800);
    out.reserve(static_cast<usize>(count.value()));

    for (i32 i = 0; i < count.value(); ++i) {
        std::vector<u8> entry_buf;
        std::optional<BinaryReader> entry_reader;
        BinaryReader* er = &r;

        if (compressed) {
            auto dec = r.read_compressed_stream();
            if (!dec) {
                return Error{std::string(block_name) + "[" + std::to_string(i) + "]: zlib failed: " + dec.error_message()};
            }
            entry_buf = std::move(dec.value());
            entry_reader.emplace(entry_buf);
            er = &(*entry_reader);
        }

        auto item_res = parse_entry(*er, i);
        if (!item_res) {
            return Error{std::string(block_name) + "[" + std::to_string(i) + "]: " + item_res.error_message()};
        }
        out.push_back(std::move(item_res.value()));
    }
    return Result<void>::success();
}

Result<SoundData> parse_sound_entry(BinaryReader& r, i32) {
    SoundData s{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    s.exists = exists.value();
    if (!s.exists) return s;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    auto kind = r.read_i32();
    auto ext = r.read_gm_string();
    auto orig = r.read_gm_string();
    auto has_data = r.read_bool32();
    if (!name || !ver || !kind || !ext || !orig || !has_data) {
        return Error{"header read error"};
    }
    s.name = std::move(name.value());
    s.kind = kind.value();
    s.extension = std::move(ext.value());
    s.orig_name = std::move(orig.value());

    if (has_data.value()) {
        if (ver.value() == 600) {
            auto dec = r.read_compressed_stream();
            if (!dec) return dec.error();
            s.data = std::move(dec.value());
        } else {
            auto raw = r.read_length_prefixed_span();
            if (!raw) return raw.error();
            s.data.assign(raw.value().begin(), raw.value().end());
        }
    }

    auto effects = r.read_i32();
    auto vol = r.read_f64();
    auto pan = r.read_f64();
    auto preload = r.read_bool32();
    if (!effects || !vol || !pan || !preload) return Error{"footer read error"};
    s.effects = effects.value();
    s.volume = vol.value();
    s.pan = pan.value();
    s.preload = preload.value();
    return s;
}

Result<SpriteData> parse_sprite_entry(BinaryReader& r, i32) {
    SpriteData sp{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    sp.exists = exists.value();
    if (!sp.exists) return sp;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    if (!name || !ver) return Error{"header read error"};
    sp.name = std::move(name.value());

    if (ver.value() != 800 && ver.value() != 810) {
        return Error{"unsupported sprite version " + std::to_string(ver.value())};
    }

    auto ox = r.read_i32();
    auto oy = r.read_i32();
    auto img_cnt = r.read_i32();
    if (!ox || !oy || !img_cnt || img_cnt.value() < 0) return Error{"origin/count read error"};
    sp.origin_x = ox.value();
    sp.origin_y = oy.value();

    sp.images.reserve(static_cast<usize>(img_cnt.value()));
    for (i32 k = 0; k < img_cnt.value(); ++k) {
        auto bmp = parse_bitmap32(r);
        if (!bmp) return Error{"subimage[" + std::to_string(k) + "]: " + bmp.error_message()};
        if (k == 0) {
            sp.width = bmp.value().width;
            sp.height = bmp.value().height;
        }
        sp.images.push_back(std::move(bmp.value()));
    }

    if (ver.value() == 810) {
        auto shape = r.read_i32();
        if (!shape) return shape.error();
        sp.bbox_shape = shape.value();
    }

    auto sep = r.read_bool32();
    if (!sep) return sep.error();
    sp.separate_masks = sep.value();

    if (img_cnt.value() > 0) {
        const i32 mask_cnt = sp.separate_masks ? img_cnt.value() : 1;
        sp.masks.reserve(static_cast<usize>(mask_cnt));
        bool first_mask = true;
        for (i32 m = 0; m < mask_cnt; ++m) {
            auto mask_res = parse_collision_mask(r);
            if (!mask_res) return Error{"mask[" + std::to_string(m) + "]: " + mask_res.error_message()};
            const auto& mk = mask_res.value();
            if (first_mask) {
                sp.bbox_left = mk.bbox_left;
                sp.bbox_right = mk.bbox_right;
                sp.bbox_bottom = mk.bbox_bottom;
                sp.bbox_top = mk.bbox_top;
                first_mask = false;
            } else {
                sp.bbox_left = std::min(sp.bbox_left, mk.bbox_left);
                sp.bbox_right = std::max(sp.bbox_right, mk.bbox_right);
                sp.bbox_bottom = std::max(sp.bbox_bottom, mk.bbox_bottom);
                sp.bbox_top = std::min(sp.bbox_top, mk.bbox_top);
            }
            sp.masks.push_back(std::move(mask_res.value()));
        }
    }

    return sp;
}

Result<BackgroundData> parse_background_entry(BinaryReader& r, i32) {
    BackgroundData bg{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    bg.exists = exists.value();
    if (!bg.exists) return bg;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    if (!name || !ver) return Error{"header read error"};
    bg.name = std::move(name.value());

    if (ver.value() != 710) {
        return Error{"unsupported background version " + std::to_string(ver.value())};
    }

    auto bmp = parse_bitmap32(r);
    if (!bmp) return bmp.error();
    bg.width = bmp.value().width;
    bg.height = bmp.value().height;
    bg.bitmap = std::move(bmp.value());
    return bg;
}

Result<PathData> parse_path_entry(BinaryReader& r, i32) {
    PathData p{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    p.exists = exists.value();
    if (!p.exists) return p;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    auto kind = r.read_i32();
    auto closed = r.read_bool32();
    auto prec = r.read_i32();
    auto pt_cnt = r.read_i32();
    if (!name || !ver || !kind || !closed || !prec || !pt_cnt || pt_cnt.value() < 0) {
        return Error{"header read error"};
    }
    p.name = std::move(name.value());
    auto& geometry = p.edit_geometry();
    geometry.kind = kind.value();
    geometry.closed = closed.value();
    geometry.precision = prec.value();

    geometry.points.reserve(static_cast<usize>(pt_cnt.value()));
    for (i32 k = 0; k < pt_cnt.value(); ++k) {
        auto x = r.read_f64();
        auto y = r.read_f64();
        auto spd = r.read_f64();
        if (!x || !y || !spd) return Error{"point read error"};
        geometry.points.push_back(PathPoint{x.value(), y.value(), spd.value()});
    }
    return p;
}

Result<ScriptData> parse_script_entry(BinaryReader& r, i32) {
    ScriptData sc{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    sc.exists = exists.value();
    if (!sc.exists) return sc;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    auto src = r.read_gm_string();
    if (!name || !ver || !src) return Error{"script read error"};
    sc.name = std::move(name.value());
    sc.source = std::move(src.value());
    return sc;
}

Result<FontData> parse_font_entry(BinaryReader& r, i32) {
    FontData f{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    f.exists = exists.value();
    if (!f.exists) return f;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    auto font_name = r.read_gm_string();
    auto size = r.read_i32();
    auto bold = r.read_bool32();
    auto italic = r.read_bool32();
    auto first_packed = r.read_u32();
    auto last = r.read_i32();
    if (!name || !ver || !font_name || !size || !bold || !italic || !first_packed || !last) {
        return Error{"font header read error"};
    }
    f.name = std::move(name.value());
    f.font_name = std::move(font_name.value());
    f.size = size.value();
    f.bold = bold.value();
    f.italic = italic.value();

    const u32 fp = first_packed.value();
    f.first = static_cast<i32>(fp & 0xFFFFu);
    f.charset = static_cast<u8>((fp >> 16u) & 0xFFu);
    const u8 aa_raw = static_cast<u8>((fp >> 24u) & 0xFFu);
    f.antialias = (aa_raw != 0u) ? static_cast<u8>(aa_raw - 1u) : 3u;
    f.last = last.value();

    for (usize g = 0; g < 256; ++g) {
        auto gx = r.read_i32();
        auto gy = r.read_i32();
        auto gw = r.read_i32();
        auto gh = r.read_i32();
        auto shift = r.read_i32();
        auto off = r.read_i32();
        if (!gx || !gy || !gw || !gh || !shift || !off) return Error{"font glyph read error"};
        f.glyphs[g] = GlyphData{gx.value(), gy.value(), gw.value(), gh.value(), shift.value(), off.value()};
    }

    auto tw = r.read_i32();
    auto th = r.read_i32();
    if (!tw || !th || tw.value() < 0 || th.value() < 0) return Error{"font texture dim error"};
    f.tex_width = tw.value();
    f.tex_height = th.value();

    if (ver.value() == 540) {
        auto dec = r.read_compressed_stream();
        if (!dec) return dec.error();
        f.alpha_map = std::move(dec.value());
    } else {
        auto raw = r.read_length_prefixed_span();
        if (!raw) return raw.error();
        f.alpha_map.assign(raw.value().begin(), raw.value().end());
    }
    return f;
}

Result<TimelineData> parse_timeline_entry(BinaryReader& r, i32) {
    TimelineData tl{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    tl.exists = exists.value();
    if (!tl.exists) return tl;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    auto count = r.read_i32();
    if (!name || !ver || !count || count.value() < 0) return Error{"timeline header error"};
    tl.name = std::move(name.value());

    tl.moments.reserve(static_cast<usize>(count.value()));
    for (i32 m = 0; m < count.value(); ++m) {
        auto step = r.read_i32();
        if (!step) return step.error();
        auto ev = parse_event(r);
        if (!ev) return ev.error();
        tl.moments.push_back(TimelineMoment{step.value(), std::move(ev.value())});
    }
    return tl;
}

Result<ObjectData> parse_object_entry(BinaryReader& r, i32) {
    ObjectData obj{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    obj.exists = exists.value();
    if (!obj.exists) return obj;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    auto sprite_idx = r.read_i32();
    auto solid = r.read_bool32();
    auto visible = r.read_bool32();
    auto depth = r.read_i32();
    auto persistent = r.read_bool32();
    auto parent = r.read_i32();
    auto mask = r.read_i32();
    if (!name || !ver || !sprite_idx || !solid || !visible || !depth || !persistent || !parent || !mask) {
        return Error{"object header error"};
    }

    obj.name = std::move(name.value());
    obj.sprite_index = sprite_idx.value();
    obj.solid = solid.value();
    obj.visible = visible.value();
    obj.depth = depth.value();
    obj.persistent = persistent.value();
    obj.parent_index = parent.value();
    obj.mask_index = mask.value();

    i32 max_cat = 8;
    if (ver.value() >= 430) {
        auto mc = r.read_i32();
        if (!mc || mc.value() < 0 || mc.value() >= 12) {
            return Error{"object invalid event category count"};
        }
        max_cat = mc.value();
    }

    for (i32 cat = 0; cat <= max_cat; ++cat) {
        for (;;) {
            auto sub_id = r.read_i32();
            if (!sub_id) return sub_id.error();
            if (sub_id.value() < 0) {
                break;
            }
            auto ev = parse_event(r);
            if (!ev) return Error{"cat " + std::to_string(cat) + " sub " + std::to_string(sub_id.value()) + ": " + ev.error_message()};
            obj.events[static_cast<usize>(cat)].push_back(ObjectEventEntry{sub_id.value(), std::move(ev.value())});
        }
    }

    return obj;
}

Result<RoomData> parse_room_entry(BinaryReader& r, i32) {
    RoomData rm{};
    auto exists = r.read_bool32();
    if (!exists) return exists.error();
    rm.exists = exists.value();
    if (!rm.exists) return rm;

    auto name = r.read_gm_string();
    auto ver = r.read_i32();
    auto caption = r.read_gm_string();
    auto width = r.read_i32();
    auto height = r.read_i32();
    auto speed = r.read_i32();
    auto persistent = r.read_bool32();
    auto color = r.read_u32();
    auto flags = r.read_u32();
    auto code = r.read_gm_string();
    if (!name || !ver || !caption || !width || !height || !speed || !persistent || !color || !flags || !code) {
        return Error{"room header error"};
    }

    rm.name = std::move(name.value());
    rm.caption = std::move(caption.value());
    rm.width = width.value();
    rm.height = height.value();
    rm.speed = speed.value();
    rm.persistent = persistent.value();
    rm.color = color.value();
    rm.show_color = (flags.value() & 1u) != 0u;
    rm.view_clear_screen = (flags.value() & 2u) == 0u; // bit 1 is inverted
    rm.creation_code = std::move(code.value());

    const i32 rm_ver = ver.value();

    // Backgrounds
    auto bg_cnt = r.read_i32();
    if (!bg_cnt || bg_cnt.value() < 0) return Error{"room invalid bg count"};
    rm.backgrounds.reserve(static_cast<usize>(bg_cnt.value()));
    for (i32 i = 0; i < bg_cnt.value(); ++i) {
        auto vis = r.read_bool32();
        auto fg = r.read_bool32();
        auto idx = r.read_i32();
        auto bx = r.read_i32();
        auto by = r.read_i32();
        auto ht = r.read_bool32();
        auto vt = r.read_bool32();
        auto hs = r.read_i32();
        auto vs = r.read_i32();
        auto st = r.read_bool32();
        if (!vis || !fg || !idx || !bx || !by || !ht || !vt || !hs || !vs || !st) {
            return Error{"room background read error"};
        }
        rm.backgrounds.push_back(RoomBackground{
            vis.value(), fg.value(), idx.value(), bx.value(), by.value(),
            ht.value(), vt.value(), hs.value(), vs.value(), st.value()});
    }

    // Views
    auto en_views = r.read_bool32();
    auto view_cnt = r.read_i32();
    if (!en_views || !view_cnt || view_cnt.value() < 0) return Error{"room invalid view header"};
    rm.enable_views = en_views.value();
    rm.views.reserve(static_cast<usize>(view_cnt.value()));
    for (i32 i = 0; i < view_cnt.value(); ++i) {
        auto vis = r.read_bool32();
        auto xv = r.read_i32();
        auto yv = r.read_i32();
        auto wv = r.read_i32();
        auto hv = r.read_i32();
        auto xp = r.read_i32();
        auto yp = r.read_i32();
        auto wp = r.read_i32();
        auto hp = r.read_i32();
        auto hb = r.read_i32();
        auto vb = r.read_i32();
        auto hs = r.read_i32();
        auto vs = r.read_i32();
        auto follow = r.read_i32();
        if (!vis || !xv || !yv || !wv || !hv || !xp || !yp || !wp || !hp || !hb || !vb || !hs || !vs || !follow) {
            return Error{"room view read error"};
        }
        rm.views.push_back(RoomView{
            vis.value(), xv.value(), yv.value(), wv.value(), hv.value(),
            xp.value(), yp.value(), wp.value(), hp.value(),
            hb.value(), vb.value(), hs.value(), vs.value(), follow.value()});
    }

    // Instances
    auto inst_cnt = r.read_i32();
    if (!inst_cnt || inst_cnt.value() < 0) return Error{"room invalid instance count"};
    rm.instances.reserve(static_cast<usize>(inst_cnt.value()));
    for (i32 i = 0; i < inst_cnt.value(); ++i) {
        auto ix = r.read_i32();
        auto iy = r.read_i32();
        auto obj_idx = r.read_i32();
        auto id = r.read_i32();
        auto icode = r.read_gm_string();
        if (!ix || !iy || !obj_idx || !id || !icode) return Error{"room instance read error"};

        RoomInstance inst{};
        inst.x = ix.value();
        inst.y = iy.value();
        inst.object_index = obj_idx.value();
        inst.id = id.value();
        inst.creation_code = std::move(icode.value());

        if (rm_ver >= 810) {
            auto sx = r.read_f64();
            auto sy = r.read_f64();
            auto col = r.read_u32();
            if (!sx || !sy || !col) return Error{"room instance v810 transform error"};
            inst.scale_x = sx.value();
            inst.scale_y = sy.value();
            inst.color = col.value();
        }
        if (rm_ver >= 811) {
            auto rot = r.read_f64();
            if (!rot) return Error{"room instance v811 rotation error"};
            inst.rotation = rot.value();
        }
        rm.instances.push_back(std::move(inst));
    }

    // Tiles
    auto tile_cnt = r.read_i32();
    if (!tile_cnt || tile_cnt.value() < 0) return Error{"room invalid tile count"};
    rm.tiles.reserve(static_cast<usize>(tile_cnt.value()));
    for (i32 i = 0; i < tile_cnt.value(); ++i) {
        auto tx = r.read_i32();
        auto ty = r.read_i32();
        auto bg_idx = r.read_i32();
        auto xo = r.read_i32();
        auto yo = r.read_i32();
        auto tw = r.read_i32();
        auto th = r.read_i32();
        auto depth = r.read_i32();
        auto id = r.read_i32();
        if (!tx || !ty || !bg_idx || !xo || !yo || !tw || !th || !depth || !id) {
            return Error{"room tile read error"};
        }

        RoomTile tile{};
        tile.x = tx.value();
        tile.y = ty.value();
        tile.background_index = bg_idx.value();
        tile.xo = xo.value();
        tile.yo = yo.value();
        tile.w = tw.value();
        tile.h = th.value();
        tile.depth = depth.value();
        tile.id = id.value();

        if (rm_ver >= 810) {
            auto sx = r.read_f64();
            auto sy = r.read_f64();
            auto packed = r.read_u32();
            if (!sx || !sy || !packed) return Error{"room tile v810 transform error"};
            tile.scale_x = sx.value();
            tile.scale_y = sy.value();
            tile.blend = packed.value() & 0x00FFFFFFu;
            tile.alpha = static_cast<f64>((packed.value() >> 24u) & 0xFFu) / 255.0;
        }
        rm.tiles.push_back(tile);
    }

    return rm;
}

Result<IncludedFileData> parse_included_file_entry(BinaryReader& r, i32) {
    IncludedFileData f{};
    auto ver = r.read_i32();
    auto fname = r.read_gm_string();
    auto orig = r.read_gm_string();
    auto exists = r.read_bool32();
    auto size = r.read_i32();
    auto store = r.read_bool32();
    if (!ver || !fname || !orig || !exists || !size || !store) {
        return Error{"included file header error"};
    }
    f.filename = std::move(fname.value());
    f.orig_name = std::move(orig.value());
    f.exists = exists.value();
    f.size = size.value();
    f.store = store.value();

    if (f.exists && f.store) {
        if (ver.value() == 620) {
            auto dec = r.read_compressed_stream();
            if (!dec) return dec.error();
            f.data = std::move(dec.value());
        } else {
            auto raw = r.read_length_prefixed_span();
            if (!raw) return raw.error();
            f.data.assign(raw.value().begin(), raw.value().end());
        }
    }

    auto exp_act = r.read_i32();
    auto exp_dir = r.read_gm_string();
    auto overwrite = r.read_bool32();
    auto free_data = r.read_bool32();
    auto remove_end = r.read_bool32();
    if (!exp_act || !exp_dir || !overwrite || !free_data || !remove_end) {
        return Error{"included file footer error"};
    }
    f.export_action = exp_act.value();
    f.export_dir = std::move(exp_dir.value());
    f.overwrite = overwrite.value();
    f.free_data = free_data.value();
    f.remove_end = remove_end.value();
    return f;
}

Result<GameHelpData> parse_help(BinaryReader& outer) {
    auto ver_res = outer.read_i32();
    if (!ver_res) return Error{"Help: version read error"};
    const i32 ver = ver_res.value();

    std::vector<u8> dec_buf;
    std::optional<BinaryReader> sub;
    BinaryReader* r = &outer;
    if (ver == 800) {
        auto dec = outer.read_compressed_stream();
        if (!dec) return Error{"Help: zlib decompress failed"};
        dec_buf = std::move(dec.value());
        sub.emplace(dec_buf);
        r = &(*sub);
    }

    GameHelpData h{};
    auto bg = r->read_u32();
    auto mimic = r->read_bool32();
    auto cap = r->read_gm_string();
    auto left = r->read_i32();
    auto top = r->read_i32();
    auto width = r->read_i32();
    auto height = r->read_i32();
    auto border = r->read_bool32();
    auto sizable = r->read_bool32();
    auto on_top = r->read_bool32();
    auto modal = r->read_bool32();
    if (!bg || !mimic || !cap || !left || !top || !width || !height || !border || !sizable || !on_top || !modal) {
        return Error{"Help: field read error"};
    }
    h.bg_color = bg.value();
    h.mimic = mimic.value();
    h.caption = std::move(cap.value());
    h.left = left.value();
    h.top = top.value();
    h.width = width.value();
    h.height = height.value();
    h.border = border.value();
    h.sizable = sizable.value();
    h.on_top = on_top.value();
    h.modal = modal.value();

    if (ver == 600) {
        auto dec = r->read_compressed_stream();
        if (!dec) return dec.error();
        h.rtf_text.assign(reinterpret_cast<const char*>(dec.value().data()), dec.value().size());
    } else {
        auto raw = r->read_gm_string();
        if (!raw) return raw.error();
        h.rtf_text = std::move(raw.value());
    }
    return h;
}

Result<Gm82EventTables> parse_gm82_event_tables(BinaryReader& outer) {
    auto dec = outer.read_compressed_stream();
    if (!dec) return Error{"GM8.2 event tables: zlib failed: " + dec.error_message()};
    BinaryReader r(dec.value());

    Gm82EventTables tables{};
    for (usize cat = 0; cat < 12; ++cat) {
        auto sub_cnt = r.read_i32();
        if (!sub_cnt || sub_cnt.value() < 0) return Error{"GM8.2 event tables: invalid sub count"};
        tables.event_objects[cat].resize(static_cast<usize>(sub_cnt.value()));
        for (i32 j = 0; j < sub_cnt.value(); ++j) {
            auto obj_cnt = r.read_i32();
            if (!obj_cnt || obj_cnt.value() < 0) return Error{"GM8.2 event tables: invalid obj count"};
            auto& vec = tables.event_objects[cat][static_cast<usize>(j)];
            vec.resize(static_cast<usize>(obj_cnt.value()));
            for (i32 k = 0; k < obj_cnt.value(); ++k) {
                auto oid = r.read_i32();
                if (!oid) return oid.error();
                vec[static_cast<usize>(k)] = oid.value();
            }
        }
    }

    auto pair_cnt = r.read_i32();
    if (!pair_cnt || pair_cnt.value() < 0) return Error{"GM8.2 event tables: invalid collision pair count"};
    const usize n_pairs = static_cast<usize>(pair_cnt.value());
    auto pair_span = r.read_span(n_pairs * 8u);
    if (!pair_span) return Error{"GM8.2 event tables: collision pair read error"};
    tables.collision_pairs.resize(n_pairs);
    const u8* pp = pair_span.value().data();
    for (usize p = 0; p < n_pairs; ++p) {
        i32 a = 0;
        i32 b = 0;
        std::memcpy(&a, pp, 4);
        std::memcpy(&b, pp + 4, 4);
        tables.collision_pairs[p] = {a, b};
        pp += 8;
    }

    return tables;
}

template <typename T>
usize count_existing(const std::vector<T>& items) noexcept {
    usize n = 0;
    for (const auto& item : items) {
        if (item.exists) ++n;
    }
    return n;
}

// GM borrows adjacent RGB for transparent texels so filtering preserves edge colors (gm81.exe.c:sub_56E4F8).
void bleed_transparent_rgb(Bitmap32& bmp) noexcept {
    if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgra.empty()) return;
    const usize stride = static_cast<usize>(bmp.width) * 4u;
    for (usize p = 0; p + 3u < bmp.bgra.size(); p += 4u) {
        if (bmp.bgra[p + 3] != 0) continue;
        const usize col = p % stride;
        usize neighbor = p;
        if (col > 0 && bmp.bgra[p - 1] != 0) neighbor = p - 4;
        else if (col + 4 < stride && bmp.bgra[p + 7] != 0) neighbor = p + 4;
        else if (p >= stride && bmp.bgra[p - stride + 3] != 0) neighbor = p - stride;
        else if (p + stride < bmp.bgra.size() && bmp.bgra[p + stride + 3] != 0) neighbor = p + stride;
        if (neighbor != p) std::copy_n(bmp.bgra.data() + neighbor, 3, bmp.bgra.data() + p);
    }
}

} // namespace

Result<SpriteMaskSettings> read_gmspr(std::span<const u8> bytes, SpriteData& sprite) {
    BinaryReader outer(bytes);
    auto magic = outer.read_u32();
    if (!magic || magic.value() != 1234321) return Error{"Invalid GMSPR header"};
    auto data = outer.read_compressed_stream();
    if (!data) return data.error();

    SpriteData empty{};
    empty.exists = true;
    empty.name = std::move(sprite.name);
    empty.width = empty.height = 1;
    sprite = std::move(empty);

    BinaryReader r(data.value());
    auto version = r.read_i32();
    if (!version || version.value() != 800) return Error{"Unsupported GMSPR version"};
    auto ox = r.read_i32();
    auto oy = r.read_i32();
    auto count = r.read_i32();
    if (!ox || !oy || !count || count.value() < 0 ||
        static_cast<usize>(count.value()) > r.remaining() / 12u) return Error{"Invalid GMSPR frame count"};

    std::vector<Bitmap32> frames;
    for (i32 i = 0; i < count.value(); ++i) {
        auto bitmap_version = r.read_i32();
        if (!bitmap_version || bitmap_version.value() != 800) return Error{"Invalid GMSPR bitmap version"};
        auto frame = read_bitmap32_v800(r);
        if (!frame) return frame.error();
        bleed_transparent_rgb(frame.value());
        frames.push_back(std::move(frame.value()));
    }
    auto shape = r.read_i32();
    auto tolerance = r.read_i32();
    auto separate = r.read_bool32();
    auto mode = r.read_i32();
    auto left = r.read_i32();
    auto right = r.read_i32();
    auto bottom = r.read_i32();
    auto top = r.read_i32();
    if (!shape || !tolerance || !separate || !mode || !left || !right || !bottom || !top)
        return Error{"Missing GMSPR mask settings"};

    sprite.origin_x = ox.value();
    sprite.origin_y = oy.value();
    sprite.images = std::move(frames);
    if (!sprite.images.empty()) {
        sprite.width = sprite.images.front().width;
        sprite.height = sprite.images.front().height;
    }
    sprite.bbox_shape = shape.value();
    sprite.separate_masks = separate.value();
    sprite.bbox_left = left.value();
    sprite.bbox_right = right.value();
    sprite.bbox_top = top.value();
    sprite.bbox_bottom = bottom.value();
    return SpriteMaskSettings{tolerance.value(), mode.value()};
}

Result<void> read_gmbck(std::span<const u8> bytes, BackgroundData& background) {
    BinaryReader outer(bytes);
    auto magic = outer.read_u32();
    if (!magic || magic.value() != 1234321u) return Error{"Invalid GMBCK header"};
    auto data = outer.read_compressed_stream();
    if (!data) return data.error();

    BackgroundData empty{};
    empty.exists = true;
    empty.name = std::move(background.name);
    background = std::move(empty);

    BinaryReader r(data.value());
    auto version = r.read_i32();
    if (!version || version.value() != 710) return Error{"Unsupported GMBCK version"};

    auto is_tileset = r.read_bool32();
    auto tile_w = r.read_i32();
    auto tile_h = r.read_i32();
    auto tile_hoff = r.read_i32();
    auto tile_voff = r.read_i32();
    auto tile_hsep = r.read_i32();
    auto tile_vsep = r.read_i32();
    if (!is_tileset || !tile_w || !tile_h || !tile_hoff || !tile_voff || !tile_hsep || !tile_vsep) {
        return Error{"Truncated GMBCK header"};
    }

    auto bitmap_version = r.read_i32();
    if (!bitmap_version || bitmap_version.value() != 800) {
        return Error{"Invalid GMBCK bitmap version"};
    }
    auto bmp = read_bitmap32_v800(r);
    if (!bmp) return bmp.error();
    bleed_transparent_rgb(bmp.value());

    background.is_tileset = is_tileset.value();
    background.width = bmp.value().width;
    background.height = bmp.value().height;
    background.bitmap = std::move(bmp.value());
    return Result<void>::success();
}

Result<GameData> ExeLoader::load_from_bytes(std::vector<u8> exe_bytes, const LoadOptions& options) {
    GameData game{};
    game.meta.exe_size = exe_bytes.size();

    // Unpack optional UPX PE32 wrapper before reading runner resources or inspecting stubs.
    if (is_upx_packed_pe(exe_bytes)) {
        auto unpacked = unpack_upx_pe(exe_bytes);
        if (unpacked) {
            GM_LOG_INFO("loader", "Unpacked UPX PE32 runner (%zu -> %zu bytes)",
                        exe_bytes.size(), unpacked.value().size());
            exe_bytes = std::move(unpacked.value());
        } else {
            GM_LOG_WARN("loader", "UPX PE32 unpacking failed (%s); continuing with packed bytes",
                        unpacked.error_message().c_str());
        }
    }

    game.meta.pe_overlay_offset = find_pe_overlay_offset(exe_bytes);
    if (game.meta.pe_overlay_offset > 0) {
        const auto runner_span = std::span<const u8>(exe_bytes).first(game.meta.pe_overlay_offset);
        read_particle_shapes(runner_span, game);
        read_message_form_assets(runner_span, game);
    }

    bool is_gm81_family = false;
    bool used_antidecompiler = false;
    usize header_pos = 0;

    // Step 1: Locate & decrypt outer executable payload.
    // Check GM 8.1/8.2 first when pe_overlay_offset >= 3 MB or when GM 8.0 magic is absent.
    const usize gm81_hint = (game.meta.pe_overlay_offset > 0)
                                ? std::min<usize>(game.meta.pe_overlay_offset, 3174000u)
                                : 0u;

    auto gm80_off = find_gm80_payload(exe_bytes, game.meta.pe_overlay_offset);
    if (gm80_off.has_value()) {
        is_gm81_family = false;
        header_pos = gm80_off.value();
        game.meta.payload_offset = header_pos;
    } else if (auto ad_res = try_decrypt_antidecompiler(exe_bytes, game.meta.pe_overlay_offset); ad_res) {
        used_antidecompiler = true;
        is_gm81_family = ad_res.value().is_gm81_family;
        header_pos = ad_res.value().header_offset;
        game.meta.payload_offset = ad_res.value().payload_offset;
        GM_LOG_INFO("loader", "Decrypted AntiDecompiler %s payload at 0x%zx",
                    is_gm81_family ? "GM 8.1" : "GM 8.0",
                    game.meta.payload_offset);
    } else {
        auto gm81_res = try_decrypt_gm81_wad(exe_bytes, gm81_hint);
        if (!gm81_res) {
            return Error{"Failed to detect GM 8.0 / 8.1 / 8.2 game data in executable (" +
                         gm81_res.error_message() + ")"};
        }
        is_gm81_family = true;
        header_pos = gm81_res.value().header_offset;
        game.meta.payload_offset = gm81_res.value().magic_offset;
        if (gm81_res.value().used_sudalv) {
            GM_LOG_INFO("loader", "Decrypted GM 8.1 SUDALV WAD at 0x%zx", game.meta.payload_offset);
        }
    }

    BinaryReader outer(exe_bytes);
    if (auto s = outer.seek(header_pos); !s) return s.error();

    // Step 2: Parse top-level header, settings, and skip embedded D3DX8.dll.
    if (!is_gm81_family) {
        auto magic = outer.read_i32();
        auto ver = outer.read_i32();
        auto dbg = outer.read_bool32();
        if (!magic || !ver || !dbg || magic.value() != 1234321 || ver.value() != 800) {
            return Error{"Invalid GM 8.0 header"};
        }
        game.meta.format_version = ver.value();
        game.meta.debug_mode = dbg.value();
        game.meta.detected_version = VersionMode::GM80;
    } else {
        auto ver = outer.read_i32();
        auto build_days = outer.read_i32();
        auto dbg = outer.read_bool32();
        auto build_secs = outer.read_i32();
        (void)build_days;
        (void)build_secs;
        if (!ver || !build_days || !dbg || !build_secs || ver.value() != 810) {
            return Error{"Invalid GM 8.1/8.2 header"};
        }
        game.meta.format_version = ver.value();
        game.meta.debug_mode = dbg.value();
        game.meta.detected_version = VersionMode::GM81;
    }

    auto opts_res = parse_options(outer);
    if (!opts_res) return opts_res.error();
    game.options = std::move(opts_res.value());

    // Skip embedded D3DX8.dll filename + zlib blob.
    auto d3dx_name = outer.read_gm_string();
    if (!d3dx_name) return Error{"Failed reading D3DX8 filename: " + d3dx_name.error_message()};
    auto d3dx_len = outer.read_u32();
    if (!d3dx_len) return Error{"Failed reading D3DX8 length"};
    if (auto sk = outer.skip(d3dx_len.value()); !sk) return Error{"Failed skipping D3DX8 blob"};

    // Step 3: Decrypt inner StreamE swap-table payload.
    auto stream_e_res = decrypt_stream_e(outer);
    if (!stream_e_res) return stream_e_res.error();
    const std::vector<u8> stream_e_bytes = std::move(stream_e_res.value());
    BinaryReader r(stream_e_bytes);

    auto junk_cnt = r.read_i32();
    if (!junk_cnt || junk_cnt.value() < 0) return Error{"StreamE: invalid junk_cnt"};
    if (auto sk = r.skip(static_cast<usize>(junk_cnt.value()) * 4u); !sk) return sk.error();

    auto edition_res = r.read_u32();
    auto game_id_res = r.read_i32();
    auto guid_res = r.read_span(16);
    if (!edition_res || !game_id_res || !guid_res) return Error{"StreamE: failed reading game ID / GUID"};

    game.meta.edition_flags = edition_res.value();
    game.meta.pro_edition = (edition_res.value() & 1u) != 0u;
    game.meta.game_id = game_id_res.value();
    std::memcpy(game.meta.guid.data(), guid_res.value().data(), 16);

    // Step 4: Parse all 16 asset blocks sequentially.
    if (auto e = parse_extensions(r, game.extensions); !e) return e.error();
    if (auto e = parse_triggers(r, game.triggers); !e) return e.error();
    if (auto e = parse_constants(r, game.constants); !e) return e.error();
    if (auto e = parse_asset_block(r, "Sounds", game.sounds, parse_sound_entry); !e) return e.error();
    if (auto e = parse_asset_block(r, "Sprites", game.sprites, parse_sprite_entry); !e) return e.error();
    if (auto e = parse_asset_block(r, "Backgrounds", game.backgrounds, parse_background_entry); !e) return e.error();
    if (auto e = parse_asset_block(r, "Paths", game.paths, parse_path_entry); !e) return e.error();
    if (auto e = parse_asset_block(r, "Scripts", game.scripts, parse_script_entry); !e) return e.error();
    if (auto e = parse_asset_block(r, "Fonts", game.fonts, parse_font_entry); !e) return e.error();
    if (auto e = parse_asset_block(r, "Timelines", game.timelines, parse_timeline_entry); !e) return e.error();
    if (auto e = parse_asset_block(r, "Objects", game.objects, parse_object_entry); !e) return e.error();
    if (auto e = parse_asset_block(r, "Rooms", game.rooms, parse_room_entry); !e) return e.error();

    auto max_id = r.read_i32();
    auto max_tile_id = r.read_i32();
    if (!max_id || !max_tile_id) return Error{"Rooms: failed reading RoomMaxId / RoomMaxTileId"};
    game.room_max_id = max_id.value();
    game.room_max_tile_id = max_tile_id.value();

    if (auto e = parse_asset_block(r, "IncludedFiles", game.included_files, parse_included_file_entry); !e) return e.error();

    auto help_res = parse_help(r);
    if (!help_res) return help_res.error();
    game.help = std::move(help_res.value());

    // Libraries
    {
        auto lib_ver = r.read_i32();
        auto lib_cnt = r.read_i32();
        if (!lib_ver || !lib_cnt || lib_cnt.value() < 0) return Error{"Libraries: invalid header"};
        game.libraries.reserve(static_cast<usize>(lib_cnt.value()));
        for (i32 i = 0; i < lib_cnt.value(); ++i) {
            auto code = r.read_gm_string();
            if (!code) return code.error();
            game.libraries.push_back(std::move(code.value()));
        }
    }

    // Room Order
    {
        auto ro_ver = r.read_i32();
        auto ro_cnt = r.read_i32();
        if (!ro_ver || !ro_cnt || ro_cnt.value() < 0) return Error{"RoomOrder: invalid header"};
        game.room_order.reserve(static_cast<usize>(ro_cnt.value()));
        for (i32 i = 0; i < ro_cnt.value(); ++i) {
            auto rid = r.read_i32();
            if (!rid) return rid.error();
            game.room_order.push_back(rid.value());
        }
    }

    // Step 5: Detect optional GM 8.2 trailing precomputed event/collision table (sub_5F783F).
    // GM 8.1 and early GM 8.2 EXEs may carry trailing resource-tree/padding bytes; only parse
    // as GM 8.2 event tables if a valid zlib length-prefixed chunk is present.
    if (is_gm81_family && r.remaining() >= 6) {
        const usize saved_pos = r.position();
        auto len_peek = r.read_u32();
        auto hdr_peek = r.read_u8();
        (void)r.seek(saved_pos);
        if (len_peek && hdr_peek &&
            len_peek.value() >= 2u &&
            len_peek.value() <= r.remaining() - 4u &&
            hdr_peek.value() == 0x78u) {
            auto ev_tbl_res = parse_gm82_event_tables(r);
            if (ev_tbl_res) {
                game.gm82_event_tables = std::move(ev_tbl_res.value());
                game.meta.has_gm82_event_tables = true;
                game.meta.detected_version = VersionMode::GM82;
            } else {
                (void)r.seek(saved_pos);
                GM_LOG_DEBUG("loader", "Trailing %zu bytes in StreamE are not GM 8.2 event tables (%s)",
                             r.remaining(), ev_tbl_res.error_message().c_str());
            }
        }
    }

    // Also identify GM 8.2 if PE stub was trimmed (< 3.5 MB) or gm82* extensions are embedded.
    // AntiDecompiler strips .reloc and moves .rsrc below 3.5 MB on GM 8.1, so skip trimmed_stub for it.
    if (is_gm81_family && game.meta.detected_version == VersionMode::GM81) {
        const bool trimmed_stub = !used_antidecompiler &&
                                  (game.meta.pe_overlay_offset > 0 && game.meta.pe_overlay_offset < 3500000u);
        bool has_gm82_ext = false;
        for (const auto& ext : game.extensions) {
            if (ascii_to_lower(ext.folder) == "gm82") {
                has_gm82_ext = true;
                break;
            }
        }
        if (trimmed_stub || has_gm82_ext) {
            game.meta.detected_version = VersionMode::GM82;
        }
    }

    if (options.version_override != VersionMode::Auto) {
        game.meta.detected_version = options.version_override;
    }

    // Only GM8.2 honors an explicitly packed ANSI_CHARSET (zero).
    if (game.meta.detected_version != VersionMode::GM82) {
        for (auto& font : game.fonts) {
            if (font.charset == 0) font.charset = 1;
        }
    }

    // Tag backgrounds referenced by room tiles.
    for (const auto& rm : game.rooms) {
        if (!rm.exists) continue;
        for (const auto& tile : rm.tiles) {
            if (tile.background_index >= 0 &&
                static_cast<usize>(tile.background_index) < game.backgrounds.size()) {
                game.backgrounds[static_cast<usize>(tile.background_index)].is_tileset = true;
            }
        }
    }

    game.meta.encoding = select_game_encoding(game, options.codepage_override);

    if (r.remaining() > 0) {
        GM_LOG_DEBUG("loader", "StreamE parsed with %zu unread trailing bytes", r.remaining());
    }

    return game;
}

Result<GameData> ExeLoader::load_from_vfs(Vfs& vfs, std::string_view exe_path, const LoadOptions& options) {
    auto bytes_res = vfs.read_all_bytes(exe_path);
    if (!bytes_res) {
        return bytes_res.error();
    }
    return load_from_bytes(std::move(bytes_res.value()), options);
}

void print_game_inspect(const GameData& game, bool verbose) {
    const auto& m = game.meta;
    const auto& o = game.options;

    std::printf("=== GameMaker Executable Inspection ===\n");
    std::printf("Version:      GM %.*s (stream v%d, encoding=%s, codepage=%u, source=%.*s)\n",
                static_cast<int>(version_mode_name(m.detected_version).size()),
                version_mode_name(m.detected_version).data(),
                m.format_version,
                m.encoding.mode == EncodingMode::Utf8 ? "UTF-8" : "LegacyANSI",
                m.encoding.codepage,
                static_cast<int>(m.encoding.source.size()), m.encoding.source.data());
    std::printf("EXE Size:     %zu bytes (PE overlay @ 0x%zx, payload @ 0x%zx)\n",
                m.exe_size, m.pe_overlay_offset, m.payload_offset);
    std::printf("Game ID:      %d (Pro=%s, Debug=%s, GUID=%s)\n",
                m.game_id,
                m.pro_edition ? "yes" : "no",
                m.debug_mode ? "yes" : "no",
                hex_encode(m.guid).c_str());
    std::printf("Settings:     v%d, fullscreen=%d, interp=%d, scale=%d, vsync=%d, uninit_as_0=%d, create_order=%d\n",
                o.version, o.fullscreen ? 1 : 0, o.interpolate_pixels ? 1 : 0, o.scale,
                o.vsync_enabled() ? 1 : 0, o.treat_uninitialized_as_zero ? 1 : 0, o.creation_event_order ? 1 : 0);

    std::printf("\n--- Assets ---\n");
    std::printf("  Extensions:     %zu\n", game.extensions.size());
    std::printf("  Triggers:       %zu (%zu active)\n", game.triggers.size(), count_existing(game.triggers));
    std::printf("  Constants:      %zu\n", game.constants.size());
    std::printf("  Sounds:         %zu (%zu active)\n", game.sounds.size(), count_existing(game.sounds));
    std::printf("  Sprites:        %zu (%zu active)\n", game.sprites.size(), count_existing(game.sprites));
    std::printf("  Backgrounds:    %zu (%zu active)\n", game.backgrounds.size(), count_existing(game.backgrounds));
    std::printf("  Paths:          %zu (%zu active)\n", game.paths.size(), count_existing(game.paths));
    std::printf("  Scripts:        %zu (%zu active)\n", game.scripts.size(), count_existing(game.scripts));
    std::printf("  Fonts:          %zu (%zu active)\n", game.fonts.size(), count_existing(game.fonts));
    std::printf("  Timelines:      %zu (%zu active)\n", game.timelines.size(), count_existing(game.timelines));
    std::printf("  Objects:        %zu (%zu active)\n", game.objects.size(), count_existing(game.objects));
    std::printf("  Rooms:          %zu (%zu active, order=%zu, max_id=%d, max_tile_id=%d)\n",
                game.rooms.size(), count_existing(game.rooms), game.room_order.size(),
                game.room_max_id, game.room_max_tile_id);
    std::printf("  Included Files: %zu\n", game.included_files.size());
    std::printf("  Libraries:      %zu\n", game.libraries.size());

    if (m.has_gm82_event_tables) {
        usize total_ev_entries = 0;
        for (const auto& cat : game.gm82_event_tables.event_objects) {
            total_ev_entries += cat.size();
        }
        std::printf("  GM 8.2 Tables:  %zu sub-event buckets, %zu collision pairs\n",
                    total_ev_entries, game.gm82_event_tables.collision_pairs.size());
    }

    if (!game.room_order.empty()) {
        const i32 first_id = game.room_order[0];
        if (first_id >= 0 && static_cast<usize>(first_id) < game.rooms.size() &&
            game.rooms[static_cast<usize>(first_id)].exists) {
            const auto& rm = game.rooms[static_cast<usize>(first_id)];
            std::printf("  First Room:     [%d] '%s' (%dx%d @ %d fps, %zu instances, %zu tiles)\n",
                        first_id, rm.name.c_str(), rm.width, rm.height, rm.speed,
                        rm.instances.size(), rm.tiles.size());
        }
    }

    if (!game.extensions.empty()) {
        std::printf("\n--- Embedded Extensions ---\n");
        for (const auto& ext : game.extensions) {
            usize total_funcs = 0;
            usize total_consts = 0;
            for (const auto& inc : ext.includes) {
                total_funcs += inc.functions.size();
                total_consts += inc.constants.size();
            }
            std::printf("  * %s (folder='%s', includes=%zu, funcs=%zu, consts=%zu)\n",
                        ext.name.c_str(), ext.folder.c_str(), ext.includes.size(), total_funcs, total_consts);
            if (verbose) {
                for (const auto& inc : ext.includes) {
                    const char* kstr = (inc.kind == 1) ? "DLL" : (inc.kind == 2) ? "GML" : (inc.kind == 3) ? "LIB" : "FILE";
                    std::printf("      - [%s] %s (%zu bytes, funcs=%zu, init='%s', final='%s')\n",
                                kstr, inc.filename.c_str(), inc.payload.size(), inc.functions.size(),
                                inc.init_func.c_str(), inc.final_func.c_str());
                }
            }
        }
    }

    if (!game.included_files.empty()) {
        std::printf("\n--- Included Files ---\n");
        for (const auto& f : game.included_files) {
            std::printf("  * %s (size=%d, stored=%zu bytes, export_action=%d, dir='%s')\n",
                        f.filename.c_str(), f.size, f.data.size(), f.export_action, f.export_dir.c_str());
        }
    }
}

} // namespace gm
