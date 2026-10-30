// mouse_look_hook.cpp - mouse look straight into the player's look update.
//
// sub_825A42B0(player, view, look state, dt) is the look update of a controlled character.
// It returns early during scripted turns (look state +73) and while the player has no look
// control, then runs sub_825A0888 (or sub_825A1280 for one controller style). Those read
// the right stick from the player's input block (player+6560: +1188/+1192 stick x/y,
// +1236/+1240 the in-game sensitivities), run it through a dead zone and response curve,
// accelerate a turn velocity (look state +4 pitch, +8 yaw) and turn the view with it
// through its vtable (+24/+28 get yaw/pitch, +32/+36 set, +40/+44 add; degrees). The
// pitch is then clamped to +-limit (float +64 of the look tuning block, see
// PitchLimit()) and the last call is sub_8259FD70, the aim assist.
//
// The hook is on sub_8259FD70: it is reached only from those two, after the clamp and only
// when the player has look control. Mouse counts are an angle, not a rate: they go straight
// into the view's yaw (+5744) and pitch (+5748), clamped to the same limit; the curve, the
// acceleration and the in-game sensitivity are skipped, the aim assist still applies.
// Turning right lowers the yaw (from the stick path); the pitch grows downward (checked in
// game - the stick path alone suggested the opposite).
//
// Zoom: the camera FOV is pdz_fov * player vtable +656 (= 1 - player+6812). The turn is
// scaled by tan(fov / 2) / tan(pdz_fov / 2), so the picture moves under the mouse at the
// same rate at the screen center whatever the zoom.
#include <rex/cvar.h>
#include <rex/hook.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../game_constants.h"
#include "../game_cvars.h"
#include "mouse_look.h"

REX_EXTERN(__imp__sub_8259FD70);

