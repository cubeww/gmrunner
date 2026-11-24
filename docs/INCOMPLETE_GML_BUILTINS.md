# Incomplete GML Built-in Functions and Variables

This document lists GameMaker 8.0 / 8.1 / 8.2 built-in GML functions and variables in `gmrunner` that are currently **unimplemented stubs**, **state-only / local mocks**, or **partially implemented** (simplified behavior or platform limitations).

> **Coverage Overview**
> - Catalog total: `1,282` built-in functions and `166` built-in variables (all registered; `0` unregistered).
> - Recently completed (not listed below):
>   - Game save/load (`game_save`, `game_load`, `action_save_game`, `action_load_game`, and F5/F6 quick-save/load) and room transitions (all 21 built-in transitions, `transition_define`, `transition_exists`, `transition_kind`, `transition_steps`).
>   - Keyboard remapping (`keyboard_set_map`, `keyboard_get_map`, `keyboard_unset_map`), native SDL3 window cursors (`window_set_cursor` for all 22 `cr_*` constants), `cursor_sprite` per-step animation, runtime font rasterization (`font_add`, `font_replace`), and `.gmbck` container decoding (`background_add_background`, `background_replace_background`).
>   - Self-drawn GM8 modal dialogs & viewers: highscore dialog (`highscore_show`, `highscore_show_ext`, `highscore_add_current`, `action_highscore_show`, `action_highscore`, `highscore_set_*`), popup menus (`show_menu`, `show_menu_pos`), color picker (`get_color`), RTF/plain-text Game Information viewer (`show_info`, `load_info`, `action_show_info`, F1 hotkey), image & text/RTF splash screens (`splash_show_image`, `splash_show_text`, `show_image`, `show_text`, `action_splash_image`, `action_splash_text`, `action_splash_settings`, `splash_set_*`), and native SDL3 file/directory dialogs (`get_open_filename`, `get_save_filename`, `get_directory`, `get_directory_alt`; desktop platforms, returning `""` on WebAssembly).

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

## 1. Legacy Video Splash Viewers

> Note: `splash_show_web` / `action_splash_web` (via `PlatformWindow::open_url`), `splash_show_image` / `show_image` / `action_splash_image`, `splash_show_text` / `show_text` / `action_splash_text`, and all `splash_set_*` / `action_splash_settings` options are implemented. Only video playback remains unimplemented.

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `splash_show_video` | `src/runtime/builtins_misc.cpp:111` | Stub | Returns `0.0`; video playback not implemented |
| `show_video` | `src/runtime/builtins_misc.cpp:146` | Stub | Returns `0.0` (legacy alias) |
| `action_show_video` | `src/runtime/builtins_actions.cpp:1065` | Stub | Returns `0.0` |
| `action_splash_video` | `src/runtime/builtins_actions.cpp:1066` | Stub | Returns `0.0` |

---

## 2. Keyboard Lock Key Control

> Note: Virtual key remapping (`keyboard_set_map`, `keyboard_get_map`, `keyboard_unset_map`) is implemented.

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `keyboard_set_numlock` | `src/runtime/builtins_gameplay.cpp:1186` | Stub | Returns `0.0` (no cross-platform API to toggle OS lock keys) |
| `keyboard_set_capslock` | `src/runtime/builtins_gameplay.cpp:1187` | Stub | Returns `0.0` |
| `keyboard_set_scrolllock` | `src/runtime/builtins_gameplay.cpp:1188` | Stub | Returns `0.0` |

---

## 3. Display Modes & Legacy Cache Hints

> Note: Native SDL3 system cursors (`window_set_cursor` for all GM8 `cr_*` constants) are implemented.

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
| `sprite_set_cache_size` | `src/runtime/builtins_gameplay.cpp:2088` | Legacy / Stub | Returns `0.0` (obsolete D3D8 sprite cache hint) |
| `sprite_set_cache_size_ext` | `src/runtime/builtins_gameplay.cpp:2089` | Legacy / Stub | Returns `0.0` |
| `texture_set_priority` | `src/runtime/builtins_graphics.cpp:827` | Legacy / Stub | Returns `0.0` (obsolete D3D8 VRAM priority hint) |
| `texture_preload` | `src/runtime/builtins_graphics.cpp:840` | Legacy / Stub | Returns `0.0` (textures are already uploaded eagerly on load) |

---

## 4. Legacy Drawing Actions & Audio Buffer Hints (Partial)

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `action_draw_font_old` | `src/runtime/builtins_actions.cpp:783` | Partial | Only applies horizontal alignment (`halign`); ignores legacy font name/size/style arguments |
| `sound_discard` | `src/audio/audio_engine.cpp:1010` | State-only / No-op | No-op (decoded audio buffers remain resident in memory) |
| `sound_restore` | `src/audio/audio_engine.cpp:1011` | State-only / No-op | No-op |

---

