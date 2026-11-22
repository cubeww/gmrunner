#include "graphics/touch_overlay.h"

#include "base/encoding.h"
#include "base/log.h"
#include "platform/clock.h"
#include "graphics/builtin_font.h"
#include "graphics/gl_headers.h"
#include "platform/vfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace gm {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 6.28318530717958647692f;
constexpr f32 kGridStep = 0.025f;
constexpr f32 kMinButtonSize = 0.026f;
constexpr f32 kMaxButtonSize = 0.160f;
constexpr f32 kMinDpadSize = 0.085f;
constexpr f32 kMaxDpadSize = 0.280f;

struct ColorStyle {
    f32 fill_r, fill_g, fill_b;
    f32 press_r, press_g, press_b;
    f32 ring_r, ring_g, ring_b;
};

constexpr std::array<ColorStyle, 6> kPalette = {{
    {0.13f, 0.17f, 0.24f,  0.30f, 0.60f, 0.92f,  0.88f, 0.94f, 1.00f}, // 0: Slate/Blue
    {0.10f, 0.22f, 0.30f,  0.18f, 0.72f, 0.96f,  0.58f, 0.90f, 1.00f}, // 1: Cyan
    {0.28f, 0.19f, 0.09f,  0.94f, 0.58f, 0.18f,  1.00f, 0.80f, 0.50f}, // 2: Amber
    {0.10f, 0.25f, 0.18f,  0.24f, 0.80f, 0.52f,  0.58f, 0.96f, 0.76f}, // 3: Emerald
    {0.29f, 0.12f, 0.15f,  0.92f, 0.30f, 0.36f,  1.00f, 0.64f, 0.68f}, // 4: Rose
    {0.22f, 0.14f, 0.30f,  0.66f, 0.42f, 0.96f,  0.82f, 0.68f, 1.00f}, // 5: Violet
}};

struct TouchKeyInfo {
    u8 vk;
    const char* short_label;
    const char* config_name;
    u8 tab; // 0=ACTION, 1=A-Z, 2=0-9/F, 3=NUM/SYM
};

constexpr TouchKeyInfo kTouchKeys[] = {
    // Tab 0: ACTION / MODIFIERS / NAV (24 keys -> 6x4)
    {160, "LSHF", "LSHIFT", 0},
    {161, "RSHF", "RSHIFT", 0},
    {162, "LCTL", "LCTRL",  0},
    {163, "RCTL", "RCTRL",  0},
    {164, "LALT", "LALT",   0},
    {165, "RALT", "RALT",   0},
    {32,  "SPC",  "SPACE",  0},
    {13,  "ENT",  "ENTER",  0},
    {27,  "ESC",  "ESCAPE", 0},
    {9,   "TAB",  "TAB",    0},
    {8,   "BKSP", "BACKSPACE", 0},
    {46,  "DEL",  "DELETE", 0},
    {37,  "LEFT", "LEFT",   0},
    {38,  "UP",   "UP",     0},
    {39,  "RGHT", "RIGHT",  0},
    {40,  "DOWN", "DOWN",   0},
    {45,  "INS",  "INSERT", 0},
    {19,  "PAUS", "PAUSE",  0},
    {36,  "HOME", "HOME",   0},
    {35,  "END",  "END",    0},
    {33,  "PGUP", "PAGEUP", 0},
    {34,  "PGDN", "PAGEDOWN", 0},
    {20,  "CAPS", "CAPSLOCK", 0},
    {44,  "PRSC", "PRINTSCREEN", 0},

    // Tab 1: A-Z (26 keys -> 7x4)
    {'A', "A", "A", 1}, {'B', "B", "B", 1}, {'C', "C", "C", 1}, {'D', "D", "D", 1},
    {'E', "E", "E", 1}, {'F', "F", "F", 1}, {'G', "G", "G", 1}, {'H', "H", "H", 1},
    {'I', "I", "I", 1}, {'J', "J", "J", 1}, {'K', "K", "K", 1}, {'L', "L", "L", 1},
    {'M', "M", "M", 1}, {'N', "N", "N", 1}, {'O', "O", "O", 1}, {'P', "P", "P", 1},
    {'Q', "Q", "Q", 1}, {'R', "R", "R", 1}, {'S', "S", "S", 1}, {'T', "T", "T", 1},
    {'U', "U", "U", 1}, {'V', "V", "V", 1}, {'W', "W", "W", 1}, {'X', "X", "X", 1},
    {'Y', "Y", "Y", 1}, {'Z', "Z", "Z", 1},

    // Tab 2: 0-9 & F1-F12 (22 keys -> 6x4)
    {'1', "1", "1", 2}, {'2', "2", "2", 2}, {'3', "3", "3", 2}, {'4', "4", "4", 2},
    {'5', "5", "5", 2}, {'6', "6", "6", 2}, {'7', "7", "7", 2}, {'8', "8", "8", 2},
    {'9', "9", "9", 2}, {'0', "0", "0", 2},
    {112, "F1",  "F1",  2}, {113, "F2",  "F2",  2}, {114, "F3",  "F3",  2}, {115, "F4",  "F4",  2},
    {116, "F5",  "F5",  2}, {117, "F6",  "F6",  2}, {118, "F7",  "F7",  2}, {119, "F8",  "F8",  2},
    {120, "F9",  "F9",  2}, {121, "F10", "F10", 2}, {122, "F11", "F11", 2}, {123, "F12", "F12", 2},

    // Tab 3: NUMPAD & SYMBOLS (26 keys -> 7x4)
    {96,  "N0", "NUM0", 3}, {97,  "N1", "NUM1", 3}, {98,  "N2", "NUM2", 3}, {99,  "N3", "NUM3", 3},
    {100, "N4", "NUM4", 3}, {101, "N5", "NUM5", 3}, {102, "N6", "NUM6", 3}, {103, "N7", "NUM7", 3},
    {104, "N8", "NUM8", 3}, {105, "N9", "NUM9", 3}, {107, "N+", "NUMADD", 3}, {109, "N-", "NUMSUB", 3},
    {106, "N*", "NUMMUL", 3}, {111, "N/", "NUMDIV", 3}, {110, "N.", "NUMDEC", 3},
    {189, "-",  "MINUS", 3}, {187, "=", "EQUALS", 3}, {219, "[", "LBRACKET", 3}, {221, "]", "RBRACKET", 3},
    {186, ";",  "SEMICOLON", 3}, {222, "'", "APOSTROPHE", 3}, {188, ",", "COMMA", 3}, {190, ".", "PERIOD", 3},
    {191, "/",  "SLASH", 3}, {192, "`", "GRAVE", 3}, {220, "\\", "BACKSLASH", 3},
};

constexpr const char* kTabNames[4] = {"ACTION", "A-Z", "0-9/F", "NUM/SYM"};

enum EditorAction : i32 {
    kActNone = 0,
    kActFlipDock,
    kActAddBtn,
    kActAddDpad,
    kActOpacity,
    kActGrid,
    kActReset,
    kActDone,

    kActBindKey,
    kActBindDpad,
    kActSizeDown,
    kActSizeUp,
    kActToggleShape,
    kActToggleMode,
    kActCycleColor,
    kActDelete,

    kActPresetArrows,
    kActPresetWasd,
    kActPresetIjkl,
    kActPresetNumpad,
};

bool hit_control_shape(f32 x, f32 y, f32 cx, f32 cy, f32 rx, f32 ry, bool circle, f32 slop = 1.12f) noexcept {
    if (rx <= 0.0f || ry <= 0.0f) return false;
    const f32 dx = (x - cx) / rx;
    const f32 dy = (y - cy) / ry;
    if (circle) {
        return (dx * dx + dy * dy) <= (slop * slop);
    }
    return std::fabs(dx) <= slop && std::fabs(dy) <= slop;
}

const TouchKeyInfo* find_key_info(u8 vk) noexcept {
    for (const auto& info : kTouchKeys) {
        if (info.vk == vk) return &info;
    }
    return nullptr;
}

i32 tab_for_vk(u8 vk) noexcept {
    if (const auto* info = find_key_info(vk)) return static_cast<i32>(info->tab);
    return 0;
}

std::string dpad_preset_name(const std::array<u8, 4>& vk) {
    if (vk == std::array<u8, 4>{37, 38, 39, 40}) return "ARRW";
    if (vk == std::array<u8, 4>{'A', 'W', 'D', 'S'}) return "WASD";
    if (vk == std::array<u8, 4>{'J', 'I', 'L', 'K'}) return "IJKL";
    if (vk == std::array<u8, 4>{100, 104, 102, 98}) return "NUM";
    return "CUST";
}

f32 snap_value(f32 v, bool snap) noexcept {
    if (!snap) return v;
    return std::round(v / kGridStep) * kGridStep;
}

std::string sanitize_label(std::string_view raw) {
    std::string out;
    out.reserve(std::min<usize>(raw.size(), 10));
    for (char c : raw) {
        if (c >= 32 && c <= 126) {
            out.push_back(c);
            if (out.size() >= 10) break;
        }
    }
    return out;
}

std::string uppercase_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    }
    return out;
}

std::string button_display_label(const TouchControl& ctrl) {
    if (!ctrl.label.empty()) return ctrl.label;
    return TouchOverlay::vk_short_label(ctrl.vk);
}

} // namespace

TouchOverlay::TouchOverlay() {
    layout_ = make_default_layout(last_aspect_);
}

TouchOverlay::~TouchOverlay() {
    shutdown();
}

std::string TouchOverlay::vk_short_label(u8 vk) {
    if (vk == 16) return "SHFT";
    if (vk == 17) return "CTRL";
    if (vk == 18) return "ALT";
    if (const auto* info = find_key_info(vk)) {
        return info->short_label;
    }
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    if (vk == 0) return "---";
    return "K" + std::to_string(static_cast<int>(vk));
}

std::string TouchOverlay::vk_config_name(u8 vk) {
    if (vk == 16) return "SHIFT";
    if (vk == 17) return "CTRL";
    if (vk == 18) return "ALT";
    if (const auto* info = find_key_info(vk)) {
        return info->config_name;
    }
    return std::to_string(static_cast<int>(vk));
}

