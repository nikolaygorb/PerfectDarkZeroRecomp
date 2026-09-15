# PerfectDarkZeroRecomp

<div align="center">
  <img src="assets/icon.png" alt="PerfectDarkZeroRecomp" width="480">
</div>

A static recompilation of [**Perfect Dark Zero**](https://en.wikipedia.org/wiki/Perfect_Dark_Zero) (2005, Xbox 360 launch title,
Title ID `4D5307D3`, retail hash `C1572363239DB6CE`) to native Windows x86-64,
built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

Static recompilation translates the Xbox 360 PowerPC code inside the game's
`default.xex` into native C++ that compiles and runs on a PC. There is no
emulator and no interpreter in the loop; file I/O, GPU commands, audio and
threading go through the ReXGlue runtime.


## Status

- Codegen runs clean (1 manual `[functions]` boundary needed for an
  `UnresolvedCall`, plus one hand-patched cross-function tail-jump compile
  error - see [Known issues](#known-issues-and-difficulties-encountered)).
- The build compiles and the executable boots to real GPU rendering.
- The optional stub sweep (opt-in via `dev_debug_runtime = true` in
  [`settings/hardware.toml`](settings/hardware.toml)) logs missing addresses
  to `logs/stub_sweep.txt`; the missing-function scan logs to
  `logs/missed_functions.txt`.

## Requirements

- CMake 3.25+
- Ninja
- Clang / LLVM (clang-cl works too) - MSVC alone will not build this (the
  generated code uses GNU/Clang builtins like `__builtin_rotateleft64`)
- [ABGX360](https://github.com/BakasuraRCE/abgx360) to dump the Xbox 360 disc,
  or [extract-xiso](https://github.com/XboxDev/extract-xiso/releases) to
  unpack an existing ISO
- Your own legally-owned copy of Perfect Dark Zero, extracted from the Xbox 360 disc/ISO

The ReXGlue SDK release archive is auto-downloaded by the CMake configure
step - see [Getting the SDK](#getting-the-sdk).

## Getting the SDK

The ReXGlue SDK version is pinned in [`CMakeLists.txt`](CMakeLists.txt)
(`REXSDK_VERSION "0.10.0"`). The CMake configure step auto-downloads the
matching release archive for your platform (Windows / Linux / macOS) into
`rexglue/<platform>/` via
[`cmake/fetch-rexglue-sdk.cmake`](cmake/fetch-rexglue-sdk.cmake) - no manual
download needed. The SDK itself is gitignored; only
[`rexglue/README.md`](rexglue/README.md) is checked in.

## Getting the game data

1. Dump the Xbox 360 disc with
   [ABGX360](https://github.com/BakasuraRCE/abgx360) (verify against the
   hashes below) or extract an existing ISO with
   [extract-xiso](https://github.com/XboxDev/extract-xiso/releases), which
   unpacks the disc's file tree.
2. Copy the extracted contents directly into `assets/`, so `default.xex`
   sits at `assets/default.xex` alongside the rest of the disc's files.
3. `assets/` is gitignored (`assets/*` in `.gitignore`, with a couple of
   small tracked exceptions) - nothing from the disc is, or should be,
   committed to this repo.

Target retail release (verify your dump matches):

| Field | Value |
|---|---|
| Title ID | `4D5307D3` |
| XEX CRC | `375EC9BB` |
| XEX Media ID | `6D6481013A4FDD3DA32BD2D0-750FF1D9` |
| DMI CRC | `3B16ECC2` |
| PFI CRC | `739CEAB3` |

## Build

```powershell
cmake --preset win-amd64-release
cmake --build --preset win-amd64-release
```

```bash
cmake --preset linux-amd64-release
cmake --build --preset linux-amd64-release
```

```bash
cmake --preset mac-amd64-release
cmake --build --preset mac-amd64-release
```

Other presets (`*-debug`, `*-relwithdebinfo`, `*-arm64`) are listed in
[`CMakePresets.json`](CMakePresets.json).

Codegen (translating `assets/default.xex` into `generated/default/*.cpp`) runs
automatically as a build step (`perfectdarkzerorecomp_codegen` CMake target)
whenever `perfectdarkzerorecomp_manifest.toml` or an included `.toml`
changes. It can also be run directly: `rexglue\win-amd64\bin\rexglue.exe
codegen`.

## Cross-platform builds

Nothing in this repo is Windows-only - `CMakeLists.txt`, the generated
`generated/rexglue.cmake`, and `src/perfectdarkzerorecomp_app.h` are already
platform-agnostic (the one `WIN32`-guarded bit is the `add_executable(...
WIN32 ...)` subsystem flag). `CMakePresets.json` already ships matching
configure/build presets for every platform the SDK publishes: `win-amd64`,
`linux-amd64`, `linux-arm64`, `mac-amd64`, `mac-arm64` (`*-debug` /
`*-release` / `*-relwithdebinfo` each) - none of that needs adding.

What's missing to actually build for one of them: download that platform's
release archive to `rexglue/<platform>/`, add a `CMakeUserPresets.json`
preset pointing `CMAKE_PREFIX_PATH` there, and actually build/run it on that
OS/architecture (the real constraint - `win-arm64` isn't published by the SDK
at all, despite `CMakePresets.json` having generic presets for it).

## Run

```powershell
cd out\build\win-amd64-release
.\perfectdarkzerorecomp.exe
```

```bash
cd out/build/linux-amd64-release
./perfectdarkzerorecomp
```

```bash
cd out/build/mac-amd64-release
./perfectdarkzerorecomp
```

Both `--game_data_root` and `--gpu_plugin` are optional: `OnConfigurePaths()`
in [`src/perfectdarkzerorecomp_app.h`](src/perfectdarkzerorecomp_app.h)
defaults `game_data_root` to `<repo_root>/assets` when it isn't set via
flag/env var, and `gpu_plugin = "xenos"` already lives in
[`settings/hardware.toml`](settings/hardware.toml).

Useful extra flags/env vars while developing:

| Flag / env var | Effect |
|---|---|
| `--game_data_root <path>` | Overrides the default `<repo_root>/assets` game-files location. |
| `--gpu_plugin xenos` | Overrides `settings/hardware.toml`'s `gpu_plugin`. Only needed if you want a different plugin than the file specifies. |
| `--graphics_backend d3d12\|vulkan\|any` | Forces the graphics API `rexgpu-xenos` uses (cvar, default `"any"`, which picks D3D12 first). See [`settings/README.md`](settings/README.md). |
| `--pdz_fps60_unlock=true` | Ported xenia-canary `game-patches` "60 FPS" patch for PDZ retail. Enabled by default in [`settings/hardware.toml`](settings/hardware.toml). See [`settings/README.md`](settings/README.md). |
| `--pdz_aspect_ratio_16_9=true` | Ported xenia-canary `game-patches` "Aspect Ratio" patch. Off by default. See [`settings/README.md`](settings/README.md). |

Logs are written to `out\build\<preset>\logs\*.log` (the exe is built `WIN32`,
so nothing prints to the console).

## Configuration

Rendering/window/vsync and input-backend defaults are checked in under
[`settings/`](settings/README.md) (`hardware.toml` / `mapping.toml`), loaded
automatically at startup. CLI flags and `REX_*` environment variables always
override them - see [`settings/README.md`](settings/README.md) for the full
reference and precedence rules.

## How this project was set up (history)

1. `rexglue init --project-name PerfectDarkZeroRecomp --xex-path
   assets\default.xex` generated `CMakeLists.txt`, `CMakePresets.json`,
   `perfectdarkzerorecomp_manifest.toml`, `generated/rexglue.cmake`,
   `src/main.cpp`, `src/perfectdarkzerorecomp_app.h`.
2. First `rexglue codegen` failed analysis with 1 `UnresolvedCall` error (a
   plain `b` branch to an address the auto-analyzer never registered as a
   function). Fixed by adding `0x8250B06C = {}` to `default_functions.toml`
   under `[functions]` and including that file from the manifest
   (`includes = ["default_functions.toml"]`).
3. Added `CMakeUserPresets.json` (`local-debug`/`local-release`/`local-relwithdebinfo`)
   so `CMAKE_PREFIX_PATH` finds the SDK without touching the generated,
   overwritable `CMakePresets.json`.
4. Added `GPU_PLUGINS xenos` to the `rexglue_setup_target()` call in
   `CMakeLists.txt` so `rexgpu-xenos*.dll` gets staged next to the exe, plus
   a post-build step to stage `settings/gamecontrollerdb.txt` next to the
   exe for SDL gamepad mapping fallback.
5. Hooks into `PerfectdarkzerorecompApp`: two-pass
   settings loading (`OnConfigurePaths` + `OnPostSetup`), the
   `graphics_backend` cvar (forces Vulkan/D3D12 via `OnPreSetup`), and the
   stub-sweep safety net.
6. First build failed to *compile* (not just analyze) with two "use of
   undeclared label" errors in `generated/default/perfectdarkzerorecomp_recomp.190.cpp`
   - "Codegen edge case: cross-function tail jumps".
7. First runtime boot's stub sweep needed its code-range constants derived:
   ran once with an intentionally oversized placeholder range, took the
   first `SetFunction: address ... outside all registered module ranges`
   log line as the real end boundary, then narrowed the range and reran to
   confirm zero out-of-range hits.
8. Ported two optional gameplay patches from xenia-canary's `game-patches`
   repository (`60 FPS`, `Aspect Ratio`) as cvars. The `60 FPS` patch is
   enabled by default in [`settings/hardware.toml`](settings/hardware.toml);
   the `Aspect Ratio` patch is off by default.

## Known issues and difficulties encountered

- **Manual function boundaries are an ongoing, iterative process**.
- **Codegen edge case: cross-function tail jumps.** PPC compilers sometimes
  emit a shared tail block split across two functions by rexglue's
  Discover/Merge phases - a `goto` across that boundary doesn't compile
  ("use of undeclared label"). The build now auto-patches these after every
  codegen run via [`cmake/fix-unresolved-tail-jumps.cmake`](cmake/fix-unresolved-tail-jumps.cmake):
  each broken `goto` is replaced with a logged early return. Duplicating or
  refactoring the shared tail block correctly would need much deeper
  disassembly work than this bring-up pass covered.
- **Stubbed-function access violations.** When the stub sweep is enabled
  (`dev_debug_runtime = true`), stubbed functions return garbage that can be
  dereferenced downstream, producing access violations like
  `Unhandled guest access violation: read of guest 0x00000000`.
  `logs/stub_sweep.txt` is the starting point: find the addresses it logged,
  work out which ones are real, add them to `default_functions.toml`, rebuild,
  repeat.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) ([releases](https://github.com/rexglue/rexglue-sdk/releases))
- [ABGX360](https://github.com/BakasuraRCE/abgx360) - used to dump the Xbox 360 disc
- [extract-xiso](https://github.com/XboxDev/extract-xiso) ([releases](https://github.com/XboxDev/extract-xiso/releases)) -
  used to unpack the Xbox 360 ISO into the file tree copied into `assets/`
- [xenia](https://github.com/xenia-project/xenia) / [xenia-canary](https://github.com/xenia-canary/xenia-canary) -
  ReXGlue's runtime is derived from Xenia's
- [xenia-canary/game-patches](https://github.com/xenia-canary/game-patches) -
  source of the ported `pdz_fps60_unlock` / `pdz_aspect_ratio_16_9` patches
- [mdqinc/SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) -
  `settings/gamecontrollerdb.txt`
