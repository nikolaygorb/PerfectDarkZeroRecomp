// steer_look.cpp - the mouse for the CamSpy, the guided rocket, the hovercraft, the jetpack and turrets.
//
// Ported from PerfectDarkZeroRestored (MIT) src/mod/mouse_look.cpp (NoteSteeredLook, AfterSteeredLook,
// FindAngleFields) and src/mod/look_tuning.cpp (the pad filter hook); see its docs/MOUSE_KEYBOARD.md.
// Launch addresses: look update sub_825A42B0 (TU3 825B3AC0), pad filter sub_8226BE48 (TU3 822733D0).
// The mouse becomes pad 0's right stick in the pad filter, after the SDK's stick dead zone; a mounted
// turret takes it into its aim directly.
#include "steer_look.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../game_constants.h"
#include "../guest_memory.h"
#include "mouse_look.h"

REXCVAR_DEFINE_BOOL(pdz_mouse_steer, true, "Mouse",
                    "The mouse steers the CamSpy, the guided rocket, the hovercraft and the jetpack (as the "
                    "right stick) and aims mounted turrets directly")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_825A42B0); // look update (owner, view, look state, f1 = dt)
REX_EXTERN(__imp__sub_8226BE48); // pad filter (slot, &x, &y); slots 4-7 = right sticks

namespace
{
  using namespace guest;

  constexpr int64_t kSteerHoldNs = 250'000'000;
  std::atomic<int64_t> g_steered_ns{INT64_MIN / 2};
  std::atomic<bool> g_steered_direct{false};
  std::atomic<uint32_t> g_human_player{0};
  std::atomic<int64_t> g_human_ns{INT64_MIN / 2};

  // Look update callers' return addresses.
  constexpr uint32_t kTurretCaller = 0x8257E6C0; // sub_8257D618
  const char *SteeredName(uint32_t caller)
  {
    switch (caller)
    {
    case 0x82530410: return "CamSpy";        // sub_82530150
    case kTurretCaller: return "turret";
    case 0x8224C1F0: return "guided rocket"; // sub_8224C050
    case 0x826660F4: return "hovercraft";    // sub_82665D48
    case 0x826767FC: return "jetpack";       // sub_826761E0
    default: return "steered object";
    }
  }

  constexpr uint32_t kPlayerInput = 6560;
  constexpr uint32_t kInputUserIndex = 1152;

  // The object's input block user index, or -1. A turret is smaller than 6560 bytes: every read is checked.
  int64_t UserOf(const uint8_t *base, uint32_t object)
  {
    if (!IsPointer(object) || object >= 0xFFFF0000u - kPlayerInput || !Readable(object + kPlayerInput, 4))
    {
      return -1;
    }
    const uint32_t input = Load32(base, object + kPlayerInput);
    if (!IsPointer(input) || (input & 3u) != 0 || !Readable(input + kInputUserIndex, 4))
    {
      return -1;
    }
    return int64_t(int32_t(Load32(base, input + kInputUserIndex)));
  }