u8 TouchOverlay::parse_vk_token(std::string_view token) {
    if (token.empty()) return 0;
    // Try pure unsigned integer first.
    bool all_digits = true;
    for (char c : token) {
        if (c < '0' || c > '9') { all_digits = false; break; }
    }
    if (all_digits) {
        if (token.size() == 1) {
            // Single digit '0'..'9' might be key 0 or numeric vk code; numeric codes < 10 are 0, 8, 9.
            const int n = token[0] - '0';
            return static_cast<u8>(std::clamp(n, 0, 255));
        }
        int val = 0;
        for (char c : token) {
            val = val * 10 + (c - '0');
            if (val > 255) return 0;
        }
        return static_cast<u8>(val);
    }

    const std::string up = uppercase_ascii(token);
    if (up == "NONE" || up == "0") return 0;
    if (up == "SHIFT") return 16;
    if (up == "CTRL" || up == "CONTROL") return 17;
    if (up == "ALT") return 18;
    if (up == "RETURN") return 13;
    if (up == "ESC") return 27;
    if (up == "SPC") return 32;
    if (up == "LCONTROL") return 162;
    if (up == "RCONTROL") return 163;
    for (const auto& info : kTouchKeys) {
        if (up == info.config_name || up == info.short_label) {
            return info.vk;
        }
    }
    if (up.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(up[0]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return ch;
    }
    return 0;
}

TouchLayout TouchOverlay::make_default_layout(f32 aspect) {
    aspect = std::clamp(aspect, 0.42f, 3.2f);
    const f32 unit_y = std::min(1.0f, aspect);
    const f32 unit_x = std::min(1.0f, 1.0f / aspect);

    TouchLayout layout{};
    layout.opacity = 1.0f;
    layout.snap_to_grid = false;
    layout.controls.reserve(8);

    // 1. Bottom-left 8-way D-Pad (Arrows).
    TouchControl dpad{};
    dpad.kind = TouchControlKind::Dpad;
    dpad.shape = TouchControlShape::Circle;
    dpad.size = 0.165f;
    const f32 dpad_rx = dpad.size * unit_x;
    const f32 dpad_ry = dpad.size * unit_y;
    dpad.cx = std::max(dpad_rx + 0.035f, 0.14f);
    dpad.cy = 1.0f - std::max(dpad_ry + 0.055f, 0.22f);
    dpad.dpad_vk = {37, 38, 39, 40};
    dpad.color_idx = 0;
    layout.controls.push_back(dpad);

    // 2. Bottom-right 4-button action diamond (GM8 standard fangame / action layout).
    const f32 btn_size = 0.066f;
    const f32 btn_rx = btn_size * unit_x;
    const f32 btn_ry = btn_size * unit_y;
    const f32 cluster_cx = 1.0f - std::max(btn_rx * 2.5f + 0.04f, 0.15f);
    const f32 cluster_cy = dpad.cy;
    const f32 spread_x = btn_rx * 1.38f;
    const f32 spread_y = btn_ry * 1.38f;

    auto make_btn = [](f32 cx, f32 cy, f32 sz, f32 asp, TouchControlShape shape,
                       u8 vk, u8 vk_extra, u8 col_idx, std::string lbl = {}) {
        TouchControl b{};
        b.kind = TouchControlKind::Button;
        b.shape = shape;
        b.cx = cx;
        b.cy = cy;
        b.size = sz;
        b.aspect = asp;
        b.vk = vk;
        b.vk_extra = vk_extra;
        b.color_idx = col_idx;
        b.label = std::move(lbl);
        return b;
    };

    // Jump (LShift + Shift), Shoot (Z), Action (X), Space.
    layout.controls.push_back(make_btn(cluster_cx - spread_x, cluster_cy + spread_y * 0.30f,
                                       btn_size, 1.0f, TouchControlShape::Circle, 160, 16, 1, "SHFT"));
    layout.controls.push_back(make_btn(cluster_cx + spread_x, cluster_cy + spread_y * 0.15f,
                                       btn_size, 1.0f, TouchControlShape::Circle, 'Z', 0, 2));
    layout.controls.push_back(make_btn(cluster_cx - spread_x * 0.15f, cluster_cy - spread_y,
                                       btn_size, 1.0f, TouchControlShape::Circle, 'X', 0, 3));
    layout.controls.push_back(make_btn(cluster_cx + spread_x * 0.15f, cluster_cy + spread_y,
                                       btn_size * 0.88f, 1.0f, TouchControlShape::Circle, 32, 0, 0));

    // 3. Top-left utility pills: R (restart), Enter, Esc.
    const f32 util_size = 0.036f;
    const f32 util_asp = 1.55f;
    const f32 util_rx = util_size * unit_x * util_asp;
    const f32 util_ry = util_size * unit_y;
    const f32 top_y = std::max(util_ry + 0.018f, 0.052f);
    const f32 gap = util_rx * 2.25f;
    const f32 start_x = 0.025f + util_rx;

    layout.controls.push_back(make_btn(start_x, top_y, util_size, util_asp, TouchControlShape::Rect, 'R', 0, 0));
    layout.controls.push_back(make_btn(start_x + gap, top_y, util_size, util_asp, TouchControlShape::Rect, 13, 0, 0));
    layout.controls.push_back(make_btn(start_x + gap * 2.0f, top_y, util_size, util_asp, TouchControlShape::Rect, 27, 0, 0));

    return layout;
}

std::string TouchOverlay::serialize_layout(const TouchLayout& layout) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(4);
    out << "GMTOUCH1\n";
    out << "opacity " << std::clamp(layout.opacity, 0.20f, 1.0f) << '\n';
    out << "snap " << (layout.snap_to_grid ? 1 : 0) << '\n';
    for (const auto& c : layout.controls) {
        if (c.kind == TouchControlKind::Dpad) {
            out << "dpad "
                << std::clamp(c.cx, 0.0f, 1.0f) << ' '
                << std::clamp(c.cy, 0.0f, 1.0f) << ' '
                << std::clamp(c.size, kMinDpadSize, kMaxDpadSize) << ' '
                << static_cast<int>(c.dpad_vk[0]) << ' '
                << static_cast<int>(c.dpad_vk[1]) << ' '
                << static_cast<int>(c.dpad_vk[2]) << ' '
                << static_cast<int>(c.dpad_vk[3]) << ' '
                << static_cast<int>(c.color_idx % kPalette.size()) << ' '
                << std::quoted(sanitize_label(c.label)) << '\n';
        } else {
            out << "btn "
                << std::clamp(c.cx, 0.0f, 1.0f) << ' '
                << std::clamp(c.cy, 0.0f, 1.0f) << ' '
                << std::clamp(c.size, kMinButtonSize, kMaxButtonSize) << ' '
                << std::clamp(c.aspect, 0.6f, 3.0f) << ' '
                << static_cast<int>(c.vk) << ' '
                << static_cast<int>(c.vk_extra) << ' '
                << (c.shape == TouchControlShape::Rect ? "rect" : "circle") << ' '
                << (c.toggle_mode ? "toggle" : "hold") << ' '
                << static_cast<int>(c.color_idx % kPalette.size()) << ' '
                << std::quoted(sanitize_label(c.label)) << '\n';
        }
    }
    return out.str();
}

bool TouchOverlay::deserialize_layout(std::string_view text, TouchLayout& out) {
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());

    std::string header;
    if (!std::getline(in, header)) return false;
    if (!header.empty() && header.back() == '\r') header.pop_back();
    if (header != "GMTOUCH1") return false;

    TouchLayout parsed{};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ls(line);
        ls.imbue(std::locale::classic());
        std::string tag;
        if (!(ls >> tag)) continue;

        if (tag == "opacity") {
            f32 op = 1.0f;
            if (ls >> op && std::isfinite(op)) {
                parsed.opacity = std::clamp(op, 0.20f, 1.0f);
            }
        } else if (tag == "snap") {
            int s = 0;
            if (ls >> s) parsed.snap_to_grid = (s != 0);
        } else if (tag == "dpad") {
            if (parsed.controls.size() >= kMaxControls) continue;
            f32 cx = 0.14f, cy = 0.76f, sz = 0.165f;
            std::string k0, k1, k2, k3;
            int col = 0;
            std::string lbl;
            if (!(ls >> cx >> cy >> sz >> k0 >> k1 >> k2 >> k3)) continue;
            if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(sz)) continue;
            if (ls >> col) {
                (void)(ls >> std::quoted(lbl));
            }
            TouchControl c{};
            c.kind = TouchControlKind::Dpad;
            c.shape = TouchControlShape::Circle;
            c.cx = std::clamp(cx, 0.0f, 1.0f);
            c.cy = std::clamp(cy, 0.0f, 1.0f);
            c.size = std::clamp(sz, kMinDpadSize, kMaxDpadSize);
            c.dpad_vk = {parse_vk_token(k0), parse_vk_token(k1), parse_vk_token(k2), parse_vk_token(k3)};
            c.color_idx = static_cast<u8>(std::max(0, col) % static_cast<int>(kPalette.size()));
            c.label = sanitize_label(lbl);
            parsed.controls.push_back(std::move(c));
        } else if (tag == "btn") {
            if (parsed.controls.size() >= kMaxControls) continue;
            f32 cx = 0.5f, cy = 0.5f, sz = 0.067f, asp = 1.0f;
            std::string vk_tok, vk2_tok, shape_tok, mode_tok;
            int col = 0;
            std::string lbl;
            if (!(ls >> cx >> cy >> sz >> asp >> vk_tok >> vk2_tok >> shape_tok >> mode_tok)) continue;
            if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(sz) || !std::isfinite(asp)) continue;
            if (ls >> col) {
                (void)(ls >> std::quoted(lbl));
            }
            TouchControl c{};
            c.kind = TouchControlKind::Button;
            c.cx = std::clamp(cx, 0.0f, 1.0f);
            c.cy = std::clamp(cy, 0.0f, 1.0f);
            c.size = std::clamp(sz, kMinButtonSize, kMaxButtonSize);
            c.aspect = std::clamp(asp, 0.6f, 3.0f);
            c.vk = parse_vk_token(vk_tok);
            c.vk_extra = parse_vk_token(vk2_tok);
            c.shape = (uppercase_ascii(shape_tok) == "RECT") ? TouchControlShape::Rect : TouchControlShape::Circle;
            c.toggle_mode = (uppercase_ascii(mode_tok) == "TOGGLE");
            c.color_idx = static_cast<u8>(std::max(0, col) % static_cast<int>(kPalette.size()));
            c.label = sanitize_label(lbl);
            parsed.controls.push_back(std::move(c));
        }
    }

    out = std::move(parsed);
    return true;
}

Result<void> TouchOverlay::init(GlGraphicsDevice& device) {
    shutdown();
    device_ = &device;

    const bool is_es = device.caps().is_gles;
    const std::string header = is_es
        ? "#version 300 es\nprecision mediump float;\n"
        : "#version 330 core\n";

    const std::string vs_src = header + R"(
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_col;
out vec4 v_col;
void main() {
    v_col = a_col;
    gl_Position = vec4(a_pos.x * 2.0 - 1.0, 1.0 - a_pos.y * 2.0, 0.0, 1.0);
}
)";

    const std::string fs_src = header + R"(
in vec4 v_col;
out vec4 o_color;
void main() {
    o_color = v_col;
}
)";

    auto sh_res = device_->create_shader(vs_src, fs_src);
    if (!sh_res) return sh_res.error();
    shader_ = sh_res.value();

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, r)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (using_default_layout_) {
        layout_ = make_default_layout(last_aspect_);
    }
    update_system_pills(last_aspect_);
    initialized_ = true;
    return Result<void>::success();
}

void TouchOverlay::shutdown() noexcept {
    if (!initialized_) return;
    if (device_ && shader_.valid()) {
        device_->destroy_shader(shader_);
        shader_ = {};
    }
    if (vbo_ != 0) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_ != 0) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    verts_.clear();
    initialized_ = false;
}

Result<void> TouchOverlay::reload_gpu_resources() {
    if (!device_) return Error{"TouchOverlay::reload_gpu_resources called without device"};
    shader_ = {};
    vao_ = 0;
    vbo_ = 0;
    initialized_ = false;
    return init(*device_);
}

void TouchOverlay::set_enabled(bool enabled, InputState* input) {
    if (!enabled && editing_) {
        exit_edit_mode(input, true);
    }
    enabled_ = enabled;
}

void TouchOverlay::set_editing(bool editing, InputState* input) {
    if (editing == editing_) return;
    if (editing) {
        enter_edit_mode(input, false);
    } else {
        exit_edit_mode(input, true);
    }
}

void TouchOverlay::set_config_path(const std::filesystem::path& path, std::function<void()> on_saved) {
    config_path_ = path;
    on_saved_callback_ = std::move(on_saved);
    (void)load_layout_from_disk();
}

bool TouchOverlay::load_layout_from_disk() {
    if (config_path_.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(config_path_, ec) || ec) {
        return false;
    }
    std::ifstream in(config_path_, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    TouchLayout loaded{};
    if (!deserialize_layout(ss.str(), loaded)) {
        GM_LOG_WARN("touch", "Invalid touch layout file '%s'; using defaults",
                    Vfs::path_to_utf8(config_path_).c_str());
        return false;
    }
    layout_ = std::move(loaded);
    using_default_layout_ = false;
    layout_dirty_ = false;
    selected_idx_ = -1;
    GM_LOG_INFO("touch", "Loaded custom touch layout (%zu controls) from '%s'",
                layout_.controls.size(), Vfs::path_to_utf8(config_path_).c_str());
    return true;
}

bool TouchOverlay::save_layout_to_disk() {
    if (config_path_.empty()) {
        layout_dirty_ = false;
        return false;
    }
    std::error_code ec;
    if (config_path_.has_parent_path()) {
        std::filesystem::create_directories(config_path_.parent_path(), ec);
    }
    if (using_default_layout_) {
        std::filesystem::remove(config_path_, ec);
        if (ec) return false;
        layout_dirty_ = false;
        if (on_saved_callback_) on_saved_callback_();
        return true;
    }
    const std::string data = serialize_layout(layout_);
    std::filesystem::path tmp;
    std::FILE* file = nullptr;
    const u64 suffix = Clock::now_us();
    for (u64 i = 0; i < 32; ++i) {
        tmp = config_path_.parent_path() / (".gm-touch-" + std::to_string(suffix + i) + ".tmp");
#if defined(_WIN32)
        const int error = _wfopen_s(&file, tmp.c_str(), L"wbx");
#else
        file = std::fopen(tmp.c_str(), "wbx");
        const int error = errno;
#endif
        if (file || error != EEXIST) break;
    }
    if (!file) {
        GM_LOG_WARN("touch", "Cannot create temporary layout file for '%s'", Vfs::path_to_utf8(config_path_).c_str());
        return false;
    }
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    const bool closed = std::fclose(file) == 0;
    if (written && closed) std::filesystem::rename(tmp, config_path_, ec);
    if (!written || !closed || ec) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        GM_LOG_WARN("touch", "Failed to save touch layout '%s'", Vfs::path_to_utf8(config_path_).c_str());
        return false;
    }
    layout_dirty_ = false;
    if (on_saved_callback_) on_saved_callback_();
    GM_LOG_INFO("touch", "Saved touch layout (%zu controls) to '%s'",
                layout_.controls.size(), Vfs::path_to_utf8(config_path_).c_str());
    return true;
}

std::string TouchOverlay::export_layout_text() const {
    return serialize_layout(layout_);
}

bool TouchOverlay::import_layout_text(std::string_view text, bool persist) {
    TouchLayout parsed{};
    if (!deserialize_layout(text, parsed)) return false;
    layout_ = std::move(parsed);
    using_default_layout_ = false;
    selected_idx_ = -1;
    drag_mode_ = DragMode::None;
    modal_ = EditorModal::None;
    layout_dirty_ = true;
    if (persist) {
        (void)save_layout_to_disk();
    }
    return true;
}

void TouchOverlay::reset_to_default(bool persist) {
    layout_ = make_default_layout(last_aspect_);
    using_default_layout_ = true;
    selected_idx_ = -1;
    drag_mode_ = DragMode::None;
    modal_ = EditorModal::None;
    reset_confirm_ = false;
    layout_dirty_ = false;
    if (persist && !config_path_.empty()) {
        layout_dirty_ = true;
        (void)save_layout_to_disk();
    }
}

