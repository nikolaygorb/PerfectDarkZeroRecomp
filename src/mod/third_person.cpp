// Third-person camera mod. See README.md next to this file.
//
// Mode id 0 (vtable 82097CF8, update sub_8220D9C8) is the gameplay camera. It
// carries a camera-local offset vector at +160/+164/+168: when it is nonzero
// the camera is moved by it (with a collision ray pulling it back in). Rolls
// animate this offset (~cm, z < 0 = behind) and reset it to zero after.
// pdz_tp_enable forces the offset every frame; bind_third_person toggles it.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/ui/keybinds.h>

#include "../game_constants.h"
#include "../game_cvars.h"
#include "third_person.h"

REX_EXTERN(__imp__sub_8220D9C8);
REX_EXTERN(__imp__sub_82637BA0);
REX_EXTERN(__imp__sub_825B2190);

namespace
{
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

  // Gameplay camera mode fields. Before its update: the offset vector and the
  // vertical FOV (58 degrees * player zoom factor, from the previous frame).
  // After its update: final camera position, look point (eye + view rotation *
  // (0, 0, kLookDistance)) and this frame's eye position.
  constexpr uint32_t kCameraPosition = 4;
  constexpr uint32_t kLookPoint = 16;
  constexpr uint32_t kFov = 112;
  constexpr uint32_t kOffset = 160;
  constexpr uint32_t kEyePosition = 188;
  constexpr double kLookDistance = 1000.0;

  // Zoom is detected from the FOV: enter as soon as it drops, leave only once
  // it is fully back; the gap keeps the camera from flapping at a threshold.
  constexpr float kBaseFov = 58.0f;
  constexpr float kZoomEnterFov = kBaseFov - 1.0f;
  constexpr float kZoomExitFov = kBaseFov - 0.3f;
  bool g_zoomed = false;

  // pdz_tp_zoom_mode values.
  constexpr int32_t kZoomKeep = 0;
  constexpr int32_t kZoomFirstPerson = 1;
  constexpr int32_t kZoomForward = 2;

  // Gameplay camera mode we last wrote our offset into, so that turning the
  // mod off can put that camera back in the head once.
  uint32_t g_offset_mode = 0;

  // Mode id 0 is also used by other camera controllers (the CamSpy has its own).
  // Only the player's camera gets the offset: the player class has
  // sub_8259B6D8 ("camera near the eyes", used by held items) at vtable +744.
  constexpr uint32_t kPlayerVtblCameraNear = 744;
  constexpr uint32_t kPlayerCameraNearFunc = 0x8259B6D8;

  bool IsPlayer(const uint8_t *base, uint32_t object)
  {
    if (!object)
    {
      return false;
    }
    const uint32_t vtbl = LoadBe32(base, object);
    if (vtbl < GameConstants::kImageBase || vtbl >= GameConstants::kImageBase + GameConstants::kImageSize)
    {
      return false;
    }
    return LoadBe32(base, vtbl + kPlayerVtblCameraNear) == kPlayerCameraNearFunc;
  }

  int64_t SteadyMs()
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // Gadget updates are not called every frame; treat a gadget as in use for a
  // short while after its last update.
  constexpr std::chrono::milliseconds kGadgetHold{150};

  // The Data Thief's hacking screen lives on its first-person model, which is
  // only used while the camera is in the head. Stamped by its update.
  std::atomic<int64_t> g_data_thief_active_ms{INT64_MIN / 2};

  bool DataThiefNeedsFirstPerson()
  {
    return REXCVAR_GET(pdz_tp_auto_fp_gadgets) &&
           SteadyMs() - g_data_thief_active_ms.load(std::memory_order_relaxed) < kGadgetHold.count();
  }

  // Binoculars (AudioScope): object and time of their last update.
  std::atomic<int64_t> g_binoculars_active_ms{INT64_MIN / 2};
  std::atomic<uint32_t> g_binoculars_object{0};

  bool BinocularsActive()
  {
    return SteadyMs() - g_binoculars_active_ms.load(std::memory_order_relaxed) < kGadgetHold.count();
  }

  // The AudioScope keeps two {ref, target} pairs: +1380/+1384 is the lock,
  // +1388/+1392 the target under the reticle. +1384 is set for exactly as long
  // as a lock holds. The target's position is at +48.
  constexpr uint32_t kBinocularsLockTarget = 1384;
  constexpr uint32_t kTargetPosition = 48;

  uint32_t BinocularsLockTarget(const uint8_t *base)
  {
    const uint32_t binoculars = g_binoculars_object.load(std::memory_order_relaxed);
    return binoculars ? LoadBe32(base, binoculars + kBinocularsLockTarget) : 0;
  }
} // namespace

