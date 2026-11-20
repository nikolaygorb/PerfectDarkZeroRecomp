// ai_rate_fixes.cpp - enemy AI rolls Rare made per frame, kept at their 30 fps rate.
//
// Ported from PerfectDarkZeroRestored (MIT) src/mod/ai_game_fixes.cpp (pdz_fix_ai_semiauto_rhythm AI14,
// pdz_fix_ai_taunt_rate AI55; docs/BUG_AUDIT.md). Launch: rand int sub_826F28C0 (TU3 827025F8), trigger roll
// in sub_821A5CB8 returning to 821A5D88, taunt roll in sub_82357F60 returning to 823582F4, AI clock 82E2E478.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "../guest_memory.h"

REXCVAR_DEFINE_BOOL(pdz_fix_ai_semiauto_rhythm, true, "Gameplay",
                    "Enemies with semi-automatic guns tap the trigger at Rare's 30 fps rhythm above 30 fps "
                    "(they fired up to 2x / 4x faster at 60 / 120 fps)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_ai_taunt_rate, true, "Gameplay",
                    "Subway's pre-fight taunts start at Rare's 30 fps rate above 30 fps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_826F28C0); // rand integer in [r3, r4]
REX_EXTERN(__imp__sub_821A5CB8); // AI shoot action's trigger step (action)

namespace
{
  using namespace guest;

  constexpr uint32_t kAiClock = 0x82E2E478; // float seconds, per level
  constexpr uint32_t kTriggerRollReturn = 0x821A5D88;
  constexpr uint32_t kTauntRollReturn = 0x823582F4;
  constexpr float kTriggerRollInterval = 1.0f / 30.0f;
  constexpr float kTriggerRollSlack = 0.001f;
  constexpr size_t kTriggerActionsMax = 512;

  struct TriggerRoll
  {
    float time = 0.0f;
    uint32_t value = 0;
  };
  std::mutex g_mutex;
  std::unordered_map<uint32_t, TriggerRoll> g_trigger_rolls;

  // sub_821A5CB8's action (its r31 is a codegen local, not in ctx).
  thread_local uint32_t t_action = 0;
} // namespace

REX_HOOK_RAW(sub_821A5CB8)
{
  const uint32_t previous = t_action;
  t_action = ctx.r3.u32;
  __imp__sub_821A5CB8(ctx, base);
  t_action = previous;
}

REX_HOOK_RAW(sub_826F28C0)
{
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr == kTauntRollReturn)
  {
    // rand(0, 100) < 5 per update: roll over a wider range at shorter frames, the same chance per second.
    const float dt = LoadF(base, kFrameDt);
    if (REXCVAR_GET(pdz_fix_ai_taunt_rate) && ctx.r4.u32 == 100 && dt > 0.0f && dt < 1.0f / 30.5f)
    {
      ctx.r4.u64 = uint32_t(std::clamp<long>(std::lround(100.0 * (1.0 / 30.0) / double(dt)), 100, 1000));
    }
    __imp__sub_826F28C0(ctx, base);
    return;
  }
  if (lr != kTriggerRollReturn || !REXCVAR_GET(pdz_fix_ai_semiauto_rhythm) || !t_action)
  {
    __imp__sub_826F28C0(ctx, base);
    return;
  }
  // Each action's press / release roll holds for 1/30 s of AI clock.
  const uint32_t action = t_action;
  const float now = LoadF(base, kAiClock);
  {
    std::lock_guard lock(g_mutex);
    const auto found = g_trigger_rolls.find(action);
    if (found != g_trigger_rolls.end() && now >= found->second.time &&
        now - found->second.time < kTriggerRollInterval - kTriggerRollSlack)
    {
      ctx.r3.u64 = found->second.value;
      return;
    }
  }
  __imp__sub_826F28C0(ctx, base);
  std::lock_guard lock(g_mutex);
  if (g_trigger_rolls.size() >= kTriggerActionsMax)
  {
    g_trigger_rolls.clear();
  }
  g_trigger_rolls[action] = TriggerRoll{now, ctx.r3.u32};
}