  // A view's yaw / pitch fields from its vtable: +24 / +28 'lfs f1,D(r3); blr', +32 / +36 'stfs f1,D(r3); blr'.
  struct AngleFields
  {
    uint32_t yaw = 0, pitch = 0;
  };
  bool FindAngleFields(const uint8_t *base, uint32_t object, AngleFields &out)
  {
    const auto in_image = [](uint32_t a) {
      return a >= GameConstants::kImageBase && a < GameConstants::kImageBase + GameConstants::kImageSize - 8u;
    };
    if (!Readable(object, 4))
    {
      return false;
    }
    const uint32_t vtbl = Load32(base, object);
    static std::atomic<uint64_t> cache{0}; // vtable << 32 | yaw << 16 | pitch
    const uint64_t cached = cache.load(std::memory_order_relaxed);
    if (cached != 0 && uint32_t(cached >> 32) == vtbl)
    {
      out.yaw = uint32_t(cached >> 16) & 0xFFFFu;
      out.pitch = uint32_t(cached) & 0xFFFFu;
      return true;
    }
    if (!in_image(vtbl))
    {
      return false;
    }
    const auto field = [&](uint32_t slot, uint32_t opcode) -> int32_t {
      const uint32_t fn = Load32(base, vtbl + slot);
      if (!in_image(fn) || Load32(base, fn + 4) != 0x4E800020u)
      {
        return -1;
      }
      const uint32_t insn = Load32(base, fn);
      if ((insn >> 26) != opcode || ((insn >> 21) & 31) != 1 || ((insn >> 16) & 31) != 3)
      {
        return -1;
      }
      const int32_t d = int16_t(insn & 0xFFFF);
      return d >= 0 && (d & 3) == 0 ? d : -1;
    };
    const int32_t get_yaw = field(24, 48), get_pitch = field(28, 48), set_yaw = field(32, 52),
                  set_pitch = field(36, 52);
    if (get_yaw < 0 || get_yaw != set_yaw || get_pitch < 0 || get_pitch != set_pitch || get_yaw == get_pitch)
    {
      return false;
    }
    out.yaw = uint32_t(get_yaw);
    out.pitch = uint32_t(get_pitch);
    cache.store((uint64_t(vtbl) << 32) | (uint64_t(out.yaw) << 16) | out.pitch, std::memory_order_relaxed);
    return true;
  }

  // Look tuning block: *(*(0x82D6B2B4) + 12), pitch limit at +64 (as mouse_look_hook.cpp).
  float PitchLimit(const uint8_t *base)
  {
    const uint32_t globals = Read32(base, 0x82D6B2B4);
    const uint32_t tuning = globals ? Read32(base, globals + 12) : 0;
    const float limit = tuning && Readable(tuning + 64, 4) ? LoadF(base, tuning + 64) : 0.0f;
    return limit > 1.0f && limit < 90.0f ? limit : 85.0f;
  }

  float WrapDegrees(float angle)
  {
    angle = std::fmod(angle, 360.0f);
    return angle >= 180.0f ? angle - 360.0f : angle < -180.0f ? angle + 360.0f : angle;
  }

  enum class Steer
  {
    kNotTaken,
    kStick,
    kDirect,
  };

  std::atomic<int64_t> g_gameplay_ns{0};

  Steer NoteSteeredLook(uint8_t *base, uint32_t owner, uint32_t object, uint32_t caller)
  {
    // Guest user 0's player: the owner (CamSpy, turret, rocket) or the view (hovercraft, jetpack).
    const uint32_t player = UserOf(base, owner) == 0 ? owner : UserOf(base, object) == 0 ? object : 0;
    const bool human_known = NowNs() - g_human_ns.load(std::memory_order_relaxed) < 1'000'000'000LL;
    if (!player || (human_known && player != g_human_player.load(std::memory_order_relaxed)))
    {
      return Steer::kNotTaken;
    }
    g_gameplay_ns.store(NowNs(), std::memory_order_relaxed);
    if (!REXCVAR_GET(pdz_mouse_steer))
    {
      return Steer::kNotTaken;
    }
    AngleFields fields;
    const bool direct = caller == kTurretCaller && FindAngleFields(base, object, fields);
    g_steered_ns.store(NowNs(), std::memory_order_relaxed);
    if (g_steered_direct.exchange(direct, std::memory_order_relaxed) != direct || !direct)
    {
      static std::atomic<uint32_t> logged{0};
      if (logged.exchange(caller, std::memory_order_relaxed) != caller)
      {
        REXLOG_INFO("steer_look: the {} takes the mouse ({})", SteeredName(caller),
                    direct ? "direct aim" : "as the right stick");
      }
    }
    pdz::mouse::MouseLook::Get().ConsumeForSteering(direct);
    return direct ? Steer::kDirect : Steer::kStick;
  }

