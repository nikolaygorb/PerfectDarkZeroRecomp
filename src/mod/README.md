# Third-person camera mod

Perfect Dark Zero is a first-person game, but it already has a third-person
camera: you get it for a moment when you roll. This mod keeps that camera on.
The code is in `third_person.cpp`.

## Usage

Press **V** to switch between first and third person. To use another key,
change `bind_third_person` in `settings/mapping.toml`.

Settings in `settings/hardware.toml` (all can be changed live from the F4 overlay):

| Setting | Default | What it does |
|---|---|---|
| `pdz_tp_enable` | `false` | Start the game in third person |
| `pdz_tp_offset_x/y/z` | `0` | Camera offset in camera space, in roughly centimetres. Negative Z is behind the player. `-80, 10, -320` works well. |
| `pdz_tp_zoom_mode` | `2` | What happens when zoomed: `0` stays behind the player, `1` goes first person, `2` slides forward |
| `pdz_tp_binoculars_fp` | `false` | Use first person while looking through the binoculars |
| `pdz_tp_auto_fp_gadgets` | `true` | Use first person while hacking with the Data Thief |
## How it works

- **The gameplay camera.** It is camera mode 0, updated by `sub_8220D9C8`. It
  stores an offset vector at `+160`. When the offset isn't zero, the camera
  moves by it and a collision ray stops it from going through walls. Rolls fill
  this vector in for about a second. The mod writes its own offset into it every
  frame.
- **Where the camera looks.** It doesn't look straight ahead. It looks at a
  point 10 m in front of the eyes (`+16`), so the crosshair lines up with where
  the bullets go.
- **Only the player's camera.** The CamSpy uses the same camera mode. The mod
  checks the camera's target and only changes the player's camera.


## What didn't work

- **Firing our own ray along the player's view to find the distance.** The
  binoculars' ray passes through characters, so the distance kept jumping
  between the target and the background, and the camera shook.
- **Always using the real distance instead of 10 m.** With the camera 80 cm to
  the side, every jump in distance, say from a nearby wall to the far
  background, made the camera turn on its own by up to about 5°.
- **Smooth transitions.** We switch views instantly on purpose: it felt better.

## How we found all this

1. Using memory dump.
2. Searched it for strings and followed them to code. Task names like
   `CameraControlUpdate` sit in a table next to their functions.
3. Read the generated C++, which keeps the original PowerPC instructions as
   comments.
4. Hooked functions with `REX_HOOK_RAW`, logged what they did while playing,
   and compared the logs with what we did in game. Most of the real answers
   came from these logs, not from reading code: for example the lock flag
   (`+1384` in the binoculars object) and where the target's position is kept.
