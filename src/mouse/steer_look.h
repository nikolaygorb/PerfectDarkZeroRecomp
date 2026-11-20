// steer_look.h - the mouse for things the player steers besides his own view.
#pragma once

#include <cstdint>

namespace pdz::mouse
{
  // The CamSpy, the guided rocket, the hovercraft, the jetpack or a turret was steered by guest user 0
  // within the last 250 ms.
  bool SteeringActive();
  // ... and it takes the mouse into its aim directly (a mounted turret).
  bool DirectSteering();
  // mouse_look_hook.cpp: the player whose own view the mouse aims on foot.
  void NoteHumanPlayer(uint32_t player);
  // When guest user 0 last had control (a look update ran for him, on foot or steering); 0 = never.
  int64_t LastGameplayLookNs();
} // namespace pdz::mouse