## 5. Process Execution & YoYo Platform Extensions

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `execute_program` | `src/runtime/builtins_core.cpp:1754` | Partial | Opens `http://`/`https://` URLs via browser; intentionally does not spawn arbitrary external executables or wait for exit |
| `execute_shell` | `src/runtime/builtins_core.cpp:1815` | Partial | Same as above: handles HTTP(S) URLs only, not arbitrary local shell file associations |
| `set_program_priority` | `src/runtime/builtins_core.cpp:1598` | Stub | Returns `0.0`; does not modify OS process scheduling priority |
| `YoYo_GetPlatform` | `src/runtime/builtins_core.cpp:1781` | Stub | Hardcoded to `0` (`os_win32`); does not report actual runtime OS |
| `YoYo_GetDevice` | `src/runtime/builtins_core.cpp:1782` | Stub | Hardcoded to `0` |
| `YoYo_GetDomain` | `src/runtime/builtins_core.cpp:1783` | Stub | Hardcoded to `""` |
| `YoYo_AddVirtualKey` | `src/runtime/builtins_core.cpp:1803` | Stub | Returns `-1`; not connected to the built-in touch overlay |
| `YoYo_DeleteVirtualKey` | `src/runtime/builtins_core.cpp:1804` | Stub | Returns `0.0` |
| `YoYo_ShowVirtualKey` | `src/runtime/builtins_core.cpp:1805` | Stub | Returns `0.0` |
| `YoYo_HideVirtualKey` | `src/runtime/builtins_core.cpp:1806` | Stub | Returns `0.0` |

---

## 6. Legacy DirectPlay Multiplayer (`mplay_*`)

> Note: Original GM8 `mplay_*` relied on deprecated Windows DirectPlay. All 29 `mplay_*` functions are currently single-process in-memory mocks or stubs with no real networking.

| Function | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `mplay_init_ipx` | `src/runtime/builtins_misc.cpp:159` | Local mock | Sets `connect_status = 1`, returns `false` |
| `mplay_init_tcpip` | `src/runtime/builtins_misc.cpp:160` | Local mock | Sets `connect_status = 2`, returns `true`; no real socket transport |
| `mplay_init_modem` | `src/runtime/builtins_misc.cpp:161` | Local mock | Sets `connect_status = 3`, returns `false` |
| `mplay_init_serial` | `src/runtime/builtins_misc.cpp:162` | Local mock | Sets `connect_status = 4`, returns `false` |
| `mplay_connect_status` | `src/runtime/builtins_misc.cpp:163` | Local mock | Returns mock `connect_status` |
| `mplay_end` | `src/runtime/builtins_misc.cpp:164` | Local mock | Resets local mock state |
| `mplay_ipaddress` | `src/runtime/builtins_misc.cpp:168` | Local mock | Hardcoded to `"127.0.0.1"` |
| `mplay_session_create` | `src/runtime/builtins_misc.cpp:169` | Local mock | Records local session/player names, returns `true` |
| `mplay_session_find` | `src/runtime/builtins_misc.cpp:176` | Stub | Returns `0` |
| `mplay_session_name` | `src/runtime/builtins_misc.cpp:177` | Local mock | Returns locally stored session name |
| `mplay_session_join` | `src/runtime/builtins_misc.cpp:178` | Local mock | Stores player name, returns `false` |
| `mplay_session_mode` | `src/runtime/builtins_misc.cpp:182` | Local mock | Stores `auto_move_session` flag |
| `mplay_session_status` | `src/runtime/builtins_misc.cpp:186` | Local mock | Returns local status (`0` or `1`) |
| `mplay_session_end` | `src/runtime/builtins_misc.cpp:187` | Local mock | Sets `session_status = 0` |
| `mplay_player_find` | `src/runtime/builtins_misc.cpp:188` | Local mock | Returns `1` if local session active, else `0` |
| `mplay_player_name` | `src/runtime/builtins_misc.cpp:189` | Local mock | Returns locally stored player name |
| `mplay_player_id` | `src/runtime/builtins_misc.cpp:190` | Stub | Returns `1` |
| `mplay_data_write` | `src/runtime/builtins_misc.cpp:191` | Local mock | Writes only to in-process map; no network sync |
| `mplay_data_read` | `src/runtime/builtins_misc.cpp:195` | Local mock | Reads only from in-process map |
| `mplay_data_mode` | `src/runtime/builtins_misc.cpp:200` | Local mock | Stores flag only |
| `mplay_message_send` | `src/runtime/builtins_misc.cpp:204` | Stub | Returns `0.0` |
| `mplay_message_send_guaranteed` | `src/runtime/builtins_misc.cpp:205` | Stub | Returns `0.0` |
| `mplay_message_receive` | `src/runtime/builtins_misc.cpp:206` | Stub | Returns `false` |
| `mplay_message_id` | `src/runtime/builtins_misc.cpp:207` | Stub | Returns `0` |
| `mplay_message_value` | `src/runtime/builtins_misc.cpp:208` | Stub | Returns `0.0` |
| `mplay_message_player` | `src/runtime/builtins_misc.cpp:209` | Stub | Returns `0` |
| `mplay_message_name` | `src/runtime/builtins_misc.cpp:210` | Stub | Returns `""` |
| `mplay_message_count` | `src/runtime/builtins_misc.cpp:211` | Stub | Returns `0` |
| `mplay_message_clear` | `src/runtime/builtins_misc.cpp:212` | Stub | Returns `0.0` |

