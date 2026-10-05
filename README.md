# Call of Duty Black Ops (BO1) — static recompilation for PC

A native Windows x64 port of the Xbox 360 version of *Call of Duty: Black Ops* (Title Update #11),
built by statically recompiling the console's PowerPC executables to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk). Campaign, Zombies and multiplayer (local and
split screen) run as two native programs, `bo1.exe` and `bo1mp.exe`, with the console's own game
code, assets and behaviour, plus PC improvements.

The goal is preservation: keeping this version of the game playable on modern hardware.

> **No game data is included.** This repository contains only the port's own code, the analysis
> configuration and tools. You need your own copy of the game (Xbox 360 disc) and its Title
> Update #11. The code generated from the game's executables is never committed either: it is
> produced on your machine from your files.
>
> This project is not affiliated with or endorsed by Activision, Treyarch or Microsoft.
> *Call of Duty* and *Black Ops* are trademarks of Activision Publishing, Inc.

## Status

| Area | State |
|---|---|
| Recompilation | All reachable code translated: 19,554 functions (campaign/Zombies), 21,820 (multiplayer) |
| Campaign | Starts and plays with cinematics; a full playthrough is still being verified |
| Zombies | Plays, also in split screen |
| Multiplayer | Local matches and split screen (2-4 players); online services do not exist anymore |
| Performance | Locked 60 FPS with ~0.3-0.5 ms frame time variation (i5-9600K + GTX 1070) |
| Audio | Stereo, headphones (virtual surround), 5.1 and 7.1 |

## What the port adds

