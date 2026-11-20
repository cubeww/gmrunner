# gmrunner

Cross-platform C++20 runtime for **GameMaker 8.0 / 8.1 / 8.2** games, targeting **Windows, Linux, macOS, WebAssembly, and Android**.

## Architecture Overview

- **`gm_base`** (`src/base/`): Fixed-width types, Delphi banker's rounding, GM8 float comparison/operator math, CRC-32/MD5/SHA-1/Marsaglia MWC PRNG, zlib streams, UTF-8/codepage transcoding, binary reader, and structured logging.
- **`gm_platform`** (`src/platform/`): SDL3 window/input/gamepad abstraction, monotonic microsecond clock & hybrid frame pacer, case-insensitive VFS with virtual named-pipe support (`\\.\pipe\gm82netpipe.*`), and emulated Windows registry.
- **`gm_loader`** (`src/loader/`): PE32 overlay scanner, GM 8.0 / 8.1 / 8.2 decryptors (`DecryptWad`, `StreamE`, GM7/GEX swap cipher), all 16 asset section parsers, embedded `.gex` unpacker, and GM 8.2 precomputed event-table loader.
- **`gm_gml`** (`src/gml/`): Complete GM 8.0 / 8.1 / 8.2 built-in catalog (331 constants, 166 built-in variables, 1,282 built-in functions), D&D action lowerer, GEX `#define` script splitter, GM8 lexer/parser, compact stack bytecode compiler, `RValue` / 2D-array `VariableTable`, and `GmlVm` bytecode interpreter.
- **`gm_graphics`** (`src/graphics/`): Abstract `IGraphicsDevice` RHI backed by a unified **OpenGL 3.3 Core (Desktop) / OpenGL ES 3.0 (Android) / WebGL 2.0 (WebAssembly)** implementation, plus batched `Renderer2D` (sprites, backgrounds, tiles, GM/sprite fonts, primitives, surfaces/FBOs, PNG/BMP decode & capture).
- **`gm_audio`** (`src/audio/`): Cross-platform software audio mixer built on `miniaudio` + `stb_vorbis` (44.1 kHz stereo f32, 64 voices, WAV/MP3/FLAC/OGG decoding, looping, volume, pan, fades).
- **`gm_runtime`** (`src/runtime/`): Exact GM8 17-stage step loop (`Runner`), object inheritance & event dispatch, `TMask` pixel-accurate & rotated collision engine, paths & `mp_grid_*`, byte-compatible `ds_*` data structures, particle systems & effects, VFS-backed files/INI/registry, and core GML + D&D built-ins.
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
