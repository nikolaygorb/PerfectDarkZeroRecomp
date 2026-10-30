# Settings

Two hand-authored, checked-in config files that the `perfectdarkzerorecomp`
binary loads automatically at startup, plus a downloaded controller mapping
database and this README.

| File | Covers |
|---|---|
| [`hardware.toml`](hardware.toml) | Renderer/backend, window, vsync, resolution, debug output, gameplay patches, language, local split-screen players |
| [`mapping.toml`](mapping.toml) | Input backend selection (gamepad/keyboard+mouse) |
| [`gamecontrollerdb.txt`](gamecontrollerdb.txt) | SDL gamepad mappings for controllers it doesn't already recognize - not hand-authored, see below |

Split in two on purpose: rendering/perf tweaks and input tweaks are separate
concerns you'll want to touch independently, and both stay tiny because
they only list the cvars this project actually cares about, not the full
~80-flag surface the SDK exposes.

**Format: flat `key = value` only, no `[Table]` headers.** Confirmed via
`rex::cvar::SerializeToTOML()`, which itself dumps a flat list - the loader
reads root-level keys, it does not recurse into nested tables. A key put
under a `[Section]` header is silently ignored (found the hard way: values
under `[Display]`/`[GPU]` headers never took effect until the headers were
removed).

## How the binary consumes them

`ReXApp` (the SDK base class) already auto-loads a single `<exe_dir>/<name>.toml`
config file via `rex::cvar::LoadConfig()`. `PerfectdarkzerorecompApp::
OnConfigurePaths()` in [`src/perfectdarkzerorecomp_app.h`](../src/perfectdarkzerorecomp_app.h)
redirects that single slot to `settings/hardware.toml`, then loads both files
a **second time** in `OnPostSetup()`. Both paths are resolved by walking up
from the exe's folder until an `assets/` directory is found, so it works from
any build preset's output directory.

The second pass exists because not every cvar is registered when
`OnConfigurePaths` runs: window/fullscreen/resolution/monitor (`rex::ui`)
are available immediately, but GPU-backend cvars like `vsync`,
`resolution_scale`, `d3d12_debug` (`rex::graphics`) only register once the
runtime/GPU backend spins up, which happens later. `LoadConfig()` is a
one-shot read, not a live watch, so without the second pass those keys would
be silently dropped (confirmed via `rex::cvar::GetFlagSource()` returning
`kDefault` for them after the first pass, `kConfig` after the second).
Re-applying an already-set value on the second pass is a harmless no-op.

## Why `game_data_root` isn't a key in `hardware.toml`

Unlike every other cvar here, `game_data_root` is read by the SDK (`ReXApp::
SetupEnvironment()`) *before* any config file loads at all - so a
`game_data_root = "..."` line in `hardware.toml` would silently have no
effect, the same one-shot-too-early problem as the two-pass cvars above but
with no later pass to catch it. Instead, `OnConfigurePaths()` defaults
`paths.game_data_root` directly to `<repo_root>/assets` in code whenever
`--game_data_root`/`REX_GAME_DATA_ROOT` didn't already set it, so
`perfectdarkzerorecomp.exe` with no flags at all just works from this repo
layout. `--game_data_root <path>` still overrides it for a game dump kept
elsewhere.

Precedence (highest wins), same as any other cvar:

```
--flag=value (CLI)  >  REX_FLAG=value (env var)  >  settings/*.toml  >  compiled-in default
```

So a `--vsync=false` on the command line, or `REX_VSYNC=false` in the
environment, always overrides whatever is in `hardware.toml`.

## Caveat: the in-game Settings overlay writes to `hardware.toml`

The SDK's built-in Settings overlay's "Save to config" button writes *every*
modified cvar into the one path `OnConfigurePaths` redirected
(`hardware.toml`), including anything you changed at runtime through the
overlay. If you experiment there, expect `git diff` to show more than the
hand-picked keys below - `git checkout -- settings/hardware.toml` reverts it
to the committed defaults.

## Caveat: engine hotkeys aren't in `mapping.toml`

`mapping.toml` only covers static-flag input cvars (table below). The
console/debug/achievements overlay toggle keys ("Keybinds" category) are
registered by the SDK *after* these files load (once the window and
overlays exist), so a key added to `mapping.toml` for one of those would
silently have no effect. Rebind those from the in-game Settings overlay
instead - that persists through the same save mechanism described above.

## Keyboard, mouse, and gamepad all work at once

The SDK always merges every input device assigned to a guest player rather
than picking one active source (`rex::input::MergeInto` - buttons OR'd,
sticks/triggers take whichever device pushed further), and the keyboard as a
gamepad ("MnK") driver marks itself a synthetic device that's always folded
into player one alongside any physical pad, never competing with it for a
slot. So there's no "backend" to choose between keyboard and gamepad the way
`input_backend` chooses between `sdl`/`xinput` - it's `mnk_mode`/`mnk_mouse`
turning the keyboard/mouse side on or off, independent of whatever pad is
also plugged in.

