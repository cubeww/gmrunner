# gmrunner

Cross-platform C++20 runtime for **GameMaker 8.0 / 8.1 / 8.2** games, targeting **Windows, Linux, macOS, WebAssembly, and Android**.

## Architecture Overview

- **`gm_base`** (`src/base/`): Fixed-width types, Delphi banker's rounding, GM8 float comparison/operator math, CRC-32/MD5/SHA-1/Marsaglia MWC PRNG, zlib streams & raw deflate/inflate, UTF-8/codepage transcoding, binary reader, and structured logging (routed to Android logcat on Android).
- **`gm_platform`** (`src/platform/`): SDL3 window/input/touch/gamepad abstraction, monotonic microsecond clock & hybrid frame pacer (with `emscripten_sleep` yielding via Asyncify/JSPI on WebAssembly), case-insensitive VFS with per-directory lookup caching, zero-extraction streaming ZIP32/ZIP64 archive mounting (`ZipVfsSource`), WebAssembly lazy browser-folder & `File.slice()` ZIP bridge (`web_bridge`), OPFS/IDBFS persistent save overlay, virtual named pipes (`\\.\pipe\gm82netpipe.*`), and emulated Windows registry.
- **`gm_loader`** (`src/loader/`): PE32 overlay scanner, GM 8.0 / 8.1 / 8.2 decryptors (`DecryptWad`, `StreamE`, GM7/GEX swap cipher), all 16 asset section parsers, embedded `.gex` unpacker, and GM 8.2 precomputed event-table loader.
- **`gm_gml`** (`src/gml/`): Complete GM 8.0 / 8.1 / 8.2 built-in catalog (331 constants, 166 built-in variables, 1,282 built-in functions), D&D action lowerer, GEX `#define` script splitter, GM8 lexer/parser, compact stack bytecode compiler, `RValue`, hybrid flat/hash `VariableTable`, and zero-allocation frame-sliced `GmlVm` bytecode interpreter with `execute_string` chunk caching.
- **`gm_graphics`** (`src/graphics/`): Abstract `IGraphicsDevice` RHI backed by a unified **OpenGL 3.3 Core (Desktop) / OpenGL ES 3.0 (Android) / WebGL 2.0 (WebAssembly)** implementation with GL state caching and context-loss recovery, fixed-function D3D8/9 uber-shader (8 lights, Gouraud/flat shading, linear/range fog, alpha test), batched 2D/3D `Renderer2D` with uniform dirty-tracking and streaming VBO ring-buffering, `C3D_Model` (`d3d_model_*`) with static VBO caching, in-house **D3D9 SM1/SM2/SM3 bytecode (`.vs3`/`.ps3`) -> GLSL 330 Core / 300 es transpiler** (`Dx9Pipeline`) with `CTAB` uniform reflection & default values, MRT/shared depth surfaces, custom vertex/index buffers with instancing, and a built-in multi-touch `TouchOverlay` virtual gamepad.
- **`gm_audio`** (`src/audio/`): Cross-platform software audio mixer built on `miniaudio`, `stb_vorbis`, `TinySoundFont`, and tracker playback (44.1 kHz stereo f32, zero-copy shared PCM buffers, groups, pitch/fades, 3D spatialization, DSP effects, FFT/wave snapshots, `FMODSIMPLEPW` crypto, mobile background suspend/resume, and WebAudio gesture unlock).
- **`gm_extensions`** (`src/extensions/`): Native cross-platform C++ shims intercepting embedded GEX DLLs and runtime `external_define`/`external_call` bindings for `gm82core`, `gm82buf`, `gm82snd`, `gm82joy`, `gm82drag`, `gm82test`, `gm82dx8`/`gm82dx9`, `GMFMODSimple`, `SuperSound`, `MaizeMusic`, and `CleanMem`.
- **`gm_runtime`** (`src/runtime/`): Exact GM8 17-stage step loop (`Runner`), GM 8.2 precomputed event tables & `application_surface` post-draw presentation, object inheritance & event dispatch, `TMask` pixel-accurate & rotated collision engine, paths & `mp_grid_*`, byte-compatible `ds_*` data structures, particle systems & effects, VFS-backed files/INI/registry, mobile/web lifecycle handling, and complete GML/D3D/D&D built-ins.
- **`gm_app`** (`src/app/`): Cross-platform entrypoint (`gmrunner` on Desktop/WASM, `libmain.so` on Android) supporting `.exe`, game directories, and zero-extraction `.zip` game archives.

## Building

Initialize git submodules first:

```bash
git submodule update --init --recursive
```

### Windows x64 (Clang + Ninja)

```bash
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release --parallel
```

### Linux x64 (GCC / Clang + Ninja)

Install X11/Wayland/OpenGL/ALSA development packages, then:

```bash
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release --parallel
```

Optional tarball packaging: `./packaging/linux/package_linux.sh`

### macOS (Apple Clang + Ninja)

```bash
# CLI binary
cmake --preset macos-release
cmake --build --preset macos-release --parallel

# Or .app bundle + .dmg
./packaging/macos/package_macos.sh
```

### WebAssembly (Emscripten + WebGL 2.0 + OPFS/IDBFS)

Activate Emscripten (`EMSDK`), then choose **Asyncify** (broad browser compatibility: Chrome, Firefox, Safari) or **JSPI** (smaller `.wasm`, modern Chrome/Firefox/Edge):

```bash
# Asyncify (default)
cmake --preset wasm-release
cmake --build --preset wasm-release --parallel

# Or JSPI
cmake --preset wasm-jspi-release
cmake --build --preset wasm-jspi-release --parallel
```

Serve `build/wasm-release/bin/` with any static HTTP server (e.g. `python3 -m http.server -d build/wasm-release/bin 8080`) and open `http://localhost:8080/gmrunner.html`. You can drop or select a **game folder** (mounted lazily on demand without copying external files into RAM), a **`.zip` archive** (streamed via `File.slice()`), or a single **`.exe`**, or autoload via `?game=path/to/game.zip`.

### Android (NDK + GLES 3.0 + Touch Overlay)

Build the native shared libraries (`libSDL3.so` + `libmain.so`) directly with CMake + NDK:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release --parallel
```

Or open/build the full Gradle project in [`android/`](android/) (`./gradlew assembleRelease`), which bundles SDL3's Java layer, a game launcher supporting both local folders (`Android/data/dev.gmrunner/files/games/`) and SAF `.zip`/`.exe` import, and the built-in multi-touch virtual gamepad overlay.

## Running (Desktop)

```bash
./build/windows-x64-release/bin/gmrunner --help
./build/windows-x64-release/bin/gmrunner path/to/game.exe
./build/windows-x64-release/bin/gmrunner path/to/game_dir
./build/windows-x64-release/bin/gmrunner path/to/game.zip
./build/windows-x64-release/bin/gmrunner --save-dir ./saves --touch-overlay path/to/game.zip
./build/windows-x64-release/bin/gmrunner --inspect path/to/game.exe
./build/windows-x64-release/bin/gmrunner --compile-check path/to/game.exe
```

## License

MIT License — see [LICENSE](LICENSE).
