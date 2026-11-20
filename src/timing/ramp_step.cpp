// ramp_step.cpp - low edges (Subway's end-train ramp) crossed above 30 fps as at Rare's 30 fps.
//
// Ported from PerfectDarkZeroRestored (MIT) src/mod/player_move.cpp (pdz_fix_ramp_step, start mode 1, edge 0.2),
// without its step log. TU3 -> launch: mover update 82697088 -> 826872F8, resolve 8269C580 -> 8268C7F0, sweep
// 8269C8F0 -> 8268CB60, probe 8269CEF0 -> 8268D160.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../guest_memory.h"

REXCVAR_DEFINE_BOOL(pdz_fix_ramp_step, true, "Gameplay",
                    "Above 30 fps the player's wall check sees Rare's 30 fps move at low edges "
                    "(Subway's end-train ramp); walls are unchanged")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_fix_ramp_step_edge, 0.2, "Gameplay",
                      "pdz_fix_ramp_step: vertical share of the wall check's push that counts as a low edge "
                      "(0 = vertical wall, 1 = floor)")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_826872F8); // mover update (character, mover, f1 = dt)
REX_EXTERN(__imp__sub_8268C7F0); // move resolve (character, type, &previous)
REX_EXTERN(__imp__sub_8268CB60); // wall sweep (character, type, &previous copy)
REX_EXTERN(__imp__sub_8268D160); // ground probe (&position, character, type, &previous) -> hit

namespace
{
  using namespace guest;

  constexpr uint32_t kMoverPosition = 360;
  constexpr float kRareRate = 30.0f;

  struct V3
  {
    float x = 0, y = 0, z = 0;
  };
  V3 LoadV(const uint8_t *base, uint32_t a) { return {LoadF(base, a), LoadF(base, a + 4), LoadF(base, a + 8)}; }
  void StoreV(uint8_t *base, uint32_t a, const V3 &v)
  {
    StoreF(base, a, v.x);
    StoreF(base, a + 4, v.y);
    StoreF(base, a + 8, v.z);
  }
  float Horizontal(const V3 &a, const V3 &b) { return std::hypot(b.x - a.x, b.z - a.z); }

  bool IsPlayer(const uint8_t *base, uint32_t character)
  {
    return IsPointer(character) && Load32(base, character) == kPlayerVtable;
  }

  struct Frame
  {
    bool in_update = false;
    uint32_t mover = 0;
    float dt = 0.0f;
    bool resolved = false;
    V3 previous, target;
    bool probed = false;
    uint32_t probe_hit = 0;
    bool swept = false;
    V3 sweep_in, sweep_out;
    bool edge = false;
    float carried = 0.0f;
  };

  struct Resolved
  {
    V3 pos;
    float dt = 0.0f;
    uint64_t update = 0;
    bool grounded = false, edge = false;
  };
  constexpr int kRing = 16;

  struct MoverState
  {
    uint32_t character = 0;
    uint64_t updates = 0, last_use = 0;
    float pending[2] = {0, 0}; // blocked part of earlier moves (x, z)
    float speed_ref = 0.0f;    // cm/s of the last unblocked move
    Resolved ring[kRing];
    int ring_head = 0, ring_n = 0;
    const Resolved &Back(int back) const { return ring[(ring_head - (back - 1) + kRing) % kRing]; }
    void Push(const Resolved &r)
    {
      ring_head = (ring_head + 1) % kRing;
      ring[ring_head] = r;
      ring_n = std::min(ring_n + 1, kRing);
    }
  };
  constexpr int kMovers = 8;
  MoverState g_movers[kMovers];
  uint64_t g_mover_clock = 0;
  std::atomic<bool> g_logged{false};