Per-action keybinds (`keybind_a`, `keybind_lstick_up`, ...) already ship with
sensible WASD-style defaults and aren't repeated in `mapping.toml` -
add any of them the same way as everything else here if you want to rebind
one. The full list isn't in the headers vendored under `rexglue/win-amd64`
(source-only, defined in the SDK's `mnk_input_driver.cpp`) - the easiest way
to see every current default is `rex::cvar::SerializeToTOML()`'s dump, or the
in-game Settings overlay's control rebinding screen.

## `hardware.toml` reference

The "Default" column shows the value committed in `hardware.toml` (what the
build actually ships with), not the SDK's compiled-in default.

| Key | Type | Default | Effect |
|---|---|---|---|
| `gpu_plugin` | string | `"xenos"` | GPU emulation plugin to load. Set here rather than on the command line now - see root [README](../README.md#run). A `--gpu_plugin` flag would still override this file if you ever needed a different plugin. |
| `graphics_backend` | string | `"any"` | Graphics API backend: `"any"` (D3D12 first when both are compiled in), `"d3d12"`, or `"vulkan"`. Project-defined cvar (not from the SDK), wired up in `OnPreSetup()` in [`src/perfectdarkzerorecomp_app.h`](../src/perfectdarkzerorecomp_app.h) - it loads the plugin itself with the requested backend before the SDK's own auto-load (which only ever requests `"any"`) runs. Falls back to automatic selection with a warning if the requested backend isn't compiled into `rexgpu-xenos`. |
| `vsync` | bool | `true` | Vertical sync. |
| `resolution_scale` | int | `2` | Internal render-target supersampling, independent of window/guest resolution. |
| `async_shader_compilation` | bool | `true` | Compile shaders on a background thread instead of blocking the render thread - reduces hitches when new shaders are first seen. |
| `native_2x_msaa` | bool | `false` | Native 2x MSAA on the emulated render targets. |
| `anisotropic_override` | int | `5` | Forces anisotropic texture filtering to this level (e.g. `16`); `0` leaves the game's own setting alone. |
| `window_width` / `window_height` | int | `1920` / `1080` | Host window size. |
| `fullscreen` | bool | `true` | Host window fullscreen. |
| `monitor` | int | `0` | Host monitor index for fullscreen. |
| `resolution` | string | `"1920x1080"` | Guest ("TV") video mode reported to the game - affects the game's own UI scale/aspect logic, separate from the host window size above. |
| `present_letterbox` | bool | `true` | Letterbox instead of stretch when window and guest aspect ratios differ. |
| `d3d12_debug` | bool | `false` | D3D12 debug layer. Leave off - noticeably slower. |
| `pdz_fps60_unlock` | bool | `true` | Port of Xenia-canary's Perfect Dark Zero "60 FPS" patch. Applied through a C++ function override and can be toggled at runtime. |
| `pdz_aspect_ratio_16_9` | bool | `false` | Port of Xenia-canary's "Aspect Ratio" patch. Applied while loading the game image; restart after changing it. |
| `pdz_fov` | double | `58.0` | Vertical field of view before zoom, range `40`-`120`. Changed in game with `bind_fov_settings` (F6). |
| `user_language` | int | `1` | Game language: `1` English, `3` German, `4` French. SDK cvar, applied by a hook in [`src/game_patches.cpp`](../src/game_patches.cpp) because the SDK's `XGetLanguage` ignores it. Languages missing from `loc/` fall back to English. Restart after changing it. |
| `pdz_gpu_wait_mode` | int | `1` | How the game's GPU wait loop idles: `0` busy spin (original), `1` yield the core, `2` sleep 200 µs. Hook in `src/render/`. |
| `pdz_tp_enable` | bool | `false` | Start in third person; toggled in game with `bind_third_person` (V). Offset keys `pdz_tp_offset_x` / `_y` / `_z` (`-80.0` / `10.0` / `-320.0`) are described in [`src/mod/README.md`](../src/mod/README.md). |
| `max_local_users` | int | `2` | Local split-screen players that can be signed in, 1 to 4. The shipped value is `2`; the SDK default is `1` (single player). SDK cvar, see [Split-screen](#split-screen). |
| `keyboard_own_player` | bool | `false` (commented out) | Keyboard and mouse are player 1 on their own and gamepads start at player 2. Shipped commented out in `hardware.toml`; uncomment to use. See [Split-screen](#split-screen). |

The source Xenia patch file also contains a "Bottomless Clip" patch. It is
not ported yet, so there is no corresponding cvar in this project.

## Split-screen

Perfect Dark Zero has local split-screen, and the runtime can sign in several
local players the way a console does when another person picks up a
controller. `hardware.toml` ships with `max_local_users = 2`, so two players
work out of the box:

- Player 1: keyboard and mouse plus gamepad 1.
- Player 2: gamepad 2, signed in when that gamepad is connected (and signed out
  when it is removed). The game sees the usual sign-in change notification.
- Each player keeps their own in-game options (look inversion, sensitivity,
  button layout) in a per-player profile folder; save games are shared and kept
  under player 1.

Common setups, all in `hardware.toml`:

```toml
# Two gamepads (default as shipped)
max_local_users = 2

# One gamepad: keyboard and mouse against the pad
max_local_users = 2
keyboard_own_player = true

# Up to four players, one gamepad each
max_local_users = 4
```

Beyond this, `input_player1` / `input_player2` pick devices explicitly and
`user_gamertag_2` names the second player. These are SDK cvars; they are not
listed in `hardware.toml` but can be added like any other key. The full
description, examples and troubleshooting are in
[`split-screen.md`](split-screen.md). Limits: at most four players, chosen
devices cover players 1 and 2 only, no per-player gamepad remapping, and the
game itself decides how split-screen looks and when it accepts new players.

## `mapping.toml` reference

| Key | Type | Default | Effect |
|---|---|---|---|
| `input_backend` | string | `"sdl"` | `""` is rejected by validation (logs a warning, falls back to default) - must be an explicit `"sdl"`, `"xinput"`, or `"nop"` (input disabled). |
| `hid_mappings_file` | string | `"gamecontrollerdb.txt"` | Extra SDL gamepad mappings loaded for controllers SDL doesn't already recognize, or `""` to skip. `gamecontrollerdb.txt` next to the exe is staged by `CMakeLists.txt` from [`gamecontrollerdb.txt`](gamecontrollerdb.txt) in this folder, sourced from [mdqinc/SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) - re-copy that file from upstream any time to refresh it. |
| `guide_button` | bool | `true` | Whether the controller Guide/PS button is routed to the guest. |
| `mnk_mode` | bool | `true` | Enables keyboard-as-controller input, merged in alongside any physical gamepad (see below). |
| `mnk_mouse` | bool | `true` | Routes mouse movement to the right stick when `mnk_mode` is on. Off means the right stick only comes from the `keybind_rstick_*` keys. |
| `mnk_sensitivity` | double | `1.0` | Mouse sensitivity for the right stick, range `0.01`-`10.0`. With `pdz_mouse_look` on it only applies where native mouse look doesn't run (menus, cutscenes, vehicles). |
| `pdz_mouse_look` | bool | `true` | Native mouse look: on foot the mouse turns the camera directly, skipping the game's stick dead zone, response curve and acceleration (aim assist still applies). Hook in [`src/mouse/mouse_look_hook.cpp`](../src/mouse/mouse_look_hook.cpp). Needs `mnk_mode` and `mnk_mouse`. |
| `pdz_mouse_sensitivity` | double | `2.0` | Native mouse look: camera turn per mouse count = 0.022 deg x this. Range `0.01`-`100.0`. Independent of the in-game sensitivity option. |
| `pdz_mouse_invert_x` / `pdz_mouse_invert_y` | bool | `false` | Invert native mouse look axes. The in-game Invert option doesn't apply to it. |
| `pdz_mouse_zoom_scaling` | bool | `true` | Scale native mouse look by `tan(fov/2) / tan(pdz_fov/2)` while zoomed, so the picture moves under the mouse at the same rate. |
| `pdz_mouse_zoom_sensitivity` | double | `1.0` | Extra factor on native mouse look while zoomed in. |
Gamepad button layout itself (DualShock 4, DualSense, Xbox, Switch Pro, ...)
isn't a setting here - SDL auto-detects the controller and maps it to the
Xbox 360 layout the game expects.

## Adding more cvars

Any cvar declared with `REXCVAR_DECLARE` under
`rexglue/win-amd64/include/rex/**/flags.h` can be added the same way: pick
the file matching its concern (hardware vs. input), add `key = value`. If it
needs to apply before the window/overlays exist (true for everything
currently listed), it belongs in one of these two files loaded from
`OnConfigurePaths`; if it's a Keybinds-category hotkey, see the caveat above
instead.

`graphics_backend` isn't an SDK cvar - it's defined with
`REXCVAR_DEFINE_STRING` in [`src/game_cvars.cpp`](../src/game_cvars.cpp),
with its declaration in [`src/game_cvars.h`](../src/game_cvars.h). Project
cvars register at static-init time, so they load with the first config pass.
Define future project-specific toggles there, read them with
`REXCVAR_GET(name)`, and document them in this table.
