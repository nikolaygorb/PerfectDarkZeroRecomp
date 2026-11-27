// physics_step_hook.cpp - Havok solver step above 60 FPS.
//
// Author: 2023PerfectDark (https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated),
// ported from its physics_timing.cpp.
//
// sub_829B61D0 is the TU0 hkpWorld step entry, used by both the main and the Havok worker
// paths: f1 = elapsed frame time, f2 = solver step (normally 1/60 s). The game drives
// keyframed colliders with velocity = (target - position) / frame time, then the world
// advances by solver steps until it reaches the frame time. Above 60 FPS a single step is
// longer than the frame, so colliders overshoot their target and later frames keep
// reversing that velocity (jitter, missed collisions). The step is clamped to the frame
// time; at 60 FPS and below (hitches included) nothing changes. f1, the game clock and the
// number of frames are untouched.
#include <algorithm>
#include <cmath>

#include <rex/hook.h>

#include "../render/engine/scene_probe.h"

REX_EXTERN(__imp__sub_829B61D0);

namespace
{
  float SolverStep(float frame_seconds, float solver_seconds)
  {
    if (!std::isfinite(frame_seconds) || !std::isfinite(solver_seconds) ||
        frame_seconds <= 0.0f || solver_seconds <= 0.0f)
    {
      return solver_seconds;
    }
    return std::min(frame_seconds, solver_seconds);
  }
} // namespace

REX_HOOK_RAW(sub_829B61D0)
{
  const float frame_seconds = static_cast<float>(ctx.f1.f64);
  const float requested = static_cast<float>(ctx.f2.f64);
  const float step = SolverStep(frame_seconds, requested);
  // Simulated frame time for temporal history (scene stream; a no-op otherwise).
  pdz::engine::scene::NoteFrameTime(frame_seconds);
  if (step < requested)
  {
    ctx.f2.f64 = step;
  }

  __imp__sub_829B61D0(ctx, base);
}