  MoverState *MoverFor(uint32_t character)
  {
    ++g_mover_clock;
    MoverState *oldest = &g_movers[0];
    for (MoverState &m : g_movers)
    {
      if (m.character == character)
      {
        m.last_use = g_mover_clock;
        return &m;
      }
      if (m.last_use < oldest->last_use)
      {
        oldest = &m;
      }
    }
    *oldest = MoverState{};
    oldest->character = character;
    oldest->last_use = g_mover_clock;
    return oldest;
  }

  thread_local Frame t_frame;
  thread_local MoverState *t_mover = nullptr;

  bool FixActive(float dt)
  {
    return REXCVAR_GET(pdz_fix_ramp_step) && dt > 0.0f && dt < 1.0f / 31.0f;
  }

  // A sweep push pointing up enough is a low edge (a slope or a step); a wall pushes sideways.
  bool EdgeTouch(const V3 &in, const V3 &out)
  {
    const float cx = out.x - in.x, cy = out.y - in.y, cz = out.z - in.z;
    const float len = std::sqrt(cx * cx + cy * cy + cz * cz);
    return len >= 0.02f && cy / len >= float(REXCVAR_GET(pdz_fix_ramp_step_edge));
  }

  // Frames back to start this sweep from (1/30 s ago) after a low-edge touch, or 0.
  int StaleStartBack(const MoverState &m, const Frame &f)
  {
    if (m.ring_n < 2 || !f.probed || !f.probe_hit || f.dt <= 0.0f)
    {
      return 0;
    }
    const Resolved &prev = m.Back(1);
    if (!prev.edge || !prev.grounded || prev.update + 1 != m.updates)
    {
      return 0;
    }
    const float limit = 1.0f / kRareRate + 0.25f * f.dt;
    float age = f.dt;
    int back = 1;
    while (back < m.ring_n)
    {
      const Resolved &r = m.Back(back);
      const Resolved &older = m.Back(back + 1);
      if (age + r.dt > limit || !older.grounded || older.update + 1 != r.update)
      {
        break;
      }
      age += r.dt;
      ++back;
    }
    if (back < 2)
    {
      return 0;
    }
    const V3 &a = m.Back(back).pos;
    const V3 &b = prev.pos;
    if (Horizontal(a, b) > 40.0f || std::fabs(a.y - b.y) > 30.0f)
    {
      return 0; // a warp or a jump in between
    }
    return back;
  }
} // namespace

REX_HOOK_RAW(sub_8268D160)
{
  const bool mine = t_frame.in_update && IsPlayer(base, ctx.r4.u32);
  __imp__sub_8268D160(ctx, base);
  if (mine)
  {
    t_frame.probed = true;
    t_frame.probe_hit = ctx.r3.u32 & 0xFF;
  }
}

REX_HOOK_RAW(sub_8268CB60)
{
  MoverState *m = t_mover;
  const bool mine = t_frame.in_update && t_frame.mover && m && IsPlayer(base, ctx.r3.u32);
  const uint32_t previous = ctx.r5.u32; // the resolve's own copy of the previous position
  V3 saved;
  int back = 0;
  Frame &f = t_frame;
  if (mine)
  {
    f.sweep_in = LoadV(base, f.mover + kMoverPosition);
    if (FixActive(f.dt) && IsPointer(previous))
    {
      back = StaleStartBack(*m, f);
      if (back > 0)
      {
        saved = LoadV(base, previous);
        StoreV(base, previous, m->Back(back).pos);
      }
    }
  }
  __imp__sub_8268CB60(ctx, base);
  if (!mine)
  {
    return;
  }
  if (back > 0)
  {
    StoreV(base, previous, saved);
  }
  f.swept = true;
  f.sweep_out = LoadV(base, f.mover + kMoverPosition);
  f.edge = EdgeTouch(f.sweep_in, f.sweep_out);
  // Never backward because of the fix.
  if ((back > 0 || f.carried > 0.0f) && f.resolved)
  {
    const float sx = f.target.x - f.previous.x, sz = f.target.z - f.previous.z;
    const float rx = f.sweep_out.x - f.previous.x, rz = f.sweep_out.z - f.previous.z;
    if (sx * rx + sz * rz < 0.0f)
    {
      StoreV(base, f.mover + kMoverPosition, f.previous);
    }
  }
}

