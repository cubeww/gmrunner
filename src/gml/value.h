#pragma once

#include "base/math.h"
#include "base/types.h"

#include <string>
#include <string_view>

namespace gm {

[[nodiscard]] std::string format_gm_real(f64 v);

class RValue {
public:
    enum class Kind : u8 {
        Real = 0,
        String = 1,
    };

    RValue() noexcept : kind_(Kind::Real), real_(0.0) {}
    explicit RValue(f64 v) noexcept : kind_(Kind::Real), real_(v) {}
    explicit RValue(i32 v) noexcept : kind_(Kind::Real), real_(static_cast<f64>(v)) {}
    explicit RValue(u32 v) noexcept : kind_(Kind::Real), real_(static_cast<f64>(v)) {}
    explicit RValue(i64 v) noexcept : kind_(Kind::Real), real_(static_cast<f64>(v)) {}
    explicit RValue(bool v) noexcept : kind_(Kind::Real), real_(v ? 1.0 : 0.0) {}
    explicit RValue(std::string s) noexcept : kind_(Kind::String), real_(0.0), str_(std::move(s)) {}
    explicit RValue(std::string_view s) : kind_(Kind::String), real_(0.0), str_(s) {}
    explicit RValue(const char* s) : kind_(Kind::String), real_(0.0), str_(s ? s : "") {}
    // DS streams store both fields, even when only one is visible to GML.
    RValue(Kind kind, f64 real, std::string str) noexcept
        : kind_(kind), real_(real), str_(std::move(str)) {}

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_real() const noexcept { return kind_ == Kind::Real; }
    [[nodiscard]] bool is_string() const noexcept { return kind_ == Kind::String; }

    [[nodiscard]] f64 as_real() const noexcept { return real_; }
    [[nodiscard]] i32 as_i32() const noexcept { return static_cast<i32>(delphi_round(real_)); }
    [[nodiscard]] i64 as_i64() const noexcept { return delphi_round(real_); }
    [[nodiscard]] bool is_truthy() const noexcept { return is_real() && gml_truthy(real_); }

    [[nodiscard]] const std::string& as_string() const noexcept { return str_; }
    [[nodiscard]] std::string& as_string_mut() noexcept { return str_; }

    // Exact GM 8.x binary & unary operations (matching gm81.exe.c sub_572D74 / sub_572ED8).
    [[nodiscard]] static Result<RValue> add(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> sub(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> mul(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> div_float(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> div_int(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> mod_int(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> bit_and(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> bit_or(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> bit_xor(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> shl(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> shr(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> log_and(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> log_or(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> log_xor(const RValue& a, const RValue& b);

    [[nodiscard]] static Result<RValue> cmp_eq(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> cmp_neq(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> cmp_lt(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> cmp_le(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> cmp_gt(const RValue& a, const RValue& b);
    [[nodiscard]] static Result<RValue> cmp_ge(const RValue& a, const RValue& b);

    [[nodiscard]] static Result<RValue> neg(const RValue& a);
    [[nodiscard]] static Result<RValue> pos(const RValue& a);
    [[nodiscard]] static Result<RValue> log_not(const RValue& a);
    [[nodiscard]] static Result<RValue> bit_not(const RValue& a);

    // Total ordering used by ds_map / ds_list_sort (matching gm81.exe.c sub_65A204):
    // Reals compare with ds_precision epsilon; any Real sorts < any String.
    [[nodiscard]] static i32 compare_for_ds(const RValue& a, const RValue& b, f64 ds_precision = kDefaultDsPrecision) noexcept;

    [[nodiscard]] std::string to_debug_string() const;

private:
    Kind kind_ = Kind::Real;
    f64 real_ = 0.0;
    std::string str_;
};

} // namespace gm