void TouchOverlay::update_system_pills(f32 aspect) noexcept {
    aspect = std::clamp(aspect, 0.42f, 3.2f);
    const f32 unit_y = std::min(1.0f, aspect);
    const f32 unit_x = std::min(1.0f, 1.0f / aspect);

    const f32 pill_ry = 0.036f * unit_y;
    const f32 pill_rx = 0.036f * unit_x * 1.48f;
    const f32 top_cy = std::max(pill_ry + 0.018f, 0.052f);

    const f32 toggle_cx = 1.0f - 0.024f - pill_rx;
    toggle_pill_ = RectBox{toggle_cx - pill_rx, top_cy - pill_ry, toggle_cx + pill_rx, top_cy + pill_ry};

    const f32 edit_rx = pill_rx * 1.18f;
    const f32 edit_cx = toggle_pill_.x0 - 0.014f - edit_rx;
    edit_pill_ = RectBox{edit_cx - edit_rx, top_cy - pill_ry, edit_cx + edit_rx, top_cy + pill_ry};
}

TouchOverlay::ResolvedGeom TouchOverlay::resolve_control(const TouchControl& ctrl, f32 aspect) const noexcept {
    aspect = std::clamp(aspect, 0.42f, 3.2f);
    const f32 unit_y = std::min(1.0f, aspect);
    const f32 unit_x = std::min(1.0f, 1.0f / aspect);

    const f32 sz = (ctrl.kind == TouchControlKind::Dpad)
        ? std::clamp(ctrl.size, kMinDpadSize, kMaxDpadSize)
        : std::clamp(ctrl.size, kMinButtonSize, kMaxButtonSize);
    const f32 asp = (ctrl.kind == TouchControlKind::Button && ctrl.shape == TouchControlShape::Rect)
        ? std::clamp(ctrl.aspect, 0.6f, 3.0f)
        : 1.0f;

    ResolvedGeom g{};
    g.ry = sz * unit_y;
    g.rx = sz * unit_x * asp;
    const f32 min_x = std::min(0.49f, g.rx + 0.005f);
    const f32 min_y = std::min(0.49f, g.ry + 0.005f);
    g.cx = std::clamp(ctrl.cx, min_x, 1.0f - min_x);
    g.cy = std::clamp(ctrl.cy, min_y, 1.0f - min_y);
    return g;
}

TouchOverlay::RectBox TouchOverlay::resize_handle_box(const TouchControl& ctrl, const ResolvedGeom& g, f32 aspect) const noexcept {
    aspect = std::clamp(aspect, 0.42f, 3.2f);
    const f32 unit_y = std::min(1.0f, aspect);
    const f32 unit_x = std::min(1.0f, 1.0f / aspect);
    const f32 hr_x = 0.024f * unit_x;
    const f32 hr_y = 0.024f * unit_y;

    const bool is_circle = (ctrl.kind == TouchControlKind::Dpad || ctrl.shape == TouchControlShape::Circle);
    const f32 off_x = g.rx * (is_circle ? 0.78f : 1.0f);
    const f32 off_y = g.ry * (is_circle ? 0.78f : 1.0f);

    // Flip inward if the bottom-right corner would fall off-screen.
    const f32 sx = (g.cx + off_x + hr_x > 0.985f) ? -1.0f : 1.0f;
    const f32 sy = (g.cy + off_y + hr_y > 0.985f) ? -1.0f : 1.0f;
    const f32 hx = g.cx + sx * off_x;
    const f32 hy = g.cy + sy * off_y;
    return RectBox{hx - hr_x, hy - hr_y, hx + hr_x, hy + hr_y};
}

void TouchOverlay::build_editor_toolbar(f32 aspect, std::vector<UiButton>& out) const {
    out.clear();
    const f32 bar_h = (aspect >= 1.0f) ? 0.072f : 0.052f;
    const f32 y0 = toolbar_at_bottom_ ? (0.985f - bar_h) : 0.110f;
    const f32 y1 = y0 + bar_h;

    struct Spec { const char* text; f32 weight; i32 act; bool active; bool accent; bool danger; };
    const i32 opac_pct = static_cast<i32>(std::lround(layout_.opacity * 100.0f));
    const std::string opac_lbl = "OP:" + std::to_string(opac_pct);
    const std::string reset_lbl = reset_confirm_ ? "SURE?" : "RESET";
    const char* dock_lbl = toolbar_at_bottom_ ? "^" : "v";

    const std::array<Spec, 7> specs = {{
        {dock_lbl,          0.55f, kActFlipDock, false, false, false},
        {"+BTN",            1.05f, kActAddBtn,   false, false, false},
        {"+PAD",            1.05f, kActAddDpad,  false, false, false},
        {opac_lbl.c_str(),  1.15f, kActOpacity,  false, false, false},
        {"GRID",            0.95f, kActGrid,     layout_.snap_to_grid, false, false},
        {reset_lbl.c_str(), 1.15f, kActReset,    false, false, reset_confirm_},
        {"DONE",            1.15f, kActDone,     false, true,  false},
    }};

    f32 total_w = 0.0f;
    for (const auto& s : specs) total_w += s.weight;
    const f32 margin_x = 0.018f;
    const f32 gap = 0.009f;
    const f32 avail = 1.0f - margin_x * 2.0f - gap * static_cast<f32>(specs.size() - 1);

    f32 x = margin_x;
    for (const auto& s : specs) {
        const f32 w = avail * (s.weight / total_w);
        out.push_back(UiButton{RectBox{x, y0, x + w, y1}, s.text, s.act, s.active, s.accent, s.danger, 255});
        x += w + gap;
    }
}

void TouchOverlay::build_inspector_bar(f32 aspect, std::vector<UiButton>& out) const {
    out.clear();
    if (selected_idx_ < 0 || static_cast<usize>(selected_idx_) >= layout_.controls.size()) {
        return;
    }
    const auto& ctrl = layout_.controls[static_cast<usize>(selected_idx_)];
    const f32 bar_h = (aspect >= 1.0f) ? 0.068f : 0.050f;
    const f32 top_bar_h = (aspect >= 1.0f) ? 0.072f : 0.052f;

    bool at_bottom = inspector_at_bottom_locked_;
    if (drag_mode_ == DragMode::None) {
        const ResolvedGeom g = resolve_control(ctrl, aspect);
        if (!toolbar_at_bottom_) {
            at_bottom = (g.cy < 0.44f);
        } else {
            at_bottom = (g.cy < 0.56f);
        }
    }

    f32 y0 = 0.0f;
    if (!toolbar_at_bottom_) {
        y0 = at_bottom ? (0.985f - bar_h) : (0.110f + top_bar_h + 0.012f);
    } else {
        y0 = at_bottom ? (0.985f - top_bar_h - 0.012f - bar_h) : 0.022f;
    }
    const f32 y1 = y0 + bar_h;

    struct Spec { std::string text; f32 weight; i32 act; bool active; bool accent; bool danger; u8 tint; };
    std::vector<Spec> specs;
    if (ctrl.kind == TouchControlKind::Button) {
        specs.push_back({"KEY:" + button_display_label(ctrl), 1.65f, kActBindKey, false, true, false, 255});
        specs.push_back({"S-", 0.72f, kActSizeDown, false, false, false, 255});
        specs.push_back({"S+", 0.72f, kActSizeUp, false, false, false, 255});
        specs.push_back({ctrl.shape == TouchControlShape::Circle ? "CIRC" : "RECT", 0.95f, kActToggleShape, false, false, false, 255});
        specs.push_back({ctrl.toggle_mode ? "TOGL" : "HOLD", 0.95f, kActToggleMode, ctrl.toggle_mode, false, false, 255});
        specs.push_back({"COL", 0.82f, kActCycleColor, false, false, false, static_cast<u8>(ctrl.color_idx % kPalette.size())});
        specs.push_back({"DEL", 0.88f, kActDelete, false, false, true, 255});
    } else {
        specs.push_back({"PAD:" + dpad_preset_name(ctrl.dpad_vk), 1.85f, kActBindDpad, false, true, false, 255});
        specs.push_back({"S-", 0.80f, kActSizeDown, false, false, false, 255});
        specs.push_back({"S+", 0.80f, kActSizeUp, false, false, false, 255});
        specs.push_back({"COL", 0.90f, kActCycleColor, false, false, false, static_cast<u8>(ctrl.color_idx % kPalette.size())});
        specs.push_back({"DEL", 0.95f, kActDelete, false, false, true, 255});
    }

    f32 total_w = 0.0f;
    for (const auto& s : specs) total_w += s.weight;
    const f32 margin_x = (ctrl.kind == TouchControlKind::Dpad) ? 0.08f : 0.018f;
    const f32 gap = 0.009f;
    const f32 avail = 1.0f - margin_x * 2.0f - gap * static_cast<f32>(specs.size() - 1);

    f32 x = margin_x;
    for (const auto& s : specs) {
        const f32 w = avail * (s.weight / total_w);
        out.push_back(UiButton{RectBox{x, y0, x + w, y1}, s.text, s.act, s.active, s.accent, s.danger, s.tint});
        x += w + gap;
    }
}

void TouchOverlay::build_key_picker_ui(f32 aspect, RectBox& card, RectBox& close_btn,
                                       std::vector<UiButton>& tabs, std::vector<UiButton>& keys) const {
    tabs.clear();
    keys.clear();

    const f32 margin_x = (aspect > 1.35f) ? 0.09f : 0.03f;
    const f32 margin_y = (aspect >= 1.0f) ? 0.08f : 0.14f;
    card = RectBox{margin_x, margin_y, 1.0f - margin_x, 1.0f - margin_y};

    const f32 pad_x = 0.016f;
    const f32 pad_y = 0.016f;
    const f32 header_h = (aspect >= 1.0f) ? 0.068f : 0.050f;
    const f32 close_w = std::min(0.11f, header_h * 1.15f / std::max(0.45f, aspect));
    close_btn = RectBox{card.x1 - pad_x - close_w, card.y0 + pad_y, card.x1 - pad_x, card.y0 + pad_y + header_h};

    // Category tabs row.
    const f32 tab_y0 = card.y0 + pad_y + header_h + 0.012f;
    const f32 tab_h = (aspect >= 1.0f) ? 0.066f : 0.048f;
    const f32 tab_gap = 0.010f;
    const f32 tab_w = (card.w() - pad_x * 2.0f - tab_gap * 3.0f) / 4.0f;
    for (i32 t = 0; t < 4; ++t) {
        const f32 tx0 = card.x0 + pad_x + static_cast<f32>(t) * (tab_w + tab_gap);
        tabs.push_back(UiButton{RectBox{tx0, tab_y0, tx0 + tab_w, tab_y0 + tab_h},
                                kTabNames[t], t, (picker_tab_ == t), false, false, 255});
    }

    // Collect keys in active tab.
    std::vector<const TouchKeyInfo*> tab_keys;
    for (const auto& info : kTouchKeys) {
        if (static_cast<i32>(info.tab) == picker_tab_) {
            tab_keys.push_back(&info);
        }
    }

    u8 current_vk = 0;
    if (!pending_add_button_ && selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
        const auto& c = layout_.controls[static_cast<usize>(selected_idx_)];
        if (dpad_bind_dir_ >= 0 && dpad_bind_dir_ < 4 && c.kind == TouchControlKind::Dpad) {
            current_vk = c.dpad_vk[static_cast<usize>(dpad_bind_dir_)];
        } else if (c.kind == TouchControlKind::Button) {
            current_vk = c.vk;
        }
    }

    const i32 cols = (tab_keys.size() > 24) ? 7 : 6;
    const i32 rows = std::max(1, static_cast<i32>((tab_keys.size() + static_cast<usize>(cols) - 1) / static_cast<usize>(cols)));
    const f32 grid_x0 = card.x0 + pad_x;
    const f32 grid_y0 = tab_y0 + tab_h + 0.016f;
    const f32 grid_w = card.w() - pad_x * 2.0f;
    const f32 grid_h = (card.y1 - pad_y) - grid_y0;
    const f32 cell_gap_x = 0.009f;
    const f32 cell_gap_y = 0.012f;
    const f32 cell_w = (grid_w - cell_gap_x * static_cast<f32>(cols - 1)) / static_cast<f32>(cols);
    const f32 cell_h = (grid_h - cell_gap_y * static_cast<f32>(rows - 1)) / static_cast<f32>(rows);

    for (usize i = 0; i < tab_keys.size(); ++i) {
        const i32 r = static_cast<i32>(i) / cols;
        const i32 c = static_cast<i32>(i) % cols;
        const f32 cx0 = grid_x0 + static_cast<f32>(c) * (cell_w + cell_gap_x);
        const f32 cy0 = grid_y0 + static_cast<f32>(r) * (cell_h + cell_gap_y);
        const auto* info = tab_keys[i];
        keys.push_back(UiButton{RectBox{cx0, cy0, cx0 + cell_w, cy0 + cell_h},
                                info->short_label, static_cast<i32>(info->vk),
                                (info->vk == current_vk), false, false, 255});
    }
}

