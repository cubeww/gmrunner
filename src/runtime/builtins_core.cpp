#include "runtime/runner.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/log.h"
#include "base/math.h"
#include "platform/clock.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace gm {

namespace {

Result<i32> delphi_floor_i32(f64 value) {
    // Delphi's Floor uses a signed 64-bit conversion and returns the low 32 bits.
    if (!(value >= -0x1p63 && value < 0x1p63)) return Error{"Invalid floating point operation."};
    return static_cast<i32>(static_cast<i64>(std::floor(value)));
}

f64 parse_gm_real(std::string_view s) {
    usize i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) return 0.0;
    std::string tmp(s.substr(i));
    char* end = nullptr;
    const f64 v = std::strtod(tmp.c_str(), &end);
    return (end != tmp.c_str() && std::isfinite(v)) ? v : 0.0;
}

Result<RValue> append_text_real(std::string& out, f64 value, bool separate_sign) {
    const bool negative = separate_sign && value < 0.0;
    if (negative) value = -value;
    const auto whole = delphi_floor_i32(value);
    if (!whole) return whole.error();
    const auto fraction = delphi_floor_i32((value - whole.value()) * 1'000'000.0);
    if (!fraction) return fraction.error();
    out.append(negative ? " -" : " ");
    out.append(std::to_string(whole.value()));
    if (fraction.value() != 0) {
        char digits[8];
        std::snprintf(digits, sizeof(digits), ".%06u", static_cast<u32>(fraction.value()) % 1'000'000u);
        out.append(digits);
    }
    return RValue(0.0);
}

usize gm81_character_width(u8 lead) noexcept {
    // Delphi accepts five- and six-byte leads and ignores the following bytes.
    if (lead < 0xC2 || lead > 0xFD) return 1;
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    if (lead < 0xF8) return 4;
    if (lead < 0xFC) return 5;
    return 6;
}

f64 delphi_datetime_from_tm(const std::tm& tm_val) {
    // Days since 1899-12-30 (Delphi TDateTime epoch = Unix epoch + 25569.0 days)
    std::tm copy = tm_val;
    const std::time_t unix_sec = std::mktime(&copy);
    return static_cast<f64>(unix_sec) / 86400.0 + 25569.0;
}

std::tm tm_from_delphi_datetime(f64 dt) {
    const std::time_t unix_sec = static_cast<std::time_t>((dt - 25569.0) * 86400.0);
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &unix_sec);
#else
    localtime_r(&unix_sec, &out);
#endif
    return out;
}

bool is_leap_year_gregorian(i32 y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

i32 days_in_month_gregorian(i32 y, i32 m) noexcept {
    static constexpr i32 kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && is_leap_year_gregorian(y)) return 29;
    return kDays[m - 1];
}

bool is_valid_date_ymd(i32 y, i32 m, i32 d) noexcept {
    if (y < 1 || y > 9999 || m < 1 || m > 12) return false;
    return d >= 1 && d <= days_in_month_gregorian(y, m);
}

bool is_valid_time_hms(i32 h, i32 mi, i32 s) noexcept {
    return h >= 0 && h < 24 && mi >= 0 && mi < 60 && s >= 0 && s < 60;
}

} // namespace

std::string LegacyTextFile::read_string() {
    std::string line;
    while (!eof()) {
        const char c = contents[pos];
        if (c == '\n' || c == '\x1a' || (c == '\r' && pos + 1 < contents.size() && contents[pos + 1] == '\n')) break;
        ++pos;
        if (c != '\r') line.push_back(c);
    }
    return line;
}

void LegacyTextFile::skip_line() {
    const usize lf = contents.find('\n', pos);
    pos = lf == std::string::npos ? contents.size() : lf + 1;
}

f64 LegacyTextFile::read_real() {
    f64 value = 0.0;
    bool negative = false;
    u32 divisor = 1;
    while (!eoln()) {
        const char c = contents[pos++];
        if (c == ' ' || c == '\t') continue;
        if (c == '-') {
            negative = true;
        } else if ((c == '.' || c == ',') && divisor == 1) {
            divisor = 10;
        } else if (c >= '0' && c <= '9') {
            if (divisor == 1) {
                value = value * 10.0 + (c - '0');
            } else {
                value += static_cast<f64>(c - '0') / static_cast<i32>(divisor);
                divisor *= 10;
            }
        } else {
            break;
        }
    }
    return negative ? -value : value;
}

Result<RValue> Runner::close_legacy_text_file() {
    if (!legacy_text_file_) return RValue(0.0);
    LegacyTextFile file = std::move(*legacy_text_file_);
    legacy_text_file_.reset();
    if (file.writable) {
        auto written = vfs_.write_all_bytes(file.path, {
            reinterpret_cast<const u8*>(file.contents.data()), file.contents.size()});
        if (!written) return written.error();
    }
    return RValue(0.0);
}

Result<RValue> Runner::open_legacy_text_file(std::string_view path, i32 mode) {
    auto closed = close_legacy_text_file();
    if (!closed) return closed.error();
    LegacyTextFile file;
    file.path = vfs_.absolute_path_gm(path);
    file.writable = mode != 0;
    if (mode == 0 || (mode == 2 && vfs_.exists(path))) {
        auto contents = vfs_.read_all_bytes(path);
        if (!contents) return contents.error();
        const auto& bytes = contents.value();
        if (!bytes.empty()) file.contents.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    } else {
        auto created = vfs_.write_all_bytes(path, {});
        if (!created) return created.error();
    }
    legacy_text_file_ = std::move(file);
    return RValue(0.0);
}

