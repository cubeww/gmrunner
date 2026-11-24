# Incomplete GML Built-in Functions and Variables

This document lists GameMaker 8.0 / 8.1 / 8.2 built-in GML functions and variables in `gmrunner` that are currently **unimplemented stubs**, **state-only / local mocks**, or **partially implemented** (simplified behavior or platform limitations).

> **Coverage Overview**
> - Catalog total: `1,282` built-in functions and `166` built-in variables (all registered; `0` unregistered).
> - Recently completed (not listed below): game save/load (`game_save`, `game_load`, `action_save_game`, `action_load_game`, and F5/F6 quick-save/load) and room transitions (all 21 built-in transitions, `transition_define`, `transition_exists`, `transition_kind`, `transition_steps`).

---

## Status Legend

| Status | Meaning |
| :--- | :--- |
| **Stub** | Directly returns a fixed constant or passes through a default argument without performing the underlying behavior |
| **State-only** | Stores settings into `Runner` fields (or no-ops), but the corresponding UI, renderer, or backend is not yet implemented |
| **Local mock** | Maintains single-process in-memory state only, with no real network or hardware transport |
| **Partial** | Common path works, but specific parameters, legacy binary formats, or cross-platform behaviors differ from original GM8 |
| **Legacy** | Relies on obsolete Win32 / DirectX 8 / DirectPlay / physical CD-ROM hardware APIs |

---

## 1. Highscore Dialog & D&D Highscore Actions

> Note: Highscore table data manipulation (`highscore_clear`, `highscore_add`, `highscore_value`, `highscore_name`, `action_highscore_clear`) is implemented. Modal highscore display dialogs are not yet implemented, so appearance setters currently only store fields.

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `highscore_show` | `src/runtime/builtins_core.cpp:1845` | Stub | Returns `0.0`; modal highscore dialog not rendered |
| `highscore_show_ext` | `src/runtime/builtins_misc.cpp:76` | Stub | Returns `0.0`; extended styled highscore dialog not rendered |
| `action_highscore_show` | `src/runtime/builtins_actions.cpp:674` | Stub | Returns `0.0` |
| `action_highscore` | `src/runtime/builtins_actions.cpp:675` | Stub | Returns `0.0` |
| `highscore_add_current` | `src/runtime/builtins_misc.cpp:37` | Partial | Inserts current `score` with hardcoded name `"Player"` instead of prompting for the player's name |
| `highscore_set_background` | `src/runtime/builtins_misc.cpp:50` | State-only | Stores `highscore_bg_`; not yet consumed by a highscore dialog |
| `highscore_set_border` | `src/runtime/builtins_misc.cpp:54` | State-only | Stores `highscore_border_` |
| `highscore_set_colors` | `src/runtime/builtins_misc.cpp:58` | State-only | Stores `highscore_col_*_` |
| `highscore_set_font` | `src/runtime/builtins_misc.cpp:64` | State-only | Stores `highscore_font_*_` |
| `highscore_set_strings` | `src/runtime/builtins_misc.cpp:70` | State-only | Stores `highscore_caption_`, `highscore_nobody_`, `highscore_escape_` |

---

## 2. System Dialogs, File Pickers & Game Info