void TouchOverlay::build_dpad_modal_ui(f32 aspect, RectBox& card, RectBox& close_btn,
                                       std::vector<UiButton>& presets, std::vector<UiButton>& dirs) const {
    presets.clear();
    dirs.clear();

    const f32 margin_x = (aspect > 1.35f) ? 0.16f : 0.05f;
    const f32 margin_y = (aspect >= 1.0f) ? 0.14f : 0.22f;
    card = RectBox{margin_x, margin_y, 1.0f - margin_x, 1.0f - margin_y};

    const f32 pad_x = 0.020f;
    const f32 pad_y = 0.018f;
    const f32 header_h = (aspect >= 1.0f) ? 0.066f : 0.048f;
    const f32 close_w = std::min(0.11f, header_h * 1.15f / std::max(0.45f, aspect));
    close_btn = RectBox{card.x1 - pad_x - close_w, card.y0 + pad_y, card.x1 - pad_x, card.y0 + pad_y + header_h};

    std::array<u8, 4> cur_vk = {37, 38, 39, 40};
    if (selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
        cur_vk = layout_.controls[static_cast<usize>(selected_idx_)].dpad_vk;
    }

    // Presets row.
    const f32 pr_y0 = card.y0 + pad_y + header_h + 0.018f;
    const f32 pr_h = (aspect >= 1.0f) ? 0.082f : 0.056f;
    const f32 pr_gap = 0.012f;
    const f32 pr_w = (card.w() - pad_x * 2.0f - pr_gap * 3.0f) / 4.0f;
    const std::string cur_preset = dpad_preset_name(cur_vk);

    struct PSpec { const char* name; const char* match; i32 act; };
    constexpr PSpec pspecs[4] = {
        {"ARROWS", "ARRW", kActPresetArrows},
        {"WASD",   "WASD", kActPresetWasd},
        {"IJKL",   "IJKL", kActPresetIjkl},
        {"NUMPAD", "NUM",  kActPresetNumpad},
    };
    for (i32 i = 0; i < 4; ++i) {
        const f32 px0 = card.x0 + pad_x + static_cast<f32>(i) * (pr_w + pr_gap);
        presets.push_back(UiButton{RectBox{px0, pr_y0, px0 + pr_w, pr_y0 + pr_h},
                                   pspecs[i].name, pspecs[i].act, (cur_preset == pspecs[i].match),
                                   false, false, 255});
    }

    // Individual 4-direction cross customization tiles below presets.
    const f32 cross_y0 = pr_y0 + pr_h + 0.032f;
    const f32 cross_y1 = card.y1 - pad_y;
    const f32 cross_cx = card.cx();
    const f32 cross_cy = (cross_y0 + cross_y1) * 0.5f;
    const f32 tile_h = std::min((cross_y1 - cross_y0) * 0.30f, 0.105f);
    const f32 tile_w = std::min((card.w() - pad_x * 2.0f) * 0.30f, 0.24f);
    const f32 step_x = tile_w * 1.08f;
    const f32 step_y = tile_h * 1.12f;

    auto make_dir = [&](i32 dir, f32 cx, f32 cy, const char* prefix) {
        dirs.push_back(UiButton{
            RectBox{cx - tile_w * 0.5f, cy - tile_h * 0.5f, cx + tile_w * 0.5f, cy + tile_h * 0.5f},
            std::string(prefix) + ":" + vk_short_label(cur_vk[static_cast<usize>(dir)]),
            dir, false, false, false, 255});
    };
    make_dir(0, cross_cx - step_x, cross_cy, "LT");
    make_dir(1, cross_cx, cross_cy - step_y, "UP");
    make_dir(2, cross_cx + step_x, cross_cy, "RT");
    make_dir(3, cross_cx, cross_cy + step_y, "DN");
}

void TouchOverlay::assert_vkey_with_modifiers(std::array<bool, 256>& keys, u8 vk) noexcept {
    if (vk == 0) return;
    keys[vk] = true;
    // Mirror GM8 aggregate modifier keys just like physical keyboard events.
    if (vk == 160 || vk == 161) keys[16] = true;
    if (vk == 162 || vk == 163) keys[17] = true;
    if (vk == 164 || vk == 165) keys[18] = true;
}

void TouchOverlay::release_all_virtual_keys(InputState& input) {
    for (usize vk = 0; vk < 256; ++vk) {
        if (active_vkeys_[vk]) {
            active_vkeys_[vk] = false;
            if (input.key_down[vk]) {
                input.key_down[vk] = false;
                input.key_released[vk] = true;
            }
            if (input.current_key == vk) input.current_key = 0;
        }
    }
    if (touch_mouse_active_) {
        if (input.mouse_down[1]) {
            input.mouse_down[1] = false;
            input.mouse_released[1] = true;
        }
        touch_mouse_active_ = false;
    }
}

void TouchOverlay::enter_edit_mode(InputState* input, bool from_pointer) {
    enabled_ = true;
    controls_visible_ = true;
    editing_ = true;
    if (input) {
        release_all_virtual_keys(*input);
    } else {
        active_vkeys_.fill(false);
        touch_mouse_active_ = false;
    }
    for (auto& c : layout_.controls) {
        c.pressed = false;
        c.latched = false;
        c.has_active_finger = false;
        c.dpad_dir.fill(false);
    }
    toggle_pill_pressed_ = false;
    edit_pill_pressed_ = false;
    play_mouse_captured_ = false;

    selected_idx_ = -1;
    drag_mode_ = DragMode::None;
    modal_ = EditorModal::None;
    pending_add_button_ = false;
    dpad_bind_dir_ = -1;
    reset_confirm_ = false;
    edit_pointer_down_ = from_pointer;
    edit_ignore_until_release_ = from_pointer;
}

void TouchOverlay::exit_edit_mode(InputState* input, bool save) {
    if (save && layout_dirty_) {
        (void)save_layout_to_disk();
    }
    editing_ = false;
    drag_mode_ = DragMode::None;
    modal_ = EditorModal::None;
    pending_add_button_ = false;
    dpad_bind_dir_ = -1;
    reset_confirm_ = false;
    edit_pointer_down_ = false;
    edit_ignore_until_release_ = false;
    play_mouse_captured_ = false;
    prev_play_mouse_down_ = false;

    if (input) {
        // Clear transient edges so closing the editor never leaks ESC or clicks to the game.
        for (auto& finger : input->touches) finger.consumed_by_editor = true;
        prev_play_mouse_down_ = input->physical_left_down;
        input->key_pressed.fill(false);
        input->key_released.fill(false);
        input->mouse_pressed.fill(false);
        input->mouse_released.fill(false);
        input->mouse_down[1] = false;
    }
}

void TouchOverlay::apply_picked_key(u8 vk) {
    if (vk == 0) return;
    if (pending_add_button_) {
        if (layout_.controls.size() < kMaxControls) {
            TouchControl b{};
            b.kind = TouchControlKind::Button;
            b.shape = TouchControlShape::Circle;
            // Offset each newly added button slightly around screen center so multiples stay visible.
            const usize count = layout_.controls.size();
            const f32 offset_x = (static_cast<f32>(count % 5) - 2.0f) * 0.045f;
            const f32 offset_y = (static_cast<f32>((count / 5) % 3) - 1.0f) * 0.055f;
            b.cx = std::clamp(0.50f + offset_x, 0.15f, 0.85f);
            b.cy = std::clamp(0.50f + offset_y, 0.25f, 0.82f);
            b.size = 0.066f;
            b.aspect = 1.0f;
            b.vk = vk;
            b.vk_extra = 0;
            b.color_idx = static_cast<u8>((count % 5) + 1);
            layout_.controls.push_back(std::move(b));
            selected_idx_ = static_cast<i32>(layout_.controls.size() - 1);
            layout_dirty_ = true;
            using_default_layout_ = false;
        }
        pending_add_button_ = false;
        modal_ = EditorModal::None;
        return;
    }

    if (selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
        auto& ctrl = layout_.controls[static_cast<usize>(selected_idx_)];
        if (ctrl.kind == TouchControlKind::Dpad && dpad_bind_dir_ >= 0 && dpad_bind_dir_ < 4) {
            ctrl.dpad_vk[static_cast<usize>(dpad_bind_dir_)] = vk;
            dpad_bind_dir_ = -1;
            layout_dirty_ = true;
            using_default_layout_ = false;
            modal_ = EditorModal::DpadBind;
            return;
        }
        if (ctrl.kind == TouchControlKind::Button) {
            ctrl.vk = vk;
            ctrl.vk_extra = 0;
            ctrl.label.clear();
            layout_dirty_ = true;
            using_default_layout_ = false;
        }
    }
    modal_ = EditorModal::None;
}

void TouchOverlay::add_new_dpad() {
    if (layout_.controls.size() >= kMaxControls) return;
    bool has_arrows = false;
    for (const auto& c : layout_.controls) {
        if (c.kind == TouchControlKind::Dpad && c.dpad_vk == std::array<u8, 4>{37, 38, 39, 40}) {
            has_arrows = true;
            break;
        }
    }
    TouchControl pad{};
    pad.kind = TouchControlKind::Dpad;
    pad.shape = TouchControlShape::Circle;
    pad.cx = 0.50f;
    pad.cy = 0.54f;
    pad.size = 0.165f;
    pad.dpad_vk = has_arrows ? std::array<u8, 4>{'A', 'W', 'D', 'S'}
                             : std::array<u8, 4>{37, 38, 39, 40};
    pad.color_idx = has_arrows ? 1 : 0;
    layout_.controls.push_back(std::move(pad));
    selected_idx_ = static_cast<i32>(layout_.controls.size() - 1);
    layout_dirty_ = true;
    using_default_layout_ = false;
}

bool TouchOverlay::update(PlatformWindow& window) {
    InputState& input = window.input_mut();
    if (!enabled_) {
        // Auto-enable when touch contacts are detected on mobile browsers or Android.
        if (PlatformWindow::is_mobile_touch_platform() || input.touch_seen) {
            enabled_ = true;
        } else {
            return false;
        }
    }

    const i32 dw = std::max(1, window.drawable_width());
    const i32 dh = std::max(1, window.drawable_height());
    const f32 aspect = std::clamp(static_cast<f32>(dw) / static_cast<f32>(dh), 0.42f, 3.2f);
    if (using_default_layout_ && std::fabs(aspect - last_aspect_) > 0.01f) {
        layout_ = make_default_layout(aspect);
    }
    last_aspect_ = aspect;
    update_system_pills(aspect);

    if (editing_) {
        update_edit_mode(window, aspect);
        return true;
    }
    return update_play_mode(window, aspect);
}

