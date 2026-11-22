# gmrunner

Cross-platform C++20 runtime for **GameMaker 8.0 / 8.1 / 8.2** games, targeting **Windows, Linux, macOS, WebAssembly, and Android**.

## Architecture Overview

- **`gm_base`** (`src/base/`): Fixed-width types, Delphi banker's rounding, GM8 float comparison/operator math, CRC-32/MD5/SHA-1/Marsaglia MWC PRNG, zlib streams, UTF-8/codepage transcoding, binary reader, and structured logging.
- **`gm_platform`** (`src/platform/`): SDL3 window/input/gamepad abstraction, monotonic microsecond clock & hybrid frame pacer, case-insensitive VFS with virtual named-pipe support (`\\.\pipe\gm82netpipe.*`), and emulated Windows registry.
- **`gm_loader`** (`src/loader/`): PE32 overlay scanner, GM 8.0 / 8.1 / 8.2 decryptors (`DecryptWad`, `StreamE`, GM7/GEX swap cipher), all 16 asset section parsers, embedded `.gex` unpacker, and GM 8.2 precomputed event-table loader.
- **`gm_gml`** (`src/gml/`): Complete GM 8.0 / 8.1 / 8.2 built-in catalog (331 constants, 166 built-in variables, 1,282 built-in functions), D&D action lowerer, GEX `#define` script splitter, GM8 lexer/parser, compact stack bytecode compiler, `RValue` / 2D-array `VariableTable`, and `GmlVm` bytecode interpreter.
- **`gm_graphics`** (`src/graphics/`): Abstract `IGraphicsDevice` RHI backed by a unified **OpenGL 3.3 Core (Desktop) / OpenGL ES 3.0 (Android) / WebGL 2.0 (WebAssembly)** implementation, fixed-function D3D8/9 uber-shader (8 lights, Gouraud/flat shading, linear/range fog, alpha test), batched 2D/3D `Renderer2D`, `C3D_Model` (`d3d_model_*`) with static VBO caching, and an in-house **D3D9 SM1/SM2/SM3 bytecode (`.vs3`/`.ps3`) -> GLSL 330 Core / 300 es transpiler** (`Dx9Pipeline`) with `CTAB` uniform reflection & default values, multi-stage VS/PS samplers, MRT (`surface_set_target_ext`), shared depth (`surface_set_depth`), and custom vertex/index buffers with hardware instancing.
- **`gm_audio`** (`src/audio/`): Cross-platform software audio mixer built on `miniaudio`, `stb_vorbis`, `TinySoundFont`, and tracker playback (44.1 kHz stereo f32, groups, pitch/fades, 3D spatialization, DSP effects, FFT/wave snapshots, `FMODSIMPLEPW` crypto).
- **`gm_extensions`** (`src/extensions/`): Native cross-platform C++ shims intercepting embedded GEX DLLs and runtime `external_define`/`external_call` bindings for `gm82core`, `gm82buf`, `gm82snd`, `gm82joy`, `gm82drag`, `gm82test`, `gm82dx8`/`gm82dx9` (shaders, CTAB reflection, uniforms, samplers, MRT/depth surfaces, custom vertex/index buffers, instancing, transforms), `GMFMODSimple`, `SuperSound`, `MaizeMusic`, and `CleanMem`.
- **`gm_runtime`** (`src/runtime/`): Exact GM8 17-stage step loop (`Runner`), GM 8.2 precomputed event tables & `application_surface` post-draw presentation, object inheritance & event dispatch, `TMask` pixel-accurate & rotated collision engine, paths & `mp_grid_*`, byte-compatible `ds_*` data structures, particle systems & effects, VFS-backed files/INI/registry, and complete GML/D3D/D&D built-ins.
- **`gm_app`** (`src/app/`): CLI parser (`--inspect`, `--compile-check`, `--verbose`, etc.) and reentrant frame-loop entrypoint.

## Building

Initialize git submodules first:

```bash
git submodule update --init --recursive
```

Configure and build on Windows x64 (Clang + Ninja):

```bash
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
```

Run:

```bash
./build/windows-x64-debug/bin/gmrunner --help
./build/windows-x64-debug/bin/gmrunner path/to/game.exe
./build/windows-x64-debug/bin/gmrunner --inspect path/to/game.exe
./build/windows-x64-debug/bin/gmrunner --compile-check path/to/game.exe
```

## License

MIT License — see [LICENSE](LICENSE).