> Note: Cross-platform self-drawn message and input dialogs (`show_message`, `show_message_ext`, `show_question`, `get_string`, `get_integer`, and `message_*` customization) are implemented. File/directory pickers, color pickers, popup menus, and RTF game info windows are not yet implemented.

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `show_menu` | `src/runtime/builtins_misc.cpp:82` | Stub | Returns default choice `arg1` without showing a popup menu |
| `show_menu_pos` | `src/runtime/builtins_misc.cpp:85` | Stub | Returns default choice `arg3` without showing a popup menu |
| `get_color` | `src/runtime/builtins_misc.cpp:88` | Stub | Returns default color `arg0` without showing a color picker |
| `get_open_filename` | `src/runtime/builtins_misc.cpp:91` | Stub | Returns `""`; not wired to SDL3 / native file open dialog |
| `get_save_filename` | `src/runtime/builtins_misc.cpp:94` | Stub | Returns `""`; not wired to SDL3 / native file save dialog |
| `get_directory` | `src/runtime/builtins_misc.cpp:97` | Stub | Returns `""`; not wired to directory picker dialog |
| `get_directory_alt` | `src/runtime/builtins_misc.cpp:100` | Stub | Returns `""`; not wired to directory picker dialog |
| `show_info` | `src/runtime/builtins_core.cpp:1511` | Stub | Returns `0.0`; RTF game information viewer not implemented |
| `load_info` | `src/runtime/builtins_core.cpp:1512` | Stub | Returns `0.0`; does not load external RTF help files |
| `action_show_info` | `src/runtime/builtins_actions.cpp:676` | Stub | Returns `0.0` |

---

## 3. Splash Screens & Legacy Media Viewers

> Note: `splash_show_web` and `action_splash_web` open URLs via `PlatformWindow::open_url`. Video, text/RTF, and image splash viewers are not yet implemented.

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `splash_show_video` | `src/runtime/builtins_misc.cpp:107` | Stub | Returns `0.0`; video playback not implemented |
| `splash_show_text` | `src/runtime/builtins_misc.cpp:108` | Stub | Returns `0.0`; text/RTF splash viewer not implemented |
| `splash_show_image` | `src/runtime/builtins_misc.cpp:109` | Stub | Returns `0.0`; image splash viewer not implemented |
| `show_image` | `src/runtime/builtins_misc.cpp:129` | Stub | Returns `0.0` (legacy alias) |
| `show_video` | `src/runtime/builtins_misc.cpp:130` | Stub | Returns `0.0` (legacy alias) |
| `show_text` | `src/runtime/builtins_misc.cpp:131` | Stub | Returns `0.0` (legacy alias) |
| `action_show_video` | `src/runtime/builtins_actions.cpp:996` | Stub | Returns `0.0` |
| `action_splash_video` | `src/runtime/builtins_actions.cpp:997` | Stub | Returns `0.0` |
| `action_splash_text` | `src/runtime/builtins_actions.cpp:998` | Stub | Returns `0.0` |
| `action_splash_image` | `src/runtime/builtins_actions.cpp:999` | Stub | Returns `0.0` |
| `splash_set_main` | `src/runtime/builtins_misc.cpp:114` | State-only | Stores `splash_main_`; no splash renderer consumes it |
| `splash_set_scale` | `src/runtime/builtins_misc.cpp:115` | State-only | Stores `splash_scale_` |
| `splash_set_cursor` | `src/runtime/builtins_misc.cpp:116` | State-only | Stores `splash_cursor_` |
| `splash_set_color` | `src/runtime/builtins_misc.cpp:117` | State-only | Stores `splash_color_` |
| `splash_set_caption` | `src/runtime/builtins_misc.cpp:118` | State-only | Stores `splash_caption_` |
| `splash_set_fullscreen` | `src/runtime/builtins_misc.cpp:119` | State-only | Stores `splash_fullscreen_` |
| `splash_set_border` | `src/runtime/builtins_misc.cpp:120` | State-only | Stores `splash_border_` |
| `splash_set_size` | `src/runtime/builtins_misc.cpp:121` | State-only | Stores `splash_size_w_`, `splash_size_h_` |
| `splash_set_position` | `src/runtime/builtins_misc.cpp:122` | State-only | Stores `splash_pos_x_`, `splash_pos_y_` |
| `splash_set_adapt` | `src/runtime/builtins_misc.cpp:123` | State-only | Stores `splash_adapt_` |
| `splash_set_top` | `src/runtime/builtins_misc.cpp:124` | State-only | Stores `splash_top_` |
| `splash_set_interrupt` | `src/runtime/builtins_misc.cpp:125` | State-only | Stores `splash_interrupt_` |
| `splash_set_stop_key` | `src/runtime/builtins_misc.cpp:126` | State-only | Stores `splash_stop_key_` |
| `splash_set_stop_mouse` | `src/runtime/builtins_misc.cpp:127` | State-only | Stores `splash_stop_mouse_` |
| `splash_set_close_button` | `src/runtime/builtins_misc.cpp:128` | State-only | Stores `splash_close_button_` |
| `action_splash_settings` | `src/runtime/builtins_actions.cpp:1004` | State-only | Stores `splash_caption_`, `splash_fullscreen_`, `splash_border_` |