bool TouchOverlay::update_play_mode(PlatformWindow& window, f32 aspect) {
    InputState& input = window.input_mut();
    std::array<bool, 256> next_vkeys{};

    for (auto& c : layout_.controls) {
        c.pressed = false;
        c.dpad_dir.fill(false);
    }
    toggle_pill_pressed_ = false;
    edit_pill_pressed_ = false;

    // Resolve all control geometries once for this frame.
    std::vector<ResolvedGeom> geoms;
    geoms.reserve(layout_.controls.size());
    for (const auto& c : layout_.controls) {
        geoms.push_back(resolve_control(c, aspect));
    }

    // Track active touch finger set for D-Pad finger locking.
    std::unordered_set<i64> cur_finger_ids;
    cur_finger_ids.reserve(input.touches.size());
    for (const auto& f : input.touches) {
        cur_finger_ids.insert(f.finger_id);
    }

    // Release D-Pad finger locks whose finger is no longer down.
    for (auto& c : layout_.controls) {
        if (c.kind == TouchControlKind::Dpad && c.has_active_finger) {
            if (!cur_finger_ids.contains(c.active_finger_id)) {
                c.has_active_finger = false;
                c.active_finger_id = -1;
            }
        }
    }

    const TouchFinger* mouse_finger = nullptr;
    bool enter_edit_requested = false;

    auto process_pointer = [&](f32 x, f32 y, i64 fid, bool is_new, bool is_touch) -> bool {
        // 1. Top-right Hide/Show pill is always interactive.
        if (toggle_pill_.contains(x, y, 0.006f)) {
            toggle_pill_pressed_ = true;
            if (is_new) {
                controls_visible_ = !controls_visible_;
                if (!controls_visible_) {
                    for (auto& c : layout_.controls) {
                        c.latched = false;
                        c.has_active_finger = false;
                    }
                }
            }
            return true;
        }

        // 2. Top-right Edit pill (interactive whenever controls are visible).
        if (controls_visible_ && edit_pill_.contains(x, y, 0.006f)) {
            edit_pill_pressed_ = true;
            if (is_new) {
                enter_edit_requested = true;
            }
            return true;
        }

        if (!controls_visible_) {
            return false;
        }

        // 3. Check D-Pads (honor existing finger lock first, then capture fresh/spatial touches).
        for (usize i = 0; i < layout_.controls.size(); ++i) {
            auto& c = layout_.controls[i];
            if (c.kind != TouchControlKind::Dpad) continue;
            const auto& g = geoms[i];
            const bool locked_to_me = is_touch && c.has_active_finger && (c.active_finger_id == fid);
            const f32 dx = (x - g.cx) / std::max(0.001f, g.rx);
            const f32 dy = (y - g.cy) / std::max(0.001f, g.ry);
            const f32 dist_sq = dx * dx + dy * dy;
            if (locked_to_me || (!c.has_active_finger && dist_sq <= 1.35f * 1.35f)) {
                if (is_touch && !c.has_active_finger) {
                    c.has_active_finger = true;
                    c.active_finger_id = fid;
                }
                c.pressed = true;
                if (dist_sq >= 0.18f * 0.18f) {
                    if (dx < -0.32f) { c.dpad_dir[0] = true; assert_vkey_with_modifiers(next_vkeys, c.dpad_vk[0]); }
                    if (dy < -0.32f) { c.dpad_dir[1] = true; assert_vkey_with_modifiers(next_vkeys, c.dpad_vk[1]); }
                    if (dx >  0.32f) { c.dpad_dir[2] = true; assert_vkey_with_modifiers(next_vkeys, c.dpad_vk[2]); }
                    if (dy >  0.32f) { c.dpad_dir[3] = true; assert_vkey_with_modifiers(next_vkeys, c.dpad_vk[3]); }
                }
                return true;
            }
        }

        // 4. Check Buttons.
        bool claimed = false;
        for (usize i = 0; i < layout_.controls.size(); ++i) {
            auto& c = layout_.controls[i];
            if (c.kind != TouchControlKind::Button) continue;
            const auto& g = geoms[i];
            const bool is_circle = (c.shape == TouchControlShape::Circle);
            if (hit_control_shape(x, y, g.cx, g.cy, g.rx, g.ry, is_circle, 1.10f)) {
                c.pressed = true;
                if (c.toggle_mode) {
                    if (is_new) {
                        c.latched = !c.latched;
                    }
                } else {
                    assert_vkey_with_modifiers(next_vkeys, c.vk);
                    assert_vkey_with_modifiers(next_vkeys, c.vk_extra);
                }
                claimed = true;
            }
        }
        return claimed;
    };

    // Process real multi-touch fingers.
    for (const auto& f : input.touches) {
        if (f.consumed_by_editor) continue;
        const bool is_new = !prev_finger_ids_.contains(f.finger_id);
        const bool claimed = process_pointer(f.x, f.y, f.finger_id, is_new, true);
        if (!claimed && !mouse_finger) {
            mouse_finger = &f;
        }
    }

    // Desktop / desktop-Web mouse support when no touch contacts are active.
    if (input.touches.empty() && !input.mouse_is_touch) {
        const f32 win_w = static_cast<f32>(std::max(1, window.width()));
        const f32 win_h = static_cast<f32>(std::max(1, window.height()));
        const f32 mx = std::clamp(input.mouse_x / win_w, 0.0f, 1.0f);
        const f32 my = std::clamp(input.mouse_y / win_h, 0.0f, 1.0f);
        const bool m_down = input.physical_left_down;
        const bool m_new = input.mouse_pressed[1] || (m_down && !prev_play_mouse_down_);

        if (m_down) {
            if (m_new || play_mouse_captured_) {
                const bool claimed = process_pointer(mx, my, -1, m_new, false);
                if (m_new && claimed) {
                    play_mouse_captured_ = true;
                }
                if (play_mouse_captured_) {
                    // Suppress captured mouse clicks so they don't also click the game behind the button.
                    input.mouse_down[1] = false;
                    input.mouse_pressed[1] = false;
                    input.mouse_released[1] = false;
                }
            }
        } else {
            if (play_mouse_captured_) {
                input.mouse_released[1] = false;
                play_mouse_captured_ = false;
            }
        }
        prev_play_mouse_down_ = m_down;
    } else {
        play_mouse_captured_ = false;
        prev_play_mouse_down_ = false;
    }

    prev_finger_ids_ = std::move(cur_finger_ids);

    if (enter_edit_requested) {
        enter_edit_mode(&input, true);
        return true;
    }

    // Apply latched toggle-mode buttons when controls are visible.
    if (controls_visible_) {
        for (const auto& c : layout_.controls) {
            if (c.kind == TouchControlKind::Button && c.toggle_mode && c.latched) {
                assert_vkey_with_modifiers(next_vkeys, c.vk);
                assert_vkey_with_modifiers(next_vkeys, c.vk_extra);
            }
        }
    }

    // Emit clean synthetic keyboard press/hold/release edges without clobbering physical keys.
    for (usize vk = 1; vk < 256; ++vk) {
        const bool was = active_vkeys_[vk];
        const bool now = next_vkeys[vk];
        if (now && !was) {
            if (!input.key_down[vk]) input.key_pressed[vk] = true;
            input.key_down[vk] = true;
            input.last_key = static_cast<u8>(vk);
            input.current_key = static_cast<u8>(vk);
        } else if (now && was) {
            input.key_down[vk] = true;
        } else if (!now && was) {
            if (input.key_down[vk]) {
                input.key_down[vk] = false;
                input.key_released[vk] = true;
            }
            if (input.current_key == vk) input.current_key = 0;
        }
        active_vkeys_[vk] = now;
    }

    // Forward unclaimed screen touches (or all touches when overlay is hidden) to left mouse button.
    if (mouse_finger) {
        input.mouse_x = mouse_finger->x * static_cast<f32>(window.width());
        input.mouse_y = mouse_finger->y * static_cast<f32>(window.height());
        if (!touch_mouse_active_) {
            if (!input.mouse_down[1]) input.mouse_pressed[1] = true;
            input.mouse_down[1] = true;
            touch_mouse_active_ = true;
        }
    } else if (touch_mouse_active_) {
        if (input.mouse_down[1]) {
            input.mouse_down[1] = false;
            input.mouse_released[1] = true;
        }
        touch_mouse_active_ = false;
    }

    return false;
}

