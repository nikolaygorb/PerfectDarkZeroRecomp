# PerfectDarkZeroRecomp

<div align="center">
  <img src="assets/icon.png" alt="PerfectDarkZeroRecomp" width="480">
</div>

A static recompilation of [**Perfect Dark Zero**](https://en.wikipedia.org/wiki/Perfect_Dark_Zero)
(Xbox 360, 2005, Title ID `4D5307D3`, **[PAL]**) to native PC, built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). The game's PowerPC code
is translated into C++ and compiled. There is no emulator.

Target retail release:

| Field | Value |
|---|---|
| Title ID | `4D5307D3` |
| XEX CRC | 375EC9BB |
| Module Hash | `AA1EA03FEC9A549C` |
| Media ID | `750FF1D9` |

> This project is not affiliated with Microsoft or Rare. It contains no game
> files: you need your own legally owned copy of the game.

## Status

- Fully playable on Windows and Linux: video, audio, gameplay, achievements and saves.
- Keyboard and mouse support.
- Local split-screen for 4 players out of the box (`max_local_users = 4`), up
  to 4 players with gamepads. Keyboard and mouse can be one of the players, see
  [Split-screen](#split-screen).
- Optional patches: 60 FPS (on by default) and 16:9.
- Third-person camera and language selection, see [Mods](#mods).

## Requirements

- CMake 3.25+, Ninja, and Clang/LLVM. MSVC alone won't build this.
- The game's files, extracted from your disc or ISO with
  [ABGX360](https://github.com/BakasuraRCE/abgx360) or
  [extract-xiso](https://github.com/XboxDev/extract-xiso/releases).

## Build

1. Clone the repo with its submodule. The ReXGlue SDK is built from source
   (`thirdparty/rexglue-sdk`).

   ```
   git clone --recursive https://github.com/nikolaygorb/PerfectDarkZeroRecomp.git
   ```

   If you already cloned without it, run `git submodule update --init --recursive`.
2. Copy the extracted game files into `assets/`, so the path is
   `assets/default.xex`. `assets/` is gitignored.
3. Build:

   ```
   cmake --preset win-amd64-release
   cmake --build --preset win-amd64-release
   ```

   There are also Linux and macOS presets (`linux-amd64-*`, `mac-*`), but only
   Windows is tested. Codegen (`default.xex` → `generated/`) runs automatically
   as part of the build.

## Run

```
out\build\win-amd64-release\perfectdarkzerorecomp.exe
```

Game files are found in `<repo>/assets` automatically; use `--game_data_root`
to point somewhere else. Logs go to `out\build\<preset>\logs\`.

## Mods

### Third-person camera

Press **V** in game to switch between first and third person. Set
`pdz_tp_enable = true` in `hardware.toml` to start in third person. Camera
offset and the rest of the options are in [`src/mod/README.md`](src/mod/README.md).

### Field of view

Press F6 to open the FoV slider (40–120 vertical degrees, original 58), with a
button to reset to the original value. Press F6 again to close it. You can also
edit `pdz_fov` in the F4 Settings overlay under Gameplay. Weapon zoom still works
at any FoV. To use another key, change `bind_fov_settings` in
`settings/mapping.toml`. Use **Save to config** to keep the value between sessions.

### Language

Set `user_language` in `hardware.toml` and restart the game:

```
# 1 = English, 3 = German, 4 = French
user_language = 1   
```

Only languages your copy of the game includes will work (check
`assets/pdz/ASSETS/loc/`). Anything else falls back to English.

### Split-screen

Set `max_local_users` in `hardware.toml` (1 to 4; the file ships with `2`).
Player 1 uses keyboard and mouse plus gamepad 1, player 2 signs in when a
second gamepad is connected, and players 3 and 4 follow with gamepads 3 and 4.

```
# Two players, one gamepad: keyboard and mouse against the pad
max_local_users = 2
keyboard_own_player = true
```

Each player keeps their own in-game options; saves are shared. Explicit device
assignment, player names and troubleshooting are in
[`settings/split-screen.md`](settings/split-screen.md).

### Local player profiles

Press **F9** to open the profile list. You can create profiles, rename them and
pick the one to play next time. Each profile has its own saves and achievements.
A new profile or name takes effect after you restart the game.

The first profile, "User", keeps the saves you already have. Other profiles
save to `profiles/<id>/` in the user data folder (`Documents/perfectdarkzerorecomp`
by default), so renaming a profile doesn't move its saves. The list is kept in
`local_profiles.toml` in that folder. Settings and the shader cache are shared.

Names are 1–15 characters: letters, digits, spaces, `-` and `_`. To use another
key, change `bind_local_profiles` in `settings/mapping.toml`.

## Configuration

All settings live in [`settings/`](settings/README.md) and are loaded at startup.
Most can also be changed in game from the F4 overlay.

| File | What it covers |
|---|---|
| `hardware.toml` | renderer, window, patches (`pdz_fps60_unlock`, `pdz_aspect_ratio_16_9`), field of view (`pdz_fov`), `pdz_gpu_wait_mode`, third-person camera (`pdz_tp_*`), language (`user_language`), split-screen (`max_local_users`, `keyboard_own_player`) |
| `mapping.toml` | input backend, keyboard and mouse (`mnk_*`), key bindings including `bind_third_person` and `bind_fov_settings` |

## Development notes

- Set `dev_debug_runtime = true` to stub every unknown function and log
  missing ones to `logs/stub_sweep.txt` and `logs/missed_functions.txt`.
- Game-specific hooks are in `src/`: patches, the GPU wait hook in `render/`,
  high-FPS physics and cutscene timing in `timing/`, and the camera mod in `mod/`.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), whose runtime is
  derived from [Xenia](https://github.com/xenia-project/xenia)
- [xenia-canary/game-patches](https://github.com/xenia-canary/game-patches),
  the source of the 60 FPS and 16:9 patches
- [2023PerfectDark/Perfect-Dark-Zero-Reinstated](https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated),
  the source of the high-FPS physics and cutscene timing fixes
- [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB),
  for `settings/gamecontrollerdb.txt` (zlib license)
- [ABGX360](https://github.com/BakasuraRCE/abgx360) and
  [extract-xiso](https://github.com/XboxDev/extract-xiso), for dumping the game

## License

MIT. See [LICENSE](LICENSE). This applies to this project's own code only, not
to the game or its assets.