---

## 4. Keyboard Remapping & Lock Key Control

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `keyboard_set_map` | `src/runtime/builtins_gameplay.cpp:1188` | Stub | Returns `0.0`; virtual key remap table not implemented |
| `keyboard_get_map` | `src/runtime/builtins_gameplay.cpp:1189` | Stub | Returns input keycode `arg0` unchanged |
| `keyboard_unset_map` | `src/runtime/builtins_gameplay.cpp:1190` | Stub | Returns `0.0`; does not reset key mappings |
| `keyboard_set_numlock` | `src/runtime/builtins_gameplay.cpp:1185` | Stub | Returns `0.0` (no cross-platform API to toggle OS lock keys) |
| `keyboard_set_capslock` | `src/runtime/builtins_gameplay.cpp:1186` | Stub | Returns `0.0` |
| `keyboard_set_scrolllock` | `src/runtime/builtins_gameplay.cpp:1187` | Stub | Returns `0.0` |

---

## 5. Display Modes, Window Cursor & Legacy Cache Hints

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `display_get_colordepth` | `src/runtime/builtins_graphics.cpp:1122` | Stub | Hardcoded to `32` |
| `display_get_frequency` | `src/runtime/builtins_graphics.cpp:1123` | Stub | Hardcoded to `60`; does not query actual display refresh rate |
| `display_set_size` | `src/runtime/builtins_graphics.cpp:1124` | Stub | Returns `true`; does not change OS display resolution |
| `display_set_colordepth` | `src/runtime/builtins_graphics.cpp:1125` | Stub | Returns `true` |
| `display_set_frequency` | `src/runtime/builtins_graphics.cpp:1126` | Stub | Returns `true` |
| `display_set_all` | `src/runtime/builtins_graphics.cpp:1127` | Stub | Returns `true` |
| `display_test_all` | `src/runtime/builtins_graphics.cpp:1128` | Stub | Returns `true` |
| `display_reset` | `src/runtime/builtins_graphics.cpp:1129` | Stub | Returns `0.0` |
| `screen_wait_vsync` | `src/runtime/builtins_graphics.cpp:1146` | Stub | Returns `0.0`; does not explicitly block for VBlank |
| `window_set_cursor` | `src/runtime/builtins_graphics.cpp:1016` | Partial | Only toggles visibility (`!= cr_none (-1)`); does not switch native SDL3 cursor shapes for `cr_*` constants |
| `sprite_set_cache_size` | `src/runtime/builtins_gameplay.cpp:2079` | Legacy / Stub | Returns `0.0` (obsolete D3D8 sprite cache hint) |
| `sprite_set_cache_size_ext` | `src/runtime/builtins_gameplay.cpp:2080` | Legacy / Stub | Returns `0.0` |
| `texture_set_priority` | `src/runtime/builtins_graphics.cpp:827` | Legacy / Stub | Returns `0.0` (obsolete D3D8 VRAM priority hint) |
| `texture_preload` | `src/runtime/builtins_graphics.cpp:840` | Legacy / Stub | Returns `0.0` (textures are already uploaded eagerly on load) |

---

