#include "graphics/dx9_shader_transpiler.h"

#include "base/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace gm {

namespace {

// D3D9 Shader Instruction Opcodes
constexpr u32 kOpNop          = 0;
constexpr u32 kOpMov          = 1;
constexpr u32 kOpAdd          = 2;
constexpr u32 kOpSub          = 3;
constexpr u32 kOpMad          = 4;
constexpr u32 kOpMul          = 5;
constexpr u32 kOpRcp          = 6;
constexpr u32 kOpRsq          = 7;
constexpr u32 kOpDp3          = 8;
constexpr u32 kOpDp4          = 9;
constexpr u32 kOpMin          = 10;
constexpr u32 kOpMax          = 11;
constexpr u32 kOpSlt          = 12;
constexpr u32 kOpSge          = 13;
constexpr u32 kOpExp          = 14;
constexpr u32 kOpLog          = 15;
constexpr u32 kOpLit          = 16;
constexpr u32 kOpDst          = 17;
constexpr u32 kOpLrp          = 18;
constexpr u32 kOpFrc          = 19;
constexpr u32 kOpM4x4         = 20;
constexpr u32 kOpM4x3         = 21;
constexpr u32 kOpM3x4         = 22;
constexpr u32 kOpM3x3         = 23;
constexpr u32 kOpM3x2         = 24;
constexpr u32 kOpCall         = 25;
constexpr u32 kOpCallNz       = 26;
constexpr u32 kOpLoop         = 27;
constexpr u32 kOpRet          = 28;
constexpr u32 kOpEndLoop      = 29;
constexpr u32 kOpLabel        = 30;
constexpr u32 kOpDcl          = 31;
constexpr u32 kOpPow          = 32;
constexpr u32 kOpCrs          = 33;
constexpr u32 kOpSgn          = 34;
constexpr u32 kOpAbs          = 35;
constexpr u32 kOpNrm          = 36;
constexpr u32 kOpSinCos       = 37;
constexpr u32 kOpRep          = 38;
constexpr u32 kOpEndRep       = 39;
constexpr u32 kOpIf           = 40;
constexpr u32 kOpIfC          = 41;
constexpr u32 kOpElse         = 42;
constexpr u32 kOpEndIf        = 43;
constexpr u32 kOpBreak        = 44;
constexpr u32 kOpBreakC       = 45;
constexpr u32 kOpMovA         = 46;
constexpr u32 kOpDefB         = 47;
constexpr u32 kOpDefI         = 48;
constexpr u32 kOpTexCoord     = 64;
constexpr u32 kOpTexKill      = 65;
constexpr u32 kOpTex          = 66;
constexpr u32 kOpExpP         = 78;
constexpr u32 kOpLogP         = 79;
constexpr u32 kOpCnd          = 80;
constexpr u32 kOpDef          = 81;
constexpr u32 kOpCmp          = 88;
constexpr u32 kOpDp2Add       = 90;
constexpr u32 kOpDsx          = 91;
constexpr u32 kOpDsy          = 92;
constexpr u32 kOpTexLdd       = 93;
constexpr u32 kOpSetP         = 94;
constexpr u32 kOpTexLdl       = 95;
constexpr u32 kOpBreakP       = 96;
constexpr u32 kOpPhase        = 0xFFFD;
constexpr u32 kOpComment      = 0xFFFE;
constexpr u32 kOpEnd          = 0xFFFF;

// D3D9 Register Types (D3DSHADER_PARAM_REGISTER_TYPE)
constexpr u32 kRegTemp        = 0;
constexpr u32 kRegInput       = 1;
constexpr u32 kRegConst       = 2;
constexpr u32 kRegAddrOrTex   = 3; // VS: a0, PS SM1/2: tN
constexpr u32 kRegRastOut     = 4; // VS SM1/2: 0=oPos, 1=oFog, 2=oPts
constexpr u32 kRegAttrOut     = 5; // VS SM1/2: oD0, oD1
constexpr u32 kRegOutput      = 6; // VS SM1/2: oT0..7, VS SM3: o0..11
constexpr u32 kRegConstInt    = 7;
constexpr u32 kRegColorOut    = 8;
constexpr u32 kRegDepthOut    = 9;
constexpr u32 kRegSampler     = 10;
constexpr u32 kRegConst2      = 11;
constexpr u32 kRegConst3      = 12;
constexpr u32 kRegConst4      = 13;
constexpr u32 kRegConstBool   = 14;
constexpr u32 kRegLoop        = 15; // aL
constexpr u32 kRegTempFloat16 = 16;
constexpr u32 kRegMiscType    = 17; // 0=vPos, 1=vFace
constexpr u32 kRegPredicate   = 19;

// D3D9 Usage Semantics
constexpr u8 kUsagePosition     = 0;
constexpr u8 kUsageBlendWeight  = 1;
constexpr u8 kUsageBlendIndices = 2;
constexpr u8 kUsageNormal       = 3;
constexpr u8 kUsagePSize        = 4;
constexpr u8 kUsageTexcoord     = 5;
constexpr u8 kUsageTangent      = 6;
constexpr u8 kUsageBinormal     = 7;
constexpr u8 kUsageTessFactor   = 8;
constexpr u8 kUsagePositionT    = 9;
constexpr u8 kUsageColor        = 10;
constexpr u8 kUsageFog          = 11;
constexpr u8 kUsageDepth        = 12;

// D3D9 Source Modifiers
constexpr u32 kModNone    = 0;
constexpr u32 kModNeg     = 1;
constexpr u32 kModBias    = 2;
constexpr u32 kModBiasNeg = 3;
constexpr u32 kModSign    = 4;
constexpr u32 kModSignNeg = 5;
constexpr u32 kModComp    = 6;
constexpr u32 kModX2      = 7;
constexpr u32 kModX2Neg   = 8;
constexpr u32 kModDz      = 9;
constexpr u32 kModDw      = 10;
constexpr u32 kModAbs     = 11;
constexpr u32 kModAbsNeg  = 12;
constexpr u32 kModNot     = 13;

u32 decode_reg_type(u32 param) noexcept {
    return ((param >> 28) & 0x7u) | (((param >> 11) & 0x3u) << 3u);
}

u32 decode_reg_num(u32 param) noexcept {
    return param & 0x7FFu;
}

u32 decode_write_mask(u32 dst_param) noexcept {
    return (dst_param >> 16) & 0xFu;
}

bool decode_dst_saturate(u32 dst_param) noexcept {
    return ((dst_param >> 20) & 0x1u) != 0;
}

u32 decode_src_mod(u32 src_param) noexcept {
    return (src_param >> 24) & 0xFu;
}

bool decode_has_rel_addr(u32 param) noexcept {
    return (param & 0x00002000u) != 0;
}

u32 decode_inst_length(u32 op_tok, u8 major_ver, u8 minor_ver) noexcept {
    if (major_ver >= 2) {
        return (op_tok >> 24) & 0xFu;
    }
    // SM 1.1–1.3 use tex/texcoord with one operand. SM 1.4 uses texld/texcrd with two.
    switch (op_tok & 0xFFFFu) {
        case kOpTex: case kOpTexCoord: return minor_ver == 4 ? 2 : 1;
        case kOpNop: case kOpElse: case kOpEndIf: case kOpEndRep: case kOpEndLoop: case kOpRet: case kOpBreak:
            return 0;
        case kOpLabel: case kOpCall: case kOpTexKill: case kOpRep: case kOpIf:
            return 1;
        case kOpMov: case kOpMovA: case kOpRcp: case kOpRsq: case kOpFrc: case kOpExp: case kOpExpP:
        case kOpLog: case kOpLogP: case kOpLit: case kOpAbs: case kOpNrm: case kOpDcl: case kOpDsx: case kOpDsy:
        case kOpCallNz: case kOpLoop: case kOpIfC: case kOpBreakC:
            return 2;
        case kOpAdd: case kOpSub: case kOpMul: case kOpDp3: case kOpDp4: case kOpMin: case kOpMax:
        case kOpSlt: case kOpSge: case kOpDst: case kOpPow: case kOpCrs: case kOpM4x4: case kOpM4x3:
        case kOpM3x4: case kOpM3x3: case kOpM3x2: case kOpTexLdl: case kOpSetP:
            return 3;
        case kOpMad: case kOpLrp: case kOpCmp: case kOpCnd: case kOpDp2Add: case kOpSinCos: case kOpSgn:
            return 4;
        case kOpDef: case kOpDefI: case kOpDefB: case kOpTexLdd:
            return 5;
        default:
            return 0;
    }
}

u32 matrix_rows(u32 op) noexcept {
    switch (op) {
        case kOpM4x4: case kOpM3x4: return 4;
        case kOpM4x3: case kOpM3x3: return 3;
        case kOpM3x2: return 2;
        default: return 0;
    }
}

bool valid_predicate(u32 token) noexcept {
    const u32 swizzle = (token >> 16) & 0xFFu;
    return (token & 0x80000000u) && decode_reg_type(token) == kRegPredicate && decode_reg_num(token) == 0 &&
           !decode_has_rel_addr(token) && (decode_src_mod(token) == kModNone || decode_src_mod(token) == kModNot) &&
           (swizzle == 0xE4u || swizzle == (swizzle & 3u) * 0x55u);
}

u32 predicated_operand_count(u32 op) noexcept {
    switch (op) {
        case kOpTexKill: return 1;
        case kOpMov: case kOpMovA: case kOpRcp: case kOpRsq: case kOpFrc: case kOpExp: case kOpExpP:
        case kOpLog: case kOpLogP: case kOpLit: case kOpAbs: case kOpNrm: case kOpDsx: case kOpDsy:
        case kOpSinCos: case kOpSgn: return 2;
        case kOpAdd: case kOpSub: case kOpMul: case kOpDp3: case kOpDp4: case kOpMin: case kOpMax:
        case kOpSlt: case kOpSge: case kOpDst: case kOpPow: case kOpCrs: case kOpM4x4: case kOpM4x3:
        case kOpM3x4: case kOpM3x3: case kOpM3x2: case kOpTex: case kOpTexLdl: return 3;
        case kOpMad: case kOpLrp: case kOpCmp: case kOpDp2Add: return 4;
        case kOpTexLdd: return 5;
        default: return 0;
    }
}

std::string format_glsl_float(f32 v) {
    if (!std::isfinite(v)) return "0.0";
    if (v == 0.0f) return "0.0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    std::string s(buf);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

std::string mask_to_str(u32 mask) {
    std::string s;
    if (mask & 1u) s.push_back('x');
    if (mask & 2u) s.push_back('y');
    if (mask & 4u) s.push_back('z');
    if (mask & 8u) s.push_back('w');
    if (s.empty()) s = "xyzw";
    return s;
}

std::string swizzle_to_str(u32 src_param) {
    static constexpr char kComp[4] = {'x', 'y', 'z', 'w'};
    const u32 sw = (src_param >> 16) & 0xFFu;
    std::string s(4, 'x');
    s[0] = kComp[(sw >> 0) & 3u];
    s[1] = kComp[(sw >> 2) & 3u];
    s[2] = kComp[(sw >> 4) & 3u];
    s[3] = kComp[(sw >> 6) & 3u];
    return s;
}

char first_swizzle_comp(u32 src_param) {
    static constexpr char kComp[4] = {'x', 'y', 'z', 'w'};
    return kComp[(src_param >> 16) & 3u];
}

std::string predicate_vector(u32 token) {
    const std::string value = "p0." + swizzle_to_str(token);
    return decode_src_mod(token) == kModNot ? "not(" + value + ")" : value;
}

std::string predicate_scalar(u32 token) {
    return std::string(decode_src_mod(token) == kModNot ? "!p0." : "p0.") + first_swizzle_comp(token);
}

std::string cmp_op_str(u32 cmp_code) {
    switch (cmp_code & 7u) {
        case 1: return ">";
        case 2: return "==";
        case 3: return ">=";
        case 4: return "<";
        case 5: return "!=";
        case 6: return "<=";
        default: return "==";
    }
}

u32 read_u32_le(const u8* p) noexcept {
    u32 v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

u16 read_u16_le(const u8* p) noexcept {
    u16 v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

std::string read_cstr_bounded(const u8* base, usize size, usize off) {
    if (off >= size) return {};
    usize end = off;
    while (end < size && base[end] != 0) ++end;
    return std::string(reinterpret_cast<const char*>(base + off), end - off);
}

void parse_ctab_block(std::span<const u8> ctab, Dx9ParsedShader& out) {
    // ctab starts right after the 'CTAB' 4-byte fourcc.
    if (ctab.size() < 28) return;
    const u32 hdr_size = read_u32_le(ctab.data() + 0);
    if (hdr_size < 28) return;
    const u32 num_consts = read_u32_le(ctab.data() + 12);
    const u32 const_info_off = read_u32_le(ctab.data() + 16);

    for (u32 i = 0; i < num_consts; ++i) {
        const usize entry_off = static_cast<usize>(const_info_off) + static_cast<usize>(i) * 20u;
        if (entry_off + 20u > ctab.size()) break;

        const u32 name_off = read_u32_le(ctab.data() + entry_off + 0);
        const u16 reg_set = read_u16_le(ctab.data() + entry_off + 4);
        const u16 reg_idx = read_u16_le(ctab.data() + entry_off + 6);
        const u16 reg_cnt = read_u16_le(ctab.data() + entry_off + 8);
        const u32 type_off = read_u32_le(ctab.data() + entry_off + 12);
        const u32 def_off = read_u32_le(ctab.data() + entry_off + 16);

        Dx9ConstantInfo info{};
        info.name = read_cstr_bounded(ctab.data(), ctab.size(), name_off);
        info.register_set = static_cast<Dx9RegisterSet>(reg_set);
        info.register_index = reg_idx;
        info.register_count = reg_cnt;

        if (type_off != 0 && static_cast<usize>(type_off) + 8u <= ctab.size()) {
            info.type_class = read_u16_le(ctab.data() + type_off + 0);
            info.base_type = read_u16_le(ctab.data() + type_off + 2);
            info.rows = read_u16_le(ctab.data() + type_off + 4);
            info.cols = read_u16_le(ctab.data() + type_off + 6);
        }

        if (def_off != 0 && reg_cnt > 0 && static_cast<usize>(def_off) < ctab.size()) {
            const usize avail = ctab.size() - static_cast<usize>(def_off);
            if (info.register_set == Dx9RegisterSet::Float4) {
                const usize want_floats = static_cast<usize>(reg_cnt) * 4u;
                info.default_floats.assign(want_floats, 0.0f);
                const usize copy_floats = std::min(want_floats, avail / 4u);
                std::memcpy(info.default_floats.data(), ctab.data() + def_off, copy_floats * 4u);
                info.has_default = true;
            } else if (info.register_set == Dx9RegisterSet::Int4) {
                const usize want_ints = static_cast<usize>(reg_cnt) * 4u;
                info.default_ints.assign(want_ints, 0);
                const usize copy_ints = std::min(want_ints, avail / 4u);
                std::memcpy(info.default_ints.data(), ctab.data() + def_off, copy_ints * 4u);
                info.has_default = true;
            } else if (info.register_set == Dx9RegisterSet::Bool) {
                const usize want_bools = static_cast<usize>(reg_cnt);
                info.default_bools.assign(want_bools, 0);
                const usize copy_bools = std::min(want_bools, avail / 4u);
                for (usize b = 0; b < copy_bools; ++b) {
                    info.default_bools[b] = read_u32_le(ctab.data() + def_off + b * 4u) != 0 ? 1u : 0u;
                }
                info.has_default = true;
            }
        }

        if (!info.name.empty()) {
            // Track max constant register from CTAB too (handles arrays/matrices indexed cleanly).
            const u32 end_reg = static_cast<u32>(info.register_index) + static_cast<u32>(info.register_count);
            if (info.register_set == Dx9RegisterSet::Float4 && end_reg > 0) {
                out.uses_const_f = true;
                out.max_const_f = std::max(out.max_const_f, end_reg);
            } else if (info.register_set == Dx9RegisterSet::Int4 && end_reg > 0) {
                out.uses_const_i = true;
                out.max_const_i = std::max(out.max_const_i, end_reg);
            } else if (info.register_set == Dx9RegisterSet::Bool && end_reg > 0) {
                out.uses_const_b = true;
                out.max_const_b = std::max(out.max_const_b, end_reg);
            } else if (info.register_set == Dx9RegisterSet::Sampler && info.register_index < 8) {
                out.sampler_mask |= static_cast<u8>(1u << info.register_index);
            }

            const usize idx = out.constants.size();
            out.constant_by_name[info.name] = idx;
            out.constants.push_back(std::move(info));
        }
    }
}

struct GlslEmitter {
    GlslEmitter(const Dx9ParsedShader& s, bool es, const std::unordered_set<std::string>* vs_a)
        : sh(s), gles3(es), vs_avail(vs_a) {}

    const Dx9ParsedShader& sh;
    bool gles3 = false;
    const std::unordered_set<std::string>* vs_avail = nullptr;

    std::unordered_map<u32, std::string> def_c_names;
    std::unordered_map<u32, std::string> def_i_names;
    std::unordered_map<u32, std::string> def_b_names;
    std::unordered_map<u32, std::string> input_reg_expr;  // vN -> GLSL expr
    std::unordered_map<u32, std::string> output_reg_name; // oN -> GLSL var name
    std::unordered_set<u32> used_temps;
    u8 writable_texcoords = 0;
    bool uses_a0 = false;
    bool uses_aL = false;
    bool uses_p0 = false;
    bool uses_vpos = false;
    bool uses_vface = false;
    u32 pos_out_reg = 0xFFFFFFFFu;
    i32 indent = 1;
    i32 loop_counter = 0;

    std::string ind() const {
        return std::string(static_cast<usize>(std::max(0, indent) * 4), ' ');
    }

    std::string const_prefix() const {
        return sh.stage == Dx9ShaderStage::Vertex ? "u_vc" : "u_pc";
    }
    std::string iconst_prefix() const {
        return sh.stage == Dx9ShaderStage::Vertex ? "u_vi" : "u_pi";
    }
    std::string bconst_prefix() const {
        return sh.stage == Dx9ShaderStage::Vertex ? "u_vb" : "u_pb";
    }
    std::string sampler_prefix() const {
        return sh.stage == Dx9ShaderStage::Vertex ? "u_vs" : "u_s";
    }

    std::string format_rel_offset(u32 rel_tok) const {
        const u32 rtype = decode_reg_type(rel_tok);
        const char comp = first_swizzle_comp(rel_tok);
        if (rtype == kRegLoop) return "aL";
        return std::string("a0.") + comp;
    }

    std::string texcoord_input(u32 stage) const {
        const std::string sem = "TEXCOORD" + std::to_string(stage);
        if (!vs_avail || vs_avail->contains(sem)) return "v_" + sem;
        return "vec4(0.0, 0.0, 0.0, 1.0)";
    }

    std::string format_base_reg(u32 reg_type, u32 reg_num, bool is_dst, const std::string* rel_expr) {
        const bool is_vs = (sh.stage == Dx9ShaderStage::Vertex);
        switch (reg_type) {
            case kRegTemp:
            case kRegTempFloat16:
                used_temps.insert(reg_num);
                return "r" + std::to_string(reg_num);
            case kRegInput: {
                if (auto it = input_reg_expr.find(reg_num); it != input_reg_expr.end()) {
                    return it->second;
                }
                if (!is_vs && sh.major_version < 3) {
                    const std::string sem = "COLOR" + std::to_string(reg_num);
                    if (!vs_avail || vs_avail->contains(sem)) return "v_" + sem;
                    return "vec4(0.0, 0.0, 0.0, 1.0)";
                }
                return "vec4(0.0, 0.0, 0.0, 1.0)";
            }
            case kRegConst:
            case kRegConst2:
            case kRegConst3:
            case kRegConst4: {
                u32 idx = reg_num;
                if (reg_type == kRegConst2) idx += 2048u;
                else if (reg_type == kRegConst3) idx += 4096u;
                else if (reg_type == kRegConst4) idx += 6144u;
                if (!rel_expr) {
                    if (auto it = def_c_names.find(idx); it != def_c_names.end()) {
                        return it->second;
                    }
                    const u32 clamped = sh.max_const_f > 0 ? std::min(idx, sh.max_const_f - 1) : 0;
                    return const_prefix() + "[" + std::to_string(clamped) + "]";
                }
                const u32 max_idx = sh.max_const_f > 0 ? (sh.max_const_f - 1) : 0;
                return const_prefix() + "[clamp(" + std::to_string(idx) + " + " + *rel_expr + ", 0, " + std::to_string(max_idx) + ")]";
            }
            case kRegAddrOrTex:
                if (is_vs) {
                    uses_a0 = true;
                    return "a0";
                } else {
                    if (sh.major_version == 1 && sh.minor_version < 4 && reg_num < 4) {
                        writable_texcoords |= static_cast<u8>(1u << reg_num);
                        return "t" + std::to_string(reg_num);
                    }
                    return texcoord_input(reg_num);
                }
            case kRegRastOut:
                if (reg_num == 0) return "o_pos";
                if (reg_num == 1) return "v_FOG0";
                return "o_pts";
            case kRegAttrOut:
                return "v_COLOR" + std::to_string(reg_num);
            case kRegOutput:
                if (sh.major_version >= 3) {
                    if (auto it = output_reg_name.find(reg_num); it != output_reg_name.end()) {
                        return it->second;
                    }
                    return "o_unused";
                }
                return "v_TEXCOORD" + std::to_string(reg_num);
            case kRegConstInt: {
                if (auto it = def_i_names.find(reg_num); it != def_i_names.end()) {
                    return it->second;
                }
                const u32 clamped = sh.max_const_i > 0 ? std::min(reg_num, sh.max_const_i - 1) : 0;
                return iconst_prefix() + "[" + std::to_string(clamped) + "]";
            }
            case kRegConstBool: {
                if (auto it = def_b_names.find(reg_num); it != def_b_names.end()) {
                    return it->second;
                }
                const u32 clamped = sh.max_const_b > 0 ? std::min(reg_num, sh.max_const_b - 1) : 0;
                return "(" + bconst_prefix() + "[" + std::to_string(clamped) + "] != 0)";
            }
            case kRegColorOut:
                return "o_col" + std::to_string(std::min<u32>(reg_num, 3u));
            case kRegDepthOut:
                return "gl_FragDepth";
            case kRegSampler:
                return sampler_prefix() + std::to_string(reg_num);
            case kRegLoop:
                uses_aL = true;
                return "vec4(float(aL))";
            case kRegMiscType:
                if (reg_num == 0) {
                    uses_vpos = true;
                    return "gm_vpos";
                }
                uses_vface = true;
                return "vec4(gl_FrontFacing ? 1.0 : -1.0)";
            case kRegPredicate:
                uses_p0 = true;
                return "p0";
            default:
                return is_dst ? "o_unused" : "vec4(0.0)";
        }
    }

    std::string format_src(u32 src_tok, const std::string* rel_expr) {
        const u32 rtype = decode_reg_type(src_tok);
        const u32 rnum = decode_reg_num(src_tok);
        std::string base = format_base_reg(rtype, rnum, false, rel_expr);
        if (rtype == kRegConstBool || rtype == kRegSampler || rtype == kRegConstInt) {
            return base;
        }
        const std::string sw = swizzle_to_str(src_tok);
        std::string expr = (sw == "xyzw") ? base : ("(" + base + ")." + sw);
        switch (decode_src_mod(src_tok)) {
            case kModNone:    break;
            case kModNeg:     expr = "(-(" + expr + "))"; break;
            case kModBias:    expr = "((" + expr + ") - 0.5)"; break;
            case kModBiasNeg: expr = "(-( (" + expr + ") - 0.5 ))"; break;
            case kModSign:    expr = "((" + expr + ") * 2.0 - 1.0)"; break;
            case kModSignNeg: expr = "(-( (" + expr + ") * 2.0 - 1.0 ))"; break;
            case kModComp:    expr = "(1.0 - (" + expr + "))"; break;
            case kModX2:      expr = "((" + expr + ") * 2.0)"; break;
            case kModX2Neg:   expr = "(-(" + expr + ") * 2.0)"; break;
            case kModDz:      expr = "((" + expr + ") / ((" + expr + ").z))"; break;
            case kModDw:      expr = "((" + expr + ") / ((" + expr + ").w))"; break;
            case kModAbs:     expr = "abs(" + expr + ")"; break;
            case kModAbsNeg:  expr = "(-abs(" + expr + "))"; break;
            case kModNot:     expr = "(1.0 - (" + expr + "))"; break;
            default:          break;
        }
        return expr;
    }

    std::string emit_assign(u32 dst_tok, const std::string* dst_rel, const std::string& rhs_vec4, const std::string& pred) {
        const u32 rtype = decode_reg_type(dst_tok);
        const u32 rnum = decode_reg_num(dst_tok);
        const u32 mask = decode_write_mask(dst_tok);
        const bool sat = decode_dst_saturate(dst_tok);
        std::string rhs = sat ? ("clamp(" + rhs_vec4 + ", 0.0, 1.0)") : rhs_vec4;

        const std::string scalar_guard = pred.empty() ? "" : "if ((" + pred + ").x) ";
        if (rtype == kRegDepthOut) {
            return ind() + scalar_guard + "gl_FragDepth = (" + rhs + ").x;\n";
        }
        if (rtype == kRegAddrOrTex && sh.stage == Dx9ShaderStage::Vertex) {
            uses_a0 = true;
            const std::string m = mask_to_str(mask);
            if (!pred.empty()) rhs = "mix(vec4(a0), " + rhs + ", " + pred + ")";
            return ind() + "a0." + m + " = ivec4(floor((" + rhs + ") + 0.5))." + m + ";\n";
        }
        if (rtype == kRegRastOut && rnum == 1) {
            return ind() + scalar_guard + "v_FOG0 = vec4((" + rhs + ").x);\n";
        }
        if (rtype == kRegRastOut && rnum == 2) {
            return ind() + scalar_guard + "gl_PointSize = (" + rhs + ").x;\n";
        }
        const std::string dst = format_base_reg(rtype, rnum, true, dst_rel);
        if (dst == "o_unused") return {};
        if (!pred.empty()) rhs = "mix(" + dst + ", " + rhs + ", " + pred + ")";
        const std::string m = mask_to_str(mask);
        if (m == "xyzw") {
            return ind() + dst + " = " + rhs + ";\n";
        }
        return ind() + dst + "." + m + " = (" + rhs + ")." + m + ";\n";
    }
};

} // namespace

bool dx9_shader_bounds_check(std::span<const u8> bytes) noexcept {
    if (bytes.size() < 8 || bytes.size() % 4 != 0) return false;
    const usize words = bytes.size() / 4;
    usize cursor = 2u + read_u16_le(bytes.data() + 6);
    if (cursor + 1 >= words) return false;
    while (cursor < words) {
        const u32 token = read_u32_le(bytes.data() + cursor * 4);
        if ((token & 0xFFFFu) == kOpEnd) return true;
        cursor += 1u + ((token >> 24) & 0xFu);
    }
    return false;
}

i32 dx9_semantic_to_attrib_location(u8 usage, u8 usage_index) noexcept {
    switch (usage) {
        case kUsagePosition:
        case kUsagePositionT: return usage_index == 0 ? 0 : -1;
        case kUsageNormal:    return usage_index == 0 ? 1 : -1;
        case kUsageTexcoord:
            if (usage_index == 0) return 2;
            if (usage_index >= 1 && usage_index <= 3) return 3 + static_cast<i32>(usage_index); // 4,5,6
            if (usage_index >= 4 && usage_index <= 7) return 6 + static_cast<i32>(usage_index); // 10,11,12,13
            return -1;
        case kUsageColor:
            if (usage_index == 0) return 3;
            if (usage_index == 1) return 7;
            return -1;
        case kUsageTangent:      return usage_index == 0 ? 8 : -1;
        case kUsageBinormal:     return usage_index == 0 ? 9 : -1;
        case kUsageBlendWeight:  return usage_index == 0 ? 14 : -1;
        case kUsageBlendIndices: return usage_index == 0 ? 15 : -1;
        default: return -1;
    }
}

std::string dx9_semantic_name(u8 usage, u8 usage_index) {
    const char* base = "TEXCOORD";
    switch (usage) {
        case kUsagePosition:     base = "POSITION"; break;
        case kUsageBlendWeight:  base = "BLENDWEIGHT"; break;
        case kUsageBlendIndices: base = "BLENDINDICES"; break;
        case kUsageNormal:       base = "NORMAL"; break;
        case kUsagePSize:        base = "PSIZE"; break;
        case kUsageTexcoord:     base = "TEXCOORD"; break;
        case kUsageTangent:      base = "TANGENT"; break;
        case kUsageBinormal:     base = "BINORMAL"; break;
        case kUsageTessFactor:   base = "TESSFACTOR"; break;
        case kUsagePositionT:    base = "POSITIONT"; break;
        case kUsageColor:        base = "COLOR"; break;
        case kUsageFog:          base = "FOG"; break;
        case kUsageDepth:        base = "DEPTH"; break;
        default:                 base = "TEXCOORD"; break;
    }
    return std::string(base) + std::to_string(usage_index);
}

i32 parse_d3d9_shader(
    std::span<const u8> bytes,
    Dx9ShaderStage expected_stage,
    Dx9ParsedShader& out_shader,
    std::string* out_error) {
    out_shader = Dx9ParsedShader{};
    out_shader.stage = expected_stage;

    if (bytes.size() < 8) {
        if (out_error) *out_error = "Shader data is too small";
        return -2;
    }

    const u16 magic_hi = read_u16_le(bytes.data() + 2);
    if (magic_hi != 0xFFFEu && magic_hi != 0xFFFFu) {
        if (out_error) *out_error = "Invalid D3D9 shader version token";
        return -3;
    }

    if ((bytes.size() % 4u) != 0) {
        if (out_error) *out_error = "Shader bytecode size is not a multiple of 4";
        return -3;
    }

    const Dx9ShaderStage found_stage = (magic_hi == 0xFFFEu) ? Dx9ShaderStage::Vertex : Dx9ShaderStage::Pixel;
    if (found_stage != expected_stage) {
        if (out_error) *out_error = "Shader stage mismatch (VS vs PS)";
        return -3;
    }

    const usize word_count = bytes.size() / 4u;
    out_shader.words.resize(word_count);
    std::memcpy(out_shader.words.data(), bytes.data(), bytes.size());

    const u32 ver = out_shader.words[0];
    out_shader.major_version = static_cast<u8>((ver >> 8) & 0xFFu);
    out_shader.minor_version = static_cast<u8>(ver & 0xFFu);

    // Scan tokens, validate D3DSIO_END, parse CTAB, DCLs, and register extents.
    std::unordered_set<u32> def_c_regs;
    std::unordered_set<u32> def_i_regs;
    std::unordered_set<u32> def_b_regs;
    bool found_end = false;
    usize i = 1;

    while (i < word_count) {
        const u32 tok = out_shader.words[i];
        const u32 op = tok & 0xFFFFu;
        if (op == kOpEnd) {
            found_end = true;
            break;
        }
        if (op == kOpComment) {
            const u32 clen = (tok >> 16) & 0x7FFFu;
            if (i + 1u + clen > word_count) {
                if (out_error) *out_error = "Truncated COMMENT block in shader";
                return -3;
            }
            if (clen >= 1 && out_shader.words[i + 1] == 0x42415443u) { // 'CTAB'
                const u8* ctab_ptr = bytes.data() + (i + 2u) * 4u;
                const usize ctab_len = static_cast<usize>(clen - 1u) * 4u;
                parse_ctab_block(std::span<const u8>(ctab_ptr, ctab_len), out_shader);
            }
            i += 1u + clen;
            continue;
        }

        const u32 len = decode_inst_length(tok, out_shader.major_version, out_shader.minor_version);
        if (i + 1u + len > word_count) {
            if (out_error) *out_error = "Truncated instruction in shader";
            return -3;
        }

        const bool predicated = (tok & (1u << 28)) != 0;
        if (predicated) {
            const usize pred_pos = 2u + (len && decode_has_rel_addr(out_shader.words[i + 1]) ? 1u : 0u);
            // SM2.x retains two extra approximation constants for sincos/sgn.
            const u32 arity = out_shader.major_version == 2 && (op == kOpSinCos || op == kOpSgn)
                ? 4u : predicated_operand_count(op);
            usize operands = 0;
            for (usize p = 1; p <= len; ++p) {
                ++operands;
                if (decode_has_rel_addr(out_shader.words[i + p])) ++p;
            }
            if (out_shader.major_version < 2 || (out_shader.major_version == 2 && out_shader.minor_version == 0) ||
                !arity || operands != arity + 1 || pred_pos > len || !valid_predicate(out_shader.words[i + pred_pos])) {
                if (out_error) *out_error = "Invalid instruction predicate";
                return -1;
            }
        }
        if (expected_stage == Dx9ShaderStage::Pixel && out_shader.major_version == 1 &&
            (op == kOpTex || op == kOpTexCoord)) {
            const bool sm14 = out_shader.minor_version == 4;
            const u32 dst = out_shader.words[i + 1];
            const u32 stage = decode_reg_num(dst);
            if (decode_reg_type(dst) != (sm14 ? kRegTemp : kRegAddrOrTex) || stage >= (sm14 ? 6u : 4u)) {
                if (out_error) *out_error = "Invalid SM1 texture destination";
                return -1;
            }
            if (sm14) {
                const u32 source = out_shader.words[i + 2];
                const u32 type = decode_reg_type(source);
                if ((type != kRegAddrOrTex && !(op == kOpTex && type == kRegTemp)) || decode_reg_num(source) >= 6) {
                    if (out_error) *out_error = "Invalid SM1.4 texture coordinate source";
                    return -1;
                }
            }
            if (op == kOpTex) out_shader.sampler_mask |= static_cast<u8>(1u << stage);
        }

        if (op == kOpDef && len >= 5) {
            def_c_regs.insert(decode_reg_num(out_shader.words[i + 1]));
        } else if (op == kOpDefI && len >= 5) {
            def_i_regs.insert(decode_reg_num(out_shader.words[i + 1]));
        } else if (op == kOpDefB && len >= 2) {
            def_b_regs.insert(decode_reg_num(out_shader.words[i + 1]));
        } else if (op == kOpDcl && len >= 2) {
            const u32 d0 = out_shader.words[i + 1];
            const u32 d1 = out_shader.words[i + 2];
            const u32 rtype = decode_reg_type(d1);
            const u32 rnum = decode_reg_num(d1);
            const u8 wmask = static_cast<u8>(decode_write_mask(d1));
            if (rtype == kRegSampler) {
                if (rnum < 8) out_shader.sampler_mask |= static_cast<u8>(1u << rnum);
            } else if (rtype == kRegInput) {
                if (expected_stage == Dx9ShaderStage::Pixel && out_shader.major_version < 3) {
                    // SM 1.x / 2.x PS input v0/v1 are implicitly COLOR0/COLOR1 (d0 carries no usage bits).
                    out_shader.inputs.push_back(Dx9DclSemantic{kUsageColor, static_cast<u8>(rnum), static_cast<u16>(rnum), wmask});
                } else {
                    const u8 usage = static_cast<u8>(d0 & 0x1Fu);
                    const u8 uidx = static_cast<u8>((d0 >> 16) & 0xFu);
                    out_shader.inputs.push_back(Dx9DclSemantic{usage, uidx, static_cast<u16>(rnum), wmask});
                }
            } else if (rtype == kRegOutput) {
                const u8 usage = static_cast<u8>(d0 & 0x1Fu);
                const u8 uidx = static_cast<u8>((d0 >> 16) & 0xFu);
                out_shader.outputs.push_back(Dx9DclSemantic{usage, uidx, static_cast<u16>(rnum), wmask});
                if (!(usage == kUsagePosition && uidx == 0)) {
                    out_shader.output_semantics.insert(dx9_semantic_name(usage, uidx));
                }
            }
        } else {
            // Track register usage extents across all operand tokens.
            usize p = 1;
            u32 operand = 0;
            while (p <= len) {
                const u32 param = out_shader.words[i + p];
                const u32 rtype = decode_reg_type(param);
                const u32 rnum = decode_reg_num(param);
                const bool rel = decode_has_rel_addr(param);
                if (rtype == kRegConst) {
                    const u32 rows = operand == (predicated ? 3u : 2u) ? matrix_rows(op) : 0;
                    const u32 count = std::max(rows, 1u);
                    const u32 capacity = expected_stage == Dx9ShaderStage::Vertex ? 256u : 224u;
                    if (rows && rnum + rows > capacity) {
                        if (out_error) *out_error = "Matrix rows exceed the constant register bank";
                        return -1;
                    }
                    if (rel) {
                        out_shader.uses_const_f = true;
                        out_shader.uses_rel_const_f = true;
                        out_shader.max_const_f = std::max(out_shader.max_const_f, capacity);
                    } else {
                        for (u32 reg = rnum; reg < rnum + count; ++reg) {
                            if (!def_c_regs.contains(reg)) {
                                out_shader.uses_const_f = true;
                                out_shader.max_const_f = std::max(out_shader.max_const_f, reg + 1u);
                            }
                        }
                    }
                } else if (rtype == kRegConstInt) {
                    if (!def_i_regs.contains(rnum)) {
                        out_shader.uses_const_i = true;
                        out_shader.max_const_i = std::max<u32>(out_shader.max_const_i, rnum + 1u);
                    }
                } else if (rtype == kRegConstBool) {
                    if (!def_b_regs.contains(rnum)) {
                        out_shader.uses_const_b = true;
                        out_shader.max_const_b = std::max<u32>(out_shader.max_const_b, rnum + 1u);
                    }
                } else if (rtype == kRegColorOut && rnum < 4) {
                    out_shader.mrt_mask |= static_cast<u8>(1u << rnum);
                } else if (rtype == kRegDepthOut) {
                    out_shader.writes_depth = true;
                } else if (rtype == kRegSampler && rnum < 8) {
                    out_shader.sampler_mask |= static_cast<u8>(1u << rnum);
                } else if (expected_stage == Dx9ShaderStage::Vertex && out_shader.major_version < 3) {
                    if (rtype == kRegAttrOut) out_shader.output_semantics.insert("COLOR" + std::to_string(rnum));
                    else if (rtype == kRegOutput) out_shader.output_semantics.insert("TEXCOORD" + std::to_string(rnum));
                    else if (rtype == kRegRastOut && rnum == 1) out_shader.output_semantics.insert("FOG0");
                }
                ++operand;
                p += rel && out_shader.major_version >= 2 ? 2u : 1u;
            }
        }

        i += 1u + len;
    }

    if (!found_end) {
        if (out_error) *out_error = "Missing D3DSIO_END token in shader";
        return -3;
    }

    if (expected_stage == Dx9ShaderStage::Vertex) {
        std::array<bool, 16> occupied{};
        for (auto& input : out_shader.inputs) {
            const i32 loc = dx9_semantic_to_attrib_location(input.usage, input.usage_index);
            if (loc >= 0 && !occupied[loc]) {
                input.attrib_location = loc;
                occupied[loc] = true;
            }
        }
        // Additional semantics (e.g. POSITION1 for instancing) share the
        // remaining locations with the corresponding vertex declaration.
        for (auto& input : out_shader.inputs) {
            if (input.attrib_location >= 0) continue;
            auto slot = std::find(occupied.begin() + 4, occupied.end(), false);
            if (slot == occupied.end()) slot = std::find(occupied.begin(), occupied.end(), false);
            if (slot == occupied.end()) {
                if (out_error) *out_error = "Too many vertex inputs";
                return -3;
            }
            input.attrib_location = static_cast<i32>(slot - occupied.begin());
            *slot = true;
        }
    }

    // Clamp max constant counts to D3D9 hardware limits.
    const u32 max_c_cap = (expected_stage == Dx9ShaderStage::Vertex) ? 256u : 224u;
    if (out_shader.max_const_f > max_c_cap) out_shader.max_const_f = max_c_cap;
    if (out_shader.max_const_i > 16u) out_shader.max_const_i = 16u;
    if (out_shader.max_const_b > 16u) out_shader.max_const_b = 16u;

    return 0;
}

Result<std::string> transpile_d3d9_to_glsl(
    const Dx9ParsedShader& sh,
    bool gles3,
    const std::unordered_set<std::string>* vs_available_semantics) {
    GlslEmitter em(sh, gles3, vs_available_semantics);
    const bool is_vs = (sh.stage == Dx9ShaderStage::Vertex);

    // First pass over DCL and DEF/DEFI/DEFB to build declarations and register maps.
    std::ostringstream defs_ss;
    std::ostringstream io_ss;
    std::unordered_set<std::string> declared_varyings;

    for (const auto& d : sh.inputs) {
        const std::string sem = dx9_semantic_name(d.usage, d.usage_index);
        if (is_vs) {
            const i32 loc = d.attrib_location;
            if (loc >= 0) {
                const std::string var = "a_v" + std::to_string(d.reg_num);
                io_ss << "layout(location = " << loc << ") in vec4 " << var << ";\n";
                em.input_reg_expr[d.reg_num] = var;
            } else {
                em.input_reg_expr[d.reg_num] = "vec4(0.0, 0.0, 0.0, 1.0)";
            }
        } else {
            // PS input varying: link by canonical semantic name.
            if (!vs_available_semantics || vs_available_semantics->contains(sem)) {
                const std::string var = "v_" + sem;
                if (declared_varyings.insert(var).second) {
                    io_ss << "in vec4 " << var << ";\n";
                }
                em.input_reg_expr[d.reg_num] = var;
            } else {
                em.input_reg_expr[d.reg_num] = "vec4(0.0, 0.0, 0.0, 1.0)";
            }
        }
    }

    if (is_vs) {
        if (sh.major_version >= 3) {
            for (const auto& d : sh.outputs) {
                if (d.usage == kUsagePosition && d.usage_index == 0) {
                    em.pos_out_reg = d.reg_num;
                    em.output_reg_name[d.reg_num] = "o_pos";
                } else {
                    const std::string sem = dx9_semantic_name(d.usage, d.usage_index);
                    const std::string var = "v_" + sem;
                    if (declared_varyings.insert(var).second) {
                        io_ss << "out vec4 " << var << ";\n";
                    }
                    em.output_reg_name[d.reg_num] = var;
                }
            }
        } else {
            // SM 1.x / 2.x VS outputs collected during parsing
            for (const auto& sem : sh.output_semantics) {
                const std::string var = "v_" + sem;
                if (declared_varyings.insert(var).second) {
                    io_ss << "out vec4 " << var << ";\n";
                }
            }
        }
    } else {
        // SM 1.x / 2.x PS implicit inputs (tN / vN)
        if (sh.major_version < 3) {
            // Emit any TEXCOORD/COLOR varyings present in vs_available_semantics if referenced later.
            for (int n = 0; n < 8; ++n) {
                const std::string sem = "TEXCOORD" + std::to_string(n);
                if (!vs_available_semantics || vs_available_semantics->contains(sem)) {
                    const std::string var = "v_" + sem;
                    if (declared_varyings.insert(var).second) {
                        io_ss << "in vec4 " << var << ";\n";
                    }
                }
            }
            for (int n = 0; n < 2; ++n) {
                const std::string sem = "COLOR" + std::to_string(n);
                if (!vs_available_semantics || vs_available_semantics->contains(sem)) {
                    const std::string var = "v_" + sem;
                    if (declared_varyings.insert(var).second) {
                        io_ss << "in vec4 " << var << ";\n";
                    }
                }
            }
        }
        // PS color outputs (MRT 0..3)
        for (u32 mrt = 0; mrt < 4; ++mrt) {
            if (sh.mrt_mask & (1u << mrt)) {
                io_ss << "layout(location = " << mrt << ") out vec4 o_col" << mrt << ";\n";
            }
        }
    }

    // Second pass: collect DEF/DEFI/DEFB and emit body instructions.
    std::ostringstream body_ss;
    const usize wc = sh.words.size();
    usize i = 1;

    while (i < wc) {
        const u32 tok = sh.words[i];
        const u32 op = tok & 0xFFFFu;
        if (op == kOpEnd) break;
        if (op == kOpComment) {
            i += 1u + ((tok >> 16) & 0x7FFFu);
            continue;
        }
        const u32 len = decode_inst_length(tok, sh.major_version, sh.minor_version);
        const u32 ctrl = (tok >> 16) & 0xFFu;
        const bool predicated = ((tok >> 28) & 1u) != 0;

        if (op == kOpDef && len >= 5) {
            const u32 rnum = decode_reg_num(sh.words[i + 1]);
            f32 f[4];
            std::memcpy(f, &sh.words[i + 2], 16);
            const std::string name = "d_c" + std::to_string(rnum);
            em.def_c_names[rnum] = name;
            defs_ss << "const vec4 " << name << " = vec4("
                    << format_glsl_float(f[0]) << ", " << format_glsl_float(f[1]) << ", "
                    << format_glsl_float(f[2]) << ", " << format_glsl_float(f[3]) << ");\n";
            i += 1u + len;
            continue;
        }
        if (op == kOpDefI && len >= 5) {
            const u32 rnum = decode_reg_num(sh.words[i + 1]);
            i32 iv[4];
            std::memcpy(iv, &sh.words[i + 2], 16);
            const std::string name = "d_i" + std::to_string(rnum);
            em.def_i_names[rnum] = name;
            defs_ss << "const ivec4 " << name << " = ivec4("
                    << iv[0] << ", " << iv[1] << ", " << iv[2] << ", " << iv[3] << ");\n";
            i += 1u + len;
            continue;
        }
        if (op == kOpDefB && len >= 2) {
            const u32 rnum = decode_reg_num(sh.words[i + 1]);
            const bool bv = (sh.words[i + 2] != 0);
            const std::string name = "d_b" + std::to_string(rnum);
            em.def_b_names[rnum] = name;
            defs_ss << "const bool " << name << " = " << (bv ? "true" : "false") << ";\n";
            i += 1u + len;
            continue;
        }
        if (op == kOpDcl || op == kOpNop || op == kOpPhase) {
            i += 1u + len;
            continue;
        }

        // Unpack parameter tokens (handling optional relative-address tokens and predicate token).
        struct Operand {
            u32 tok = 0;
            bool has_rel = false;
            std::string rel_expr;
        };
        std::vector<Operand> ops;
        usize p = 1;
        while (p <= len) {
            Operand o{};
            o.tok = sh.words[i + p];
            if (decode_has_rel_addr(o.tok) && sh.major_version == 1) {
                o.has_rel = true;
                em.uses_a0 = true;
                o.rel_expr = "a0.x";
                ++p;
            } else if (decode_has_rel_addr(o.tok) && p + 1 <= len) {
                o.has_rel = true;
                const u32 address = sh.words[i + p + 1];
                if (decode_reg_type(address) == kRegLoop) em.uses_aL = true;
                else em.uses_a0 = true;
                o.rel_expr = em.format_rel_offset(address);
                p += 2;
            } else {
                p += 1;
            }
            ops.push_back(std::move(o));
        }

        std::string pred_mask;
        if (predicated) {
            em.uses_p0 = true;
            pred_mask = predicate_vector(ops[1].tok);
            ops.erase(ops.begin() + 1);
        }

        auto src_s = [&](usize idx) -> std::string {
            if (idx >= ops.size()) return "vec4(0.0)";
            return em.format_src(ops[idx].tok, ops[idx].has_rel ? &ops[idx].rel_expr : nullptr);
        };
        auto emit_dst = [&](const std::string& rhs) {
            if (ops.empty()) return;
            body_ss << em.emit_assign(ops[0].tok, ops[0].has_rel ? &ops[0].rel_expr : nullptr, rhs, pred_mask);
        };

        switch (op) {
            case kOpMov:
            case kOpMovA:
                emit_dst(src_s(1));
                break;
            case kOpAdd:
                emit_dst("(" + src_s(1) + " + " + src_s(2) + ")");
                break;
            case kOpSub:
                emit_dst("(" + src_s(1) + " - " + src_s(2) + ")");
                break;
            case kOpMul:
                emit_dst("(" + src_s(1) + " * " + src_s(2) + ")");
                break;
            case kOpMad:
                emit_dst("(" + src_s(1) + " * " + src_s(2) + " + " + src_s(3) + ")");
                break;
            case kOpDp3:
                emit_dst("vec4(dot((" + src_s(1) + ").xyz, (" + src_s(2) + ").xyz))");
                break;
            case kOpDp4:
                emit_dst("vec4(dot(" + src_s(1) + ", " + src_s(2) + "))");
                break;
            case kOpDp2Add:
                emit_dst("vec4(dot((" + src_s(1) + ").xy, (" + src_s(2) + ").xy) + (" + src_s(3) + ").x)");
                break;
            case kOpMin:
                emit_dst("min(" + src_s(1) + ", " + src_s(2) + ")");
                break;
            case kOpMax:
                emit_dst("max(" + src_s(1) + ", " + src_s(2) + ")");
                break;
            case kOpSlt:
                emit_dst("vec4(lessThan(" + src_s(1) + ", " + src_s(2) + "))");
                break;
            case kOpSge:
                emit_dst("vec4(greaterThanEqual(" + src_s(1) + ", " + src_s(2) + "))");
                break;
            case kOpCmp:
                // D3D9 CMP: dst = (src0 >= 0.0) ? src1 : src2
                emit_dst("mix(" + src_s(3) + ", " + src_s(2) + ", greaterThanEqual(" + src_s(1) + ", vec4(0.0)))");
                break;
            case kOpCnd:
                emit_dst("mix(" + src_s(3) + ", " + src_s(2) + ", greaterThan(" + src_s(1) + ", vec4(0.5)))");
                break;
            case kOpLrp:
                // D3D9 LRP: dst = src0 * (src1 - src2) + src2 = mix(src2, src1, src0)
                emit_dst("mix(" + src_s(3) + ", " + src_s(2) + ", " + src_s(1) + ")");
                break;
            case kOpFrc:
                emit_dst("fract(" + src_s(1) + ")");
                break;
            case kOpAbs:
                emit_dst("abs(" + src_s(1) + ")");
                break;
            case kOpSgn:
                emit_dst("sign(" + src_s(1) + ")");
                break;
            case kOpRcp:
                emit_dst("vec4(gm_rcp((" + src_s(1) + ").x))");
                break;
            case kOpRsq:
                emit_dst("vec4(gm_rsq((" + src_s(1) + ").x))");
                break;
            case kOpExp:
            case kOpExpP:
                emit_dst("vec4(exp2((" + src_s(1) + ").x))");
                break;
            case kOpLog:
            case kOpLogP:
                emit_dst("vec4(gm_log2((" + src_s(1) + ").x))");
                break;
            case kOpPow:
                emit_dst("vec4(gm_pow((" + src_s(1) + ").x, (" + src_s(2) + ").x))");
                break;
            case kOpNrm:
                emit_dst("gm_nrm(" + src_s(1) + ")");
                break;
            case kOpCrs:
                emit_dst("vec4(cross((" + src_s(1) + ").xyz, (" + src_s(2) + ").xyz), 0.0)");
                break;
            case kOpLit:
                emit_dst("gm_lit(" + src_s(1) + ")");
                break;
            case kOpDst:
                emit_dst("vec4(1.0, (" + src_s(1) + ").y * (" + src_s(2) + ").y, (" + src_s(1) + ").z, (" + src_s(2) + ").w)");
                break;
            case kOpSinCos:
                if (ops.size() >= 2) {
                    const std::string sc_expr = "gm_sincos((" + src_s(1) + ").x)";
                    emit_dst(sc_expr);
                }
                break;
            case kOpM4x4:
            case kOpM4x3:
            case kOpM3x4:
            case kOpM3x3:
            case kOpM3x2: {
                if (ops.size() >= 3) {
                    const u32 rows = matrix_rows(op);
                    const bool dp4 = (op == kOpM4x4 || op == kOpM4x3);
                    const std::string v = src_s(1);
                    const u32 mtype = decode_reg_type(ops[2].tok);
                    const u32 mbase = decode_reg_num(ops[2].tok);
                    std::ostringstream ms;
                    ms << "vec4(";
                    for (u32 r = 0; r < 4; ++r) {
                        if (r > 0) ms << ", ";
                        if (r < rows) {
                            const std::string row_reg = em.format_base_reg(
                                mtype, mbase + r, false, ops[2].has_rel ? &ops[2].rel_expr : nullptr);
                            if (dp4) ms << "dot(" << v << ", " << row_reg << ")";
                            else ms << "dot((" << v << ").xyz, (" << row_reg << ").xyz)";
                        } else {
                            ms << "0.0";
                        }
                    }
                    ms << ")";
                    emit_dst(ms.str());
                }
                break;
            }
            case kOpTex:
                if (!is_vs && sh.major_version == 1) {
                    const u32 stage = decode_reg_num(ops[0].tok);
                    const std::string coord = sh.minor_version == 4 ? src_s(1) : em.texcoord_input(stage);
                    emit_dst("texture(" + em.sampler_prefix() + std::to_string(stage) + ", (" + coord + ").xy)");
                } else if (ops.size() >= 3) {
                    const std::string coord = src_s(1);
                    const std::string samp = src_s(2);
                    const u32 tex_mode = ctrl & 3u;
                    if (tex_mode == 1u) {
                        // texldp (projective)
                        emit_dst("textureProj(" + samp + ", (" + coord + ").xyw)");
                    } else if (tex_mode == 2u && !is_vs) {
                        // texldb (LOD bias)
                        emit_dst("texture(" + samp + ", (" + coord + ").xy, (" + coord + ").w)");
                    } else if (is_vs) {
                        emit_dst("textureLod(" + samp + ", (" + coord + ").xy, 0.0)");
                    } else {
                        emit_dst("texture(" + samp + ", (" + coord + ").xy)");
                    }
                }
                break;
            case kOpTexLdl:
                if (ops.size() >= 3) {
                    const std::string coord = src_s(1);
                    const std::string samp = src_s(2);
                    emit_dst("textureLod(" + samp + ", (" + coord + ").xy, (" + coord + ").w)");
                }
                break;
            case kOpTexLdd:
                if (ops.size() >= 5) {
                    emit_dst("textureGrad(" + src_s(2) + ", (" + src_s(1) + ").xy, (" + src_s(3) + ").xy, (" + src_s(4) + ").xy)");
                }
                break;
            case kOpTexKill:
                if (!is_vs && !ops.empty()) {
                    const u32 dst = ops[0].tok;
                    std::string v = sh.major_version == 1 && sh.minor_version < 4
                        ? em.texcoord_input(decode_reg_num(dst))
                        : em.format_base_reg(decode_reg_type(dst), decode_reg_num(dst), false, nullptr);
                    if (!pred_mask.empty()) v = "mix(vec4(0.0), " + v + ", " + pred_mask + ")";
                    body_ss << em.ind() << "if (any(lessThan((" << v << ").xyz, vec3(0.0)))) discard;\n";
                }
                break;
            case kOpDsx:
                if (!is_vs) emit_dst("dFdx(" + src_s(1) + ")");
                break;
            case kOpDsy:
                if (!is_vs) emit_dst("dFdy(" + src_s(1) + ")");
                break;
            case kOpTexCoord:
                if (!is_vs && !ops.empty()) {
                    const u32 rn = decode_reg_num(ops[0].tok);
                    if (sh.minor_version == 4) emit_dst(src_s(1));
                    else emit_dst("vec4(clamp((" + em.texcoord_input(rn) + ").xyz, 0.0, 1.0), 1.0)");
                }
                break;
            case kOpIf:
                if (!ops.empty()) {
                    const u32 cond = ops[0].tok;
                    if (decode_reg_type(cond) == kRegPredicate) em.uses_p0 = true;
                    const std::string test = decode_reg_type(cond) == kRegPredicate ? predicate_scalar(cond) : src_s(0);
                    body_ss << em.ind() << "if (" << test << ") {\n";
                    ++em.indent;
                }
                break;
            case kOpIfC:
                if (ops.size() >= 2) {
                    body_ss << em.ind() << "if ((" << src_s(0) << ").x " << cmp_op_str(ctrl)
                            << " (" << src_s(1) << ").x) {\n";
                    ++em.indent;
                }
                break;
            case kOpElse:
                em.indent = std::max(1, em.indent - 1);
                body_ss << em.ind() << "} else {\n";
                ++em.indent;
                break;
            case kOpEndIf:
                em.indent = std::max(1, em.indent - 1);
                body_ss << em.ind() << "}\n";
                break;
            case kOpRep:
                if (!ops.empty()) {
                    const std::string iv = "_rep" + std::to_string(em.loop_counter++);
                    body_ss << em.ind() << "for (int " << iv << " = 0; " << iv << " < (" << src_s(0) << ").x; ++" << iv << ") {\n";
                    ++em.indent;
                }
                break;
            case kOpEndRep:
            case kOpEndLoop:
                em.indent = std::max(1, em.indent - 1);
                body_ss << em.ind() << "}\n";
                break;
            case kOpLoop:
                if (ops.size() >= 2) {
                    em.uses_aL = true;
                    const std::string iv = "_lp" + std::to_string(em.loop_counter++);
                    const std::string cnt = src_s(1);
                    body_ss << em.ind() << "aL = (" << cnt << ").y;\n";
                    body_ss << em.ind() << "for (int " << iv << " = 0; " << iv << " < (" << cnt << ").x; ++" << iv << ", aL += (" << cnt << ").z) {\n";
                    ++em.indent;
                }
                break;
            case kOpBreak:
                body_ss << em.ind() << "break;\n";
                break;
            case kOpBreakC:
                if (ops.size() >= 2) {
                    body_ss << em.ind() << "if ((" << src_s(0) << ").x " << cmp_op_str(ctrl)
                            << " (" << src_s(1) << ").x) break;\n";
                }
                break;
            case kOpBreakP:
                if (!ops.empty()) {
                    em.uses_p0 = true;
                    body_ss << em.ind() << "if (" << predicate_scalar(ops[0].tok) << ") break;\n";
                }
                break;
            case kOpSetP:
                if (ops.size() >= 3) {
                    em.uses_p0 = true;
                    const std::string m = mask_to_str(decode_write_mask(ops[0].tok));
                    const std::string a = src_s(1);
                    const std::string b = src_s(2);
                    const char* fn = "equal";
                    switch (ctrl & 7u) {
                        case 1: fn = "greaterThan"; break;
                        case 2: fn = "equal"; break;
                        case 3: fn = "greaterThanEqual"; break;
                        case 4: fn = "lessThan"; break;
                        case 5: fn = "notEqual"; break;
                        case 6: fn = "lessThanEqual"; break;
                        default: break;
                    }
                    const std::string result = std::string(fn) + "(" + a + ", " + b + ")";
                    body_ss << em.ind() << "p0." << m << " = " << result << "." << m << ";\n";
                }
                break;
            case kOpRet:
                body_ss << em.ind() << "return;\n";
                break;
            default:
                GM_LOG_WARN("gfx", "dx9_transpiler: unhandled D3D9 opcode %u", op);
                break;
        }

        i += 1u + len;
    }

    // Assemble complete GLSL source.
    std::ostringstream out;
    if (gles3) {
        out << "#version 300 es\n"
            << "precision highp float;\n"
            << "precision highp int;\n"
            << "precision highp sampler2D;\n\n";
    } else {
        out << "#version 330 core\n\n";
    }

    // Standard D3D9 math helpers avoiding driver UB on rcp/rsq/log2/pow/nrm/sincos/lit.
    out << "float gm_rcp(float x) { return x == 0.0 ? 0.0 : (1.0 / x); }\n"
        << "float gm_rsq(float x) { float ax = abs(x); return ax <= 0.0 ? 0.0 : inversesqrt(ax); }\n"
        << "float gm_log2(float x) { return log2(max(abs(x), 1e-30)); }\n"
        << "float gm_pow(float a, float b) { float aa = abs(a); return (aa == 0.0 && b == 0.0) ? 1.0 : pow(max(aa, 1e-30), b); }\n"
        << "vec4 gm_nrm(vec4 v) { float d = dot(v.xyz, v.xyz); return d <= 0.0 ? vec4(0.0) : (v * inversesqrt(d)); }\n"
        << "vec4 gm_sincos(float a) { return vec4(cos(a), sin(a), 0.0, 1.0); }\n"
        << "vec4 gm_lit(vec4 s) { float p = clamp(s.w, -128.0, 128.0); return vec4(1.0, max(s.x, 0.0), (s.x > 0.0 && s.y > 0.0) ? pow(s.y, p) : 0.0, 1.0); }\n\n";

    // Runtime uniforms
    if (is_vs) {
        out << "uniform float u_gm_yflip;\n"
            << "uniform vec2 u_pixel_center_offset;\n";
    } else {
        out << "uniform float u_gm_yflip;\n"
            << "uniform vec4 u_gm_viewport;\n"
            << "uniform int u_alpha_test;\n"
            << "uniform float u_alpha_ref;\n"
            << "uniform int u_alpha_func;\n";
    }

    if (sh.uses_const_f && sh.max_const_f > 0) {
        out << "uniform vec4 " << em.const_prefix() << "[" << sh.max_const_f << "];\n";
    }
    if (sh.uses_const_i && sh.max_const_i > 0) {
        out << "uniform ivec4 " << em.iconst_prefix() << "[" << sh.max_const_i << "];\n";
    }
    if (sh.uses_const_b && sh.max_const_b > 0) {
        out << "uniform int " << em.bconst_prefix() << "[" << sh.max_const_b << "];\n";
    }

    const u32 max_samplers = is_vs ? 4u : 8u;
    for (u32 s = 0; s < max_samplers; ++s) {
        if (sh.sampler_mask & (1u << s)) {
            out << "uniform sampler2D " << em.sampler_prefix() << s << ";\n";
        }
    }
    out << "\n";

    // Inputs / Outputs
    out << io_ss.str() << "\n";

    // Immediate DEF/DEFI/DEFB constants
    if (!defs_ss.str().empty()) {
        out << defs_ss.str() << "\n";
    }

    // main()
    out << "void main() {\n";
    if (!is_vs && sh.major_version == 1) em.used_temps.insert(0);
    for (u32 stage = 0; stage < 4; ++stage) {
        if (em.writable_texcoords & (1u << stage))
            out << "    vec4 t" << stage << " = " << em.texcoord_input(stage) << ";\n";
    }
    std::vector<u32> sorted_temps(em.used_temps.begin(), em.used_temps.end());
    std::sort(sorted_temps.begin(), sorted_temps.end());
    for (u32 t : sorted_temps) {
        out << "    vec4 r" << t << " = vec4(0.0);\n";
    }
    if (em.uses_a0) out << "    ivec4 a0 = ivec4(0);\n";
    if (em.uses_aL) out << "    int aL = 0;\n";
    if (em.uses_p0) out << "    bvec4 p0 = bvec4(false);\n";
    if (is_vs) {
        out << "    vec4 o_pos = vec4(0.0, 0.0, 0.0, 1.0);\n";
        for (const auto& v : declared_varyings) {
            out << "    " << v << " = vec4(0.0, 0.0, 0.0, 1.0);\n";
        }
    } else {
        for (u32 mrt = 0; mrt < 4; ++mrt) {
            if (sh.mrt_mask & (1u << mrt)) {
                out << "    o_col" << mrt << " = vec4(0.0);\n";
            }
        }
        if (em.uses_vpos) {
            out << "    vec4 gm_vpos = vec4(gl_FragCoord.x - 0.5, "
                << "(u_gm_yflip < 0.0 ? gl_FragCoord.y : (u_gm_viewport.w - gl_FragCoord.y)) - 0.5, "
                << "gl_FragCoord.z, gl_FragCoord.w);\n";
        }
    }

    out << body_ss.str();

    if (is_vs) {
        // Renderer projection matrices already map Z into OpenGL [-w, +w] clip space;
        // Keep rasterization adjustments separate from the D3D matrices visible to shaders.
        out << "    gl_Position = vec4(o_pos.x, o_pos.y * u_gm_yflip, o_pos.z, o_pos.w);\n"
            << "    gl_Position.xy += u_pixel_center_offset * gl_Position.w;\n";
        if (gles3 && body_ss.str().find("gl_PointSize") == std::string::npos) {
            out << "    gl_PointSize = 1.0;\n";
        }
    } else {
        if (sh.major_version == 1) out << "    o_col0 = r0;\n";
        // D3D9 alpha test still runs after pixel shaders when D3DRS_ALPHATESTENABLE is set.
        out << "    if (u_alpha_test != 0) {\n"
            << "        float a = o_col0.a;\n"
            << "        bool pass = true;\n"
            << "        if (u_alpha_func == 1) pass = false;\n"
            << "        else if (u_alpha_func == 2) pass = (a < u_alpha_ref);\n"
            << "        else if (u_alpha_func == 3) pass = (abs(a - u_alpha_ref) < 0.002);\n"
            << "        else if (u_alpha_func == 4) pass = (a <= u_alpha_ref + 0.0001);\n"
            << "        else if (u_alpha_func == 5) pass = (a > u_alpha_ref);\n"
            << "        else if (u_alpha_func == 6) pass = (abs(a - u_alpha_ref) >= 0.002);\n"
            << "        else if (u_alpha_func == 7) pass = (a >= u_alpha_ref - 0.0001);\n"
            << "        if (!pass) discard;\n"
            << "    }\n";
    }
    out << "}\n";

    return out.str();
}

const char* ff_texcoord_glsl() noexcept {
    return R"(
uniform int u_texcoord_mode;

vec4 ff_texcoord(vec2 uv, vec3 pos, vec3 normal) {
    if (u_texcoord_mode == 0) return vec4(uv, 0.0, 1.0);
    if (u_texcoord_mode == 2) return vec4(pos, 1.0);

    mat3 mv = mat3(u_view * u_world);
    vec3 n = normal;
    if (determinant(mv) != 0.0) n = transpose(inverse(mv)) * normal;
    float nlen = length(n);
    if (nlen > 0.0) n /= nlen;
    if (u_texcoord_mode == 1) return vec4(n, 1.0);

    float plen = length(pos);
    vec3 r = reflect(plen > 0.0 ? pos / plen : vec3(0.0), n);
    if (u_texcoord_mode == 3) return vec4(r, 1.0);

    // GM8's D3D9 sphere map is centered on the negative camera Z direction.
    float diameter = 2.0 * length(r - vec3(0.0, 0.0, 1.0));
    vec2 offset = diameter > 0.0 ? r.xy / diameter : vec2(0.0);
    return vec4(vec2(0.5) + offset, 0.0, 1.0);
}
)";
}

std::string build_ff_compat_vs_glsl(bool gles3) {
    std::ostringstream out;
    if (gles3) {
        out << "#version 300 es\nprecision highp float;\n";
    } else {
        out << "#version 330 core\n";
    }
    out << R"(
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_norm;
layout(location = 2) in vec4 a_uv;
layout(location = 3) in vec4 a_col;

uniform mat4 u_world;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec4 u_model_tint;
uniform float u_gm_yflip;
uniform vec2 u_pixel_center_offset;

out vec4 v_TEXCOORD0;
out vec4 v_COLOR0;
out vec4 v_NORMAL0;
out vec4 v_POSITION0;
)" << ff_texcoord_glsl() << R"(
void main() {
    vec4 wp = u_world * vec4(a_pos.xyz, 1.0);
    vec4 vp = u_view * wp;
    vec4 cp = u_proj * vp;
    gl_Position = vec4(cp.x, cp.y * u_gm_yflip, cp.z, cp.w);
    gl_Position.xy += u_pixel_center_offset * gl_Position.w;
    gl_PointSize = 1.0;
    v_TEXCOORD0 = ff_texcoord(a_uv.xy, vp.xyz, a_norm.xyz);
    v_COLOR0 = a_col * u_model_tint;
    v_NORMAL0 = vec4(a_norm.xyz, 0.0);
    v_POSITION0 = cp;
}
)";
    return out.str();
}

std::string build_ff_compat_ps_glsl(bool gles3) {
    std::ostringstream out;
    if (gles3) {
        out << "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\n";
    } else {
        out << "#version 330 core\n";
    }
    out << R"(
in vec4 v_TEXCOORD0;
in vec4 v_COLOR0;

uniform sampler2D u_s0;
uniform int u_tex_blending;
uniform int u_alpha_test;
uniform float u_alpha_ref;
uniform int u_alpha_func;

layout(location = 0) out vec4 o_col0;

void main() {
    vec4 tc = texture(u_s0, v_TEXCOORD0.xy);
    vec4 res = (u_tex_blending != 0) ? (tc * v_COLOR0) : tc;
    if (u_alpha_test != 0) {
        float a = res.a;
        bool pass = true;
        if (u_alpha_func == 1) pass = false;
        else if (u_alpha_func == 2) pass = (a < u_alpha_ref);
        else if (u_alpha_func == 3) pass = (abs(a - u_alpha_ref) < 0.002);
        else if (u_alpha_func == 4) pass = (a <= u_alpha_ref + 0.0001);
        else if (u_alpha_func == 5) pass = (a > u_alpha_ref);
        else if (u_alpha_func == 6) pass = (abs(a - u_alpha_ref) >= 0.002);
        else if (u_alpha_func == 7) pass = (a >= u_alpha_ref - 0.0001);
        if (!pass) discard;
    }
    o_col0 = res;
}
)";
    return out.str();
}

std::unordered_set<std::string> ff_compat_vs_output_semantics() {
    return {"TEXCOORD0", "COLOR0", "NORMAL0", "POSITION0"};
}

} // namespace gm
