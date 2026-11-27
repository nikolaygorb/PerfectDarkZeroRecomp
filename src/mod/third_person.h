// Third-person camera mod.
#pragma once

#include <cstdint>

namespace third_person
{
  // Registers the toggle hotkey (bind_third_person, default V).
  void RegisterBinds();

  // The player's gameplay camera mode (guest address, 0 before the first
  // update) and its fields after an update: camera position and eye position
  // (world vec3, big-endian floats). The camera is away from the eyes in
  // third person (this mod, rolls).
  uint32_t PlayerCameraMode();
  constexpr uint32_t kModeCameraPosition = 4;
  constexpr uint32_t kModeEyePosition = 188;
} // namespace third_person