## 6. Dynamic Resources & Legacy Drawing Actions (Partial)

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `font_add` | `src/runtime/builtins_gameplay.cpp:2330` | Partial | Creates `FontData` metadata without dynamically rasterizing system TrueType fonts into a glyph atlas; rendering falls back to the built-in font (`font_add_sprite` works properly) |
| `font_replace` | `src/runtime/builtins_gameplay.cpp:2343` | Partial | Same as above: updates metadata only without re-rasterizing glyphs |
| `background_add_background` | `src/runtime/builtins_gameplay.cpp:2235` | Partial | Decodes standard image files (PNG/BMP/JPG) instead of GM8 native `.gmbck` container files |
| `background_replace_background` | `src/runtime/builtins_gameplay.cpp:2243` | Partial | Same as above: does not parse GM8 `.gmbck` container format |
| `action_draw_font_old` | `src/runtime/builtins_actions.cpp:714` | Partial | Only applies horizontal alignment (`halign`); ignores legacy font name/size/style arguments |
| `sound_discard` | `src/audio/audio_engine.cpp:1010` | State-only / No-op | No-op (decoded audio buffers remain resident in memory) |
| `sound_restore` | `src/audio/audio_engine.cpp:1011` | State-only / No-op | No-op |

---

## 7. Process Execution & YoYo Platform Extensions

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `execute_program` | `src/runtime/builtins_core.cpp:1749` | Partial | Opens `http://`/`https://` URLs via browser; intentionally does not spawn arbitrary external executables or wait for exit |
| `execute_shell` | `src/runtime/builtins_core.cpp:1810` | Partial | Same as above: handles HTTP(S) URLs only, not arbitrary local shell file associations |
| `set_program_priority` | `src/runtime/builtins_core.cpp:1593` | Stub | Returns `0.0`; does not modify OS process scheduling priority |
| `YoYo_GetPlatform` | `src/runtime/builtins_core.cpp:1776` | Stub | Hardcoded to `0` (`os_win32`); does not report actual runtime OS |
| `YoYo_GetDevice` | `src/runtime/builtins_core.cpp:1777` | Stub | Hardcoded to `0` |
| `YoYo_GetDomain` | `src/runtime/builtins_core.cpp:1778` | Stub | Hardcoded to `""` |
| `YoYo_AddVirtualKey` | `src/runtime/builtins_core.cpp:1798` | Stub | Returns `-1`; not connected to the built-in touch overlay |
| `YoYo_DeleteVirtualKey` | `src/runtime/builtins_core.cpp:1799` | Stub | Returns `0.0` |
| `YoYo_ShowVirtualKey` | `src/runtime/builtins_core.cpp:1800` | Stub | Returns `0.0` |
| `YoYo_HideVirtualKey` | `src/runtime/builtins_core.cpp:1801` | Stub | Returns `0.0` |

---

## 8. Legacy DirectPlay Multiplayer (`mplay_*`)

