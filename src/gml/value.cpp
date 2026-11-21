#include "gml/value.h"

#include <cmath>
#include <cstdio>

namespace gm {

std::string format_gm_real(f64 v) {
    if (!std::isfinite(v)) return "0";
    const i64 r = delphi_round(v);
    if (std::fabs(v - static_cast<f64>(r)) < kGmlEpsilon &&
        std::fabs(v) < 1e15) {
        return std::to_string(r);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

Result<RValue> RValue::add(const RValue& a, const RValue& b) {
    if (a.is_real() && b.is_real()) {
        return RValue(a.real_ + b.real_);
    }
    if (a.is_string() && b.is_string()) {
        std::string out;
        out.reserve(a.str_.size() + b.str_.size());
        out.append(a.str_);
        out.append(b.str_);
        return RValue(std::move(out));
    }
    return Error{"Wrong type of arguments to +."};
}

Result<RValue> RValue::sub(const RValue& a, const RValue& b) {
    if (a.is_real() && b.is_real()) {
        return RValue(a.real_ - b.real_);
    }
    return Error{"Wrong type of arguments to -."};
}

Result<RValue> RValue::mul(const RValue& a, const RValue& b) {
    if (a.is_real() && b.is_real()) {
        return RValue(a.real_ * b.real_);
    }
    // Verified in gm81.exe.c sub_572ED8 case 212: real * string repeats string round(real) times.
    if (a.is_real() && b.is_string()) {
        const i64 count = delphi_round(a.real_);
        if (count <= 0 || b.str_.empty()) {
            return RValue(std::string{});
        }
        std::string out;
        out.reserve(b.str_.size() * static_cast<usize>(count));
        for (i64 i = 0; i < count; ++i) {
            out.append(b.str_);
        }
        return RValue(std::move(out));
    }
    return Error{"Wrong type of arguments to *."};
}

Result<RValue> RValue::div_float(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments to /."};
    }
    if (b.real_ == 0.0) {
        return Error{"Division by 0."};
    }
    return RValue(a.real_ / b.real_);
}

Result<RValue> RValue::div_int(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments to div."};
    }
    if (b.real_ == 0.0) {
        return Error{"Division by 0."};
    }
    return RValue(gml_div(a.real_, b.real_));
}

Result<RValue> RValue::mod_int(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments to mod."};
    }
    if (b.real_ == 0.0) {
        return Error{"Modulo by 0."};
    }
    return RValue(gml_mod(a.real_, b.real_));
}

Result<RValue> RValue::bit_and(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments for &."};
    }
    return RValue(static_cast<f64>(delphi_round(a.real_) & delphi_round(b.real_)));
}

Result<RValue> RValue::bit_or(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments for |."};
    }
    return RValue(static_cast<f64>(delphi_round(a.real_) | delphi_round(b.real_)));
}

Result<RValue> RValue::bit_xor(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments for ^."};
    }
    return RValue(static_cast<f64>(delphi_round(a.real_) ^ delphi_round(b.real_)));
}

Result<RValue> RValue::shl(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments for <<."};
    }
    const u64 lhs = static_cast<u64>(delphi_round(a.real_));
    const u32 shift = static_cast<u32>(delphi_round(b.real_)) & 63u;
    return RValue(static_cast<f64>(static_cast<i64>(lhs << shift)));
}

Result<RValue> RValue::shr(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments for >>."};
    }
    const u64 lhs = static_cast<u64>(delphi_round(a.real_));
    const u32 shift = static_cast<u32>(delphi_round(b.real_)) & 63u;
    return RValue(static_cast<f64>(static_cast<i64>(lhs >> shift)));
}

Result<RValue> RValue::log_and(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments for &&."};
    }
    return RValue(gml_truthy(a.real_) && gml_truthy(b.real_));
}

Result<RValue> RValue::log_or(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments for ||."};
    }
    return RValue(gml_truthy(a.real_) || gml_truthy(b.real_));
}

Result<RValue> RValue::log_xor(const RValue& a, const RValue& b) {
    if (!a.is_real() || !b.is_real()) {
        return Error{"Wrong type of arguments for ^^."};
    }
    return RValue(gml_truthy(a.real_) != gml_truthy(b.real_));
}

Result<RValue> RValue::cmp_eq(const RValue& a, const RValue& b) {
    if (a.kind_ != b.kind_) {
        return Error{"Cannot compare arguments."};
    }
    if (a.is_real()) {
        return RValue(gml_equals(a.real_, b.real_));
    }
    return RValue(a.str_ == b.str_);
}

Result<RValue> RValue::cmp_neq(const RValue& a, const RValue& b) {
    if (a.kind_ != b.kind_) {
        return Error{"Cannot compare arguments."};
    }
    if (a.is_real()) {
        return RValue(!gml_equals(a.real_, b.real_));
    }
    return RValue(a.str_ != b.str_);
}

Result<RValue> RValue::cmp_lt(const RValue& a, const RValue& b) {
    if (a.kind_ != b.kind_) {
        return Error{"Cannot compare arguments."};
    }
    if (a.is_real()) {
        return RValue(gml_less(a.real_, b.real_));
    }
    return RValue(a.str_ < b.str_);
}

Result<RValue> RValue::cmp_le(const RValue& a, const RValue& b) {
    if (a.kind_ != b.kind_) {
        return Error{"Cannot compare arguments."};
    }
    if (a.is_real()) {
        return RValue(gml_less_eq(a.real_, b.real_));
    }
    return RValue(a.str_ <= b.str_);
}

Result<RValue> RValue::cmp_gt(const RValue& a, const RValue& b) {
    if (a.kind_ != b.kind_) {
        return Error{"Cannot compare arguments."};
    }
    if (a.is_real()) {
        return RValue(gml_greater(a.real_, b.real_));
    }
    return RValue(a.str_ > b.str_);
}

Result<RValue> RValue::cmp_ge(const RValue& a, const RValue& b) {
    if (a.kind_ != b.kind_) {
        return Error{"Cannot compare arguments."};
    }
    if (a.is_real()) {
        return RValue(gml_greater_eq(a.real_, b.real_));
    }
    return RValue(a.str_ >= b.str_);
}

Result<RValue> RValue::neg(const RValue& a) {
    if (!a.is_real()) {
        return Error{"Wrong type of arguments to unary -."};
    }
    return RValue(-a.real_);
}

Result<RValue> RValue::pos(const RValue& a) {
    if (!a.is_real()) {
        return Error{"Wrong type of arguments to unary +."};
    }
    return a;
}

Result<RValue> RValue::log_not(const RValue& a) {
    if (!a.is_real()) {
        return Error{"Wrong type of arguments to !."};
    }
    return RValue(!gml_truthy(a.real_));
}

Result<RValue> RValue::bit_not(const RValue& a) {
    if (!a.is_real()) {
        return Error{"Wrong type of arguments to ~."};
    }
    return RValue(static_cast<f64>(~delphi_round(a.real_)));
}

i32 RValue::compare_for_ds(const RValue& a, const RValue& b, f64 ds_precision) noexcept {
    if (a.is_real() && b.is_real()) {
        const f64 diff = a.real_ - b.real_;
        if (std::fabs(diff) < ds_precision) return 0;
        return diff < 0.0 ? -1 : 1;
    }
    if (a.is_string() && b.is_string()) {
        const int c = a.str_.compare(b.str_);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    return a.is_real() ? -1 : 1;
}

std::string RValue::to_debug_string() const {
    if (is_string()) {
        return "\"" + str_ + "\"";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", real_);
    return std::string(buf);
}

} // namespace gm
