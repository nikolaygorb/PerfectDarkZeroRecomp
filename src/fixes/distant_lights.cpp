// distant_lights.cpp - lights kept or dropped by their distance, not their depth along the view.
//
// Ported from PerfectDarkZeroRestored (MIT) src/mod/distant_lights.cpp (pdz_fix_distant_lights). Launch score
// sub_825F6B30 (TU3 sub_826064D0): (z_view + radius) / f2 + 1, no clamp; same light layout as TU3.
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "../guest_memory.h"

REXCVAR_DEFINE_BOOL(pdz_fix_distant_lights, true, "Gameplay",
                    "Distant window lights no longer switch off when looked at head-on (Rare's bug): lights are "
                    "kept by their distance from the camera instead of their depth along the view")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_distant_lights_reach, 1.0, "Gameplay",
                      "pdz_fix_distant_lights: the distance is multiplied by this before Rare's range test "
                      "(1 = Rare's range; 0.8 = lights reach further)")
    .range(0.05, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_825F6B30); // a light's score: (r3 light, r10 pass, f2 range) -> f1

namespace
{
  using namespace guest;

  constexpr uint32_t kBuffer = 0x82E1AB84; // render buffer index (TU3 82E1BA20)
  constexpr uint32_t kType = 680, kRadius = 684, kViewPos = 1232, kRgb = 828, kActive = 1448;
} // namespace

REX_HOOK_RAW(sub_825F6B30)
{
  const uint32_t light = ctx.r3.u32, pass = ctx.r10.u32 & 0xFF;
  const double range = ctx.f2.f64;
  __imp__sub_825F6B30(ctx, base);
  if (!REXCVAR_GET(pdz_fix_distant_lights))
  {
    return;
  }
  // Rare's gates: enabled in this buffer, active, not directional, in this pass, not black.
  const uint32_t b = Load32(base, kBuffer);
  if (b > 1 || Load32(base, light + (b + 356) * 4) == 0 || int32_t(Load32(base, light + kActive)) < 1 ||
      Load32(base, light + kType) == 1 || pass >= 32 || !((Load32(base, light + b * 340) >> pass) & 1u))
  {
    return;
  }
  const uint32_t rgb = light + b * 16 + kRgb;
  if (!(LoadF(base, rgb) > 0.0f || LoadF(base, rgb + 4) > 0.0f || LoadF(base, rgb + 8) > 0.0f))
  {
    return;
  }
  const uint32_t v = light + kViewPos + 12 * (2 * pass + b);
  const double vx = LoadF(base, v), vy = LoadF(base, v + 4), vz = LoadF(base, v + 8);
  const double dist = std::sqrt(vx * vx + vy * vy + vz * vz);
  if (!(range > 0.0) || !std::isfinite(dist))
  {
    return;
  }
  const double reach = REXCVAR_GET(pdz_distant_lights_reach);
  ctx.f1.f64 = double(float((-dist * reach + LoadF(base, light + kRadius)) / range + 1.0));
}