REX_HOOK_RAW(sub_8268C7F0)
{
  const uint32_t previous = ctx.r5.u32;
  MoverState *m = t_mover;
  const bool mine = t_frame.in_update && t_frame.mover && m && IsPointer(previous) &&
                    IsPlayer(base, ctx.r3.u32);
  if (!mine)
  {
    __imp__sub_8268C7F0(ctx, base);
    return;
  }
  Frame &f = t_frame;
  f.resolved = true;
  f.previous = LoadV(base, previous);
  f.target = LoadV(base, f.mover + kMoverPosition);
  const float dt = f.dt;
  const bool fix = FixActive(dt);
  float mx = f.target.x - f.previous.x, mz = f.target.z - f.previous.z;
  const float len = std::hypot(mx, mz);
  if (!fix || len < 0.01f || mx * m->pending[0] + mz * m->pending[1] < 0.0f)
  {
    m->pending[0] = m->pending[1] = 0.0f;
    if (!fix || len < 0.01f)
    {
      m->speed_ref = 0.0f;
    }
  }
  else if (m->pending[0] != 0.0f || m->pending[1] != 0.0f)
  {
    // Up to the 1/30 s move at the speed she reached the edge with.
    const float cap = std::max(len / dt, m->speed_ref) / kRareRate;
    float tx = mx + m->pending[0], tz = mz + m->pending[1];
    const float tl = std::hypot(tx, tz);
    if (tl > cap)
    {
      tx *= cap / tl, tz *= cap / tl;
    }
    f.carried = std::hypot(tx, tz) - len;
    StoreF(base, f.mover + kMoverPosition, f.previous.x + tx);
    StoreF(base, f.mover + kMoverPosition + 8, f.previous.z + tz);
    mx = tx, mz = tz;
    if (!g_logged.exchange(true, std::memory_order_relaxed))
    {
      REXLOG_INFO("ramp_step: blocked move carried at a low edge ({:.0f} fps)", 1.0f / dt);
    }
  }
  __imp__sub_8268C7F0(ctx, base);

  Resolved r;
  r.pos = LoadV(base, f.mover + kMoverPosition);
  r.dt = dt;
  r.update = m->updates;
  r.grounded = f.probed && f.probe_hit != 0;
  r.edge = f.swept && f.edge;
  m->Push(r);

  if (fix)
  {
    const V3 after = r.pos;
    const float ax = after.x - f.previous.x, az = after.z - f.previous.z;
    const float tried = std::hypot(mx, mz);
    const float got = std::hypot(ax, az);
    const bool blocked =
        tried >= 0.01f && f.swept && Horizontal(f.sweep_in, f.sweep_out) >= 0.1f * tried && got < tried;
    if (blocked && f.edge)
    {
      m->pending[0] = mx - ax, m->pending[1] = mz - az;
    }
    else
    {
      if (!blocked && f.carried <= 0.0f)
      {
        m->speed_ref = len / dt;
      }
      m->pending[0] = m->pending[1] = 0.0f;
    }
  }
}

REX_HOOK_RAW(sub_826872F8)
{
  const uint32_t character = ctx.r3.u32, mover = ctx.r4.u32;
  if (!IsPointer(mover) || !IsPlayer(base, character))
  {
    __imp__sub_826872F8(ctx, base);
    return;
  }
  MoverState *m = MoverFor(character);
  ++m->updates;
  t_frame = Frame{};
  t_frame.in_update = true;
  t_frame.mover = mover;
  t_frame.dt = float(ctx.f1.f64);
  t_mover = m;
  __imp__sub_826872F8(ctx, base);
  t_frame = Frame{};
  t_mover = nullptr;
}