---

## 7. Physical CD Audio & Windows MCI (Legacy)

> Note: Depends on physical CD-ROM hardware and the Windows MCI subsystem.

| Function | Location | Status | Current Behavior |
| :--- | :--- | :--- | :--- |
| `cd_init` | `src/runtime/builtins_misc.cpp:217` | Legacy / Stub | Returns `0.0` |
| `cd_present` | `src/runtime/builtins_misc.cpp:218` | Legacy / Stub | Returns `false` |
| `cd_number` | `src/runtime/builtins_misc.cpp:219` | Legacy / Stub | Returns `0` |
| `cd_playing` | `src/runtime/builtins_misc.cpp:220` | Legacy / Stub | Returns `false` |
| `cd_paused` | `src/runtime/builtins_misc.cpp:221` | Legacy / Stub | Returns `false` |
| `cd_track` | `src/runtime/builtins_misc.cpp:222` | Legacy / Stub | Returns `0` |
| `cd_length` | `src/runtime/builtins_misc.cpp:223` | Legacy / Stub | Returns `0` |
| `cd_track_length` | `src/runtime/builtins_misc.cpp:224` | Legacy / Stub | Returns `0` |
| `cd_position` | `src/runtime/builtins_misc.cpp:225` | Legacy / Stub | Returns `0` |
| `cd_track_position` | `src/runtime/builtins_misc.cpp:226` | Legacy / Stub | Returns `0` |
| `cd_set_position` | `src/runtime/builtins_misc.cpp:227` | Legacy / Stub | Returns `0.0` |
| `cd_set_track_position` | `src/runtime/builtins_misc.cpp:228` | Legacy / Stub | Returns `0.0` |
| `cd_play` | `src/runtime/builtins_misc.cpp:229` | Legacy / Stub | Returns `0.0` |
| `cd_stop` | `src/runtime/builtins_misc.cpp:230` | Legacy / Stub | Returns `0.0` |
| `cd_pause` | `src/runtime/builtins_misc.cpp:231` | Legacy / Stub | Returns `0.0` |
| `cd_resume` | `src/runtime/builtins_misc.cpp:232` | Legacy / Stub | Returns `0.0` |
| `cd_open_door` | `src/runtime/builtins_misc.cpp:233` | Legacy / Stub | Returns `0.0` |
| `cd_close_door` | `src/runtime/builtins_misc.cpp:234` | Legacy / Stub | Returns `0.0` |
| `MCI_command` | `src/runtime/builtins_misc.cpp:235` | Legacy / Stub | Returns `""` |
| `action_cd_play` | `src/runtime/builtins_actions.cpp:1090` | Legacy / Stub | Returns `0.0` |
| `action_cd_stop` | `src/runtime/builtins_actions.cpp:1091` | Legacy / Stub | Returns `0.0` |
| `action_cd_pause` | `src/runtime/builtins_actions.cpp:1092` | Legacy / Stub | Returns `0.0` |
| `action_cd_resume` | `src/runtime/builtins_actions.cpp:1093` | Legacy / Stub | Returns `0.0` |
| `action_cd_present` | `src/runtime/builtins_actions.cpp:1094` | Legacy / Stub | Returns `false` |
| `action_cd_playing` | `src/runtime/builtins_actions.cpp:1095` | Legacy / Stub | Returns `false` |

---

## 8. Incomplete Built-in Variables

| Variable | Location | Status | Current Behavior / Missing Pieces |
| :--- | :--- | :--- | :--- |
| `os_type` | `src/runtime/runner.cpp:2693` | Hardcoded | Always returns `0` (`os_windows`) regardless of host platform |
| `os_device` | `src/runtime/runner.cpp:2694` | Hardcoded | Always returns `0` |
| `os_browser` | `src/runtime/runner.cpp:2695` | Hardcoded | Always returns `-1` (`browser_not_a_browser`), even on WebAssembly |
| `os_version` | `src/runtime/runner.cpp:2696` | Hardcoded | Always returns `0` |
| `display_aa` | `src/runtime/runner.cpp:2699` | Hardcoded | Always returns `0` |
| `async_load` | `src/runtime/runner.cpp:2700` | Hardcoded | Always returns `-1` |
| `secure_mode` | `src/runtime/runner.cpp:2686` | Hardcoded | Always returns `0` (sandboxed secure mode not enforced) |

---

## Suggested Implementation Priority

1. **Optional / nice-to-have**: `display_get_frequency` (real display refresh rate query), `os_type`/`os_browser` & `YoYo_GetPlatform` (host platform detection), `YoYo_*VirtualKey` (wiring to built-in touch overlay), `action_draw_font_old` (legacy GM5 font action), and video splash playback (`splash_show_video`/`show_video`).
2. **Low priority / legacy**: `display_set_*` (superseded by borderless/scaled windows), `keyboard_set_*lock` (OS lock key toggling), `mplay_*` (DirectPlay), `cd_*`/`MCI_command` (CD-ROM hardware), and `sprite_set_cache_size`/`texture_preload`.