  // Between the turret's look update and its own conversion back to radians, wrap and pitch clamp.
  void AfterSteeredLook(uint8_t *base, uint32_t object)
  {
    AngleFields f;
    if (!FindAngleFields(base, object, f))
    {
      return;
    }
    const auto turn = pdz::mouse::MouseLook::Get().ConsumeSteer(1.0);
    if (turn.yaw == 0.0 && turn.pitch == 0.0)
    {
      return;
    }
    const float yaw_in = LoadF(base, object + f.yaw);
    const float pitch_in = LoadF(base, object + f.pitch);
    const float limit = PitchLimit(base);
    const float delta = float(turn.pitch);
    float pitch_out = pitch_in + delta;
    if (delta > 0.0f && pitch_out > limit)
    {
      pitch_out = std::max(pitch_in, limit);
    }
    else if (delta < 0.0f && pitch_out < -limit)
    {
      pitch_out = std::min(pitch_in, -limit);
    }
    StoreF(base, object + f.yaw, WrapDegrees(yaw_in - float(turn.yaw)));
    StoreF(base, object + f.pitch, pitch_out);
  }

  // Pad filter state (TU3 82D6C0D0): 60 bytes per slot, learned centre +0 / +4, dead zone in use +8 / +12.
  constexpr uint32_t kPadFilterState = 0x82D6B3A8;
  constexpr uint32_t kPadFilterStride = 60;
  constexpr uint32_t kPadDeadzoneInUse = 8;
} // namespace

namespace pdz::mouse
{
  bool SteeringActive()
  {
    return NowNs() - g_steered_ns.load(std::memory_order_relaxed) < kSteerHoldNs;
  }

  bool DirectSteering()
  {
    return SteeringActive() && g_steered_direct.load(std::memory_order_relaxed);
  }

  void NoteHumanPlayer(uint32_t player)
  {
    g_human_player.store(player, std::memory_order_relaxed);
    g_human_ns.store(NowNs(), std::memory_order_relaxed);
  }

  int64_t LastGameplayLookNs()
  {
    return g_gameplay_ns.load(std::memory_order_relaxed);
  }
} // namespace pdz::mouse

REX_HOOK_RAW(sub_825A42B0)
{
  const uint32_t owner = ctx.r3.u32, view = ctx.r4.u32, caller = uint32_t(ctx.lr);
  Steer steer = Steer::kNotTaken;
  if (owner && view != owner)
  {
    steer = NoteSteeredLook(base, owner, view, caller);
  }
  else if (owner && UserOf(base, owner) == 0)
  {
    g_gameplay_ns.store(NowNs(), std::memory_order_relaxed);
  }
  __imp__sub_825A42B0(ctx, base);
  if (steer == Steer::kDirect)
  {
    AfterSteeredLook(base, view);
  }
}

REX_HOOK_RAW(sub_8226BE48)
{
  const uint32_t slot = ctx.r3.u32, x_address = ctx.r4.u32, y_address = ctx.r5.u32;
  if (slot == 4)
  {
    // Pad 0's right stick: the mouse while something is steered. A controller pushed further wins.
    const auto stick =
        pdz::mouse::MouseLook::Get().TakeSteerStick(pdz::mouse::SteeringActive() && !pdz::mouse::DirectSteering());
    if ((stick.x != 0.0 || stick.y != 0.0) && IsPointer(x_address) && IsPointer(y_address))
    {
      const uint32_t pad = kPadFilterState + slot * kPadFilterStride;
      const uint32_t addresses[2] = {x_address, y_address};
      const double values[2] = {stick.x, stick.y};
      for (uint32_t axis = 0; axis < 2; ++axis)
      {
        if (values[axis] == 0.0)
        {
          continue;
        }
        // From the filter's learned centre, just past the dead zone it uses, out to the rim.
        const float centre = std::clamp(LoadF(base, pad + axis * 4), -8000.0f, 8000.0f);
        const float deadzone = std::clamp(LoadF(base, pad + kPadDeadzoneInUse + axis * 4), 0.0f, 30000.0f);
        const double room = std::max(0.0, 32767.0 - std::fabs(double(centre)) - double(deadzone) - 2.0);
        const double offset =
            (double(deadzone) + 1.0 + std::fabs(values[axis]) * room) * (values[axis] < 0 ? -1 : 1);
        const int32_t current = int32_t(Load32(base, addresses[axis]));
        if (std::fabs(double(current) - centre) < std::fabs(offset))
        {
          Store32(base, addresses[axis],
                  uint32_t(int32_t(std::clamp(double(centre) + offset, -32767.0, 32767.0))));
        }
      }
    }
  }
  __imp__sub_8226BE48(ctx, base);
}
