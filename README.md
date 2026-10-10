# Perfect Dark Zero Reinstated

<div align="center">
  <img src="assets/icon.png" alt="PerfectDarkZeroRecomp" width="480">
</div>

A static recompilation of [**Perfect Dark Zero**](https://en.wikipedia.org/wiki/Perfect_Dark_Zero)
(Xbox 360, 2005, Title ID `4D5307D3`) to native PC, built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). The game's PowerPC code
is translated into C++ and compiled. There is no emulator.

A community enhancement of **Perfect Dark Zero**, built on [PerfectDarkZeroRecomp](https://github.com/nikolaygorb/PerfectDarkZeroRecomp).

This project aims to make PDZ feel more fluid, responsive, and comfortable on modern PCs while preserving the original game's concept, missions, atmosphere, and personality. It is especially for fans who missed the pace and freedom of **GoldenEye 007** and **Perfect Dark**, and found PDZ's more deliberate combat closer to hardcore, semi-tactical shooters such as Counter-Strike or the Tom Clancy games.

The enhancement explores that earlier style through optional gameplay rules, improved controls, expanded customization, and new content. Original campaign rules remain available so players can choose how they want to play.

[Download the modded update](https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated/releases/latest) · [Report a problem](https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated/issues)

> This is an unofficial fan project, unaffiliated with Microsoft or Rare. You must provide files from your own legally owned copy of Perfect Dark Zero. The update is not a standalone copy of the game.

## What this project adds

### Gameplay and controls

- **Original, Modern, and Classic campaign rules**, with changes to health recovery and combat behavior in the enhanced modes.
- Enhanced-mode weapon options, including a no-spread setting and automatic fire while zoomed for supported weapons.
- Keyboard and mouse controls, with **mouse menu navigation enabled by default** in the modded distribution.
- A first-/third-person camera toggle, bound to **Y** in the modded configuration.
- Weapon raising near walls and obstacles, with firing and reloading able to interrupt the raised state.
- Visible stowed weapons and fixes to weapon placement.
- Physics timing corrections and separate cutscene frame-rate handling.

### Combat Arena customization

- Expanded character selection using characters from across the game.
- Separate **Head Customize** and **Body Customize** choices for Player 1.
- Individual bot customization through bot options.
- Separate male, female, and unrestricted random choices for heads and bodies.
- Custom combinations, including **Joanna — Short Dress**, and matching first-person hand models for the dress characters.

Head/body combinations are still being refined. Some models need individual adjustments to attachments, textures, or animations.

### Missions and presentation

- **Surface, Mission 14**, enabled by default in the modded update, with custom terrain, soundtrack, and mission artwork.
- A custom Joanna Short Dress appearance for the Nightclub mission.
- Revised menus, an in-game changelog, and a GitHub-backed updater.
- Tuned hardware defaults and a mild shadow lift intended to reveal dark details without washing out the whole picture.

## Current release and ongoing work

The published enhancement update is [**modded-v1.5.1**, based on Recomp 1.5](https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated/releases/tag/modded-v1.5.1). Read its release notes before installing.

The underlying recompilation supplies the Windows game runtime, including gameplay, video, audio, saves, and achievements. This enhancement builds on that work.

Shader-cache warmup, compilation scheduling, and temporary texture/pop-in problems are under active investigation. Newer local experiments are **not automatically included in the published release**, and a completely hitch-free first run is not promised.

**Challenges** and **Progression** are currently locked menu entries. Their names describe planned features; playable challenges, prestige, and combat-rating systems are not available yet.

## Installing the modded update

The current archive updates an **existing modded PDZ installation**. It is not a complete first-time installation and does not supply the required base-game data.

1. Close the game and back up your installation and saves.
2. Download `PDZ-Recomp-Modded-Update.zip` from this fork's [Releases](https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated/releases).
3. Extract it and copy the contents of its `PDZ-Recomp` folder into your existing modded installation.
4. Start `perfectdarkzerorecomp.exe`.

The in-game updater targets this fork's releases and preserves settings, profiles, saves, and base-game files. Existing settings can therefore differ from the defaults supplied for a fresh modded configuration.

Keep Surface enabled to play Mission 14. Its supporting files are part of the modded update; an upstream executable by itself does not provide the same enhancement setup.

## Settings and controls

Configuration is stored in `settings`. Many runtime options are also accessible through **F4**; some changes require restarting the game.

| Setting or control | Purpose |
| --- | --- |
| `settings/hardware.toml` | Rendering, window behavior, gameplay rules, camera, language, and mod options |
| `settings/mapping.toml` | Input configuration and key bindings |
| `Y` / `bind_third_person` | Switch camera views in the modded configuration |
| `pdz_mouse_menu_navigation` | Mouse navigation in supported game menus |
| `pdz_campaign_rules` | `0` = Original, `1` = Modern, `2` = Classic |
| `pdz_shadow_lift` | Adjust dark detail; `0` restores the original shadow treatment |
| `user_language` | Select a language included in your game data |

For example, `user_language = 1` selects English, `3` German, and `4` French. Availability depends on your copy's `pdz/ASSETS/loc/` files. A setting cannot add missing localized audio or text.

## How it runs

PerfectDarkZeroRecomp uses the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) to translate the Xbox 360 game's PowerPC code into C++, which is compiled for PC. The game runs as a native application rather than inside an emulator. ReXGlue's runtime builds on work from Xenia.

