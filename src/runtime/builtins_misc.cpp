#include "runtime/runner.h"

#include "base/log.h"
#include "base/math.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>

namespace gm {

namespace {

// Minimal loopback state for legacy DirectPlay mplay_* built-ins.
struct MplayState {
    i32 connect_status = 0; // 0=not connected, 1=IPX, 2=TCP/IP, 3=Modem, 4=Serial
    i32 session_status = 0; // 0=none, 1=created, 2=joined
    bool guaranteed_data = false;
    bool auto_move_session = true;
    std::string session_name;
    std::string player_name;
    std::unordered_map<i32, RValue> shared_data;
};

MplayState& mplay_state() {
    static MplayState s;
    return s;
}

} // namespace

void Runner::register_builtins_misc() {
    // -----------------------------------------------------------------------
    // Highscore Configuration & Legacy Dialogs
    // -----------------------------------------------------------------------
    register_builtin("highscore_add_current", [](Runner& r, i32, i32, std::span<const RValue>) {
        const f64 sc = r.score_;
        for (usize i = 0; i < r.highscores_.size(); ++i) {
            if (sc > r.highscores_[i].score) {
                for (usize j = r.highscores_.size() - 1; j > i; --j) {
                    r.highscores_[j] = r.highscores_[j - 1];
                }
                r.highscores_[i] = HighscoreEntry{"Player", sc};
                break;
            }
        }
        return RValue(0.0);
    });
    register_builtin("highscore_set_background", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.highscore_bg_ = arg_i32(a, 0, -1);
        return RValue(0.0);
    });
    register_builtin("highscore_set_border", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.highscore_border_ = arg_bool(a, 0, true);
        return RValue(0.0);
    });
    register_builtin("highscore_set_colors", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.highscore_col_back_ = static_cast<u32>(arg_i32(a, 0)) & 0xFFFFFFu;
        r.highscore_col_new_ = static_cast<u32>(arg_i32(a, 1)) & 0xFFFFFFu;
        r.highscore_col_other_ = static_cast<u32>(arg_i32(a, 2)) & 0xFFFFFFu;
        return RValue(0.0);
    });
    register_builtin("highscore_set_font", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.highscore_font_name_ = std::string(arg_str(a, 0));
        r.highscore_font_size_ = arg_i32(a, 1, 12);
        r.highscore_font_style_ = arg_i32(a, 2, 0);
        return RValue(0.0);
    });
    register_builtin("highscore_set_strings", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.highscore_caption_ = std::string(arg_str(a, 0));
        r.highscore_nobody_ = std::string(arg_str(a, 1));
        r.highscore_escape_ = std::string(arg_str(a, 2));
        return RValue(0.0);
    });
    register_builtin("highscore_show_ext", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });

    register_builtin("message_text_charset", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("show_menu", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_i32(a, 1, 0));
    });
    register_builtin("show_menu_pos", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_i32(a, 3, 0));
    });
    register_builtin("get_color", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(arg_i32(a, 0, 0));
    });
    register_builtin("get_open_filename", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(std::string{});
    });
    register_builtin("get_save_filename", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(std::string{});
    });
    register_builtin("get_directory", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(std::string{});
    });
    register_builtin("get_directory_alt", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(std::string{});
    });

    // -----------------------------------------------------------------------
    // Splash Screens & Legacy Media Helpers
    // -----------------------------------------------------------------------
    register_builtin("splash_show_video", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("splash_show_text", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("splash_show_image", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("splash_show_web", [](Runner&, i32, i32, std::span<const RValue> a) {
        (void)PlatformWindow::open_url(arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("splash_set_main", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_main_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("splash_set_scale", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_scale_ = arg_real(a, 0, -1.0); return RValue(0.0); });
    register_builtin("splash_set_cursor", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_cursor_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("splash_set_color", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_color_ = static_cast<u32>(arg_i32(a, 0)) & 0xFFFFFFu; return RValue(0.0); });
    register_builtin("splash_set_caption", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_caption_ = std::string(arg_str(a, 0)); return RValue(0.0); });
    register_builtin("splash_set_fullscreen", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_fullscreen_ = arg_bool(a, 0, false); return RValue(0.0); });
    register_builtin("splash_set_border", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_border_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("splash_set_size", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_size_w_ = arg_i32(a, 0); r.splash_size_h_ = arg_i32(a, 1); return RValue(0.0); });
    register_builtin("splash_set_position", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_pos_x_ = arg_i32(a, 0); r.splash_pos_y_ = arg_i32(a, 1); return RValue(0.0); });
    register_builtin("splash_set_adapt", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_adapt_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("splash_set_top", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_top_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("splash_set_interrupt", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_interrupt_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("splash_set_stop_key", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_stop_key_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("splash_set_stop_mouse", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_stop_mouse_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("splash_set_close_button", [](Runner& r, i32, i32, std::span<const RValue> a) { r.splash_close_button_ = arg_bool(a, 0, true); return RValue(0.0); });
    register_builtin("show_image", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("show_video", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("show_text", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });

    // -----------------------------------------------------------------------
    // Legacy DirectPlay Multiplayer (mplay_*)
    // -----------------------------------------------------------------------
    register_builtin("mplay_init_ipx", [](Runner&, i32, i32, std::span<const RValue>) { mplay_state().connect_status = 1; return RValue(false); });
    register_builtin("mplay_init_tcpip", [](Runner&, i32, i32, std::span<const RValue>) { mplay_state().connect_status = 2; return RValue(true); });
    register_builtin("mplay_init_modem", [](Runner&, i32, i32, std::span<const RValue>) { mplay_state().connect_status = 3; return RValue(false); });
    register_builtin("mplay_init_serial", [](Runner&, i32, i32, std::span<const RValue>) { mplay_state().connect_status = 4; return RValue(false); });
    register_builtin("mplay_connect_status", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(mplay_state().connect_status); });
    register_builtin("mplay_end", [](Runner&, i32, i32, std::span<const RValue>) {
        mplay_state() = MplayState{};
        return RValue(0.0);
    });
    register_builtin("mplay_ipaddress", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(std::string("127.0.0.1")); });
    register_builtin("mplay_session_create", [](Runner&, i32, i32, std::span<const RValue> a) {
        auto& s = mplay_state();
        s.session_name = std::string(arg_str(a, 0));
        s.player_name = std::string(arg_str(a, 2));
        s.session_status = 1;
        return RValue(true);
    });
    register_builtin("mplay_session_find", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("mplay_session_name", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(mplay_state().session_name); });
    register_builtin("mplay_session_join", [](Runner&, i32, i32, std::span<const RValue> a) {
        mplay_state().player_name = std::string(arg_str(a, 1));
        return RValue(false);
    });
    register_builtin("mplay_session_mode", [](Runner&, i32, i32, std::span<const RValue> a) {
        mplay_state().auto_move_session = arg_bool(a, 0, true);
        return RValue(0.0);
    });
    register_builtin("mplay_session_status", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(mplay_state().session_status); });
    register_builtin("mplay_session_end", [](Runner&, i32, i32, std::span<const RValue>) { mplay_state().session_status = 0; return RValue(0.0); });
    register_builtin("mplay_player_find", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(mplay_state().session_status > 0 ? 1 : 0); });
    register_builtin("mplay_player_name", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(mplay_state().player_name); });
    register_builtin("mplay_player_id", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(1); });
    register_builtin("mplay_data_write", [](Runner&, i32, i32, std::span<const RValue> a) {
        if (a.size() >= 2) mplay_state().shared_data[arg_i32(a, 0)] = a[1];
        return RValue(0.0);
    });
    register_builtin("mplay_data_read", [](Runner&, i32, i32, std::span<const RValue> a) {
        const auto& sd = mplay_state().shared_data;
        if (auto it = sd.find(arg_i32(a, 0)); it != sd.end()) return it->second;
        return RValue(0.0);
    });
    register_builtin("mplay_data_mode", [](Runner&, i32, i32, std::span<const RValue> a) {
        mplay_state().guaranteed_data = arg_bool(a, 0, false);
        return RValue(0.0);
    });
    register_builtin("mplay_message_send", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("mplay_message_send_guaranteed", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("mplay_message_receive", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(false); });
    register_builtin("mplay_message_id", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("mplay_message_value", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("mplay_message_player", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("mplay_message_name", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(std::string{}); });
    register_builtin("mplay_message_count", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("mplay_message_clear", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });

    // -----------------------------------------------------------------------
    // Legacy CD Audio & MCI (non-optical cross-platform safe defaults)
    // -----------------------------------------------------------------------
    register_builtin("cd_init", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("cd_present", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(false); });
    register_builtin("cd_number", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("cd_playing", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(false); });
    register_builtin("cd_paused", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(false); });
    register_builtin("cd_track", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("cd_length", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("cd_track_length", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("cd_position", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("cd_track_position", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("cd_set_position", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("cd_set_track_position", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("cd_play", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("cd_stop", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("cd_pause", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("cd_resume", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("cd_open_door", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("cd_close_door", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("MCI_command", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(std::string{}); });

    // -----------------------------------------------------------------------
    // Fixed-Function D3D Projection, Transforms & Primitives
    // (Used by GM80_parasoko 3D puzzle rooms & GM81_kamilia HUD projections)
    // -----------------------------------------------------------------------
    register_builtin("d3d_start", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.renderer().d3d_start();
        return RValue(true);
    });
    register_builtin("d3d_end", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.renderer().d3d_end();
        return RValue(true);
    });
    register_builtin("d3d_set_perspective", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_perspective(arg_bool(a, 0));
        return RValue(0.0);
    });
    register_builtin("d3d_set_hidden", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_hidden(arg_bool(a, 0));
        return RValue(0.0);
    });
    register_builtin("d3d_set_depth", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_depth(static_cast<f32>(arg_real(a, 0)));
        return RValue(0.0);
    });
    register_builtin("d3d_set_zwriteenable", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_zwriteenable(arg_bool(a, 0, true));
        return RValue(0.0);
    });
    register_builtin("d3d_set_culling", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_culling(arg_bool(a, 0));
        return RValue(0.0);
    });
    register_builtin("d3d_set_lighting", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("d3d_set_shading", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("d3d_set_fog", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });

    register_builtin("d3d_set_projection", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_projection(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            static_cast<f32>(arg_real(a, 6)), static_cast<f32>(arg_real(a, 7)), static_cast<f32>(arg_real(a, 8)));
        return RValue(0.0);
    });
    register_builtin("d3d_set_projection_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_projection_ext(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            static_cast<f32>(arg_real(a, 6)), static_cast<f32>(arg_real(a, 7)), static_cast<f32>(arg_real(a, 8)),
            static_cast<f32>(arg_real(a, 9, 45.0)), static_cast<f32>(arg_real(a, 10, 4.0 / 3.0)),
            static_cast<f32>(arg_real(a, 11, 1.0)), static_cast<f32>(arg_real(a, 12, 32000.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_set_projection_ortho", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_projection_ortho(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4, 0.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_set_projection_perspective", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_set_projection_perspective(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4, 0.0)));
        return RValue(0.0);
    });

    register_builtin("d3d_transform_set_identity", [](Runner& r, i32, i32, std::span<const RValue>) { r.renderer().d3d_transform_set_identity(); return RValue(0.0); });
    register_builtin("d3d_transform_set_translation", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_transform_set_translation(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)));
        return RValue(0.0);
    });
    register_builtin("d3d_transform_set_scaling", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_transform_set_scaling(static_cast<f32>(arg_real(a, 0, 1.0)), static_cast<f32>(arg_real(a, 1, 1.0)), static_cast<f32>(arg_real(a, 2, 1.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_transform_set_rotation_x", [](Runner& r, i32, i32, std::span<const RValue> a) { r.renderer().d3d_transform_set_rotation_x(static_cast<f32>(arg_real(a, 0))); return RValue(0.0); });
    register_builtin("d3d_transform_set_rotation_y", [](Runner& r, i32, i32, std::span<const RValue> a) { r.renderer().d3d_transform_set_rotation_y(static_cast<f32>(arg_real(a, 0))); return RValue(0.0); });
    register_builtin("d3d_transform_set_rotation_z", [](Runner& r, i32, i32, std::span<const RValue> a) { r.renderer().d3d_transform_set_rotation_z(static_cast<f32>(arg_real(a, 0))); return RValue(0.0); });
    register_builtin("d3d_transform_set_rotation_axis", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_transform_set_rotation_axis(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)));
        return RValue(0.0);
    });
    register_builtin("d3d_transform_add_translation", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_transform_add_translation(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)));
        return RValue(0.0);
    });
    register_builtin("d3d_transform_add_scaling", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_transform_add_scaling(static_cast<f32>(arg_real(a, 0, 1.0)), static_cast<f32>(arg_real(a, 1, 1.0)), static_cast<f32>(arg_real(a, 2, 1.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_transform_add_rotation_x", [](Runner& r, i32, i32, std::span<const RValue> a) { r.renderer().d3d_transform_add_rotation_x(static_cast<f32>(arg_real(a, 0))); return RValue(0.0); });
    register_builtin("d3d_transform_add_rotation_y", [](Runner& r, i32, i32, std::span<const RValue> a) { r.renderer().d3d_transform_add_rotation_y(static_cast<f32>(arg_real(a, 0))); return RValue(0.0); });
    register_builtin("d3d_transform_add_rotation_z", [](Runner& r, i32, i32, std::span<const RValue> a) { r.renderer().d3d_transform_add_rotation_z(static_cast<f32>(arg_real(a, 0))); return RValue(0.0); });
    register_builtin("d3d_transform_add_rotation_axis", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_transform_add_rotation_axis(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)));
        return RValue(0.0);
    });
    register_builtin("d3d_transform_stack_clear", [](Runner& r, i32, i32, std::span<const RValue>) { r.renderer().d3d_transform_stack_clear(); return RValue(0.0); });
    register_builtin("d3d_transform_stack_empty", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.renderer().d3d_transform_stack_empty()); });
    register_builtin("d3d_transform_stack_push", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.renderer().d3d_transform_stack_push()); });
    register_builtin("d3d_transform_stack_pop", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.renderer().d3d_transform_stack_pop()); });
    register_builtin("d3d_transform_stack_top", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.renderer().d3d_transform_stack_top()); });
    register_builtin("d3d_transform_stack_discard", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.renderer().d3d_transform_stack_discard()); });

    register_builtin("d3d_primitive_begin", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_begin(arg_i32(a, 0, 4), TextureHandle{});
        return RValue(0.0);
    });
    register_builtin("d3d_primitive_begin_texture", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_begin(arg_i32(a, 0, 4), TextureHandle{static_cast<u32>(arg_i32(a, 1, 0))});
        return RValue(0.0);
    });
    register_builtin("d3d_primitive_end", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.renderer().primitive_end();
        return RValue(0.0);
    });
    register_builtin("d3d_vertex", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex_3d(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                         0.0f, 0.0f, r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("d3d_vertex_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex_3d(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                         0.0f, 0.0f, static_cast<u32>(arg_i32(a, 3)), static_cast<f32>(arg_real(a, 4, 1.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_vertex_texture", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex_3d(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                         static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("d3d_vertex_texture_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex_3d(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                         static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
                                         static_cast<u32>(arg_i32(a, 5)), static_cast<f32>(arg_real(a, 6, 1.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_vertex_normal", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex_3d(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                         0.0f, 0.0f, r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("d3d_vertex_normal_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex_3d(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                         0.0f, 0.0f, static_cast<u32>(arg_i32(a, 6)), static_cast<f32>(arg_real(a, 7, 1.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_vertex_normal_texture", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex_3d(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                         static_cast<f32>(arg_real(a, 6)), static_cast<f32>(arg_real(a, 7)), r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("d3d_vertex_normal_texture_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex_3d(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                         static_cast<f32>(arg_real(a, 6)), static_cast<f32>(arg_real(a, 7)),
                                         static_cast<u32>(arg_i32(a, 8)), static_cast<f32>(arg_real(a, 9, 1.0)));
        return RValue(0.0);
    });

    register_builtin("d3d_draw_block", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_draw_block(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            TextureHandle{static_cast<u32>(arg_i32(a, 6, 0))},
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_draw_floor", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_draw_floor(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            TextureHandle{static_cast<u32>(arg_i32(a, 6, 0))},
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_draw_wall", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_draw_wall(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            TextureHandle{static_cast<u32>(arg_i32(a, 6, 0))},
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)));
        return RValue(0.0);
    });
    register_builtin("d3d_draw_cylinder", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_draw_cylinder(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            TextureHandle{static_cast<u32>(arg_i32(a, 6, 0))},
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)),
            arg_bool(a, 9, true), arg_i32(a, 10, 24));
        return RValue(0.0);
    });
    register_builtin("d3d_draw_cone", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_draw_cone(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            TextureHandle{static_cast<u32>(arg_i32(a, 6, 0))},
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)),
            arg_bool(a, 9, true), arg_i32(a, 10, 24));
        return RValue(0.0);
    });
    register_builtin("d3d_draw_ellipsoid", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().d3d_draw_ellipsoid(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            TextureHandle{static_cast<u32>(arg_i32(a, 6, 0))},
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)),
            arg_i32(a, 9, 24));
        return RValue(0.0);
    });
}

} // namespace gm