> Note: Original GM8 `mplay_*` relied on deprecated Windows DirectPlay. All 29 `mplay_*` functions are currently single-process in-memory mocks or stubs with no real networking.

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `mplay_init_ipx` | `src/runtime/builtins_misc.cpp:136` | Local mock | Sets `connect_status = 1`, returns `false` |
| `mplay_init_tcpip` | `src/runtime/builtins_misc.cpp:137` | Local mock | Sets `connect_status = 2`, returns `true`; no real socket transport |
| `mplay_init_modem` | `src/runtime/builtins_misc.cpp:138` | Local mock | Sets `connect_status = 3`, returns `false` |
| `mplay_init_serial` | `src/runtime/builtins_misc.cpp:139` | Local mock | Sets `connect_status = 4`, returns `false` |
| `mplay_connect_status` | `src/runtime/builtins_misc.cpp:140` | Local mock | Returns mock `connect_status` |
| `mplay_end` | `src/runtime/builtins_misc.cpp:141` | Local mock | Resets local mock state |
| `mplay_ipaddress` | `src/runtime/builtins_misc.cpp:145` | Local mock | Hardcoded to `"127.0.0.1"` |
| `mplay_session_create` | `src/runtime/builtins_misc.cpp:146` | Local mock | Records local session/player names, returns `true` |
| `mplay_session_find` | `src/runtime/builtins_misc.cpp:153` | Stub | Returns `0` |
| `mplay_session_name` | `src/runtime/builtins_misc.cpp:154` | Local mock | Returns locally stored session name |
| `mplay_session_join` | `src/runtime/builtins_misc.cpp:155` | Local mock | Stores player name, returns `false` |
| `mplay_session_mode` | `src/runtime/builtins_misc.cpp:159` | Local mock | Stores `auto_move_session` flag |
| `mplay_session_status` | `src/runtime/builtins_misc.cpp:163` | Local mock | Returns local status (`0` or `1`) |
| `mplay_session_end` | `src/runtime/builtins_misc.cpp:164` | Local mock | Sets `session_status = 0` |
| `mplay_player_find` | `src/runtime/builtins_misc.cpp:165` | Local mock | Returns `1` if local session active, else `0` |
| `mplay_player_name` | `src/runtime/builtins_misc.cpp:166` | Local mock | Returns locally stored player name |
| `mplay_player_id` | `src/runtime/builtins_misc.cpp:167` | Stub | Returns `1` |
| `mplay_data_write` | `src/runtime/builtins_misc.cpp:168` | Local mock | Writes only to in-process map; no network sync |
| `mplay_data_read` | `src/runtime/builtins_misc.cpp:172` | Local mock | Reads only from in-process map |
| `mplay_data_mode` | `src/runtime/builtins_misc.cpp:177` | Local mock | Stores flag only |
| `mplay_message_send` | `src/runtime/builtins_misc.cpp:181` | Stub | Returns `0.0` |
| `mplay_message_send_guaranteed` | `src/runtime/builtins_misc.cpp:182` | Stub | Returns `0.0` |
| `mplay_message_receive` | `src/runtime/builtins_misc.cpp:183` | Stub | Returns `false` |
| `mplay_message_id` | `src/runtime/builtins_misc.cpp:184` | Stub | Returns `0` |
| `mplay_message_value` | `src/runtime/builtins_misc.cpp:185` | Stub | Returns `0.0` |
| `mplay_message_player` | `src/runtime/builtins_misc.cpp:186` | Stub | Returns `0` |
| `mplay_message_name` | `src/runtime/builtins_misc.cpp:187` | Stub | Returns `""` |
| `mplay_message_count` | `src/runtime/builtins_misc.cpp:188` | Stub | Returns `0` |
| `mplay_message_clear` | `src/runtime/builtins_misc.cpp:189` | Stub | Returns `0.0` |

---

## 9. Physical CD Audio & Windows MCI (Legacy)

> Note: Depends on physical CD-ROM hardware and the Windows MCI subsystem.