void TouchOverlay::update_edit_mode(PlatformWindow& window, f32 aspect) {
    InputState& input = window.input_mut();

    std::unordered_set<i64> cur_finger_ids;
    cur_finger_ids.reserve(input.touches.size());
    for (const auto& f : input.touches) cur_finger_ids.insert(f.finger_id);

    // Resolve single active editor pointer (touch primary, real desktop mouse fallback).
    bool p_down = false;
    bool p_pressed = false;
    bool p_released = false;
    f32 px = 0.0f;
    f32 py = 0.0f;

    if (!input.touches.empty()) {
        const TouchFinger* active = nullptr;
        if (edit_pointer_down_ && edit_pointer_is_touch_) {
            for (const auto& f : input.touches) {
                if (f.finger_id == edit_pointer_id_) { active = &f; break; }
            }
        }
        if (active) {
            p_down = true;
            px = active->x;
            py = active->y;
        } else if (!edit_pointer_down_) {
            for (const auto& f : input.touches) {
                if (!prev_finger_ids_.contains(f.finger_id)) {
                    active = &f;
                    break;
                }
            }
            if (active) {
                p_down = true;
                p_pressed = true;
                px = active->x;
                py = active->y;
                edit_pointer_is_touch_ = true;
                edit_pointer_id_ = active->finger_id;
            }
        } else {
            p_released = true;
        }
    } else if (edit_pointer_down_ && edit_pointer_is_touch_) {
        p_released = true;
    } else if (!input.mouse_is_touch) {
        const f32 win_w = static_cast<f32>(std::max(1, window.width()));
        const f32 win_h = static_cast<f32>(std::max(1, window.height()));
        px = std::clamp(input.mouse_x / win_w, 0.0f, 1.0f);
        py = std::clamp(input.mouse_y / win_h, 0.0f, 1.0f);
        p_down = input.physical_left_down;
        if (p_down && !edit_pointer_down_) {
            p_pressed = true;
            edit_pointer_is_touch_ = false;
            edit_pointer_id_ = -1;
        } else if (!p_down && edit_pointer_down_) {
            p_released = true;
        }
    } else if (edit_pointer_down_) {
        p_released = true;
    }

    prev_finger_ids_ = std::move(cur_finger_ids);

    // Wait for the finger/click that tapped the EDIT pill to lift before processing taps.
    if (edit_ignore_until_release_) {
        if (!p_down) {
            edit_ignore_until_release_ = false;
            edit_pointer_down_ = false;
        }
        input.key_pressed.fill(false);
        input.key_released.fill(false);
        input.mouse_pressed.fill(false);
        input.mouse_released.fill(false);
        return;
    }

    edit_pointer_down_ = p_down;

    // 1. Handle physical keyboard shortcuts / direct key binding capture.
    if (modal_ == EditorModal::KeyPicker) {
        if (input.key_pressed[27]) { // ESC closes picker
            if (pending_add_button_) {
                pending_add_button_ = false;
                modal_ = EditorModal::None;
            } else if (dpad_bind_dir_ >= 0) {
                dpad_bind_dir_ = -1;
                modal_ = EditorModal::DpadBind;
            } else {
                modal_ = EditorModal::None;
            }
        } else {
            // Prefer specific modifier codes (160..165) over aggregate (16..18).
            u8 captured_vk = 0;
            for (u8 vk = 160; vk <= 165; ++vk) {
                if (input.key_pressed[vk]) { captured_vk = vk; break; }
            }
            if (captured_vk == 0) {
                for (usize vk = 1; vk < 256; ++vk) {
                    if (vk == 16 || vk == 17 || vk == 18 || vk == 27) continue;
                    if (input.key_pressed[vk]) {
                        captured_vk = static_cast<u8>(vk);
                        break;
                    }
                }
            }
            if (captured_vk != 0) {
                apply_picked_key(captured_vk);
            }
        }
    } else if (modal_ == EditorModal::DpadBind) {
        if (input.key_pressed[27]) {
            modal_ = EditorModal::None;
        }
    } else {
        if (input.key_pressed[27]) {
            exit_edit_mode(&input, true);
            return;
        }
        if ((input.key_pressed[46] || input.key_pressed[8]) &&
            selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
            layout_.controls.erase(layout_.controls.begin() + selected_idx_);
            selected_idx_ = -1;
            drag_mode_ = DragMode::None;
            layout_dirty_ = true;
            using_default_layout_ = false;
        }
    }

    // 2. Handle pointer release.
    if (p_released) {
        drag_mode_ = DragMode::None;
    }

    // 3. Handle active drag (move or resize).
    if (p_down && drag_mode_ != DragMode::None &&
        selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
        auto& ctrl = layout_.controls[static_cast<usize>(selected_idx_)];
        const f32 unit_y = std::min(1.0f, aspect);
        const f32 unit_x = std::min(1.0f, 1.0f / aspect);

        if (drag_mode_ == DragMode::Move) {
            const ResolvedGeom g = resolve_control(ctrl, aspect);
            const f32 raw_x = snap_value(px + drag_offset_x_, layout_.snap_to_grid);
            const f32 raw_y = snap_value(py + drag_offset_y_, layout_.snap_to_grid);
            const f32 min_x = std::min(0.49f, g.rx + 0.005f);
            const f32 min_y = std::min(0.49f, g.ry + 0.005f);
            ctrl.cx = std::clamp(raw_x, min_x, 1.0f - min_x);
            ctrl.cy = std::clamp(raw_y, min_y, 1.0f - min_y);
            layout_dirty_ = true;
            using_default_layout_ = false;
        } else if (drag_mode_ == DragMode::Resize) {
            const ResolvedGeom g = resolve_control(ctrl, aspect);
            const f32 dx_iso = (px - g.cx) / std::max(0.001f, unit_x);
            const f32 dy_iso = (py - g.cy) / std::max(0.001f, unit_y);
            const f32 dist = std::max(0.008f, std::hypot(dx_iso, dy_iso));
            const f32 scale = dist / std::max(0.008f, resize_start_dist_);
            const f32 min_s = (ctrl.kind == TouchControlKind::Dpad) ? kMinDpadSize : kMinButtonSize;
            const f32 max_s = (ctrl.kind == TouchControlKind::Dpad) ? kMaxDpadSize : kMaxButtonSize;
            ctrl.size = std::clamp(resize_start_size_ * scale, min_s, max_s);
            layout_dirty_ = true;
            using_default_layout_ = false;
        }
    }

    // 4. Handle fresh pointer press.
    if (p_pressed) {
        if (modal_ == EditorModal::KeyPicker) {
            RectBox card{}, close_btn{};
            std::vector<UiButton> tabs, keys;
            build_key_picker_ui(aspect, card, close_btn, tabs, keys);

            if (close_btn.contains(px, py, 0.008f) || !card.contains(px, py, 0.0f)) {
                if (pending_add_button_) {
                    pending_add_button_ = false;
                    modal_ = EditorModal::None;
                } else if (dpad_bind_dir_ >= 0) {
                    dpad_bind_dir_ = -1;
                    modal_ = EditorModal::DpadBind;
                } else {
                    modal_ = EditorModal::None;
                }
            } else {
                bool handled = false;
                for (const auto& t : tabs) {
                    if (t.rect.contains(px, py, 0.004f)) {
                        picker_tab_ = t.action_id;
                        handled = true;
                        break;
                    }
                }
                if (!handled) {
                    for (const auto& k : keys) {
                        if (k.rect.contains(px, py, 0.004f)) {
                            apply_picked_key(static_cast<u8>(k.action_id));
                            break;
                        }
                    }
                }
            }
        } else if (modal_ == EditorModal::DpadBind) {
            RectBox card{}, close_btn{};
            std::vector<UiButton> presets, dirs;
            build_dpad_modal_ui(aspect, card, close_btn, presets, dirs);

            if (close_btn.contains(px, py, 0.008f) || !card.contains(px, py, 0.0f)) {
                modal_ = EditorModal::None;
            } else if (selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
                auto& ctrl = layout_.controls[static_cast<usize>(selected_idx_)];
                bool handled = false;
                for (const auto& pr : presets) {
                    if (pr.rect.contains(px, py, 0.004f)) {
                        switch (pr.action_id) {
                            case kActPresetArrows: ctrl.dpad_vk = {37, 38, 39, 40}; break;
                            case kActPresetWasd:   ctrl.dpad_vk = {'A', 'W', 'D', 'S'}; break;
                            case kActPresetIjkl:   ctrl.dpad_vk = {'J', 'I', 'L', 'K'}; break;
                            case kActPresetNumpad: ctrl.dpad_vk = {100, 104, 102, 98}; break;
                            default: break;
                        }
                        layout_dirty_ = true;
                        using_default_layout_ = false;
                        modal_ = EditorModal::None;
                        handled = true;
                        break;
                    }
                }
                if (!handled) {
                    for (const auto& d : dirs) {
                        if (d.rect.contains(px, py, 0.004f)) {
                            dpad_bind_dir_ = d.action_id;
                            pending_add_button_ = false;
                            picker_tab_ = tab_for_vk(ctrl.dpad_vk[static_cast<usize>(dpad_bind_dir_)]);
                            modal_ = EditorModal::KeyPicker;
                            break;
                        }
                    }
                }
            }
        } else {
            // Main Edit Mode canvas: check Toolbar -> Inspector -> Resize Handle -> Controls.
            bool handled = false;

            std::vector<UiButton> toolbar;
            build_editor_toolbar(aspect, toolbar);
            for (const auto& b : toolbar) {
                if (!b.rect.contains(px, py, 0.004f)) continue;
                handled = true;
                if (b.action_id != kActReset) reset_confirm_ = false;
                switch (b.action_id) {
                    case kActFlipDock:
                        toolbar_at_bottom_ = !toolbar_at_bottom_;
                        break;
                    case kActAddBtn:
                        if (layout_.controls.size() < kMaxControls) {
                            pending_add_button_ = true;
                            dpad_bind_dir_ = -1;
                            picker_tab_ = 1; // A-Z is convenient for adding extra keys
                            modal_ = EditorModal::KeyPicker;
                        }
                        break;
                    case kActAddDpad:
                        add_new_dpad();
                        break;
                    case kActOpacity: {
                        constexpr std::array<f32, 5> kOpSteps = {1.0f, 0.80f, 0.60f, 0.40f, 0.25f};
                        usize best = 0;
                        f32 best_diff = 10.0f;
                        for (usize i = 0; i < kOpSteps.size(); ++i) {
                            const f32 d = std::fabs(layout_.opacity - kOpSteps[i]);
                            if (d < best_diff) { best_diff = d; best = i; }
                        }
                        layout_.opacity = kOpSteps[(best + 1) % kOpSteps.size()];
                        layout_dirty_ = true;
                        using_default_layout_ = false;
                        break;
                    }
                    case kActGrid:
                        layout_.snap_to_grid = !layout_.snap_to_grid;
                        layout_dirty_ = true;
                        using_default_layout_ = false;
                        break;
                    case kActReset:
                        if (!reset_confirm_) {
                            reset_confirm_ = true;
                        } else {
                            reset_to_default(true);
                        }
                        break;
                    case kActDone:
                        exit_edit_mode(&input, true);
                        return;
                    default:
                        break;
                }
                break;
            }

            if (!handled && selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
                std::vector<UiButton> inspector;
                build_inspector_bar(aspect, inspector);
                for (const auto& b : inspector) {
                    if (!b.rect.contains(px, py, 0.004f)) continue;
                    handled = true;
                    reset_confirm_ = false;
                    auto& ctrl = layout_.controls[static_cast<usize>(selected_idx_)];
                    switch (b.action_id) {
                        case kActBindKey:
                            pending_add_button_ = false;
                            dpad_bind_dir_ = -1;
                            picker_tab_ = tab_for_vk(ctrl.vk);
                            modal_ = EditorModal::KeyPicker;
                            break;
                        case kActBindDpad:
                            dpad_bind_dir_ = -1;
                            modal_ = EditorModal::DpadBind;
                            break;
                        case kActSizeDown: {
                            const f32 step = (ctrl.kind == TouchControlKind::Dpad) ? 0.015f : 0.008f;
                            const f32 min_s = (ctrl.kind == TouchControlKind::Dpad) ? kMinDpadSize : kMinButtonSize;
                            ctrl.size = std::max(min_s, ctrl.size - step);
                            layout_dirty_ = true;
                            using_default_layout_ = false;
                            break;
                        }
                        case kActSizeUp: {
                            const f32 step = (ctrl.kind == TouchControlKind::Dpad) ? 0.015f : 0.008f;
                            const f32 max_s = (ctrl.kind == TouchControlKind::Dpad) ? kMaxDpadSize : kMaxButtonSize;
                            ctrl.size = std::min(max_s, ctrl.size + step);
                            layout_dirty_ = true;
                            using_default_layout_ = false;
                            break;
                        }
                        case kActToggleShape:
                            if (ctrl.kind == TouchControlKind::Button) {
                                ctrl.shape = (ctrl.shape == TouchControlShape::Circle)
                                    ? TouchControlShape::Rect
                                    : TouchControlShape::Circle;
                                ctrl.aspect = (ctrl.shape == TouchControlShape::Rect) ? 1.55f : 1.0f;
                                layout_dirty_ = true;
                                using_default_layout_ = false;
                            }
                            break;
                        case kActToggleMode:
                            if (ctrl.kind == TouchControlKind::Button) {
                                ctrl.toggle_mode = !ctrl.toggle_mode;
                                layout_dirty_ = true;
                                using_default_layout_ = false;
                            }
                            break;
                        case kActCycleColor:
                            ctrl.color_idx = static_cast<u8>((ctrl.color_idx + 1) % kPalette.size());
                            layout_dirty_ = true;
                            using_default_layout_ = false;
                            break;
                        case kActDelete:
                            layout_.controls.erase(layout_.controls.begin() + selected_idx_);
                            selected_idx_ = -1;
                            drag_mode_ = DragMode::None;
                            layout_dirty_ = true;
                            using_default_layout_ = false;
                            break;
                        default:
                            break;
                    }
                    break;
                }
            }

            if (!handled) {
                reset_confirm_ = false;

                // Check selected control's resize handle first.
                if (selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
                    const auto& ctrl = layout_.controls[static_cast<usize>(selected_idx_)];
                    const ResolvedGeom g = resolve_control(ctrl, aspect);
                    const RectBox handle = resize_handle_box(ctrl, g, aspect);
                    if (handle.contains(px, py, 0.016f)) {
                        const f32 unit_y = std::min(1.0f, aspect);
                        const f32 unit_x = std::min(1.0f, 1.0f / aspect);
                        const f32 dx_iso = (px - g.cx) / std::max(0.001f, unit_x);
                        const f32 dy_iso = (py - g.cy) / std::max(0.001f, unit_y);
                        resize_start_dist_ = std::max(0.010f, std::hypot(dx_iso, dy_iso));
                        resize_start_size_ = ctrl.size;
                        inspector_at_bottom_locked_ = toolbar_at_bottom_ ? (g.cy < 0.56f) : (g.cy < 0.44f);
                        drag_mode_ = DragMode::Resize;
                        handled = true;
                    }
                }

                // Hit-test controls (prefer smallest containing control so overlapped buttons remain selectable).
                if (!handled) {
                    i32 best_idx = -1;
                    f32 best_area = 1e9f;
                    ResolvedGeom best_g{};
                    for (usize i = 0; i < layout_.controls.size(); ++i) {
                        const auto& c = layout_.controls[i];
                        const ResolvedGeom g = resolve_control(c, aspect);
                        const bool is_circle = (c.kind == TouchControlKind::Dpad || c.shape == TouchControlShape::Circle);
                        if (hit_control_shape(px, py, g.cx, g.cy, g.rx, g.ry, is_circle, 1.12f)) {
                            const f32 area = g.rx * g.ry;
                            if (area <= best_area) {
                                best_area = area;
                                best_idx = static_cast<i32>(i);
                                best_g = g;
                            }
                        }
                    }
                    if (best_idx >= 0) {
                        selected_idx_ = best_idx;
                        drag_offset_x_ = best_g.cx - px;
                        drag_offset_y_ = best_g.cy - py;
                        inspector_at_bottom_locked_ = toolbar_at_bottom_ ? (best_g.cy < 0.56f) : (best_g.cy < 0.44f);
                        drag_mode_ = DragMode::Move;
                    } else {
                        selected_idx_ = -1;
                        drag_mode_ = DragMode::None;
                    }
                }
            }
        }
    }

    // Consume all transient input while Edit Mode owns the screen.
    input.key_pressed.fill(false);
    input.key_released.fill(false);
    input.mouse_pressed.fill(false);
    input.mouse_released.fill(false);
}

// ---------------------------------------------------------------------------
// Immediate-mode triangle geometry helpers
// ---------------------------------------------------------------------------

void TouchOverlay::push_tri(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                            f32 r, f32 g, f32 b, f32 a) {
    verts_.push_back(Vertex{x0, y0, r, g, b, a});
    verts_.push_back(Vertex{x1, y1, r, g, b, a});
    verts_.push_back(Vertex{x2, y2, r, g, b, a});
}

void TouchOverlay::push_rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 r, f32 g, f32 b, f32 a) {
    push_tri(x0, y0, x1, y0, x1, y1, r, g, b, a);
    push_tri(x0, y0, x1, y1, x0, y1, r, g, b, a);
}

void TouchOverlay::push_rect_outline(f32 x0, f32 y0, f32 x1, f32 y1, f32 tx, f32 ty,
                                     f32 r, f32 g, f32 b, f32 a) {
    push_rect(x0, y0, x1, y0 + ty, r, g, b, a);
    push_rect(x0, y1 - ty, x1, y1, r, g, b, a);
    push_rect(x0, y0 + ty, x0 + tx, y1 - ty, r, g, b, a);
    push_rect(x1 - tx, y0 + ty, x1, y1 - ty, r, g, b, a);
}

void TouchOverlay::push_rounded_rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 rad_x, f32 rad_y,
                                     f32 r, f32 g, f32 b, f32 a) {
    const f32 max_rx = (x1 - x0) * 0.48f;
    const f32 max_ry = (y1 - y0) * 0.48f;
    const f32 rx = std::clamp(rad_x, 0.0f, max_rx);
    const f32 ry = std::clamp(rad_y, 0.0f, max_ry);
    if (rx <= 0.0001f || ry <= 0.0001f) {
        push_rect(x0, y0, x1, y1, r, g, b, a);
        return;
    }

    // Center + side rectangles.
    push_rect(x0 + rx, y0, x1 - rx, y1, r, g, b, a);
    push_rect(x0, y0 + ry, x0 + rx, y1 - ry, r, g, b, a);
    push_rect(x1 - rx, y0 + ry, x1, y1 - ry, r, g, b, a);

    // 4 rounded corners (4 segments each).
    constexpr i32 kSeg = 4;
    const f32 corners[4][3] = {
        {x0 + rx, y0 + ry, kPi},
        {x1 - rx, y0 + ry, kPi * 1.5f},
        {x1 - rx, y1 - ry, 0.0f},
        {x0 + rx, y1 - ry, kPi * 0.5f},
    };
    for (const auto& c : corners) {
        for (i32 i = 0; i < kSeg; ++i) {
            const f32 a0 = c[2] + (kPi * 0.5f) * static_cast<f32>(i) / static_cast<f32>(kSeg);
            const f32 a1 = c[2] + (kPi * 0.5f) * static_cast<f32>(i + 1) / static_cast<f32>(kSeg);
            push_tri(c[0], c[1],
                     c[0] + std::cos(a0) * rx, c[1] + std::sin(a0) * ry,
                     c[0] + std::cos(a1) * rx, c[1] + std::sin(a1) * ry,
                     r, g, b, a);
        }
    }
}

void TouchOverlay::push_rounded_outline(f32 x0, f32 y0, f32 x1, f32 y1, f32 rad_x, f32 rad_y,
                                        f32 tx, f32 ty, f32 r, f32 g, f32 b, f32 a) {
    const f32 max_rx = std::max(0.0f, (x1 - x0) * 0.48f);
    const f32 max_ry = std::max(0.0f, (y1 - y0) * 0.48f);
    const f32 eff_tx = std::min(tx, max_rx);
    const f32 eff_ty = std::min(ty, max_ry);
    const f32 rx = std::clamp(rad_x, eff_tx, max_rx);
    const f32 ry = std::clamp(rad_y, eff_ty, max_ry);
    const f32 irx = std::max(0.0f, rx - eff_tx);
    const f32 iry = std::max(0.0f, ry - eff_ty);

    // Straight border edges.
    push_rect(x0 + rx, y0, x1 - rx, y0 + eff_ty, r, g, b, a);
    push_rect(x0 + rx, y1 - eff_ty, x1 - rx, y1, r, g, b, a);
    push_rect(x0, y0 + ry, x0 + eff_tx, y1 - ry, r, g, b, a);
    push_rect(x1 - eff_tx, y0 + ry, x1, y1 - ry, r, g, b, a);

    // 4 corner arcs.
    constexpr i32 kSeg = 4;
    const f32 corners[4][3] = {
        {x0 + rx, y0 + ry, kPi},
        {x1 - rx, y0 + ry, kPi * 1.5f},
        {x1 - rx, y1 - ry, 0.0f},
        {x0 + rx, y1 - ry, kPi * 0.5f},
    };
    for (const auto& c : corners) {
        for (i32 i = 0; i < kSeg; ++i) {
            const f32 a0 = c[2] + (kPi * 0.5f) * static_cast<f32>(i) / static_cast<f32>(kSeg);
            const f32 a1 = c[2] + (kPi * 0.5f) * static_cast<f32>(i + 1) / static_cast<f32>(kSeg);
            const f32 c0 = std::cos(a0), s0 = std::sin(a0);
            const f32 c1 = std::cos(a1), s1 = std::sin(a1);
            const f32 ix0 = c[0] + c0 * irx, iy0 = c[1] + s0 * iry;
            const f32 ox0 = c[0] + c0 * rx,  oy0 = c[1] + s0 * ry;
            const f32 ix1 = c[0] + c1 * irx, iy1 = c[1] + s1 * iry;
            const f32 ox1 = c[0] + c1 * rx,  oy1 = c[1] + s1 * ry;
            push_tri(ix0, iy0, ox0, oy0, ox1, oy1, r, g, b, a);
            push_tri(ix0, iy0, ox1, oy1, ix1, iy1, r, g, b, a);
        }
    }
}