- **Frame pacing**: a precise 60 FPS limiter (the console's vsync is uneven under emulation),
  optional PC vsync and variable refresh rate (G-Sync/FreeSync); multiplayer always runs at the
  common 60 Hz so every player simulates movement the same way.
- **Graphics**: internal resolution x1-x3, FSR 1 / CAS upscaling, 16:9 or 4:3, SMAA (including an
  ultra preset with color edge detection), 16x anisotropic filtering, higher quality shadows and
  level of detail.
- **Split screen**: players 2-4 sign in to their own local profile (own settings and saves) when
  their controller is connected; optional keyboard-and-mouse player; full width views; the
  emulated GPU thread is prioritized while several views are drawn.
- **Audio**: 5.1/7.1 output, a headphone virtualizer, measured output latency, subtitle option.
- **Developer tools**: an in-game developer console (F1) with performance, loading, engine state
  and dvar tabs; a compact overlay (F2); screenshots (F12).
- **Kernel/system**: saves and profile in *Saved Games*, campaign <-> multiplayer switching,
  DLC installation, file streaming diagnostics, many fixes in the runtime.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The port: kernel hooks, engine hooks, settings, graphics/audio/player options, developer UI |
| `mp/` | The multiplayer executable (shares `src/`, its own manifest and CMake project) |
| `config/` | ReXGlue analysis configuration of each executable (function boundaries, setjmp, thunks) |
| `bo1_manifest.toml`, `mp/bo1mp_manifest.toml` | Recompilation manifests (Title Update #11 executables) |
| `generated/rexglue.cmake` | SDK boilerplate (the generated code itself goes to `generated/default/`, ignored) |
| `tools/` | Test runner, sampling profiler, XEX/STFS extractors, analysis scripts |

The port needs the ReXGlue SDK with this project's runtime changes (D3D12 renderer fixes,
split screen profiles, audio output stage, timers, SMAA...):
**[ItsManueh/rexglue-sdk, branch `bo1`](https://github.com/ItsManueh/rexglue-sdk/tree/bo1)**.

## Building

### Requirements

- Windows 10/11 x64, a Direct3D 12 GPU.
- A CPU with AVX2/BMI2/FMA (Intel Haswell / AMD Zen or newer); configure with
  `-DBO1_NATIVE_CPU=OFF` for older CPUs.
- LLVM/Clang (clang and clang++ on `PATH`), CMake 3.25+ and Ninja.
- Python 3 with `pycryptodome` and `capstone` (only for the tools).

### 1. Build the SDK

```bat
git clone --recursive -b bo1 https://github.com/ItsManueh/rexglue-sdk.git %USERPROFILE%\Tools\rexglue-src
cmake -S %USERPROFILE%\Tools\rexglue-src -B %USERPROFILE%\Tools\rexglue-build -G "Ninja Multi-Config" ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_USE_VULKAN=OFF ^
  -DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_BUILD_TESTS=OFF ^
  -DCMAKE_INSTALL_PREFIX=%USERPROFILE%\Tools\rexglue-sdk-custom\win-amd64
```

Then `tools\build_sdk.ps1` builds, installs and copies the runtime DLLs next to the game. It reuses
the `rexglue.exe` recompiler of the official v0.10.0 release, unpacked in
`%USERPROFILE%\Tools\rexglue-sdk\win-amd64`.

Point the game project at the installed SDK with a `CMakeUserPresets.json` (in the project root
and in `mp/`):

```json
{
  "version": 6,
  "configurePresets": [{
    "name": "local-relwithdebinfo",
    "inherits": "win-amd64-relwithdebinfo",
    "cacheVariables": { "CMAKE_PREFIX_PATH": "C:/Users/<you>/Tools/rexglue-sdk-custom/win-amd64" }
  }]
}
```

### 2. Prepare the game files

1. **Game folder**: the decrypted file system of your disc (`default.xex`, `default_mp.xex`,
   `*.ff`, `*.pak`, `*.bik`...), for example extracted from an image of your own disc.
2. **Title Update #11**: extract the update package (STFS) into `assets/tu/tu11`:
   `python tools/stfs.py extract <title update package> assets/tu/tu11`
   (it contains `default.xexp`, `default_mp.xexp` and the patched fast files).
3. Copy `default.xex` and `default_mp.xex` from the game folder into `assets/`.

### 3. Dump the Title Update #11 executables (once)

The recompiler does not apply `.xexp` patches, but the runtime does. A first *bootstrap* build,
translated from the original executables, loads them with the update applied and dumps the result:

```bat
copy /y config\bo1_manifest.base.toml bo1_manifest.toml
cmake --preset local-relwithdebinfo -DBO1_BOOTSTRAP=ON
cmake --build out\build\local-relwithdebinfo --target bo1_codegen
cmake --preset local-relwithdebinfo -DBO1_BOOTSTRAP=ON
cmake --build out\build\local-relwithdebinfo
out\build\local-relwithdebinfo\bo1.exe --game_data_root="<game folder>" --bo1_dump_xex=%CD%\assets\default_tu11.xex
git checkout bo1_manifest.toml
```

Do the same in `mp\` with `config\bo1mp_manifest.base.toml` -> `mp\bo1mp_manifest.toml`, the
`bo1mp_codegen` target, `bo1mp.exe` and `assets\default_mp_tu11.xex`.

The configure step only adds the translated code to the build if it already exists, so after
generating it (or whenever the manifest changes), configure again before building.

### 4. Build the port

```bat
cmake --preset local-relwithdebinfo -DBO1_BOOTSTRAP=OFF
cmake --build out\build\local-relwithdebinfo --target bo1_codegen
cmake --preset local-relwithdebinfo -DBO1_BOOTSTRAP=OFF
cmake --build out\build\local-relwithdebinfo
cd mp
cmake --preset local-relwithdebinfo -DBO1_BOOTSTRAP=OFF
cmake --build out\build\local-relwithdebinfo --target bo1mp_codegen
cmake --preset local-relwithdebinfo -DBO1_BOOTSTRAP=OFF
cmake --build out\build\local-relwithdebinfo
```

The first build translates the executables (`generated/`) and compiles them; it takes a while.
`bo1mp.exe` is copied next to `bo1.exe`, so both executables can switch to each other like on the
console.

### 5. Run

Start `out\build\local-relwithdebinfo\bo1.exe`. The first run creates `bo1.toml` next to it: set
`game_data_root` to your game folder. Every option of the port (prefixed `bo1_`) is described in
that file; the main ones:

| Option | Default | |
|---|---|---|
| `bo1_internal_resolution` | `1` | 1 = console (960x544), 2, 3 |
| `bo1_antialiasing` | `smaa_ultra` | `smaa_ultra`, `smaa`, `fxaa`, `off` |
| `bo1_upscaler` | `fsr` | `fsr`, `cas`, `bilinear` |
| `bo1_aspect_ratio` | `16:9` | `16:9`, `4:3` |
| `bo1_frame_pacing` | `port` | `port` (precise limiter) or `console` |
| `bo1_vsync` / `bo1_vrr` | `false` / `true` | PC vsync, variable refresh rate |
| `bo1_shadows` / `bo1_lod` | `high` | `high` or `console` |
| `bo1_audio_output` | `auto` | `auto`, `stereo`, `headphones`, `5.1`, `7.1` |
| `bo1_audio_latency` | `normal` | `normal` (~55 ms) or `low` (~35 ms) |
| `bo1_subtitles` | `game` | `game`, `on`, `off` |
| `bo1_split_screen` | `true` | local profiles for players 2-4 |
| `bo1_keyboard_player` | `shared` | `shared` or `own` (keyboard/mouse is its own player) |
| `bo1_split_screen_view` | `console` | `console` (side bars) or `full` |
| `bo1_gamertag` / `bo1_player_names` | Windows user | names of player 1 and players 2-4 |

Keys: **F1** developer console, **F2** overlay, **F12** screenshot.

## Tools

- `tools/run.ps1`: automated test runs (logs, timed screenshots, console commands such as
  `press <player> <button>` to drive the menus); uses a separate test profile.
- `tools/sampler/`: sampling profiler; `tools/stutter_report.py` crosses its samples with the
  hitches in the log.
- `tools/xex_extract.py`, `tools/stfs.py`, `tools/ppcdis.py`: XEX and STFS extraction, PowerPC
  disassembly; the other scripts help find functions the analysis misses.

## License

The port's own code, configuration and tools in this repository are released under the
[MIT License](LICENSE). The license does not cover the game: its executables, data and the code
generated from them belong to their owners and are never part of this repository. The ReXGlue SDK
fork keeps its own BSD 3-Clause license.

## Credits

- [ReXGlue](https://github.com/rexglue/rexglue-sdk) (Tom Clay) and the
  [Xenia](https://github.com/xenia-project/xenia) project, on which its runtime is based.
- SMAA by Jorge Jimenez et al.; AMD FidelityFX (FSR 1, CAS).
- The game belongs to Activision Publishing, Inc. and Treyarch.
