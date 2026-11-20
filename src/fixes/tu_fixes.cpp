// tu_fixes.cpp - Rare's Title Update 3 fixes on the launch exe.
//
// Ported from PerfectDarkZeroRestored (MIT) src/mod/tu_fixes.cpp (launch addresses) and midasm_hooks.toml
// (U7, U8, U9, U11, U13+U17, U16; see its docs/BUG_AUDIT.md "Title Update 3 changes"). The cover cooldown (U1)
// is written from that audit's description: TU3 player +7520, its code isn't in the fork.
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/ppc/stack.h>

#include "../guest_memory.h"

REXCVAR_DEFINE_BOOL(pdz_fix_tu_datacore_null, true, "Gameplay",
                    "TU3 fix: Datacore script null check on its tracked character")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_tu_bot_task_null, true, "Gameplay",
                    "TU3 fix: bot tasks without their mode object are skipped")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_tu_base_slot_null, true, "Gameplay",
                    "TU3 fix: empty slots of the multiplayer base table are skipped")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_tu_lv10_health, true, "Gameplay",
                    "TU3 fix: Jungle script gives list entry 14 the 15000 health entry 13 gets")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_tu_flag_drop, true, "Gameplay",
                    "TU3 fix: a destroyed flag weapon drops the flag only when its holder is the carrier")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_tu_teleport, true, "Gameplay",
                    "TU3 fix: teleported characters are re-inserted in the world lists, attached objects follow")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_fix_tu_cover_cooldown, true, "Gameplay",
                    "TU3 fix: no cover scan for 1 s after leaving cover (the Cover Move exploit)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_8253CB78);
REX_EXTERN(__imp__sub_82569708);
REX_EXTERN(__imp__sub_8240FFA0);
REX_EXTERN(__imp__sub_82440190);
REX_EXTERN(__imp__sub_8259D940);
REX_EXTERN(__imp__sub_8259DBA8);
REX_EXTERN(__imp__sub_8252A8E0);
REX_EXTERN(__imp__sub_824F11C0);
REX_EXTERN(__imp__sub_824F4AB8);
REX_EXTERN(sub_82508F78);
REX_EXTERN(sub_82508EA8);

namespace
{
  using namespace guest;

  void Note(const char *what, uint32_t &count)
  {
    if (count++ % 100 == 0)
    {
      REXLOG_INFO("tu_fixes: {} ({} time(s))", what, count);
    }
  }

  PPCFunc *GuestFunction(uint32_t address)
  {
    static PPCFunc *const trap = rex::runtime::ResolveIndirectFunction(0);
    PPCFunc *fn = rex::runtime::ResolveIndirectFunction(address);
    return fn != trap ? fn : nullptr;
  }

  uint32_t CallVirtual(PPCContext &ctx, uint8_t *base, uint32_t object, uint32_t slot, uint32_t r4 = 0,
                       uint32_t r5 = 0)
  {
    PPCFunc *fn = GuestFunction(Load32(base, Load32(base, object) + slot));
    if (!fn)
    {
      return 0;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = object;
    frame.ctx.r4.u64 = r4;
    frame.ctx.r5.u64 = r5;
    fn(frame.ctx, base);
    return frame.ctx.r3.u32;
  }

  void CallObject(PPCContext &ctx, uint8_t *base, PPCFunc *fn, uint32_t object, uint32_t r4 = 0)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = object;
    frame.ctx.r4.u64 = r4;
    fn(frame.ctx, base);
  }

  // U13 + U17
  thread_local uint32_t g_strip_character = 0;
  thread_local uint32_t g_strip_weapon = 0;
  thread_local uint32_t g_flag_weapon = 0; // the flag weapon destructor's this (its r31: a codegen local)
  constexpr uint32_t kFlagWeaponDropReturn = 0x82569760;
  constexpr uint32_t kPlayerRecord = 664;
  constexpr uint32_t kRecordCarrierId = 932;
  constexpr uint32_t kRecordTeam = 940;

  bool IgnoreFlagDrop(PPCContext &ctx, uint8_t *base)
  {
    static uint32_t count = 0;
    if (uint32_t(ctx.lr) != kFlagWeaponDropReturn || !REXCVAR_GET(pdz_fix_tu_flag_drop))
    {
      return false;
    }
    if (!IsPointer(g_strip_character) || g_strip_weapon != g_flag_weapon)
    {
      return false;
    }
    const uint32_t record = CallVirtual(ctx, base, g_strip_character, kPlayerRecord);
    if (!IsPointer(record))
    {
      return false;
    }
    uint32_t id = Load32(base, record + kRecordCarrierId);
    if (id == 2 || id == 3)
    {
      id += Load32(base, record + kRecordTeam) << 2;
    }
    if (id == Load32(base, ctx.r3.u32 + (ctx.r4.u32 + 25) * 4))
    {
      return false;
    }
    Note("flag weapon of a non-carrier destroyed: flag kept", count);
    return true;
  }