void Runner::register_builtins_core() {
    // -----------------------------------------------------------------------
    // Math & RNG
    // -----------------------------------------------------------------------
    register_builtin("is_real", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(!a.empty() && a[0].is_real());
    });
    register_builtin("is_string", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(!a.empty() && a[0].is_string());
    });

    register_builtin("random", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.random_uniform(arg_real(a, 0)));
    });
    register_builtin("random_range", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 x1 = arg_real(a, 0);
        const f64 x2 = arg_real(a, 1);
        return RValue(x1 + r.random_uniform(x2 - x1));
    });
    register_builtin("irandom", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.random_int_inclusive(static_cast<i64>(std::floor(arg_real(a, 0)))));
    });
    register_builtin("irandom_range", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i64 lo = static_cast<i64>(std::floor(arg_real(a, 0)));
        const i64 hi = static_cast<i64>(std::floor(arg_real(a, 1)));
        const i64 mn = std::min(lo, hi);
        const i64 mx = std::max(lo, hi);
        return RValue(mn + r.random_int_inclusive(mx - mn));
    });
    register_builtin("random_set_seed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.set_random_seed(static_cast<u32>(arg_i32(a, 0)));
        return RValue(0.0);
    });
    register_builtin("random_get_seed", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.random_seed());
    });
    register_builtin("randomize", [](Runner& r, i32, i32, std::span<const RValue>) {
        const u32 s = static_cast<u32>(Clock::now_us() ^ 0x9E3779B9u);
        r.set_random_seed(s);
        return RValue(0.0);
    });
    register_builtin("choose", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        const i64 idx = r.random_int_inclusive(static_cast<i64>(a.size() - 1));
        return a[static_cast<usize>(idx)];
    });

    register_builtin("abs", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::fabs(arg_real(a, 0))); });
    register_builtin("sign", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 v = arg_real(a, 0);
        return RValue(v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0));
    });
    register_builtin("round", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(delphi_round_f64(arg_real(a, 0))); });
    register_builtin("floor", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::floor(arg_real(a, 0))); });
    register_builtin("ceil", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::ceil(arg_real(a, 0))); });
    register_builtin("frac", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 v = arg_real(a, 0);
        return RValue(v - std::trunc(v));
    });
    register_builtin("sqrt", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::sqrt(std::max(0.0, arg_real(a, 0)))); });
    register_builtin("sqr", [](Runner&, i32, i32, std::span<const RValue> a) { const f64 v = arg_real(a, 0); return RValue(v * v); });
    register_builtin("exp", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::exp(arg_real(a, 0))); });
    register_builtin("ln", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::log(arg_real(a, 0))); });
    register_builtin("log2", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::log2(arg_real(a, 0))); });
    register_builtin("log10", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::log10(arg_real(a, 0))); });
    register_builtin("logn", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::log(arg_real(a, 1)) / std::log(arg_real(a, 0)));
    });
    register_builtin("power", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::pow(arg_real(a, 0), arg_real(a, 1)));
    });

    register_builtin("sin", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::sin(arg_real(a, 0))); });
    register_builtin("cos", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::cos(arg_real(a, 0))); });
    register_builtin("tan", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::tan(arg_real(a, 0))); });
    register_builtin("arcsin", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::asin(std::clamp(arg_real(a, 0), -1.0, 1.0))); });
    register_builtin("arccos", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::acos(std::clamp(arg_real(a, 0), -1.0, 1.0))); });
    register_builtin("arctan", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::atan(arg_real(a, 0))); });
    register_builtin("arctan2", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::atan2(arg_real(a, 0), arg_real(a, 1))); });
    register_builtin("dsin", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::sin(deg_to_rad(arg_real(a, 0)))); });
    register_builtin("dcos", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::cos(deg_to_rad(arg_real(a, 0)))); });
    register_builtin("dtan", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(std::tan(deg_to_rad(arg_real(a, 0)))); });
    register_builtin("darcsin", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(rad_to_deg(std::asin(std::clamp(arg_real(a, 0), -1.0, 1.0)))); });
    register_builtin("darccos", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(rad_to_deg(std::acos(std::clamp(arg_real(a, 0), -1.0, 1.0)))); });
    register_builtin("darctan", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(rad_to_deg(std::atan(arg_real(a, 0)))); });
    register_builtin("darctan2", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(rad_to_deg(std::atan2(arg_real(a, 0), arg_real(a, 1)))); });
    register_builtin("degtorad", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(deg_to_rad(arg_real(a, 0))); });
    register_builtin("radtodeg", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(rad_to_deg(arg_real(a, 0))); });

    register_builtin("min", [](Runner&, i32, i32, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        const RValue* best = &a[0];
        for (usize i = 1; i < a.size(); ++i) {
            if (RValue::compare_for_ds(a[i], *best, kGmlEpsilon) < 0) best = &a[i];
        }
        return *best;
    });
    register_builtin("max", [](Runner&, i32, i32, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        const RValue* best = &a[0];
        for (usize i = 1; i < a.size(); ++i) {
            if (RValue::compare_for_ds(a[i], *best, kGmlEpsilon) > 0) best = &a[i];
        }
        return *best;
    });
    register_builtin("min3", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 x0 = arg_real(a, 0);
        const f64 x1 = arg_real(a, 1);
        const f64 x2 = arg_real(a, 2);
        return RValue(std::min({x0, x1, x2}));
    });
    register_builtin("max3", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 x0 = arg_real(a, 0);
        const f64 x1 = arg_real(a, 1);
        const f64 x2 = arg_real(a, 2);
        return RValue(std::max({x0, x1, x2}));
    });
    register_builtin("mean", [](Runner&, i32, i32, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        f64 s = 0.0;
        for (const auto& v : a) s += v.as_real();
        return RValue(s / static_cast<f64>(a.size()));
    });
    register_builtin("median", [](Runner&, i32, i32, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        std::vector<f64> vals;
        vals.reserve(a.size());
        for (const auto& v : a) vals.push_back(v.as_real());
        std::sort(vals.begin(), vals.end());
        return RValue(vals[vals.size() / 2]);
    });
    register_builtin("clamp", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::clamp(arg_real(a, 0), arg_real(a, 1), arg_real(a, 2)));
    });
    register_builtin("lerp", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 v1 = arg_real(a, 0);
        const f64 v2 = arg_real(a, 1);
        return RValue(v1 + (v2 - v1) * arg_real(a, 2));
    });

    register_builtin("point_distance", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::hypot(arg_real(a, 2) - arg_real(a, 0), arg_real(a, 3) - arg_real(a, 1)));
    });
    register_builtin("point_distance_3d", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 dx = arg_real(a, 3) - arg_real(a, 0);
        const f64 dy = arg_real(a, 4) - arg_real(a, 1);
        const f64 dz = arg_real(a, 5) - arg_real(a, 2);
        return RValue(std::sqrt(dx * dx + dy * dy + dz * dz));
    });
    register_builtin("point_direction", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(gm_point_direction(arg_real(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3)));
    });
    register_builtin("lengthdir_x", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_real(a, 0) * std::cos(deg_to_rad(arg_real(a, 1))));
    });
    register_builtin("lengthdir_y", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(-arg_real(a, 0) * std::sin(deg_to_rad(arg_real(a, 1))));
    });
    register_builtin("dot_product", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_real(a, 0) * arg_real(a, 2) + arg_real(a, 1) * arg_real(a, 3));
    });
    register_builtin("dot_product_3d", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_real(a, 0) * arg_real(a, 3) + arg_real(a, 1) * arg_real(a, 4) + arg_real(a, 2) * arg_real(a, 5));
    });
    register_builtin("dot_product_normalised", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 l1 = std::hypot(arg_real(a, 0), arg_real(a, 1));
        const f64 l2 = std::hypot(arg_real(a, 2), arg_real(a, 3));
        if (l1 == 0.0 || l2 == 0.0) return RValue(0.0);
        return RValue((arg_real(a, 0) * arg_real(a, 2) + arg_real(a, 1) * arg_real(a, 3)) / (l1 * l2));
    });
    register_builtin("dot_product_normalized", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 l1 = std::hypot(arg_real(a, 0), arg_real(a, 1));
        const f64 l2 = std::hypot(arg_real(a, 2), arg_real(a, 3));
        if (l1 == 0.0 || l2 == 0.0) return RValue(0.0);
        return RValue((arg_real(a, 0) * arg_real(a, 2) + arg_real(a, 1) * arg_real(a, 3)) / (l1 * l2));
    });

    // -----------------------------------------------------------------------
    // Strings & Formatting
    // -----------------------------------------------------------------------
    register_builtin("real", [](Runner&, i32, i32, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        if (a[0].is_real()) return a[0];
        return RValue(parse_gm_real(a[0].as_string()));
    });
    register_builtin("string", [](Runner&, i32, i32, std::span<const RValue> a) {
        if (a.empty()) return RValue(std::string{});
        if (a[0].is_string()) return a[0];
        return RValue(format_gm_real(a[0].as_real()));
    });
    register_builtin("string_format", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 val = arg_real(a, 0);
        const i32 tot = std::clamp(arg_i32(a, 1), 0, 128);
        const i32 dec = std::clamp(arg_i32(a, 2), 0, 32);
        const i32 width = tot + (dec > 0 ? dec + 1 : 0);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%*.*f", width, dec, val);
        return RValue(std::string(buf));
    });
    register_builtin("chr", [](Runner& r, i32, i32, std::span<const RValue> a) -> Result<RValue> {
        if (r.game().meta.encoding == EncodingMode::LegacyAnsi) {
            return RValue(std::string(1, static_cast<char>(arg_i32(a, 0) & 0xFF)));
        }
        const f64 rounded = delphi_round_f64(arg_real(a, 0));
        if (!(rounded >= -0x1p63 && rounded < 0x1p63)) return Error{"Invalid floating point operation."};
        const u8 byte = static_cast<u8>(static_cast<i64>(rounded));
        const u32 codepage = r.renderer().font_codepage(r.game());
        // Delphi's sprite-font conversion drops NUL on legacy ANSI codepages.
        if (r.renderer().using_sprite_font() && byte == 0 && codepage != 65001) return RValue(std::string{});
        return RValue(codepage_byte_to_utf8(byte, codepage));
    });
    register_builtin("ansi_char", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::string(1, static_cast<char>(arg_i32(a, 0) & 0xFF)));
    });
    register_builtin("ord", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        if (s.empty()) return RValue(0);
        if (r.game().meta.encoding == EncodingMode::Utf8 && (static_cast<u8>(s[0]) & 0x80u) != 0u) {
            usize off = 0;
            return RValue(static_cast<i32>(utf8_decode_next(s, off)));
        }
        return RValue(static_cast<i32>(static_cast<u8>(s[0])));
    });
    register_builtin("string_length", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        if (r.game().meta.encoding == EncodingMode::LegacyAnsi) return RValue(static_cast<f64>(s.size()));
        usize count = 0;
        for (usize offset = 0; offset < s.size(); offset += gm81_character_width(static_cast<u8>(s[offset]))) {
            ++count;
        }
        return RValue(static_cast<f64>(count));
    });
    register_builtin("string_byte_length", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<i32>(arg_str(a, 0).size()));
    });
    register_builtin("string_byte_at", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        const i32 idx = std::max(1, arg_i32(a, 1)) - 1;
        if (idx < 0 || static_cast<usize>(idx) >= s.size()) return RValue(0);
        return RValue(static_cast<i32>(static_cast<u8>(s[static_cast<usize>(idx)])));
    });
    register_builtin("string_pos", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view sub = arg_str(a, 0);
        const std::string_view str = arg_str(a, 1);
        if (sub.empty()) return RValue(0);
        const usize p = str.find(sub);
        return RValue(p == std::string_view::npos ? 0 : static_cast<i32>(p + 1));
    });
    register_builtin("string_copy", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        const i32 idx = std::max(1, arg_i32(a, 1)) - 1;
        const i32 cnt = std::max(0, arg_i32(a, 2));
        if (static_cast<usize>(idx) >= s.size() || cnt == 0) return RValue(std::string{});
        return RValue(std::string(s.substr(static_cast<usize>(idx), static_cast<usize>(cnt))));
    });
    register_builtin("string_char_at", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        if (s.empty()) return RValue(std::string{});
        const i32 idx = std::max(1, arg_i32(a, 1)) - 1;
        if (static_cast<usize>(idx) >= s.size()) return RValue(std::string{});
        return RValue(std::string(1, s[static_cast<usize>(idx)]));
    });
    register_builtin("string_delete", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::string s(arg_str(a, 0));
        const i32 idx = std::max(1, arg_i32(a, 1)) - 1;
        const i32 cnt = std::max(0, arg_i32(a, 2));
        if (static_cast<usize>(idx) < s.size() && cnt > 0) {
            s.erase(static_cast<usize>(idx), static_cast<usize>(cnt));
        }
        return RValue(std::move(s));
    });
    register_builtin("string_insert", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view sub = arg_str(a, 0);
        std::string s(arg_str(a, 1));
        const usize idx = static_cast<usize>(std::clamp(arg_i32(a, 2) - 1, 0, static_cast<i32>(s.size())));
        s.insert(idx, sub);
        return RValue(std::move(s));
    });
    register_builtin("string_replace", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::string s(arg_str(a, 0));
        const std::string_view sub = arg_str(a, 1);
        const std::string_view rep = arg_str(a, 2);
        if (!sub.empty()) {
            const usize p = s.find(sub);
            if (p != std::string::npos) s.replace(p, sub.size(), rep);
        }
        return RValue(std::move(s));
    });
    register_builtin("string_replace_all", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::string s(arg_str(a, 0));
        const std::string_view sub = arg_str(a, 1);
        const std::string_view rep = arg_str(a, 2);
        if (!sub.empty()) {
            usize pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                s.replace(pos, sub.size(), rep);
                pos += rep.size();
            }
        }
        return RValue(std::move(s));
    });
    register_builtin("string_count", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view sub = arg_str(a, 0);
        const std::string_view s = arg_str(a, 1);
        if (sub.empty()) return RValue(0);
        i32 cnt = 0;
        usize pos = 0;
        while ((pos = s.find(sub, pos)) != std::string_view::npos) {
            ++cnt;
            pos += sub.size();
        }
        return RValue(cnt);
    });
    register_builtin("string_lower", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(ascii_to_lower(arg_str(a, 0)));
    });
    register_builtin("string_upper", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::string s(arg_str(a, 0));
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return RValue(std::move(s));
    });
    register_builtin("string_repeat", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        const i32 n = std::max(0, arg_i32(a, 1));
        std::string out;
        out.reserve(s.size() * static_cast<usize>(n));
        for (i32 i = 0; i < n; ++i) out.append(s);
        return RValue(std::move(out));
    });
    register_builtin("string_letters", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::string out;
        for (char c : arg_str(a, 0)) {
            if (std::isalpha(static_cast<unsigned char>(c))) out.push_back(c);
        }
        return RValue(std::move(out));
    });
    register_builtin("string_digits", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::string out;
        for (char c : arg_str(a, 0)) {
            if (std::isdigit(static_cast<unsigned char>(c))) out.push_back(c);
        }
        return RValue(std::move(out));
    });
    register_builtin("string_lettersdigits", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::string out;
        for (char c : arg_str(a, 0)) {
            if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
        }
        return RValue(std::move(out));
    });

    // Clipboard
    register_builtin("clipboard_has_text", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(!PlatformWindow::clipboard_get_text().empty());
    });
    register_builtin("clipboard_get_text", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(PlatformWindow::clipboard_get_text());
    });
    register_builtin("clipboard_set_text", [](Runner&, i32, i32, std::span<const RValue> a) {
        PlatformWindow::clipboard_set_text(arg_str(a, 0));
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Date & Time
    // -----------------------------------------------------------------------
    register_builtin("date_current_datetime", [](Runner&, i32, i32, std::span<const RValue>) {
        const std::time_t now = std::time(nullptr);
        return RValue(static_cast<f64>(now) / 86400.0 + 25569.0);
    });
    register_builtin("date_current_date", [](Runner&, i32, i32, std::span<const RValue>) {
        const std::time_t now = std::time(nullptr);
        return RValue(std::floor(static_cast<f64>(now) / 86400.0 + 25569.0));
    });
    register_builtin("date_current_time", [](Runner&, i32, i32, std::span<const RValue>) {
        const std::time_t now = std::time(nullptr);
        const f64 d = static_cast<f64>(now) / 86400.0 + 25569.0;
        return RValue(d - std::floor(d));
    });
    register_builtin("date_create_datetime", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::tm t{};
        t.tm_year = arg_i32(a, 0) - 1900;
        t.tm_mon = arg_i32(a, 1) - 1;
        t.tm_mday = arg_i32(a, 2);
        t.tm_hour = arg_i32(a, 3);
        t.tm_min = arg_i32(a, 4);
        t.tm_sec = arg_i32(a, 5);
        return RValue(delphi_datetime_from_tm(t));
    });
    register_builtin("date_get_year", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(tm_from_delphi_datetime(arg_real(a, 0)).tm_year + 1900);
    });
    register_builtin("date_get_month", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(tm_from_delphi_datetime(arg_real(a, 0)).tm_mon + 1);
    });
    register_builtin("date_get_day", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(tm_from_delphi_datetime(arg_real(a, 0)).tm_mday);
    });
    register_builtin("date_get_hour", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(tm_from_delphi_datetime(arg_real(a, 0)).tm_hour);
    });
    register_builtin("date_get_minute", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(tm_from_delphi_datetime(arg_real(a, 0)).tm_min);
    });
    register_builtin("date_get_second", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(tm_from_delphi_datetime(arg_real(a, 0)).tm_sec);
    });
    register_builtin("date_get_weekday", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(tm_from_delphi_datetime(arg_real(a, 0)).tm_wday + 1);
    });
    register_builtin("date_second_span", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::fabs(arg_real(a, 1) - arg_real(a, 0)) * 86400.0);
    });
    register_builtin("date_create_date", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::tm t{};
        t.tm_year = arg_i32(a, 0) - 1900;
        t.tm_mon = arg_i32(a, 1) - 1;
        t.tm_mday = arg_i32(a, 2);
        return RValue(std::floor(delphi_datetime_from_tm(t)));
    });
    register_builtin("date_create_time", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 sec = arg_real(a, 0) * 3600.0 + arg_real(a, 1) * 60.0 + arg_real(a, 2);
        return RValue(sec / 86400.0);
    });
    register_builtin("date_valid_datetime", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(is_valid_date_ymd(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2)) &&
                      is_valid_time_hms(arg_i32(a, 3), arg_i32(a, 4), arg_i32(a, 5)));
    });
    register_builtin("date_valid_date", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(is_valid_date_ymd(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2)));
    });
    register_builtin("date_valid_time", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(is_valid_time_hms(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2)));
    });
    register_builtin("date_inc_year", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        t.tm_year += arg_i32(a, 1);
        const i32 dim = days_in_month_gregorian(t.tm_year + 1900, t.tm_mon + 1);
        if (t.tm_mday > dim) t.tm_mday = dim;
        return RValue(delphi_datetime_from_tm(t));
    });
    register_builtin("date_inc_month", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        const i32 total_m = t.tm_year * 12 + t.tm_mon + arg_i32(a, 1);
        i32 ny = total_m / 12;
        i32 nm = total_m % 12;
        if (nm < 0) { nm += 12; --ny; }
        t.tm_year = ny;
        t.tm_mon = nm;
        const i32 dim = days_in_month_gregorian(t.tm_year + 1900, t.tm_mon + 1);
        if (t.tm_mday > dim) t.tm_mday = dim;
        return RValue(delphi_datetime_from_tm(t));
    });
    register_builtin("date_inc_week", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_real(a, 0) + arg_real(a, 1) * 7.0);
    });
    register_builtin("date_inc_day", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_real(a, 0) + arg_real(a, 1));
    });
    register_builtin("date_inc_hour", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_real(a, 0) + arg_real(a, 1) / 24.0);
    });
    register_builtin("date_inc_minute", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_real(a, 0) + arg_real(a, 1) / 1440.0);
    });
    register_builtin("date_inc_second", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_real(a, 0) + arg_real(a, 1) / 86400.0);
    });
    register_builtin("date_get_week", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        return RValue(t.tm_yday / 7 + 1);
    });
    register_builtin("date_get_day_of_year", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        return RValue(t.tm_yday + 1);
    });
    register_builtin("date_get_hour_of_year", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        return RValue(t.tm_yday * 24 + t.tm_hour);
    });
    register_builtin("date_get_minute_of_year", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        return RValue((t.tm_yday * 24 + t.tm_hour) * 60 + t.tm_min);
    });
    register_builtin("date_get_second_of_year", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        return RValue(((t.tm_yday * 24 + t.tm_hour) * 60 + t.tm_min) * 60 + t.tm_sec);
    });
    register_builtin("date_year_span", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::fabs(arg_real(a, 1) - arg_real(a, 0)) / 365.25);
    });
    register_builtin("date_month_span", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::fabs(arg_real(a, 1) - arg_real(a, 0)) / 30.4375);
    });
    register_builtin("date_week_span", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::fabs(arg_real(a, 1) - arg_real(a, 0)) / 7.0);
    });
    register_builtin("date_day_span", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::fabs(arg_real(a, 1) - arg_real(a, 0)));
    });
    register_builtin("date_hour_span", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::fabs(arg_real(a, 1) - arg_real(a, 0)) * 24.0);
    });
    register_builtin("date_minute_span", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::fabs(arg_real(a, 1) - arg_real(a, 0)) * 1440.0);
    });
    register_builtin("date_compare_datetime", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 d = arg_real(a, 0) - arg_real(a, 1);
        return RValue(std::fabs(d) < 1e-9 ? 0 : (d < 0.0 ? -1 : 1));
    });
    register_builtin("date_compare_date", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 d = std::floor(arg_real(a, 0)) - std::floor(arg_real(a, 1));
        return RValue(d == 0.0 ? 0 : (d < 0.0 ? -1 : 1));
    });
    register_builtin("date_compare_time", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 t1 = arg_real(a, 0) - std::floor(arg_real(a, 0));
        const f64 t2 = arg_real(a, 1) - std::floor(arg_real(a, 1));
        const f64 d = t1 - t2;
        return RValue(std::fabs(d) < 1e-9 ? 0 : (d < 0.0 ? -1 : 1));
    });
    register_builtin("date_date_of", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(std::floor(arg_real(a, 0)));
    });
    register_builtin("date_time_of", [](Runner&, i32, i32, std::span<const RValue> a) {
        const f64 v = arg_real(a, 0);
        return RValue(v - std::floor(v));
    });
    register_builtin("date_datetime_string", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        return RValue(std::string(buf));
    });
    register_builtin("date_date_string", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        return RValue(std::string(buf));
    });
    register_builtin("date_time_string", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        return RValue(std::string(buf));
    });
    register_builtin("date_days_in_month", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        return RValue(days_in_month_gregorian(t.tm_year + 1900, t.tm_mon + 1));
    });
    register_builtin("date_days_in_year", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        return RValue(is_leap_year_gregorian(t.tm_year + 1900) ? 366 : 365);
    });
    register_builtin("date_leap_year", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::tm t = tm_from_delphi_datetime(arg_real(a, 0));
        return RValue(is_leap_year_gregorian(t.tm_year + 1900));
    });
    register_builtin("date_is_today", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::time_t now = std::time(nullptr);
        const f64 today = std::floor(static_cast<f64>(now) / 86400.0 + 25569.0);
        return RValue(std::floor(arg_real(a, 0)) == today);
    });

    // -----------------------------------------------------------------------
    // Hashing (GM 8.1+ built-ins)
    // -----------------------------------------------------------------------
    register_builtin("md5_string_utf8", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        const auto d = md5_bytes(std::span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size()));
        return RValue(hex_encode(d, false));
    });
    register_builtin("md5_string_unicode", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::u16string u16 = utf8_to_utf16(arg_str(a, 0));
        const auto d = md5_bytes(std::span<const u8>(reinterpret_cast<const u8*>(u16.data()), u16.size() * 2));
        return RValue(hex_encode(d, false));
    });
    register_builtin("md5_file", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(std::string{});
        return RValue(hex_encode(md5_bytes(bytes.value()), false));
    });
    register_builtin("sha1_string_utf8", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        const auto d = sha1_bytes(std::span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size()));
        return RValue(hex_encode(d, false));
    });
    register_builtin("sha1_file", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(std::string{});
        return RValue(hex_encode(sha1_bytes(bytes.value()), false));
    });

    // -----------------------------------------------------------------------
    // Files, Filenames, Directories, INI, Registry & CLI Params
    // -----------------------------------------------------------------------
    register_builtin("file_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().exists(arg_str(a, 0)));
    });
    register_builtin("file_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().delete_file(arg_str(a, 0)).ok());
    });
    register_builtin("file_rename", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(false);
        if (!r.vfs().write_all_bytes(arg_str(a, 1), bytes.value())) return RValue(false);
        (void)r.vfs().delete_file(arg_str(a, 0));
        return RValue(true);
    });
    register_builtin("file_copy", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(false);
        return RValue(r.vfs().write_all_bytes(arg_str(a, 1), bytes.value()).ok());
    });
    register_builtin("directory_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().directory_exists(arg_str(a, 0)));
    });
    register_builtin("directory_create", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().create_directory(arg_str(a, 0)).ok());
    });
    register_builtin("file_find_first", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.file_find_first(arg_str(a, 0), static_cast<u32>(arg_i32(a, 1))));
    });
    register_builtin("file_find_next", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.file_find_next());
    });
    register_builtin("file_find_close", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.file_find_close();
        return RValue(0.0);
    });

    register_builtin("filename_name", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string norm = Vfs::normalize_separators(arg_str(a, 0));
        const usize pos = norm.find_last_of('/');
        return RValue(pos == std::string::npos ? norm : norm.substr(pos + 1));
    });
    register_builtin("filename_path", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        const usize pos = s.find_last_of("/\\");
        return RValue(pos == std::string_view::npos ? std::string{} : std::string(s.substr(0, pos + 1)));
    });
    register_builtin("filename_dir", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        const usize pos = s.find_last_of("/\\");
        return RValue(pos == std::string_view::npos ? std::string{} : std::string(s.substr(0, pos)));
    });
    register_builtin("filename_ext", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view s = arg_str(a, 0);
        const usize slash = s.find_last_of("/\\");
        const usize dot = s.find_last_of('.');
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
            return RValue(std::string{});
        }
        return RValue(std::string(s.substr(dot)));
    });
    register_builtin("filename_change_ext", [](Runner&, i32, i32, std::span<const RValue> a) {
        std::string s(arg_str(a, 0));
        const std::string_view new_ext = arg_str(a, 1);
        const usize slash = s.find_last_of("/\\");
        const usize dot = s.find_last_of('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            s.erase(dot);
        }
        s.append(new_ext);
        return RValue(std::move(s));
    });

    // Text files
    register_builtin("file_text_open_read", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.text_file_open(arg_str(a, 0), 0));
    });
    register_builtin("file_text_open_write", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.text_file_open(arg_str(a, 0), 1));
    });
    register_builtin("file_text_open_append", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.text_file_open(arg_str(a, 0), 2));
    });
    register_builtin("file_text_close", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.text_file_close(arg_i32(a, 0));
        return RValue(0.0);
    });
    register_builtin("file_text_write_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* tf = r.get_text_file(arg_i32(a, 0))) tf->write_buf.append(arg_str(a, 1));
        return RValue(0.0);
    });
    register_builtin("file_text_write_real", [](Runner& r, i32, i32, std::span<const RValue> a) -> Result<RValue> {
        auto* tf = r.get_text_file(arg_i32(a, 0));
        if (!tf || tf->mode == 0) return RValue(0.0);

        return append_text_real(tf->write_buf, arg_real(a, 1), r.version_mode() != VersionMode::GM80);
    });
    register_builtin("file_text_writeln", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* tf = r.get_text_file(arg_i32(a, 0))) tf->write_buf.append("\r\n");
        return RValue(0.0);
    });
    register_builtin("file_text_read_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto* tf = r.get_text_file(arg_i32(a, 0));
        if (!tf || tf->line_idx >= tf->lines.size()) return RValue(std::string{});
        const std::string& line = tf->lines[tf->line_idx];
        if (tf->col_idx >= line.size()) return RValue(std::string{});
        std::string out = line.substr(tf->col_idx);
        tf->col_idx = line.size();
        return RValue(std::move(out));
    });
    register_builtin("file_text_read_real", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto* tf = r.get_text_file(arg_i32(a, 0));
        if (!tf) return RValue(0.0);
        while (tf->line_idx < tf->lines.size()) {
            const std::string& line = tf->lines[tf->line_idx];
            while (tf->col_idx < line.size() && std::isspace(static_cast<unsigned char>(line[tf->col_idx]))) {
                ++tf->col_idx;
            }
            if (tf->col_idx < line.size()) {
                const char* start = line.c_str() + tf->col_idx;
                char* end = nullptr;
                const f64 v = std::strtod(start, &end);
                tf->col_idx = static_cast<usize>(end - line.c_str());
                return RValue(std::isfinite(v) ? v : 0.0);
            }
            ++tf->line_idx;
            tf->col_idx = 0;
        }
        return RValue(0.0);
    });
    register_builtin("file_text_readln", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto* tf = r.get_text_file(arg_i32(a, 0));
        if (!tf || tf->line_idx >= tf->lines.size()) return RValue(std::string{});
        std::string rem = tf->col_idx < tf->lines[tf->line_idx].size()
                              ? tf->lines[tf->line_idx].substr(tf->col_idx)
                              : std::string{};
        ++tf->line_idx;
        tf->col_idx = 0;
        return RValue(std::move(rem));
    });
    register_builtin("file_text_eof", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto* tf = r.get_text_file(arg_i32(a, 0));
        return RValue(!tf || tf->line_idx >= tf->lines.size());
    });
    register_builtin("file_text_eoln", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto* tf = r.get_text_file(arg_i32(a, 0));
        return RValue(!tf || tf->line_idx >= tf->lines.size() || tf->col_idx >= tf->lines[tf->line_idx].size());
    });

    // Binary files
    register_builtin("file_bin_open", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.bin_file_open(arg_str(a, 0), arg_i32(a, 1)));
    });
    register_builtin("file_bin_rewrite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* bf = r.get_bin_file(arg_i32(a, 0)); bf && bf->mode != 0) {
            bf->data.clear();
            bf->pos = 0;
            bf->dirty = true;
        }
        return RValue(0.0);
    });
    register_builtin("file_bin_close", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.bin_file_close(arg_i32(a, 0));
        return RValue(0.0);
    });
    register_builtin("file_bin_size", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto* bf = r.get_bin_file(arg_i32(a, 0));
        return RValue(bf ? static_cast<i64>(bf->data.size()) : 0);
    });
    register_builtin("file_bin_position", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto* bf = r.get_bin_file(arg_i32(a, 0));
        return RValue(bf ? static_cast<i64>(bf->pos) : 0);
    });
    register_builtin("file_bin_seek", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* bf = r.get_bin_file(arg_i32(a, 0))) {
            bf->pos = static_cast<usize>(std::max(0, arg_i32(a, 1)));
        }
        return RValue(0.0);
    });
    register_builtin("file_bin_write_byte", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* bf = r.get_bin_file(arg_i32(a, 0)); bf && bf->mode != 0) {
            if (bf->pos >= bf->data.size()) bf->data.resize(bf->pos + 1, 0);
            bf->data[bf->pos++] = static_cast<u8>(arg_i32(a, 1) & 0xFF);
            bf->dirty = true;
        }
        return RValue(0.0);
    });
    register_builtin("file_bin_read_byte", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto* bf = r.get_bin_file(arg_i32(a, 0));
        if (!bf || bf->mode == 1 || bf->pos >= bf->data.size()) return RValue(0);
        return RValue(static_cast<i32>(bf->data[bf->pos++]));
    });

    // INI files
    register_builtin("ini_open", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ini_open(arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("ini_open_from_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ini_open_from_string(arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("ini_close", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.ini_close());
    });
    register_builtin("ini_read_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ini_read_string(arg_str(a, 0), arg_str(a, 1), arg_str(a, 2)));
    });
    register_builtin("ini_read_real", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ini_read_real(arg_str(a, 0), arg_str(a, 1), arg_real(a, 2)));
    });
    register_builtin("ini_write_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ini_write_string(arg_str(a, 0), arg_str(a, 1), arg_str(a, 2));
        return RValue(0.0);
    });
    register_builtin("ini_write_real", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ini_write_real(arg_str(a, 0), arg_str(a, 1), arg_real(a, 2));
        return RValue(0.0);
    });
    register_builtin("ini_key_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ini_key_exists(arg_str(a, 0), arg_str(a, 1)));
    });
    register_builtin("ini_section_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ini_section_exists(arg_str(a, 0)));
    });
    register_builtin("ini_key_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ini_key_delete(arg_str(a, 0), arg_str(a, 1));
        return RValue(0.0);
    });
    register_builtin("ini_section_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ini_section_delete(arg_str(a, 0));
        return RValue(0.0);
    });

    // Registry & CLI Parameters
    register_builtin("registry_set_root", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.vfs().registry_set_root(arg_i32(a, 0));
        return RValue(0.0);
    });
    register_builtin("registry_write_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.vfs().registry_write_string(std::nullopt, arg_str(a, 0), arg_str(a, 1));
        return RValue(0.0);
    });
    register_builtin("registry_write_real", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.vfs().registry_write_real(std::nullopt, arg_str(a, 0), arg_real(a, 1));
        return RValue(0.0);
    });
    register_builtin("registry_read_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().registry_read_string(std::nullopt, arg_str(a, 0)));
    });
    register_builtin("registry_read_real", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().registry_read_real(std::nullopt, arg_str(a, 0)));
    });
    register_builtin("registry_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().registry_exists(std::nullopt, arg_str(a, 0)));
    });
    register_builtin("registry_write_string_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.vfs().registry_write_string(arg_str(a, 0), arg_str(a, 1), arg_str(a, 2));
        return RValue(0.0);
    });
    register_builtin("registry_write_real_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.vfs().registry_write_real(arg_str(a, 0), arg_str(a, 1), arg_real(a, 2));
        return RValue(0.0);
    });
    register_builtin("registry_read_string_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().registry_read_string(arg_str(a, 0), arg_str(a, 1)));
    });
    register_builtin("registry_read_real_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().registry_read_real(arg_str(a, 0), arg_str(a, 1)));
    });
    register_builtin("registry_exists_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.vfs().registry_exists(arg_str(a, 0), arg_str(a, 1)));
    });
    register_builtin("parameter_count", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(static_cast<i32>(r.cmd_args_.size() > 0 ? r.cmd_args_.size() - 1 : 0));
    });
    register_builtin("parameter_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 i = arg_i32(a, 0);
        if (i < 0 || static_cast<usize>(i) >= r.cmd_args_.size()) return RValue(std::string{});
        return RValue(r.cmd_args_[static_cast<usize>(i)]);
    });
    register_builtin("environment_get_variable", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string name(arg_str(a, 0));
#if defined(_WIN32)
        char* buf = nullptr;
        size_t len = 0;
        std::string out;
        if (_dupenv_s(&buf, &len, name.c_str()) == 0 && buf != nullptr) {
            out.assign(buf);
            std::free(buf);
        }
        return RValue(std::move(out));
#else
        const char* v = std::getenv(name.c_str());
        return RValue(v ? std::string(v) : std::string{});
#endif
    });

    // -----------------------------------------------------------------------
    // Data Structures (ds_*)
    // -----------------------------------------------------------------------
    register_builtin("ds_set_precision", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().set_precision(arg_real(a, 0));
        return RValue(0.0);
    });
    register_builtin("ds_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().exists(arg_i32(a, 0), arg_i32(a, 1)));
    });

    // Stack
    register_builtin("ds_stack_create", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.ds().stack_create()); });
    register_builtin("ds_stack_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().stack_destroy(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_stack_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().stack_clear(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_stack_copy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().stack_copy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("ds_stack_size", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().stack_size(arg_i32(a, 0))); });
    register_builtin("ds_stack_empty", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().stack_empty(arg_i32(a, 0))); });
    register_builtin("ds_stack_push", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 2) r.ds().stack_push(arg_i32(a, 0), a[1]);
        return RValue(0.0);
    });
    register_builtin("ds_stack_pop", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().stack_pop(arg_i32(a, 0)); });
    register_builtin("ds_stack_top", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().stack_top(arg_i32(a, 0)); });
    register_builtin("ds_stack_write", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().stack_write(arg_i32(a, 0))); });
    register_builtin("ds_stack_read", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().stack_read(arg_i32(a, 0), arg_str(a, 1)); return RValue(0.0); });

    // Queue
    register_builtin("ds_queue_create", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.ds().queue_create()); });
    register_builtin("ds_queue_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().queue_destroy(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_queue_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().queue_clear(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_queue_copy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().queue_copy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("ds_queue_size", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().queue_size(arg_i32(a, 0))); });
    register_builtin("ds_queue_empty", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().queue_empty(arg_i32(a, 0))); });
    register_builtin("ds_queue_enqueue", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 2) r.ds().queue_enqueue(arg_i32(a, 0), a[1]);
        return RValue(0.0);
    });
    register_builtin("ds_queue_dequeue", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().queue_dequeue(arg_i32(a, 0)); });
    register_builtin("ds_queue_head", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().queue_head(arg_i32(a, 0)); });
    register_builtin("ds_queue_tail", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().queue_tail(arg_i32(a, 0)); });
    register_builtin("ds_queue_write", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().queue_write(arg_i32(a, 0))); });
    register_builtin("ds_queue_read", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().queue_read(arg_i32(a, 0), arg_str(a, 1)); return RValue(0.0); });

    // List
    register_builtin("ds_list_create", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.ds().list_create()); });
    register_builtin("ds_list_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().list_destroy(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_list_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().list_clear(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_list_copy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().list_copy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("ds_list_size", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().list_size(arg_i32(a, 0))); });
    register_builtin("ds_list_empty", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().list_empty(arg_i32(a, 0))); });
    register_builtin("ds_list_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 2) r.ds().list_add(arg_i32(a, 0), a[1]);
        return RValue(0.0);
    });
    register_builtin("ds_list_insert", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 3) r.ds().list_insert(arg_i32(a, 0), arg_i32(a, 1), a[2]);
        return RValue(0.0);
    });
    register_builtin("ds_list_replace", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 3) r.ds().list_replace(arg_i32(a, 0), arg_i32(a, 1), a[2]);
        return RValue(0.0);
    });
    register_builtin("ds_list_delete", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().list_delete(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("ds_list_find_index", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 2 ? RValue(r.ds().list_find_index(arg_i32(a, 0), a[1])) : RValue(-1);
    });
    register_builtin("ds_list_find_value", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return r.ds().list_find_value(arg_i32(a, 0), arg_i32(a, 1));
    });
    register_builtin("ds_list_sort", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().list_sort(arg_i32(a, 0), arg_bool(a, 1, true));
        return RValue(0.0);
    });
    register_builtin("ds_list_shuffle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().list_shuffle(arg_i32(a, 0), r.rng_);
        return RValue(0.0);
    });
    register_builtin("ds_list_write", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().list_write(arg_i32(a, 0))); });
    register_builtin("ds_list_read", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().list_read(arg_i32(a, 0), arg_str(a, 1)); return RValue(0.0); });

    // Map
    register_builtin("ds_map_create", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.ds().map_create()); });
    register_builtin("ds_map_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().map_destroy(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_map_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().map_clear(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_map_copy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().map_copy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("ds_map_size", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().map_size(arg_i32(a, 0))); });
    register_builtin("ds_map_empty", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().map_empty(arg_i32(a, 0))); });
    register_builtin("ds_map_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 3) r.ds().map_add(arg_i32(a, 0), a[1], a[2]);
        return RValue(0.0);
    });
    register_builtin("ds_map_replace", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 3) r.ds().map_replace(arg_i32(a, 0), a[1], a[2]);
        return RValue(0.0);
    });
    register_builtin("ds_map_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 2) r.ds().map_delete(arg_i32(a, 0), a[1]);
        return RValue(0.0);
    });
    register_builtin("ds_map_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 2 ? RValue(r.ds().map_exists(arg_i32(a, 0), a[1])) : RValue(false);
    });
    register_builtin("ds_map_find_value", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 2 ? r.ds().map_find_value(arg_i32(a, 0), a[1]) : RValue(0.0);
    });
    register_builtin("ds_map_find_first", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().map_find_first(arg_i32(a, 0)); });
    register_builtin("ds_map_find_last", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().map_find_last(arg_i32(a, 0)); });
    register_builtin("ds_map_find_next", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 2 ? r.ds().map_find_next(arg_i32(a, 0), a[1]) : RValue(0.0);
    });
    register_builtin("ds_map_find_previous", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 2 ? r.ds().map_find_previous(arg_i32(a, 0), a[1]) : RValue(0.0);
    });
    register_builtin("ds_map_write", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().map_write(arg_i32(a, 0))); });
    register_builtin("ds_map_read", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().map_read(arg_i32(a, 0), arg_str(a, 1)); return RValue(0.0); });

    // Priority
    register_builtin("ds_priority_create", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.ds().priority_create()); });
    register_builtin("ds_priority_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().priority_destroy(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_priority_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().priority_clear(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_priority_copy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().priority_copy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("ds_priority_size", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().priority_size(arg_i32(a, 0))); });
    register_builtin("ds_priority_empty", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().priority_empty(arg_i32(a, 0))); });
    register_builtin("ds_priority_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 3) r.ds().priority_add(arg_i32(a, 0), a[1], a[2]);
        return RValue(0.0);
    });
    register_builtin("ds_priority_change_priority", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 3) r.ds().priority_change_priority(arg_i32(a, 0), a[1], a[2]);
        return RValue(0.0);
    });
    register_builtin("ds_priority_find_priority", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 2 ? r.ds().priority_find_priority(arg_i32(a, 0), a[1]) : RValue(0.0);
    });
    register_builtin("ds_priority_delete_value", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 2) r.ds().priority_delete_value(arg_i32(a, 0), a[1]);
        return RValue(0.0);
    });
    register_builtin("ds_priority_delete_min", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().priority_delete_min(arg_i32(a, 0)); });
    register_builtin("ds_priority_find_min", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().priority_find_min(arg_i32(a, 0)); });
    register_builtin("ds_priority_delete_max", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().priority_delete_max(arg_i32(a, 0)); });
    register_builtin("ds_priority_find_max", [](Runner& r, i32, i32, std::span<const RValue> a) { return r.ds().priority_find_max(arg_i32(a, 0)); });
    register_builtin("ds_priority_write", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().priority_write(arg_i32(a, 0))); });
    register_builtin("ds_priority_read", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().priority_read(arg_i32(a, 0), arg_str(a, 1)); return RValue(0.0); });

    // Grid
    register_builtin("ds_grid_create", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_create(arg_i32(a, 0), arg_i32(a, 1)));
    });
    register_builtin("ds_grid_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().grid_destroy(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("ds_grid_copy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().grid_copy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("ds_grid_resize", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().grid_resize(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2));
        return RValue(0.0);
    });
    register_builtin("ds_grid_width", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().grid_width(arg_i32(a, 0))); });
    register_builtin("ds_grid_height", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().grid_height(arg_i32(a, 0))); });
    register_builtin("ds_grid_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 2) r.ds().grid_clear(arg_i32(a, 0), a[1]);
        return RValue(0.0);
    });
    register_builtin("ds_grid_set", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 4) r.ds().grid_set(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), a[3]);
        return RValue(0.0);
    });
    register_builtin("ds_grid_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 4) r.ds().grid_add(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), a[3]);
        return RValue(0.0);
    });
    register_builtin("ds_grid_multiply", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().grid_multiply(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_real(a, 3));
        return RValue(0.0);
    });
    register_builtin("ds_grid_get", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return r.ds().grid_get(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2));
    });
    register_builtin("ds_grid_set_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 6) r.ds().grid_set_region(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), a[5]);
        return RValue(0.0);
    });
    register_builtin("ds_grid_get_sum", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_get_sum(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4)));
    });
    register_builtin("ds_grid_get_min", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_get_min(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4)));
    });
    register_builtin("ds_grid_get_max", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_get_max(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4)));
    });
    register_builtin("ds_grid_get_mean", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_get_mean(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4)));
    });
    register_builtin("ds_grid_value_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 6 ? RValue(r.ds().grid_value_exists(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), a[5])) : RValue(false);
    });
    register_builtin("ds_grid_add_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 6) r.ds().grid_add_region(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), a[5]);
        return RValue(0.0);
    });
    register_builtin("ds_grid_multiply_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().grid_multiply_region(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), arg_real(a, 5));
        return RValue(0.0);
    });
    register_builtin("ds_grid_set_disk", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 5) r.ds().grid_set_disk(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), a[4]);
        return RValue(0.0);
    });
    register_builtin("ds_grid_add_disk", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 5) r.ds().grid_add_disk(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), a[4]);
        return RValue(0.0);
    });
    register_builtin("ds_grid_multiply_disk", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().grid_multiply_disk(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4));
        return RValue(0.0);
    });
    register_builtin("ds_grid_set_grid_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().grid_set_grid_region(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), arg_i32(a, 5), arg_i32(a, 6), arg_i32(a, 7));
        return RValue(0.0);
    });
    register_builtin("ds_grid_add_grid_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().grid_add_grid_region(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), arg_i32(a, 5), arg_i32(a, 6), arg_i32(a, 7));
        return RValue(0.0);
    });
    register_builtin("ds_grid_multiply_grid_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().grid_multiply_grid_region(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), arg_i32(a, 5), arg_i32(a, 6), arg_i32(a, 7));
        return RValue(0.0);
    });
    register_builtin("ds_grid_get_disk_sum", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_get_disk_sum(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3)));
    });
    register_builtin("ds_grid_get_disk_min", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_get_disk_min(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3)));
    });
    register_builtin("ds_grid_get_disk_max", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_get_disk_max(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3)));
    });
    register_builtin("ds_grid_get_disk_mean", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.ds().grid_get_disk_mean(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3)));
    });
    register_builtin("ds_grid_value_x", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 6 ? RValue(r.ds().grid_value_x(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), a[5])) : RValue(-1);
    });
    register_builtin("ds_grid_value_y", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 6 ? RValue(r.ds().grid_value_y(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), a[5])) : RValue(-1);
    });
    register_builtin("ds_grid_value_disk_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 5 ? RValue(r.ds().grid_value_disk_exists(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), a[4])) : RValue(false);
    });
    register_builtin("ds_grid_value_disk_x", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 5 ? RValue(r.ds().grid_value_disk_x(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), a[4])) : RValue(-1);
    });
    register_builtin("ds_grid_value_disk_y", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return a.size() >= 5 ? RValue(r.ds().grid_value_disk_y(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), a[4])) : RValue(-1);
    });
    register_builtin("ds_grid_shuffle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.ds().grid_shuffle(arg_i32(a, 0), r.rng_);
        return RValue(0.0);
    });
    register_builtin("ds_grid_write", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.ds().grid_write(arg_i32(a, 0))); });
    register_builtin("ds_grid_read", [](Runner& r, i32, i32, std::span<const RValue> a) { r.ds().grid_read(arg_i32(a, 0), arg_str(a, 1)); return RValue(0.0); });

    // -----------------------------------------------------------------------
    // Scripts, Dynamic Execution & Variable Reflection
    // -----------------------------------------------------------------------
    register_builtin("script_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().scripts.size() &&
                      r.game().scripts[static_cast<usize>(id)].exists);
    });
    auto fn_script_name = [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0);
        if (id >= 0 && static_cast<usize>(id) < r.game().scripts.size() && r.game().scripts[static_cast<usize>(id)].exists) {
            return RValue(r.game().scripts[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    };
    register_builtin("script_get_name", fn_script_name);
    register_builtin("script_name", fn_script_name);
    register_builtin("script_get_text", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0);
        if (id >= 0 && static_cast<usize>(id) < r.game().scripts.size() && r.game().scripts[static_cast<usize>(id)].exists) {
            return RValue(r.game().scripts[static_cast<usize>(id)].source);
        }
        return RValue(std::string{});
    });
    register_builtin("script_execute", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        const i32 sid = arg_i32(a, 0);
        if (sid < 0) return RValue(0.0);
        auto res = r.vm().execute_script(static_cast<u32>(sid), self_id, other_id, a.subspan(1));
        if (!res) {
            r.error_occurred_ = true;
            r.error_last_ = res.error_message();
            GM_LOG_WARN("runtime", "script_execute(%d) error: %s", sid, r.error_last_.c_str());
            return RValue(0.0);
        }
        return res.value();
    });
    register_builtin("execute_string", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        auto res = r.vm().execute_string(arg_str(a, 0), self_id, other_id, a.subspan(1));
        if (!res) {
            r.error_occurred_ = true;
            r.error_last_ = res.error_message();
            GM_LOG_WARN("runtime", "execute_string error: %s", r.error_last_.c_str());
            return RValue(0.0);
        }
        return res.value();
    });
    register_builtin("execute_file", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(0.0);
        std::string_view code(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        auto res = r.vm().execute_string(code, self_id, other_id, a.subspan(1));
        return res ? res.value() : RValue(0.0);
    });

    register_builtin("variable_global_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto id = r.program().symbols.find_user_var(arg_str(a, 0));
        return RValue(id.has_value() && r.vm().globals().exists(*id));
    });
    register_builtin("variable_global_get", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto id = r.program().symbols.find_user_var(arg_str(a, 0));
        if (!id) return RValue(0.0);
        const auto* v = r.vm().globals().get_scalar(*id);
        return v ? *v : RValue(0.0);
    });
    register_builtin("variable_global_set", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 2) {
            const u32 id = r.program().symbols.intern_user_var(arg_str(a, 0));
            r.vm().globals().set_scalar(id, a[1]);
        }
        return RValue(0.0);
    });
    register_builtin("variable_global_array_get", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto id = r.program().symbols.find_user_var(arg_str(a, 0));
        if (!id) return RValue(0.0);
        auto res = r.vm().globals().get_array(*id, arg_i32(a, 1), 0, 1);
        return (res && res.value()) ? *res.value() : RValue(0.0);
    });
    register_builtin("variable_global_array_set", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 3) {
            const u32 id = r.program().symbols.intern_user_var(arg_str(a, 0));
            (void)r.vm().globals().set_array(id, arg_i32(a, 1), 0, 1, a[2]);
        }
        return RValue(0.0);
    });
    register_builtin("variable_global_array2_get", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto id = r.program().symbols.find_user_var(arg_str(a, 0));
        if (!id) return RValue(0.0);
        auto res = r.vm().globals().get_array(*id, arg_i32(a, 1), arg_i32(a, 2), 2);
        return (res && res.value()) ? *res.value() : RValue(0.0);
    });
    register_builtin("variable_global_array2_set", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 4) {
            const u32 id = r.program().symbols.intern_user_var(arg_str(a, 0));
            (void)r.vm().globals().set_array(id, arg_i32(a, 1), arg_i32(a, 2), 2, a[3]);
        }
        return RValue(0.0);
    });
    register_builtin("variable_local_exists", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* vars = r.get_instance_vars(self_id);
        const auto id = r.program().symbols.find_user_var(arg_str(a, 0));
        return RValue(vars && id && vars->exists(*id));
    });
    register_builtin("variable_local_get", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* vars = r.get_instance_vars(self_id);
        const auto id = r.program().symbols.find_user_var(arg_str(a, 0));
        if (!vars || !id) return RValue(0.0);
        const auto* v = vars->get_scalar(*id);
        return v ? *v : RValue(0.0);
    });
    register_builtin("variable_local_set", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* vars = r.get_instance_vars(self_id); vars && a.size() >= 2) {
            const u32 id = r.program().symbols.intern_user_var(arg_str(a, 0));
            vars->set_scalar(id, a[1]);
        }
        return RValue(0.0);
    });
    register_builtin("variable_local_array_get", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* vars = r.get_instance_vars(self_id);
        const auto id = r.program().symbols.find_user_var(arg_str(a, 0));
        if (!vars || !id) return RValue(0.0);
        auto res = vars->get_array(*id, arg_i32(a, 1), 0, 1);
        return (res && res.value()) ? *res.value() : RValue(0.0);
    });
    register_builtin("variable_local_array_set", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* vars = r.get_instance_vars(self_id); vars && a.size() >= 3) {
            const u32 id = r.program().symbols.intern_user_var(arg_str(a, 0));
            (void)vars->set_array(id, arg_i32(a, 1), 0, 1, a[2]);
        }
        return RValue(0.0);
    });
    register_builtin("variable_local_array2_get", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* vars = r.get_instance_vars(self_id);
        const auto id = r.program().symbols.find_user_var(arg_str(a, 0));
        if (!vars || !id) return RValue(0.0);
        auto res = vars->get_array(*id, arg_i32(a, 1), arg_i32(a, 2), 2);
        return (res && res.value()) ? *res.value() : RValue(0.0);
    });
    register_builtin("variable_local_array2_set", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* vars = r.get_instance_vars(self_id); vars && a.size() >= 4) {
            const u32 id = r.program().symbols.intern_user_var(arg_str(a, 0));
            (void)vars->set_array(id, arg_i32(a, 1), arg_i32(a, 2), 2, a[3]);
        }
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Dialogs, System, Game Control & Highscores
    // -----------------------------------------------------------------------
    register_builtin("show_debug_message", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string s = a.empty() ? std::string{} : (a[0].is_string() ? a[0].as_string() : format_gm_real(a[0].as_real()));
        GM_LOG_INFO("gml", "%s", s.c_str());
        return RValue(0.0);
    });
    register_builtin("show_message", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string s = a.empty() ? std::string{} : (a[0].is_string() ? a[0].as_string() : format_gm_real(a[0].as_real()));
        r.window().show_message(r.room_caption_.empty() ? "GameMaker" : r.room_caption_, s);
        return RValue(0.0);
    });
    register_builtin("show_question", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string s = a.empty() ? std::string{} : (a[0].is_string() ? a[0].as_string() : format_gm_real(a[0].as_real()));
        return RValue(r.window().show_question(r.room_caption_.empty() ? "GameMaker" : r.room_caption_, s));
    });
    register_builtin("show_error", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string msg(arg_str(a, 0));
        const bool abort_game = arg_bool(a, 1, false);
        r.error_occurred_ = true;
        r.error_last_ = msg;
        GM_LOG_ERROR("gml", "show_error: %s", msg.c_str());
        if (r.game().options.display_errors) {
            r.window().show_message("Error", msg);
        }
        if (abort_game) r.request_game_end();
        return RValue(0.0);
    });
    register_builtin("show_message_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string s = a.empty() ? std::string{} : (a[0].is_string() ? a[0].as_string() : format_gm_real(a[0].as_real()));
        r.window().show_message(r.room_caption_.empty() ? "GameMaker" : r.room_caption_, s);
        return RValue(1);
    });
    register_builtin("show_info", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("load_info", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_background", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_alpha", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_button", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_text_font", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_button_font", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_input_font", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_mouse_color", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_input_color", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_caption", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_position", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("message_size", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("get_string", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.window().prompt_string(arg_str(a, 0), arg_str(a, 1)));
    });
    register_builtin("get_integer", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 def_val = arg_real(a, 1);
        const std::string def_str = std::to_string(static_cast<i64>(delphi_round(def_val)));
        const std::string input = r.window().prompt_string(arg_str(a, 0), def_str);
        if (input == def_str) return RValue(def_val);
        char* end = nullptr;
        const f64 parsed = std::strtod(input.c_str(), &end);
        return RValue((end && end != input.c_str()) ? parsed : def_val);
    });
    register_builtin("sleep", [](Runner& r, i32, i32, std::span<const RValue> a) -> Result<RValue> {
        const f64 ms = std::max(0.0, arg_real(a, 0));
        if (!r.sleep_ms(ms)) {
            return Error{"Window close requested during sleep"};
        }
        return RValue(0.0);
    });
    register_builtin("set_program_priority", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("window_handle", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(static_cast<f64>(r.window().native_window_handle()));
    });

    // -----------------------------------------------------------------------
    // External DLL Interception (external_define*, external_call*, external_free, get_function_address)
    // -----------------------------------------------------------------------
    auto fn_ext_define = [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string_view dll = arg_str(a, 0);
        const std::string_view fn = arg_str(a, 1);
        const i32 call_type = arg_i32(a, 2, 0);
        const ExternalValueType ret_type = (arg_i32(a, 3, 0) == 1) ? ExternalValueType::String : ExternalValueType::Real;
        const i32 nargs = std::clamp(arg_i32(a, 4, 0), 0, 16);
        std::vector<ExternalValueType> arg_types;
        arg_types.reserve(static_cast<usize>(nargs));
        for (i32 i = 0; i < nargs; ++i) {
            arg_types.push_back((arg_i32(a, static_cast<usize>(5 + i), 0) == 1) ? ExternalValueType::String : ExternalValueType::Real);
        }
        return RValue(r.dll_registry().define_external(dll, fn, call_type, ret_type, std::move(arg_types)));
    };
    auto fn_ext_define_fixed = [](i32 fixed_nargs) {
        return [fixed_nargs](Runner& r, i32, i32, std::span<const RValue> a) {
            const std::string_view dll = arg_str(a, 0);
            const std::string_view fn = arg_str(a, 1);
            // The fixed cdecl forms place the return type last. Variants 5..8
            // accept only real parameters and omit their types from the declaration.
            const usize typed = fixed_nargs <= 4 ? static_cast<usize>(fixed_nargs) : 0;
            std::vector<ExternalValueType> arg_types(static_cast<usize>(fixed_nargs), ExternalValueType::Real);
            for (usize i = 0; i < typed; ++i) {
                if (arg_i32(a, 2 + i) == 1) arg_types[i] = ExternalValueType::String;
            }
            const auto ret_type = arg_i32(a, 2 + typed) == 1 ? ExternalValueType::String : ExternalValueType::Real;
            return RValue(r.dll_registry().define_external(dll, fn, 0, ret_type, std::move(arg_types)));
        };
    };
    register_builtin("external_define", fn_ext_define);
    register_builtin("external_define0", fn_ext_define_fixed(0));
    register_builtin("external_define1", fn_ext_define_fixed(1));
    register_builtin("external_define2", fn_ext_define_fixed(2));
    register_builtin("external_define3", fn_ext_define_fixed(3));
    register_builtin("external_define4", fn_ext_define_fixed(4));
    register_builtin("external_define5", fn_ext_define_fixed(5));
    register_builtin("external_define6", fn_ext_define_fixed(6));
    register_builtin("external_define7", fn_ext_define_fixed(7));
    register_builtin("external_define8", fn_ext_define_fixed(8));

    auto fn_ext_call = [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        const i32 id = arg_i32(a, 0, 0);
        return r.dll_registry().call_external(r, id, a.subspan(1));
    };
    register_builtin("external_call", fn_ext_call);
    register_builtin("external_call0", fn_ext_call);
    register_builtin("external_call1", fn_ext_call);
    register_builtin("external_call2", fn_ext_call);
    register_builtin("external_call3", fn_ext_call);
    register_builtin("external_call4", fn_ext_call);
    register_builtin("external_call5", fn_ext_call);
    register_builtin("external_call6", fn_ext_call);
    register_builtin("external_call7", fn_ext_call);
    register_builtin("external_call8", fn_ext_call);

    register_builtin("external_free", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.dll_registry().free_external(r, arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("get_function_address", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(DllRegistry::get_function_address(arg_str(a, 0)));
    });

    // -----------------------------------------------------------------------
    // Legacy File, Included Files, Disk & Program Execution Built-ins
    // -----------------------------------------------------------------------
    register_builtin("file_open_read", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return r.open_legacy_text_file(arg_str(a, 0), 0);
    });
    register_builtin("file_open_write", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return r.open_legacy_text_file(arg_str(a, 0), 1);
    });
    register_builtin("file_open_append", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return r.open_legacy_text_file(arg_str(a, 0), 2);
    });
    register_builtin("file_close", [](Runner& r, i32, i32, std::span<const RValue>) {
        return r.close_legacy_text_file();
    });
    const auto legacy_op = [this](std::string_view name, bool write, auto op) {
        register_builtin(name, [write, op](Runner& r, i32, i32, std::span<const RValue> a) -> Result<RValue> {
            auto& file = r.legacy_text_file_;
            if (!file || file->writable != write) {
                return Error{write ? "File is not opened for writing." : "File is not opened for reading."};
            }
            return op(*file, a);
        });
    };
    legacy_op("file_write_string", true, [](LegacyTextFile& file, std::span<const RValue> a) {
        file.contents.append(arg_str(a, 0));
        return RValue(0.0);
    });
    legacy_op("file_write_real", true, [](LegacyTextFile& file, std::span<const RValue> a) {
        // The implicit file API keeps GM8.0's signed-floor formatting even in GM8.1.
        return append_text_real(file.contents, arg_real(a, 0), false);
    });
    legacy_op("file_writeln", true, [](LegacyTextFile& file, auto) {
        file.contents.append("\r\n");
        return RValue(0.0);
    });
    legacy_op("file_read_string", false, [](LegacyTextFile& file, auto) { return RValue(file.read_string()); });
    legacy_op("file_read_real", false, [](LegacyTextFile& file, auto) { return RValue(file.read_real()); });
    legacy_op("file_readln", false, [](LegacyTextFile& file, auto) {
        file.skip_line();
        return RValue(0.0);
    });
    legacy_op("file_eof", false, [](LegacyTextFile& file, auto) { return RValue(file.eof()); });
    legacy_op("file_eoln", false, [](LegacyTextFile& file, auto) { return RValue(file.eoln()); });
    register_builtin("file_attributes", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string_view path = arg_str(a, 0);
        const i32 attr = arg_i32(a, 1);
        if (!r.vfs().exists(path)) return RValue(false);
        const bool is_dir = r.vfs().directory_exists(path);
        if ((attr & 16) != 0) return RValue(is_dir);
        return RValue(!is_dir);
    });
    register_builtin("filename_drive", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view p = arg_str(a, 0);
        if (p.size() >= 2 && p[1] == ':') return RValue(std::string(p.substr(0, 2)));
        return RValue(std::string{});
    });
    register_builtin("export_include_file", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string_view name = arg_str(a, 0);
        const std::string lower = ascii_to_lower(name);
        for (const auto& inc : r.game().included_files) {
            if (!inc.exists || !inc.store) continue;
            if (ascii_to_lower(inc.filename) == lower) {
                return RValue(static_cast<bool>(r.vfs().write_all_bytes(inc.filename, inc.data)));
            }
        }
        return RValue(false);
    });
    register_builtin("export_include_file_location", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string_view name = arg_str(a, 0);
        const std::string_view loc = arg_str(a, 1);
        const std::string lower = ascii_to_lower(name);
        for (const auto& inc : r.game().included_files) {
            if (!inc.exists || !inc.store) continue;
            if (ascii_to_lower(inc.filename) == lower) {
                return RValue(static_cast<bool>(r.vfs().write_all_bytes(loc, inc.data)));
            }
        }
        return RValue(false);
    });
    register_builtin("discard_include_file", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string_view name = arg_str(a, 0);
        (void)r.vfs().delete_file(name);
        return RValue(true);
    });
    register_builtin("execute_program", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view prog = arg_str(a, 0);
        if (prog.starts_with("http://") || prog.starts_with("https://")) {
            (void)PlatformWindow::open_url(prog);
        }
        return RValue(0.0);
    });
    register_builtin("disk_free", [](Runner& r, i32, i32, std::span<const RValue> a) {
        std::error_code ec;
        const auto p = a.empty() || arg_str(a, 0).empty()
                           ? r.vfs().resolve_for_read(".")
                           : r.vfs().resolve_for_read(arg_str(a, 0));
        const auto si = std::filesystem::space(p, ec);
        return RValue(ec ? 0.0 : static_cast<f64>(si.available));
    });
    register_builtin("disk_size", [](Runner& r, i32, i32, std::span<const RValue> a) {
        std::error_code ec;
        const auto p = a.empty() || arg_str(a, 0).empty()
                           ? r.vfs().resolve_for_read(".")
                           : r.vfs().resolve_for_read(arg_str(a, 0));
        const auto si = std::filesystem::space(p, ec);
        return RValue(ec ? 0.0 : static_cast<f64>(si.capacity));
    });

    // -----------------------------------------------------------------------
    // YoYo_* (GM 8.1 Platform / Web / Virtual Key Built-ins)
    // -----------------------------------------------------------------------
    register_builtin("YoYo_GetPlatform", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); }); // os_win32
    register_builtin("YoYo_GetDevice", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("YoYo_GetDomain", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(std::string{}); });
    register_builtin("YoYo_GetTimer", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(static_cast<f64>(Clock::now_us()));
    });
    register_builtin("YoYo_OpenURL", [](Runner&, i32, i32, std::span<const RValue> a) {
        (void)PlatformWindow::open_url(arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("YoYo_OpenURL_ext", [](Runner&, i32, i32, std::span<const RValue> a) {
        (void)PlatformWindow::open_url(arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("YoYo_OpenURL_full", [](Runner&, i32, i32, std::span<const RValue> a) {
        (void)PlatformWindow::open_url(arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("YoYo_EnableAlphaBlend", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_alpha_blend_enable(arg_bool(a, 0, true));
        return RValue(0.0);
    });
    register_builtin("YoYo_AddVirtualKey", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(-1); });
    register_builtin("YoYo_DeleteVirtualKey", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("YoYo_ShowVirtualKey", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("YoYo_HideVirtualKey", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("game_end", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.request_game_end();
        return RValue(0.0);
    });
    register_builtin("game_restart", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.request_game_restart();
        return RValue(0.0);
    });
    register_builtin("execute_shell", [](Runner&, i32, i32, std::span<const RValue> a) {
        const std::string_view prog = arg_str(a, 0);
        if (prog.starts_with("http://") || prog.starts_with("https://")) {
            (void)PlatformWindow::open_url(prog);
        }
        return RValue(0.0);
    });
    register_builtin("highscore_clear", [](Runner& r, i32, i32, std::span<const RValue>) {
        for (auto& h : r.highscores_) h = HighscoreEntry{};
        return RValue(0.0);
    });
    register_builtin("highscore_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const std::string name(arg_str(a, 0));
        const f64 sc = arg_real(a, 1);
        for (usize i = 0; i < r.highscores_.size(); ++i) {
            if (sc > r.highscores_[i].score) {
                for (usize j = r.highscores_.size() - 1; j > i; --j) {
                    r.highscores_[j] = r.highscores_[j - 1];
                }
                r.highscores_[i] = HighscoreEntry{name, sc};
                break;
            }
        }
        return RValue(0.0);
    });
    register_builtin("highscore_value", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 p = arg_i32(a, 0) - 1;
        if (p < 0 || p >= 10) return RValue(0.0);
        return RValue(r.highscores_[static_cast<usize>(p)].score);
    });
    register_builtin("highscore_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 p = arg_i32(a, 0) - 1;
        if (p < 0 || p >= 10) return RValue(std::string{});
        return RValue(r.highscores_[static_cast<usize>(p)].name);
    });
    register_builtin("highscore_show", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("game_save", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("game_load", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
}

} // namespace gm