## Building the source

**Source availability:** this repository currently retains the upstream source baseline, while the release download contains additional enhancement work. Building this checkout does not yet reproduce every feature in the modded release. These instructions build the source currently present here.

The Windows build requires **CMake 3.25+**, **Ninja**, and **Clang/LLVM**, along with the required Windows development toolchain. MSVC alone is insufficient. Windows is the tested target; Linux and macOS presets do not imply enhancement support on those platforms.

```sh
git clone --recursive https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated.git
cd Perfect-Dark-Zero-Reinstated
```

For an existing checkout without its SDK submodule:

```sh
git submodule update --init --recursive
```

Extract your own game data with a suitable tool such as [ABGX360](https://github.com/BakasuraRCE/abgx360) or [extract-xiso](https://github.com/XboxDev/extract-xiso/releases). Place it in `assets/`, with the executable at `assets/default.xex`. This directory is excluded from Git.

```sh
cmake --preset win-amd64-release
cmake --build --preset win-amd64-release
```

Code generation runs during the build. The ReXGlue SDK is built from the `thirdparty/rexglue-sdk` submodule.

Run the resulting Windows executable:

```powershell
.\out\build\win-amd64-release\perfectdarkzerorecomp.exe
```

The source build discovers the repository's `assets` directory automatically. Use `--game_data_root` for another location. Build-run logs are written beneath `out/build/<preset>/logs/`.

## Reporting issues

Report enhancement-specific problems in [this fork's issue tracker](https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated/issues). Include your release version, mission or multiplayer mode, relevant character/head/body choices, and steps to reproduce. Screenshots and logs help; for rendering problems, include your GPU and driver version.

Please distinguish published releases from local test builds. Do not attach game dumps, copyrighted game data, or credentials.

## Credits

- [nikolaygorb / PerfectDarkZeroRecomp](https://github.com/nikolaygorb/PerfectDarkZeroRecomp) — the original recompilation and foundation for this enhancement.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and [Xenia](https://github.com/xenia-project/xenia) — recompilation tooling and runtime foundations.
- [xenia-canary/game-patches](https://github.com/xenia-canary/game-patches) — the original 60 FPS and widescreen patches.
- [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) — controller mappings, distributed under its zlib license.
- [ABGX360](https://github.com/BakasuraRCE/abgx360) and [extract-xiso](https://github.com/XboxDev/extract-xiso) — game-data extraction tools.
- Rare and the original Perfect Dark Zero development team — the game this project celebrates.

## License

The project's own code is covered by the [MIT license](LICENSE). Game content remains the property of its respective owners, and third-party components retain their own licenses. The code license does not grant rights to redistribute the game or its assets.