  // U16
  constexpr uint32_t kTeleportMoveReturn = 0x8242ADC8;
  constexpr uint32_t kTeleportPlaceReturn = 0x8242ADD8;
  constexpr uint32_t kTeleportWarpReturn = 0x8242ADE8;
  constexpr uint32_t kWorldMatrix = 16;
  constexpr uint32_t kApplyMove = 68;
  struct Teleport
  {
    uint32_t character = 0;
    uint32_t child = 0;
    bool children = false;
    uint8_t old_matrix[64] = {};
  };
  thread_local Teleport g_teleport;

  void BeforeTeleportMove(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    g_teleport = {};
    g_teleport.character = character;
    g_teleport.child = Load32(base, character + 88);
    if (!IsPointer(g_teleport.child) || !Load32(base, g_teleport.child + 72))
    {
      return;
    }
    rex::ppc::stack_guard guard(ctx);
    const uint8_t zero[64] = {};
    const uint32_t matrix = rex::ppc::stack_push(ctx, base, zero, 64);
    CallVirtual(ctx, base, character, kWorldMatrix, matrix);
    std::memcpy(g_teleport.old_matrix, base + matrix, 64);
    g_teleport.children = true;
  }

  void AfterTeleportMove(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    static uint32_t count = 0;
    if (g_teleport.character != character)
    {
      g_teleport = {};
      return;
    }
    CallObject(ctx, base, sub_82508F78, character);
    CallObject(ctx, base, sub_82508EA8, character, 0);
    if (g_teleport.children && Load32(base, g_teleport.child + 72))
    {
      rex::ppc::stack_guard guard(ctx);
      const uint8_t zero[64] = {};
      const uint32_t old_matrix = rex::ppc::stack_push(ctx, base, g_teleport.old_matrix, 64);
      const uint32_t new_matrix = rex::ppc::stack_push(ctx, base, zero, 64);
      CallVirtual(ctx, base, character, kWorldMatrix, new_matrix);
      uint32_t moved = 0;
      for (uint32_t child = g_teleport.child; IsPointer(child) && moved < 64;
           child = Load32(base, child + 96), ++moved)
      {
        CallObject(ctx, base, sub_82508F78, child);
        CallVirtual(ctx, base, child, kApplyMove, old_matrix, new_matrix);
        CallObject(ctx, base, sub_82508EA8, child, 0);
      }
    }
    g_teleport = {};
    Note("teleport: character re-inserted in the world lists", count);
  }

  // U1: cover state player +7332 (0 out, 1 entering, 2 transition, 3 in cover, 4 popped out, 5 exiting).
  constexpr uint32_t kCoverState = 7332;
  constexpr float kCoverCooldown = 1.0f;
  struct Cooldown
  {
    uint32_t player = 0;
    float left = 0.0f;
    uint32_t last = 0; // cover state after the previous update
  };

  // Left cover: exit finished or instant (-> 0) or exit animation started (-> 5).
  bool LeftCover(uint32_t from, uint32_t to)
  {
    return from != to && ((to == 0 && from != 0) || to == 5);
  }
  Cooldown g_cover[4];

  Cooldown &CoverFor(uint32_t player)
  {
    for (Cooldown &c : g_cover)
    {
      if (c.player == player)
      {
        return c;
      }
    }
    for (Cooldown &c : g_cover)
    {
      if (c.player == 0)
      {
        c = {player, 0.0f, 0};
        return c;
      }
    }
    // More players than slots (a new level): start over.
    for (Cooldown &c : g_cover)
    {
      c = {};
    }
    g_cover[0] = {player, 0.0f, 0};
    return g_cover[0];
  }
} // namespace

// U7: before 822F89A0 / 822F8D64 in sub_822F8000 (r31 = the script).
bool PdzTuDatacoreNull(PPCRegister &r31)
{
  static uint32_t count = 0;
  const uint8_t *base = Base();
  if (!base || !REXCVAR_GET(pdz_fix_tu_datacore_null) || Load32(base, r31.u32 + 2592) != 0)
  {
    return false;
  }
  Note("Datacore script: tracked character gone, block skipped", count);
  return true;
}

// U8: the four bot task functions (r31 = the task).
bool PdzTuBotTaskNull(PPCRegister &r31)
{
  static uint32_t count = 0;
  const uint8_t *base = Base();
  if (!base || !REXCVAR_GET(pdz_fix_tu_bot_task_null) || Load32(base, r31.u32 + 76) != 0)
  {
    return false;
  }
  Note("bot task without its mode object, skipped", count);
  return true;
}

// U9: before 821766FC in sub_821766E8 (r8 = the slot's record).
bool PdzTuBaseSlotNull(PPCRegister &r8)
{
  static uint32_t count = 0;
  if (r8.u32 != 0 || !REXCVAR_GET(pdz_fix_tu_base_slot_null))
  {
    return false;
  }
  Note("base table: empty slot skipped", count);
  return true;
}