REX_HOOK_RAW(sub_8220D9C8)
{
  const uint32_t mode = ctx.r3.u32;
  const uint32_t target = ctx.r4.u32;

  const bool is_player = IsPlayer(base, target);
  const float fov = LoadBeFloat(base, mode + kFov);
  if (is_player && fov > 0.0f)
  {
    g_zoomed = g_zoomed ? fov < kZoomExitFov : fov < kZoomEnterFov;
  }
  // A zoom that starts with the binoculars counts as a binocular zoom until it
  // ends.
  static bool binoculars_zoom = false;
  binoculars_zoom = g_zoomed && (binoculars_zoom || BinocularsActive());
  int32_t zoom_mode = kZoomKeep;
  if (g_zoomed)
  {
    zoom_mode = binoculars_zoom && REXCVAR_GET(pdz_tp_binoculars_fp) ? kZoomFirstPerson
                                                                      : REXCVAR_GET(pdz_tp_zoom_mode);
  }

  bool offset_written = false;
  if (REXCVAR_GET(pdz_tp_enable) && is_player && zoom_mode != kZoomFirstPerson &&
      !DataThiefNeedsFirstPerson())
  {
    offset_written = true;
    double x = REXCVAR_GET(pdz_tp_offset_x);
    double y = REXCVAR_GET(pdz_tp_offset_y);
    double z = REXCVAR_GET(pdz_tp_offset_z);
    if (zoom_mode == kZoomForward && z < kLookDistance)
    {
      // The camera looks at eye + (0, 0, kLookDistance), not parallel to the
      // eyes. Slide it along that line (camera -> look point) up to eye depth:
      // the screen centre stays on the same ray, so the crosshair does not
      // jump, and the camera clears the character's head.
      const double k = kLookDistance / (kLookDistance - z);
      x *= k;
      y *= k;
      z = 0.0;
    }
    StoreBeFloat(base, mode + kOffset + 0, static_cast<float>(x));
    StoreBeFloat(base, mode + kOffset + 4, static_cast<float>(y));
    StoreBeFloat(base, mode + kOffset + 8, static_cast<float>(z));
    g_offset_mode = mode;
  }
  else if (mode == g_offset_mode)
  {
    // The game only clears the offset at the end of a roll; do it ourselves.
    StoreBeFloat(base, mode + kOffset + 0, 0.0f);
    StoreBeFloat(base, mode + kOffset + 4, 0.0f);
    StoreBeFloat(base, mode + kOffset + 8, 0.0f);
    g_offset_mode = 0;
  }

  __imp__sub_8220D9C8(ctx, base);

  // While the binoculars hold a lock, they keep the target on the line through
  // the camera position along the eye direction, but the camera looks at the
  // eye-line point kLookDistance ahead, about 3.5 degrees off that line - a
  // third of the screen at binocular zoom. Look straight at the locked target
  // instead. Once a lock has happened during this zoom, keep looking along that
  // line after it ends, until the zoom ends, so releasing the lock does not
  // swing the view back.
  static bool lock_seen_this_zoom = false;
  if (!binoculars_zoom)
  {
    lock_seen_this_zoom = false;
  }
  if (!offset_written || !binoculars_zoom)
  {
    return;
  }

  if (const uint32_t locked = BinocularsLockTarget(base))
  {
    lock_seen_this_zoom = true;
    // Ignore a stale or garbage target: it must be ahead within binocular range.
    float distance2 = 0.0f;
    for (uint32_t axis = 0; axis < 12; axis += 4)
    {
      const float d =
          LoadBeFloat(base, locked + kTargetPosition + axis) - LoadBeFloat(base, mode + kCameraPosition + axis);
      distance2 += d * d;
    }
    if (distance2 > 100.0f * 100.0f && distance2 < 30000.0f * 30000.0f)
    {
      std::memcpy(base + mode + kLookPoint, base + locked + kTargetPosition, 12);
      return;
    }
  }

  if (lock_seen_this_zoom)
  {
    // Look point = camera + (look point - eye): parallel to the eye line.
    for (uint32_t axis = 0; axis < 12; axis += 4)
    {
      const float look = LoadBeFloat(base, mode + kCameraPosition + axis) +
                         LoadBeFloat(base, mode + kLookPoint + axis) -
                         LoadBeFloat(base, mode + kEyePosition + axis);
      StoreBeFloat(base, mode + kLookPoint + axis, look);
    }
  }
}

// Data Thief (vtable 820CEB48) +4: hacking minigame update.
REX_HOOK_RAW(sub_82637BA0)
{
  g_data_thief_active_ms.store(SteadyMs(), std::memory_order_relaxed);
  __imp__sub_82637BA0(ctx, base);
}

// Binoculars (AudioScope, vtable 820CA998) +4: update.
REX_HOOK_RAW(sub_825B2190)
{
  g_binoculars_active_ms.store(SteadyMs(), std::memory_order_relaxed);
  g_binoculars_object.store(ctx.r3.u32, std::memory_order_relaxed);
  __imp__sub_825B2190(ctx, base);
}

namespace third_person
{
  void RegisterBinds()
  {
    rex::ui::RegisterBind("bind_third_person", "V", "Toggle third-person camera", []
                          {
      // Held keys auto-repeat; only a press after a quiet gap counts.
      static auto last_event = std::chrono::steady_clock::time_point{};
      const auto now = std::chrono::steady_clock::now();
      const bool fresh_press = now - last_event > std::chrono::milliseconds(300);
      last_event = now;
      if (fresh_press)
      {
        REXCVAR_SET(pdz_tp_enable, !REXCVAR_GET(pdz_tp_enable));
      } });
  }
} // namespace third_person
