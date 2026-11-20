// rate_fixes.cpp - per-frame steps Rare tuned for 30 fps, kept at that rate above 30 fps.
//
// Ported from PerfectDarkZeroRestored (MIT): src/mod/prop_motion_rate.cpp (doors) and
// src/mod/rate_fixes.cpp (turret, idle fidget, screen flicker); TU3 addresses mapped to launch.
// Door threshold is scaled from Rare's 30 fps, not 60.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../guest_memory.h"

REXCVAR_DEFINE_BOOL(pdz_fix_door_motion, true, "Gameplay",
                    "Doors and other animated props open fully above 30 fps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_turret_rate, true, "Gameplay",
                    "Bridge's ship turret aims at Rare's 30 fps speed above 30 fps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_idle_fidget_rate, true, "Gameplay",
                    "Gun idle fidgets come at Rare's 30 fps rate above 30 fps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_screen_flicker_rate, true, "Gameplay",
                    "Framebuffer-effect screen flicker starts at Rare's 30 fps rate above 30 fps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_826F5720); // identity quaternion test (TU3 sub_82705460)
REX_EXTERN(__imp__sub_825C1108); // DP_Turret update (TU3 sub_825D0B28)
REX_EXTERN(__imp__sub_826F2B28); // rand01 (TU3 sub_82702860)

namespace
{
  using namespace guest;

  constexpr double kRareHz = 30.0;

  // Frames of Rare's 30 fps in this frame; 0 when not above 30 fps.
  double Steps30(uint8_t *base)
  {
    const float dt = LoadF(base, kFrameDt);
    if (!std::isfinite(dt) || dt <= 0.0f || dt >= 1.0f / 30.5f)
    {
      return 0.0;
    }
    return double(dt) * kRareHz;
  }

  void LogOnce(std::atomic<bool> &flag, const char *what, double steps)
  {
    if (!flag.exchange(true, std::memory_order_relaxed))
    {
      REXLOG_INFO("rate_fixes: {} at Rare's 30 fps rate ({:.0f} fps)", what, kRareHz / steps);
    }
  }

  // ---- doors: sub_8250F9C0 drops a frame's root motion when its rotation is "identity" ----
  constexpr float kRareEps = 1e-5f; // [82075D7C]
  constexpr float kMinEps = 2.5e-7f;
  std::atomic<bool> g_door_logged{false};

  // ---- turret (sub_825C1108; offsets as TU3) ----
  constexpr uint32_t kTurretState = 944;
  constexpr uint32_t kTurretYaw = 956;
  constexpr uint32_t kTurretPitch = 960;
  constexpr uint32_t kTurretYawTarget = 964;
  constexpr uint32_t kTurretPitchTarget = 968;
  constexpr uint32_t kTurretWait = 972;
  constexpr float kYawShare = 0.1f;   // [820E0F78]
  constexpr float kPitchShare = 0.2f; // [82075FA4]
  constexpr float kYawDone = 1.0f;    // state 2 -> 3
  constexpr float kAimDone = 0.1f;    // state 3 -> 4
  std::atomic<bool> g_turret_logged{false};

  float RateShare(float k, double steps)
  {
    return float(1.0 - std::pow(1.0 - double(k), steps));
  }

  // ---- per-frame chances: rand01() < p ----
  struct Roll
  {
    const char *name;
    uint32_t lr;  // the rand01 call's return address
    float chance; // Rare's per-frame p
    std::atomic<bool> logged{false};
  };
  Roll g_fidget{"idle fidget", 0x824ED6F4, 0.005f};   // sub_824ED1E8, [820EC528]
  Roll g_flicker{"screen flicker", 0x825E5A88, 0.01f}; // sub_825E5878, [8208DCE4]

  void RateRoll(Roll &roll, PPCContext &ctx, uint8_t *base)
  {
    const double steps = Steps30(base);
    if (steps <= 0.0)
    {
      return;
    }
    // Under p with chance p' = 1 - (1 - p)^steps.
    const double p_now = 1.0 - std::pow(1.0 - double(roll.chance), steps);
    if (p_now > 0.0)
    {
      ctx.f1.f64 = double(float(ctx.f1.f64 * (double(roll.chance) / p_now)));
      LogOnce(roll.logged, roll.name, steps);
    }
  }
} // namespace

REX_HOOK_RAW(sub_826F5720)
{
  const uint32_t q = ctx.r3.u32;
  __imp__sub_826F5720(ctx, base);
  if (ctx.r3.u32 == 0 || !REXCVAR_GET(pdz_fix_door_motion))
  {
    return;
  }
  const double steps = Steps30(base);
  if (steps <= 0.0)
  {
    return;
  }
  // 1 - cos(angle / 2) grows with the angle squared.
  const float eps = std::max(kRareEps * float(steps * steps), kMinEps);
  if (std::fabs(LoadF(base, q + 12) - 1.0f) > eps)
  {
    ctx.r3.u64 = 0; // not identity at this rate: the caller applies the motion
    LogOnce(g_door_logged, "prop root motion", steps);
  }
}

REX_HOOK_RAW(sub_825C1108)
{
  const uint32_t turret = ctx.r3.u32;
  if (!IsPointer(turret) || !REXCVAR_GET(pdz_fix_turret_rate))
  {
    __imp__sub_825C1108(ctx, base);
    return;
  }
  const double steps = double(ctx.f1.f64) * kRareHz;
  const uint32_t s0 = Load32(base, turret + kTurretState);
  const float y0 = LoadF(base, turret + kTurretYaw);
  const float p0 = LoadF(base, turret + kTurretPitch);
  const float w0 = LoadF(base, turret + kTurretWait);
  __imp__sub_825C1108(ctx, base);
  if (!(steps > 0.0 && steps < 0.999))
  {
    return;
  }
  const uint32_t s1 = Load32(base, turret + kTurretState);
  const float y1 = LoadF(base, turret + kTurretYaw);
  const float p1 = LoadF(base, turret + kTurretPitch);
  const float ky = RateShare(kYawShare, steps), kp = RateShare(kPitchShare, steps);
  const float ty = LoadF(base, turret + kTurretYawTarget), tp = LoadF(base, turret + kTurretPitchTarget);
  float y = y1, p = p1;
  bool fixed = false;
  if (s0 == 0 && (y1 != y0 || p1 != p0))
  {
    y = (0.0f - y0) * ky + y0; // back to centre
    p = (0.0f - p0) * kp + p0;
    fixed = true;
  }
  else if (s0 == 2 && y1 != y0)
  {
    y = (ty - y0) * ky + y0; // yaw only
    fixed = true;
    if (s1 != 2 && !(std::fabs(ty - y) < kYawDone))
    {
      Store32(base, turret + kTurretState, 2);
      StoreF(base, turret + kTurretWait, w0);
    }
  }
  else if (s0 == 3 && (y1 != y0 || p1 != p0))
  {
    y = (ty - y0) * ky + y0; // both
    p = (tp - p0) * kp + p0;
    fixed = true;
    if (s1 != 3 && !(std::fabs(ty - y) < kAimDone && std::fabs(tp - p) < kAimDone))
    {
      Store32(base, turret + kTurretState, 3);
      StoreF(base, turret + kTurretWait, w0);
    }
  }
  if (fixed)
  {
    StoreF(base, turret + kTurretYaw, y);
    StoreF(base, turret + kTurretPitch, p);
    LogOnce(g_turret_logged, "turret aim", steps);
  }
}

REX_HOOK_RAW(sub_826F2B28)
{
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_826F2B28(ctx, base);
  if (lr == g_fidget.lr)
  {
    if (REXCVAR_GET(pdz_fix_idle_fidget_rate))
    {
      RateRoll(g_fidget, ctx, base);
    }
  }
  else if (lr == g_flicker.lr)
  {
    if (REXCVAR_GET(pdz_fix_screen_flicker_rate))
    {
      RateRoll(g_flicker, ctx, base);
    }
  }
}