// U11: before 823067BC (r17 = the list). True re-runs the entry-13 call at 823067B8 for entry 14.
bool PdzTuLv10Health(PPCRegister &r17, PPCRegister &r3, PPCRegister &f1)
{
  static bool injected = false;
  static uint32_t count = 0;
  if (injected)
  {
    injected = false;
    return false;
  }
  const uint8_t *base = Base();
  if (!base || !REXCVAR_GET(pdz_fix_tu_lv10_health) || Load32(base, r17.u32 + 4) < 15)
  {
    return false;
  }
  const uint32_t entries = Load32(base, r17.u32 + 8);
  const uint32_t character = IsPointer(entries) ? Load32(base, entries + 14 * 8 + 4) : 0;
  if (!IsPointer(character))
  {
    return false;
  }
  Note("Jungle script: list entry 14 gets 15000 health", count);
  r3.u64 = character;
  f1.f64 = 15000.0;
  injected = true;
  return true;
}

// U13 + U17: sub_8253CB78(character, weapon) strips a weapon.
REX_HOOK_RAW(sub_8253CB78)
{
  const uint32_t character = g_strip_character;
  const uint32_t weapon = g_strip_weapon;
  g_strip_character = ctx.r3.u32;
  g_strip_weapon = ctx.r4.u32;
  __imp__sub_8253CB78(ctx, base);
  g_strip_character = character;
  g_strip_weapon = weapon;
}

// The flag weapon's destructor: its "flag dropped" call returns to 82569760.
REX_HOOK_RAW(sub_82569708)
{
  const uint32_t weapon = g_flag_weapon;
  g_flag_weapon = ctx.r3.u32;
  __imp__sub_82569708(ctx, base);
  g_flag_weapon = weapon;
}

REX_HOOK_RAW(sub_8240FFA0)
{
  if (!IgnoreFlagDrop(ctx, base))
  {
    __imp__sub_8240FFA0(ctx, base);
  }
}

REX_HOOK_RAW(sub_82440190)
{
  if (!IgnoreFlagDrop(ctx, base))
  {
    __imp__sub_82440190(ctx, base);
  }
}

// U16: the teleport's move calls in sub_8242AB48.
REX_HOOK_RAW(sub_8259D940)
{
  if (uint32_t(ctx.lr) == kTeleportMoveReturn && REXCVAR_GET(pdz_fix_tu_teleport))
  {
    BeforeTeleportMove(ctx, base, ctx.r3.u32);
  }
  __imp__sub_8259D940(ctx, base);
}

REX_HOOK_RAW(sub_8259DBA8)
{
  const bool teleport = uint32_t(ctx.lr) == kTeleportPlaceReturn && REXCVAR_GET(pdz_fix_tu_teleport);
  const uint32_t character = ctx.r3.u32;
  __imp__sub_8259DBA8(ctx, base);
  if (teleport)
  {
    AfterTeleportMove(ctx, base, character);
  }
}

REX_HOOK_RAW(sub_8252A8E0)
{
  const bool teleport = uint32_t(ctx.lr) == kTeleportWarpReturn && REXCVAR_GET(pdz_fix_tu_teleport);
  const uint32_t character = ctx.r3.u32;
  if (teleport)
  {
    BeforeTeleportMove(ctx, base, character);
  }
  __imp__sub_8252A8E0(ctx, base);
  if (teleport)
  {
    AfterTeleportMove(ctx, base, character);
  }
}

// U1: the player's cover update (player, f1 = dt).
REX_HOOK_RAW(sub_824F11C0)
{
  const uint32_t player = ctx.r3.u32;
  if (!IsPointer(player))
  {
    __imp__sub_824F11C0(ctx, base);
    return;
  }
  const float dt = float(ctx.f1.f64);
  Cooldown &c = CoverFor(player);
  if (c.left > 0.0f && dt > 0.0f && dt < 1.0f)
  {
    c.left -= dt;
  }
  const uint32_t s0 = Load32(base, player + kCoverState);
  if (LeftCover(c.last, s0))
  {
    c.left = kCoverCooldown; // forced exit (sub_824F1610) between updates
  }
  __imp__sub_824F11C0(ctx, base);
  const uint32_t s1 = Load32(base, player + kCoverState);
  if (LeftCover(s0, s1))
  {
    c.left = kCoverCooldown;
  }
  c.last = s1;
}

// U1: the cover scan / prompt, called from the cover update only.
REX_HOOK_RAW(sub_824F4AB8)
{
  static uint32_t count = 0;
  const uint32_t player = ctx.r3.u32;
  if (REXCVAR_GET(pdz_fix_tu_cover_cooldown))
  {
    for (const Cooldown &c : g_cover)
    {
      if (c.player == player && c.left > 0.0f)
      {
        Note("cover scan skipped after leaving cover", count);
        return;
      }
    }
  }
  __imp__sub_824F4AB8(ctx, base);
}
