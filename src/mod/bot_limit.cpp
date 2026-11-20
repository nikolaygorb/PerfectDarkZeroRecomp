// bot_limit.cpp - offline matches with up to 32 players (you + 31 bots).
//
// Ported from PerfectDarkZeroRestored (MIT) src/mod/player_limit.cpp and its cut_scenarios.cpp option hooks
// (KeepOfflineMaxPlayers), per its docs/BUG_AUDIT.md "Bot limit (MB)" launch plan. Mid-function hooks are in
// patches.toml.
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(pdz_bots_31, true, "Gameplay",
                    "Offline multiplayer: up to 31 bots (32 players); the original allows 15. Applies when the "
                    "match setup opens")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_8246CBD0); // bot limit for the setup defaults: 16 except on Live
REX_EXTERN(__imp__sub_823B2C28); // option value list: count
REX_EXTERN(__imp__sub_823B0388); // setup option setter (id, value)
REX_EXTERN(sub_823AFC00);        // setup option getter (id)

namespace
{
  constexpr uint32_t kOptionMaxPlayers = 5;
  constexpr uint32_t kOptionBotLimit = 76; // BOT_MAXCOUNT (TU3 77)
  constexpr uint32_t kListPlayers2To32 = 13;
  constexpr uint32_t kOfflineBotLimit = 31;
  constexpr int32_t kMaxPlayers = 32;

  bool On() { return REXCVAR_GET(pdz_bots_31); }

  thread_local bool t_setting = false;

  int32_t GetOption(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = id;
    sub_823AFC00(frame.ctx, base);
    return frame.ctx.r3.s32;
  }

  void SetOption(PPCContext &ctx, uint8_t *base, uint32_t id, uint32_t value)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = id;
    frame.ctx.r4.u64 = value;
    __imp__sub_823B0388(frame.ctx, base);
  }
} // namespace

// Before 823B1F7C (li r3,15: the offline list) in the max players router: list 13 instead.
bool PdzPlayersMaxList(PPCRegister &r3)
{
  if (!On())
  {
    return false;
  }
  r3.u64 = kListPlayers2To32;
  return true;
}

// Before 823B0AD0: skip the base config's force-to-16 of max players.
bool PdzKeepPlayersMax()
{
  return On();
}

// Before 82184DF4: no free perception slot, slot -1 is about to be used.
void PdzPerceptionSlotsFull()
{
  static std::atomic<int64_t> last{0};
  static std::atomic<uint32_t> count{0};
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - last.load(std::memory_order_relaxed) >= 1000)
  {
    last.store(now, std::memory_order_relaxed);
    REXLOG_WARN("bot_limit: perception slot table full (170 slots): slot -1 used ({} time(s))", n);
  }
}

REX_HOOK_RAW(sub_8246CBD0)
{
  __imp__sub_8246CBD0(ctx, base);
  if (ctx.r3.u32 == 16 && On())
  {
    ctx.r3.u64 = kOfflineBotLimit;
  }
}

// Option 76's list is a fixed 17 (bots 0-16); its values come from the bot count's list (0-63).
REX_HOOK_RAW(sub_823B2C28)
{
  if (ctx.r3.u32 == kOptionBotLimit && On())
  {
    ctx.r3.u64 = kOfflineBotLimit + 1;
    return;
  }
  __imp__sub_823B2C28(ctx, base);
}

REX_HOOK_RAW(sub_823B0388)
{
  const uint32_t id = ctx.r3.u32;
  const bool on = On() && !t_setting;
  if (on && id == kOptionMaxPlayers && ctx.r4.u32 == 16)
  {
    ctx.r4.u64 = kMaxPlayers; // setup defaults and saved setups store 16
  }
  PPCContext before = ctx;
  __imp__sub_823B0388(ctx, base);
  if (!on)
  {
    return;
  }
  // Offline max players is only a cap: keep it at 32 after every option set (a new profile starts at 8).
  t_setting = true;
  const int32_t max_players = GetOption(before, base, kOptionMaxPlayers);
  if (max_players > 0 && max_players < kMaxPlayers)
  {
    SetOption(before, base, kOptionMaxPlayers, kMaxPlayers);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
    {
      REXLOG_INFO("bot_limit: max players {} -> {} (bot limit {})", max_players,
                  GetOption(before, base, kOptionMaxPlayers), GetOption(before, base, kOptionBotLimit));
    }
  }
  t_setting = false;
}