| Function | Location | Status | Current Behavior |
| :--- | :--- | :--- | :--- |
| `cd_init` | `src/runtime/builtins_misc.cpp:194` | Legacy / Stub | Returns `0.0` |
| `cd_present` | `src/runtime/builtins_misc.cpp:195` | Legacy / Stub | Returns `false` |
| `cd_number` | `src/runtime/builtins_misc.cpp:196` | Legacy / Stub | Returns `0` |
| `cd_playing` | `src/runtime/builtins_misc.cpp:197` | Legacy / Stub | Returns `false` |
| `cd_paused` | `src/runtime/builtins_misc.cpp:198` | Legacy / Stub | Returns `false` |
| `cd_track` | `src/runtime/builtins_misc.cpp:199` | Legacy / Stub | Returns `0` |
| `cd_length` | `src/runtime/builtins_misc.cpp:200` | Legacy / Stub | Returns `0` |
| `cd_track_length` | `src/runtime/builtins_misc.cpp:201` | Legacy / Stub | Returns `0` |
| `cd_position` | `src/runtime/builtins_misc.cpp:202` | Legacy / Stub | Returns `0` |
| `cd_track_position` | `src/runtime/builtins_misc.cpp:203` | Legacy / Stub | Returns `0` |
| `cd_set_position` | `src/runtime/builtins_misc.cpp:204` | Legacy / Stub | Returns `0.0` |
| `cd_set_track_position` | `src/runtime/builtins_misc.cpp:205` | Legacy / Stub | Returns `0.0` |
| `cd_play` | `src/runtime/builtins_misc.cpp:206` | Legacy / Stub | Returns `0.0` |
| `cd_stop` | `src/runtime/builtins_misc.cpp:207` | Legacy / Stub | Returns `0.0` |
| `cd_pause` | `src/runtime/builtins_misc.cpp:208` | Legacy / Stub | Returns `0.0` |
| `cd_resume` | `src/runtime/builtins_misc.cpp:209` | Legacy / Stub | Returns `0.0` |
| `cd_open_door` | `src/runtime/builtins_misc.cpp:210` | Legacy / Stub | Returns `0.0` |
| `cd_close_door` | `src/runtime/builtins_misc.cpp:211` | Legacy / Stub | Returns `0.0` |
| `MCI_command` | `src/runtime/builtins_misc.cpp:212` | Legacy / Stub | Returns `""` |
| `action_cd_play` | `src/runtime/builtins_actions.cpp:1010` | Legacy / Stub | Returns `0.0` |
| `action_cd_stop` | `src/runtime/builtins_actions.cpp:1011` | Legacy / Stub | Returns `0.0` |
| `action_cd_pause` | `src/runtime/builtins_actions.cpp:1012` | Legacy / Stub | Returns `0.0` |
| `action_cd_resume` | `src/runtime/builtins_actions.cpp:1013` | Legacy / Stub | Returns `0.0` |
| `action_cd_present` | `src/runtime/builtins_actions.cpp:1014` | Legacy / Stub | Returns `false` |
| `action_cd_playing` | `src/runtime/builtins_actions.cpp:1015` | Legacy / Stub | Returns `false` |

---

## 10. Incomplete Built-in Variables

| Variable | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `cursor_sprite` (animation) | `src/runtime/runner.cpp:2002` | Partial | Always draws subimage `0` instead of advancing `cursor_subimg` per step |
| `os_type` | `src/runtime/runner.cpp:2679` | Hardcoded | Always returns `0` (`os_windows`) regardless of host platform |
| `os_device` | `src/runtime/runner.cpp:2680` | Hardcoded | Always returns `0` |
| `os_browser` | `src/runtime/runner.cpp:2681` | Hardcoded | Always returns `-1` (`browser_not_a_browser`), even on WebAssembly |
| `os_version` | `src/runtime/runner.cpp:2682` | Hardcoded | Always returns `0` |
| `display_aa` | `src/runtime/runner.cpp:2685` | Hardcoded | Always returns `0` |
| `async_load` | `src/runtime/runner.cpp:2686` | Hardcoded | Always returns `-1` |
| `secure_mode` | `src/runtime/runner.cpp:2672` | Hardcoded | Always returns `0` (sandboxed secure mode not enforced) |

---

## Suggested Implementation Priority

1. **High practical impact**: `keyboard_set_map`/`keyboard_get_map`/`keyboard_unset_map`, `window_set_cursor` (SDL3 system cursors), `font_add`/`font_replace` (runtime TrueType rasterization), and `cursor_sprite` frame animation.
2. **Medium practical impact**: `highscore_show`/`highscore_show_ext`/`highscore_add_current` (reusing the self-drawn modal dialog framework), `get_open_filename`/`get_save_filename`/`get_directory` (SDL3 file dialogs), `show_menu`/`show_menu_pos`, `get_color`, `splash_show_image`/`splash_show_text`, `show_info`/`load_info`, and `.gmbck` decoding in `background_add_background`.
3. **Low priority / legacy**: `display_set_*` (superseded by borderless/scaled windows), `mplay_*` (DirectPlay), `cd_*`/`MCI_command` (CD-ROM hardware), and `sprite_set_cache_size`/`texture_preload`.