void TouchOverlay::push_circle(f32 cx, f32 cy, f32 rx, f32 ry, f32 r, f32 g, f32 b, f32 a, i32 segments) {
    for (i32 i = 0; i < segments; ++i) {
        const f32 a0 = kTwoPi * static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(i + 1) / static_cast<f32>(segments);
        push_tri(cx, cy,
                 cx + std::cos(a0) * rx, cy + std::sin(a0) * ry,
                 cx + std::cos(a1) * rx, cy + std::sin(a1) * ry,
                 r, g, b, a);
    }
}

void TouchOverlay::push_ring(f32 cx, f32 cy, f32 rx_in, f32 ry_in, f32 rx_out, f32 ry_out,
                             f32 r, f32 g, f32 b, f32 a, i32 segments) {
    for (i32 i = 0; i < segments; ++i) {
        const f32 a0 = kTwoPi * static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(i + 1) / static_cast<f32>(segments);
        const f32 c0 = std::cos(a0), s0 = std::sin(a0);
        const f32 c1 = std::cos(a1), s1 = std::sin(a1);
        const f32 ix0 = cx + c0 * rx_in,  iy0 = cy + s0 * ry_in;
        const f32 ox0 = cx + c0 * rx_out, oy0 = cy + s0 * ry_out;
        const f32 ix1 = cx + c1 * rx_in,  iy1 = cy + s1 * ry_in;
        const f32 ox1 = cx + c1 * rx_out, oy1 = cy + s1 * ry_out;
        push_tri(ix0, iy0, ox0, oy0, ox1, oy1, r, g, b, a);
        push_tri(ix0, iy0, ox1, oy1, ix1, iy1, r, g, b, a);
    }
}

void TouchOverlay::push_menu_icon(f32 cx, f32 cy, f32 sx, f32 sy, f32 r, f32 g, f32 b, f32 a) {
    const f32 ty = sy * 0.22f;
    push_rect(cx - sx * 0.55f, cy - sy * 0.52f, cx + sx * 0.55f, cy - sy * 0.52f + ty, r, g, b, a);
    push_rect(cx - sx * 0.55f, cy - ty * 0.5f,  cx + sx * 0.55f, cy + ty * 0.5f,       r, g, b, a);
    push_rect(cx - sx * 0.55f, cy + sy * 0.52f - ty, cx + sx * 0.55f, cy + sy * 0.52f, r, g, b, a);
}

void TouchOverlay::push_text_centered(std::string_view text, f32 cx, f32 cy, f32 max_w, f32 max_h,
                                      f32 aspect, f32 r, f32 g, f32 b, f32 a) {
    if (text.empty() || max_w <= 0.0f || max_h <= 0.0f) return;
    aspect = std::clamp(aspect, 0.42f, 3.2f);

    // 7x7 visible ink inside an 8-unit advance per character.
    const f32 total_units_x = static_cast<f32>(text.size()) * 8.0f - 1.0f;
    const f32 px_h_from_h = max_h / 7.5f;
    const f32 px_h_from_w = (max_w * aspect) / std::max(1.0f, total_units_x);
    const f32 px_h = std::min(px_h_from_h, px_h_from_w);
    const f32 px_w = px_h / aspect;
    if (px_h <= 0.0001f || px_w <= 0.0001f) return;

    const f32 str_w = total_units_x * px_w;
    const f32 str_h = 7.0f * px_h;
    f32 x0 = cx - str_w * 0.5f;
    const f32 y0 = cy - str_h * 0.5f;

    for (char ch : text) {
        const u8 code = static_cast<u8>(ch);
        if (code >= 32 && code < 128) {
            const u8* rows = kBuiltinFont8x8[code - 32];
            for (i32 row = 0; row < 8; ++row) {
                const u8 bits = rows[row];
                if (bits == 0) continue;
                i32 col = 0;
                while (col < 8) {
                    if ((bits & (1u << col)) == 0) { ++col; continue; }
                    const i32 start = col;
                    while (col < 8 && (bits & (1u << col)) != 0) ++col;
                    push_rect(x0 + static_cast<f32>(start) * px_w,
                              y0 + static_cast<f32>(row) * px_h,
                              x0 + static_cast<f32>(col) * px_w,
                              y0 + static_cast<f32>(row + 1) * px_h,
                              r, g, b, a);
                }
            }
        }
        x0 += 8.0f * px_w;
    }
}

void TouchOverlay::draw_ui_button(const UiButton& btn, f32 aspect) {
    const f32 unit_y = std::min(1.0f, aspect);
    const f32 unit_x = std::min(1.0f, 1.0f / aspect);
    const f32 rad_y = std::min(btn.rect.h() * 0.28f, 0.014f * unit_y);
    const f32 rad_x = rad_y / aspect;
    const f32 bw_y = 0.0032f * unit_y;
    const f32 bw_x = 0.0032f * unit_x;
    const f32 drag_fade = (drag_mode_ != DragMode::None) ? 0.55f : 1.0f;

    f32 fr = 0.14f, fg = 0.18f, fb = 0.25f, fa = 0.88f;
    f32 br = 0.48f, bg = 0.56f, bb = 0.68f, ba = 0.72f;
    f32 tr = 0.94f, tg = 0.96f, tb = 1.00f, ta = 0.96f;

    if (btn.danger) {
        fr = 0.45f; fg = 0.14f; fb = 0.16f;
        br = 0.96f; bg = 0.42f; bb = 0.44f;
    } else if (btn.accent) {
        fr = 0.14f; fg = 0.46f; fb = 0.82f;
        br = 0.55f; bg = 0.82f; bb = 1.00f;
    } else if (btn.active) {
        fr = 0.16f; fg = 0.40f; fb = 0.56f;
        br = 0.45f; bg = 0.88f; bb = 1.00f;
    } else if (btn.tint_color_idx < kPalette.size()) {
        const auto& pal = kPalette[btn.tint_color_idx];
        fr = pal.press_r * 0.52f; fg = pal.press_g * 0.52f; fb = pal.press_b * 0.52f;
        br = pal.ring_r; bg = pal.ring_g; bb = pal.ring_b;
    }

    push_rounded_rect(btn.rect.x0, btn.rect.y0, btn.rect.x1, btn.rect.y1,
                      rad_x, rad_y, fr, fg, fb, fa * drag_fade);
    push_rounded_outline(btn.rect.x0, btn.rect.y0, btn.rect.x1, btn.rect.y1,
                         rad_x, rad_y, bw_x, bw_y, br, bg, bb, ba * drag_fade);
    push_text_centered(btn.text, btn.rect.cx(), btn.rect.cy(),
                       btn.rect.w() * 0.84f, btn.rect.h() * 0.48f,
                       aspect, tr, tg, tb, ta * drag_fade);
}

void TouchOverlay::draw_control(const TouchControl& ctrl, const ResolvedGeom& g, f32 aspect,
                                f32 alpha_scale, bool selected) {
    const auto& pal = kPalette[ctrl.color_idx % kPalette.size()];
    const bool lit = !editing_ && (ctrl.pressed || ctrl.latched);
    const f32 op = std::clamp(alpha_scale, 0.15f, 1.0f);

    if (ctrl.kind == TouchControlKind::Dpad) {
        // Outer base disc & rim.
        push_circle(g.cx, g.cy, g.rx, g.ry, pal.fill_r, pal.fill_g, pal.fill_b,
                    (editing_ ? 0.42f : 0.26f) * op, 28);
        push_ring(g.cx, g.cy, g.rx * 0.92f, g.ry * 0.92f, g.rx, g.ry,
                  selected ? 0.38f : pal.ring_r,
                  selected ? 0.92f : pal.ring_g,
                  selected ? 1.00f : pal.ring_b,
                  (selected ? 0.95f : (editing_ ? 0.62f : 0.36f)) * op, 28);

        // Cross arms.
        const f32 arm_w = g.rx * 0.30f;
        const f32 arm_h = g.ry * 0.30f;
        push_rect(g.cx - g.rx * 0.78f, g.cy - arm_h, g.cx + g.rx * 0.78f, g.cy + arm_h,
                  pal.ring_r, pal.ring_g, pal.ring_b, 0.15f * op);
        push_rect(g.cx - arm_w, g.cy - g.ry * 0.78f, g.cx + arm_w, g.cy + g.ry * 0.78f,
                  pal.ring_r, pal.ring_g, pal.ring_b, 0.15f * op);

        // Directional highlight arrows.
        auto arrow_col = [&](bool on, f32& r, f32& gr, f32& b, f32& a) {
            if (on) { r = pal.press_r; gr = pal.press_g; b = pal.press_b; a = 0.92f * op; }
            else    { r = pal.ring_r;  gr = pal.ring_g;  b = pal.ring_b;  a = 0.48f * op; }
        };
        f32 ar, ag, ab, aa;
        const bool is_arrows = (ctrl.dpad_vk == std::array<u8, 4>{37, 38, 39, 40});
        if (is_arrows) {
            arrow_col(ctrl.dpad_dir[0], ar, ag, ab, aa);
            push_tri(g.cx - g.rx * 0.82f, g.cy,
                     g.cx - g.rx * 0.42f, g.cy - g.ry * 0.24f,
                     g.cx - g.rx * 0.42f, g.cy + g.ry * 0.24f, ar, ag, ab, aa);

            arrow_col(ctrl.dpad_dir[1], ar, ag, ab, aa);
            push_tri(g.cx, g.cy - g.ry * 0.82f,
                     g.cx - g.rx * 0.24f, g.cy - g.ry * 0.42f,
                     g.cx + g.rx * 0.24f, g.cy - g.ry * 0.42f, ar, ag, ab, aa);

            arrow_col(ctrl.dpad_dir[2], ar, ag, ab, aa);
            push_tri(g.cx + g.rx * 0.82f, g.cy,
                     g.cx + g.rx * 0.42f, g.cy - g.ry * 0.24f,
                     g.cx + g.rx * 0.42f, g.cy + g.ry * 0.24f, ar, ag, ab, aa);

            arrow_col(ctrl.dpad_dir[3], ar, ag, ab, aa);
            push_tri(g.cx, g.cy + g.ry * 0.82f,
                     g.cx - g.rx * 0.24f, g.cy + g.ry * 0.42f,
                     g.cx + g.rx * 0.24f, g.cy + g.ry * 0.42f, ar, ag, ab, aa);
        } else {
            // Non-arrow D-Pad (e.g. WASD / IJKL / Custom): draw directional key labels inside arms.
            const f32 dist = 0.57f;
            const f32 offsets[4][2] = {{-dist, 0.0f}, {0.0f, -dist}, {dist, 0.0f}, {0.0f, dist}};
            for (i32 d = 0; d < 4; ++d) {
                arrow_col(ctrl.dpad_dir[static_cast<usize>(d)], ar, ag, ab, aa);
                push_text_centered(vk_short_label(ctrl.dpad_vk[static_cast<usize>(d)]),
                                   g.cx + offsets[d][0] * g.rx, g.cy + offsets[d][1] * g.ry,
                                   g.rx * 0.44f, g.ry * 0.34f, aspect, ar, ag, ab, std::min(1.0f, aa + 0.25f));
            }
        }

        if (editing_) {
            push_text_centered(dpad_preset_name(ctrl.dpad_vk), g.cx, g.cy,
                               g.rx * 0.60f, g.ry * 0.24f, aspect, 0.95f, 0.98f, 1.0f, 0.82f);
        }
    } else {
        const f32 br = lit ? pal.press_r : pal.fill_r;
        const f32 bg = lit ? pal.press_g : pal.fill_g;
        const f32 bb = lit ? pal.press_b : pal.fill_b;
        const f32 ba = (lit ? 0.66f : (editing_ ? 0.46f : 0.30f)) * op;

        const f32 rr = selected ? 0.38f : (lit ? 1.0f : pal.ring_r);
        const f32 rg = selected ? 0.92f : (lit ? 1.0f : pal.ring_g);
        const f32 rb = selected ? 1.00f : (lit ? 1.0f : pal.ring_b);
        const f32 ra = (selected ? 0.96f : (lit ? 0.90f : (editing_ ? 0.64f : 0.40f))) * op;

        if (ctrl.shape == TouchControlShape::Circle) {
            push_circle(g.cx, g.cy, g.rx, g.ry, br, bg, bb, ba, 24);
            push_ring(g.cx, g.cy, g.rx * 0.88f, g.ry * 0.88f, g.rx, g.ry, rr, rg, rb, ra, 24);
        } else {
            const f32 rad_y = g.ry * 0.38f;
            const f32 rad_x = rad_y / aspect;
            const f32 tx = std::min(g.rx * 0.08f, 0.0045f);
            const f32 ty = tx * aspect;
            push_rounded_rect(g.cx - g.rx, g.cy - g.ry, g.cx + g.rx, g.cy + g.ry,
                              rad_x, rad_y, br, bg, bb, ba);
            push_rounded_outline(g.cx - g.rx, g.cy - g.ry, g.cx + g.rx, g.cy + g.ry,
                                 rad_x, rad_y, tx, ty, rr, rg, rb, ra);
        }

        // Toggle-mode subtle indicator dot near bottom of button.
        if (ctrl.toggle_mode) {
            const f32 dot_ry = std::max(0.005f, g.ry * 0.12f);
            const f32 dot_rx = dot_ry / aspect;
            const f32 dot_y = g.cy + g.ry * 0.62f;
            push_circle(g.cx, dot_y, dot_rx, dot_ry,
                        ctrl.latched ? 0.35f : 0.95f,
                        ctrl.latched ? 1.00f : 0.85f,
                        ctrl.latched ? 0.62f : 0.35f,
                        (ctrl.latched ? 0.98f : 0.68f) * op, 12);
        }

        const std::string lbl = button_display_label(ctrl);
        const f32 text_max_w = g.rx * (ctrl.shape == TouchControlShape::Circle ? 1.38f : 1.60f);
        const f32 text_max_h = g.ry * (ctrl.shape == TouchControlShape::Circle ? 0.72f : 0.86f);
        push_text_centered(lbl, g.cx, g.cy - (ctrl.toggle_mode ? g.ry * 0.08f : 0.0f),
                           text_max_w, text_max_h, aspect,
                           0.97f, 0.99f, 1.0f, (lit ? 0.98f : (editing_ ? 0.92f : 0.74f)) * op);
    }

    // Selection glow ring & corner resize handle in Edit Mode.
    if (selected && editing_) {
        if (ctrl.kind == TouchControlKind::Dpad || ctrl.shape == TouchControlShape::Circle) {
            push_ring(g.cx, g.cy, g.rx * 1.02f, g.ry * 1.02f, g.rx * 1.08f, g.ry * 1.08f,
                      0.35f, 0.90f, 1.0f, 0.85f, 28);
        } else {
            const f32 pad_y = 0.005f;
            const f32 pad_x = pad_y / aspect;
            push_rect_outline(g.cx - g.rx - pad_x, g.cy - g.ry - pad_y,
                              g.cx + g.rx + pad_x, g.cy + g.ry + pad_y,
                              pad_x * 0.7f, pad_y * 0.7f, 0.35f, 0.90f, 1.0f, 0.85f);
        }

        const RectBox h = resize_handle_box(ctrl, g, aspect);
        push_circle(h.cx(), h.cy(), h.w() * 0.5f, h.h() * 0.5f, 0.12f, 0.78f, 1.0f, 0.96f, 16);
        push_ring(h.cx(), h.cy(), h.w() * 0.40f, h.h() * 0.40f, h.w() * 0.52f, h.h() * 0.52f,
                  1.0f, 1.0f, 1.0f, 0.95f, 16);
        // Tiny inner resize cross/dot.
        push_rect(h.cx() - h.w() * 0.22f, h.cy() - h.h() * 0.06f,
                  h.cx() + h.w() * 0.22f, h.cy() + h.h() * 0.06f, 0.04f, 0.12f, 0.20f, 0.95f);
        push_rect(h.cx() - h.w() * 0.06f, h.cy() - h.h() * 0.22f,
                  h.cx() + h.w() * 0.06f, h.cy() + h.h() * 0.22f, 0.04f, 0.12f, 0.20f, 0.95f);
    }
}