REXCVAR_DEFINE_BOOL(pdz_mouse_zoom_scaling, true, "Mouse",
                    "Scale mouse look with the camera's field of view, so zoomed aiming turns "
                    "the picture by the same amount per mouse count")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_mouse_zoom_sensitivity, 1.0, "Mouse",
                      "Extra mouse look factor while zoomed in; the turn is already scaled to "
                      "the FOV")
    .range(0.01, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace
{
  // View (player or whatever it looks through): angles in degrees, read by its vtable
  // +24/+28 getters. Other view classes keep them elsewhere and are left alone.
  constexpr uint32_t kViewYaw = 5744;
  constexpr uint32_t kViewPitch = 5748;
  constexpr uint32_t kVtblGetYaw = 24;
  constexpr uint32_t kVtblGetPitch = 28;
  constexpr uint32_t kGetYawFunc = 0x8253E9A8;
  constexpr uint32_t kGetPitchFunc = 0x8253E9B0;

  // Player: input block and its guest user index (negative = no local pad; the SDK
  // merges keyboard and mouse into user 0).
  constexpr uint32_t kPlayerInput = 6560;
  constexpr uint32_t kInputUserIndex = 1152;

  // Player zoom: vtable +656 returns 1 - player+6812.
  constexpr uint32_t kVtblZoom = 656;
  constexpr uint32_t kZoomFunc = 0x8259D0D8;
  constexpr uint32_t kPlayerZoomOut = 6812;

  // Look tuning block: *(*(0x82D6B2B4) + 12), pitch limit (degrees) at +64.
  constexpr uint32_t kGameGlobals = 0x82D6B2B4;
  constexpr uint32_t kGlobalsLookTuning = 12;
  constexpr uint32_t kTuningPitchLimit = 64;
  constexpr float kFallbackPitchLimit = 85.0f;

  constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;

  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  float LoadBeFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = LoadBe32(base, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void StoreBeFloat(uint8_t *base, uint32_t address, float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t *p = base + address;
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
  }

  bool InImage(uint32_t address)
  {
    return address >= GameConstants::kImageBase &&
           address < GameConstants::kImageBase + GameConstants::kImageSize;
  }

  // Object's vtable slot holds func.
  bool HasVirtual(const uint8_t *base, uint32_t object, uint32_t slot, uint32_t func)
  {
    if (!object)
    {
      return false;
    }
    const uint32_t vtbl = LoadBe32(base, object);
    return InImage(vtbl) && LoadBe32(base, vtbl + slot) == func;
  }

  bool PlausibleFov(double fov)
  {
    return std::isfinite(fov) && fov > 0.5 && fov < 170.0;
  }

  double ZoomScale(const uint8_t *base, uint32_t player)
  {
    if (!REXCVAR_GET(pdz_mouse_zoom_scaling) || !HasVirtual(base, player, kVtblZoom, kZoomFunc))
    {
      return 1.0;
    }
    const double base_fov = REXCVAR_GET(pdz_fov);
    const double fov = base_fov * (1.0 - LoadBeFloat(base, player + kPlayerZoomOut));
    if (!PlausibleFov(fov))
    {
      return 1.0;
    }
    const double scale = std::tan(fov * 0.5 * kRadiansPerDegree) /
                         std::tan(base_fov * 0.5 * kRadiansPerDegree);
    if (scale >= 0.999)
    {
      return 1.0;
    }
    return std::clamp(scale * REXCVAR_GET(pdz_mouse_zoom_sensitivity), 0.02, 1.0);
  }

  float PitchLimit(const uint8_t *base)
  {
    const uint32_t globals = LoadBe32(base, kGameGlobals);
    const uint32_t tuning = globals ? LoadBe32(base, globals + kGlobalsLookTuning) : 0;
    const float limit = tuning ? LoadBeFloat(base, tuning + kTuningPitchLimit) : 0.0f;
    return limit > 1.0f && limit < 90.0f ? limit : kFallbackPitchLimit;
  }

  float WrapDegrees(float angle)
  {
    angle = std::fmod(angle, 360.0f);
    if (angle >= 180.0f)
    {
      angle -= 360.0f;
    }
    else if (angle < -180.0f)
    {
      angle += 360.0f;
    }
    return angle;
  }

  void ApplyMouseLook(uint8_t *base, uint32_t player, uint32_t view)
  {
    auto &look = pdz::mouse::MouseLook::Get();
    if (!base || !look.Enabled())
    {
      return;
    }
    // Only views whose angles we know where to find; elsewhere Consume() is not called
    // and the watchdog hands the mouse back to the emulated stick.
    if (!HasVirtual(base, view, kVtblGetYaw, kGetYawFunc) ||
        !HasVirtual(base, view, kVtblGetPitch, kGetPitchFunc))
    {
      return;
    }
    const uint32_t input = player ? LoadBe32(base, player + kPlayerInput) : 0;
    if (!input || static_cast<int32_t>(LoadBe32(base, input + kInputUserIndex)) != 0)
    {
      return;
    }

    const auto turn = look.Consume(ZoomScale(base, player));
    if (!turn)
    {
      return;
    }

    const float yaw_in = LoadBeFloat(base, view + kViewYaw);
    const float pitch_in = LoadBeFloat(base, view + kViewPitch);
    const float yaw_out = WrapDegrees(yaw_in - static_cast<float>(turn->yaw));

    // Clamped only in the direction of the motion: recoil may have pushed the pitch past
    // the limit already, the mouse must not snap it back.
    const float limit = PitchLimit(base);
    const float delta = static_cast<float>(turn->pitch);
    float pitch_out = pitch_in + delta;
    if (delta > 0.0f && pitch_out > limit)
    {
      pitch_out = std::max(pitch_in, limit);
    }
    else if (delta < 0.0f && pitch_out < -limit)
    {
      pitch_out = std::min(pitch_in, -limit);
    }

    StoreBeFloat(base, view + kViewYaw, yaw_out);
    StoreBeFloat(base, view + kViewPitch, pitch_out);
  }
} // namespace

// sub_8259FD70(player, view, look state, f1, f2 = dt): aim assist, the last step of the
// look update.
REX_HOOK_RAW(sub_8259FD70)
{
  ApplyMouseLook(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8259FD70(ctx, base);
}