void TouchOverlay::render(i32 drawable_w, i32 drawable_h) {
    if (!initialized_ || !enabled_ || !device_ || drawable_w <= 0 || drawable_h <= 0) {
        return;
    }

    const f32 aspect = std::clamp(static_cast<f32>(drawable_w) / static_cast<f32>(drawable_h), 0.42f, 3.2f);
    if (using_default_layout_ && std::fabs(aspect - last_aspect_) > 0.01f) {
        layout_ = make_default_layout(aspect);
    }
    last_aspect_ = aspect;
    update_system_pills(aspect);
    verts_.clear();

    if (!editing_) {
        // 1. Top-right Hide/Show pill (always visible).
        {
            const f32 rad_y = toggle_pill_.h() * 0.34f;
            const f32 rad_x = rad_y / aspect;
            const f32 bg_a = toggle_pill_pressed_ ? 0.58f : (controls_visible_ ? 0.30f : 0.22f);
            push_rounded_rect(toggle_pill_.x0, toggle_pill_.y0, toggle_pill_.x1, toggle_pill_.y1,
                              rad_x, rad_y, 0.12f, 0.16f, 0.24f, bg_a);
            push_rounded_outline(toggle_pill_.x0, toggle_pill_.y0, toggle_pill_.x1, toggle_pill_.y1,
                                 rad_x, rad_y, 0.0024f, 0.0024f * aspect,
                                 0.88f, 0.94f, 1.0f, controls_visible_ ? 0.42f : 0.28f);
            push_menu_icon(toggle_pill_.cx(), toggle_pill_.cy(),
                           toggle_pill_.w() * 0.28f, toggle_pill_.h() * 0.30f,
                           0.92f, 0.96f, 1.0f, controls_visible_ ? 0.82f : 0.56f);
        }

        if (controls_visible_) {
            // 2. Top-right Edit pill.
            {
                const f32 rad_y = edit_pill_.h() * 0.34f;
                const f32 rad_x = rad_y / aspect;
                const f32 bg_a = edit_pill_pressed_ ? 0.62f : 0.30f;
                push_rounded_rect(edit_pill_.x0, edit_pill_.y0, edit_pill_.x1, edit_pill_.y1,
                                  rad_x, rad_y, 0.12f, 0.16f, 0.24f, bg_a);
                push_rounded_outline(edit_pill_.x0, edit_pill_.y0, edit_pill_.x1, edit_pill_.y1,
                                     rad_x, rad_y, 0.0024f, 0.0024f * aspect,
                                     0.55f, 0.82f, 1.0f, 0.48f);
                push_text_centered("EDIT", edit_pill_.cx(), edit_pill_.cy(),
                                   edit_pill_.w() * 0.76f, edit_pill_.h() * 0.44f,
                                   aspect, 0.90f, 0.96f, 1.0f, 0.80f);
            }

            // 3. Draw all active virtual controls with user opacity.
            for (const auto& ctrl : layout_.controls) {
                const ResolvedGeom g = resolve_control(ctrl, aspect);
                draw_control(ctrl, g, aspect, layout_.opacity, false);
            }
        }
    } else {
        // Edit Mode backdrop scrim + alignment grid.
        push_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.03f, 0.05f, 0.09f, 0.52f);

        const f32 grid_a = layout_.snap_to_grid ? 0.16f : 0.06f;
        const f32 step = layout_.snap_to_grid ? 0.05f : 0.10f;
        const f32 line_x = 0.0012f;
        const f32 line_y = line_x * aspect;
        for (f32 x = step; x < 0.999f; x += step) {
            push_rect(x - line_x * 0.5f, 0.0f, x + line_x * 0.5f, 1.0f, 0.60f, 0.78f, 1.0f, grid_a);
        }
        for (f32 y = step; y < 0.999f; y += step) {
            push_rect(0.0f, y - line_y * 0.5f, 1.0f, y + line_y * 0.5f, 0.60f, 0.78f, 1.0f, grid_a);
        }

        // Draw all virtual controls (unselected first, selected on top).
        for (usize i = 0; i < layout_.controls.size(); ++i) {
            if (static_cast<i32>(i) == selected_idx_) continue;
            const ResolvedGeom g = resolve_control(layout_.controls[i], aspect);
            draw_control(layout_.controls[i], g, aspect, std::max(0.45f, layout_.opacity), false);
        }
        if (selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
            const ResolvedGeom g = resolve_control(layout_.controls[static_cast<usize>(selected_idx_)], aspect);
            draw_control(layout_.controls[static_cast<usize>(selected_idx_)], g, aspect,
                         std::max(0.65f, layout_.opacity), true);
        }

        // Editor Toolbar + Selected Control Inspector (or hint pill).
        std::vector<UiButton> toolbar;
        build_editor_toolbar(aspect, toolbar);
        for (const auto& b : toolbar) {
            draw_ui_button(b, aspect);
        }

        if (selected_idx_ >= 0 && static_cast<usize>(selected_idx_) < layout_.controls.size()) {
            std::vector<UiButton> inspector;
            build_inspector_bar(aspect, inspector);
            for (const auto& b : inspector) {
                draw_ui_button(b, aspect);
            }
        } else if (modal_ == EditorModal::None) {
            const f32 bar_h = (aspect >= 1.0f) ? 0.072f : 0.052f;
            const f32 hy = toolbar_at_bottom_ ? (0.985f - bar_h - 0.036f) : (0.110f + bar_h + 0.034f);
            push_text_centered("TAP OR DRAG CONTROL TO EDIT", 0.5f, hy, 0.88f, 0.028f,
                               aspect, 0.82f, 0.90f, 1.0f, 0.78f);
        }

        // Modal overlays (Key Picker / D-Pad Binding).
        if (modal_ == EditorModal::KeyPicker) {
            push_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.02f, 0.03f, 0.06f, 0.56f);
            RectBox card{}, close_btn{};
            std::vector<UiButton> tabs, keys;
            build_key_picker_ui(aspect, card, close_btn, tabs, keys);

            const f32 rad_y = 0.020f * std::min(1.0f, aspect);
            const f32 rad_x = rad_y / aspect;
            push_rounded_rect(card.x0, card.y0, card.x1, card.y1, rad_x, rad_y, 0.08f, 0.11f, 0.16f, 0.95f);
            push_rounded_outline(card.x0, card.y0, card.x1, card.y1, rad_x, rad_y,
                                 0.003f, 0.003f * aspect, 0.36f, 0.68f, 0.98f, 0.82f);

            std::string title = "PICK KEY (OR PRESS KEY)";
            if (pending_add_button_) {
                title = "ADD BUTTON - PICK KEY";
            } else if (dpad_bind_dir_ >= 0 && dpad_bind_dir_ < 4) {
                static constexpr const char* kDirNames[4] = {"LEFT", "UP", "RIGHT", "DOWN"};
                title = std::string("D-PAD ") + kDirNames[dpad_bind_dir_] + " KEY";
            }
            push_text_centered(title, (card.x0 + close_btn.x0) * 0.5f, close_btn.cy(),
                               (close_btn.x0 - card.x0) * 0.90f, close_btn.h() * 0.44f,
                               aspect, 0.92f, 0.96f, 1.0f, 0.95f);

            draw_ui_button(UiButton{close_btn, "X", 0, false, false, true, 255}, aspect);
            for (const auto& t : tabs) draw_ui_button(t, aspect);
            for (const auto& k : keys) draw_ui_button(k, aspect);
        } else if (modal_ == EditorModal::DpadBind) {
            push_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.02f, 0.03f, 0.06f, 0.56f);
            RectBox card{}, close_btn{};
            std::vector<UiButton> presets, dirs;
            build_dpad_modal_ui(aspect, card, close_btn, presets, dirs);

            const f32 rad_y = 0.020f * std::min(1.0f, aspect);
            const f32 rad_x = rad_y / aspect;
            push_rounded_rect(card.x0, card.y0, card.x1, card.y1, rad_x, rad_y, 0.08f, 0.11f, 0.16f, 0.95f);
            push_rounded_outline(card.x0, card.y0, card.x1, card.y1, rad_x, rad_y,
                                 0.003f, 0.003f * aspect, 0.36f, 0.68f, 0.98f, 0.82f);

            push_text_centered("D-PAD BINDING", (card.x0 + close_btn.x0) * 0.5f, close_btn.cy(),
                               (close_btn.x0 - card.x0) * 0.88f, close_btn.h() * 0.44f,
                               aspect, 0.92f, 0.96f, 1.0f, 0.95f);

            draw_ui_button(UiButton{close_btn, "X", 0, false, false, true, 255}, aspect);
            for (const auto& pr : presets) draw_ui_button(pr, aspect);
            for (const auto& d : dirs) draw_ui_button(d, aspect);
        }
    }

    if (verts_.empty()) return;

    device_->bind_framebuffer(FramebufferHandle{});
    device_->set_viewport(ViewportRect{0, 0, drawable_w, drawable_h});
    device_->set_scissor(false);
    device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    device_->set_cull_mode(CullMode::None);
    device_->set_polygon_mode(2);
    device_->set_color_mask(true, true, true, true);
    device_->set_blend_state(BlendState{true, BlendFactor::SrcAlpha, BlendFactor::InvSrcAlpha,
                                        BlendFactor::One, BlendFactor::InvSrcAlpha,
                                        BlendOp::Add, BlendOp::Add});
    device_->bind_shader(shader_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts_.size() * sizeof(Vertex)),
                 verts_.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size()));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

} // namespace gm
