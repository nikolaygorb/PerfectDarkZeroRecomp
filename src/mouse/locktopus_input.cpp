// locktopus_input.cpp - the Locktopus on the keyboard and mouse.
//
// Ported from PerfectDarkZeroRestored (MIT) src/mod/minigame_input.cpp (SteerPick, pdz_fix_minigame_keyboard);
// see its docs/MOUSE_KEYBOARD.md "The Locktopus". Rare reads only the left stick and snapped keys can't reach
// every pin: the stick keys turn a virtual pick round the circle (left / right) and push it out / in (up /
// down), Shift slows it, the mouse moves it too. Launch: lockbot update sub_82639498 (TU3 826492E8) reads
// the holder's input block +1196 / +1200 (TU3 controller +44 / +48), written here just before it.
#include "locktopus_input.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "../guest_memory.h"
#include "mouse_look.h"

REXCVAR_DEFINE_BOOL(pdz_locktopus_mnk, true, "Mouse",
                    "The Locktopus on the keyboard and mouse: the stick keys turn the pick round the circle and "
                    "push it out / in, Shift slows it, the mouse moves it. A controller is unchanged")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_locktopus_key_turn_s, 2.5, "Mouse",
                      "Locktopus: seconds the left / right keys take to turn the pick a full circle")
    .range(0.2, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_locktopus_key_slow, 0.4, "Mouse",
                      "Locktopus: the keys' speed while Shift is held, as a fraction of the full speed")
    .range(0.05, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_locktopus_mouse_counts, 400.0, "Mouse",
                      "Locktopus: mouse counts from the centre to full deflection")
    .range(20.0, 10000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82639498); // lockbot update (subgame, f1 = dt)

namespace
{
  using namespace guest;

  constexpr double kPi = 3.14159265358979323846;
  constexpr double kRadiusPerSecond = 1.0; // up / down keys: centre to full deflection in 1 s
  constexpr double kControllerPush = 0.1;
  constexpr int64_t kNewLockNs = 150'000'000;

  constexpr uint32_t kSubGadget = 4;
  constexpr uint32_t kGadgetHolder = 92;
  constexpr uint32_t kHolderInput = 6560;
  constexpr uint32_t kInputPad = 1152;
  constexpr uint32_t kInputLeftStick = 1196;

  // ---- the stick keys (mnk's binds) ----
  enum Dir
  {
    kLeft,
    kRight,
    kUp,
    kDown,
    kDirs
  };
  constexpr const char *kBinds[2][kDirs] = {
      {"keybind_lstick_left", "keybind_lstick_right", "keybind_lstick_up", "keybind_lstick_down"},
      {"keybind_rstick_left", "keybind_rstick_right", "keybind_rstick_up", "keybind_rstick_down"}};

  std::vector<rex::ui::VirtualKey> KeysOf(const std::string &bind)
  {
    std::vector<rex::ui::VirtualKey> keys;
    size_t pos = 0;
    while (pos <= bind.size())
    {
      size_t comma = bind.find(',', pos);
      if (comma == std::string::npos)
      {
        comma = bind.size();
      }
      std::string token = bind.substr(pos, comma - pos);
      token.erase(0, token.find_first_not_of(" \t"));
      token.erase(token.find_last_not_of(" \t") + 1);
      // Binds with modifiers (Shift+Up) are other pad buttons.
      if (!token.empty() && token.find('+') == std::string::npos)
      {
        const auto vk = rex::ui::ParseVirtualKey(token);
        if (vk != rex::ui::VirtualKey::kNone)
        {
          keys.push_back(vk);
        }
      }
      pos = comma + 1;
    }
    return keys;
  }

  class KeyTracker final : public rex::ui::WindowInputListener, public rex::ui::WindowListener
  {
  public:
    void OnKeyDown(rex::ui::KeyEvent &e) override { Set(e.virtual_key(), true); }
    void OnKeyUp(rex::ui::KeyEvent &e) override { Set(e.virtual_key(), false); }
    void OnLostFocus(rex::ui::UISetupEvent &) override
    {
      std::lock_guard lock(mu_);
      std::fill(std::begin(down_), std::end(down_), false);
    }

    // Each direction: held on either stick's keys. slow = Shift.
    void Read(bool dirs[kDirs], bool &slow)
    {
      std::lock_guard lock(mu_);
      for (int d = 0; d < kDirs; ++d)
      {
        dirs[d] = false;
        for (int stick = 0; stick < 2; ++stick)
        {
          for (const auto vk : KeysOf(rex::cvar::GetFlagByName(kBinds[stick][d])))
          {
            dirs[d] = dirs[d] || (uint32_t(vk) < 256 && down_[uint32_t(vk)]);
          }
        }
      }
      using VK = rex::ui::VirtualKey;
      slow = down_[uint32_t(VK::kShift)] || down_[uint32_t(VK::kLShift)] || down_[uint32_t(VK::kRShift)];
    }

  private:
    void Set(rex::ui::VirtualKey vk, bool down)
    {
      using VK = rex::ui::VirtualKey;
      std::lock_guard lock(mu_);
      const uint32_t k = uint32_t(vk);
      if (k < 256)
      {
        down_[k] = down;
      }
      // Shift arrives as kShift on some backends and as kLShift / kRShift on others.
      if (vk == VK::kLShift || vk == VK::kRShift)
      {
        down_[uint32_t(VK::kShift)] = down;
      }
    }

    std::mutex mu_;
    bool down_[256] = {};
  };
  KeyTracker g_keys;

  // ---- the virtual stick (x right, y up, 1 = full deflection) ----
  struct Pick
  {
    double angle = kPi / 2.0;
    double radius = 0.0;
    bool used = false;
  };

  struct State
  {
    Pick pick;
    int64_t last_ns = 0;
    float wrote_x = 0.0f, wrote_y = 0.0f;
    bool wrote = false;
  };
  State g;

  void SteerPick(Pick &pick, double kx, double ky, bool slow, double mdx, double mdy, double dt)
  {
    const double slow_k = slow ? REXCVAR_GET(pdz_locktopus_key_slow) : 1.0;
    const double turn_s = std::max(0.2, REXCVAR_GET(pdz_locktopus_key_turn_s));
    bool moved = false;
    if (kx != 0.0)
    {
      pick.angle -= (kx > 0 ? 1.0 : -1.0) * (2.0 * kPi / turn_s) * slow_k * dt; // right = clockwise
      moved = true;
    }
    if (ky != 0.0)
    {
      pick.radius = std::clamp(pick.radius + (ky > 0 ? 1.0 : -1.0) * kRadiusPerSecond * slow_k * dt, 0.0, 1.0);
      moved = true;
    }
    if (mdx != 0.0 || mdy != 0.0)
    {
      const double counts = REXCVAR_GET(pdz_locktopus_mouse_counts);
      double x = pick.radius * std::cos(pick.angle) + mdx / counts;
      double y = pick.radius * std::sin(pick.angle) - mdy / counts; // mouse up = stick up
      const double r = std::hypot(x, y);
      if (r > 1.0)
      {
        x /= r;
        y /= r;
      }
      pick.radius = std::min(r, 1.0);
      if (r > 0.02)
      {
        pick.angle = std::atan2(y, x);
      }
      moved = true;
    }
    if (moved)
    {
      pick.angle = std::fmod(pick.angle, 2.0 * kPi);
      if (pick.angle < 0.0)
      {
        pick.angle += 2.0 * kPi;
      }
      pick.used = true;
    }
  }

  // Guest user 0's input block of the player holding the lockbot, or 0.
  uint32_t HolderInput(const uint8_t *base, uint32_t sub)
  {
    const uint32_t gadget = Read32(base, sub + kSubGadget);
    const uint32_t holder = gadget ? Read32(base, gadget + kGadgetHolder) : 0;
    if (!holder || Read32(base, holder) != kPlayerVtable)
    {
      return 0;
    }
    const uint32_t input = Read32(base, holder + kHolderInput);
    if (!input || !Readable(input + kInputPad, kInputLeftStick + 8 - kInputPad, true) ||
        Load32(base, input + kInputPad) != 0)
    {
      return 0;
    }
    return input;
  }

  void BeforeLockbotUpdate(uint8_t *base, uint32_t sub)
  {
    auto &look = pdz::mouse::MouseLook::Get();
    const int64_t now = NowNs();
    double mdx = 0.0, mdy = 0.0;
    look.TakeRawMotion(mdx, mdy);
    if (now - g.last_ns > kNewLockNs)
    {
      g = State{}; // a new lock starts at the centre
      mdx = mdy = 0.0;
      g.last_ns = now - 1;
    }
    const double dt = std::clamp(double(now - g.last_ns) * 1e-9, 0.0, 0.05);
    g.last_ns = now;
    if (!REXCVAR_GET(pdz_locktopus_mnk) || look.OverlayActive())
    {
      return;
    }
    const uint32_t input = HolderInput(base, sub);
    if (!input)
    {
      return;
    }
    bool dirs[kDirs];
    bool slow = false;
    g_keys.Read(dirs, slow);
    const double kx = double(dirs[kRight]) - double(dirs[kLeft]);
    const double ky = double(dirs[kUp]) - double(dirs[kDown]);
    SteerPick(g.pick, kx, ky, slow, mdx, mdy, dt);

    // A controller's left stick pushed (not our last write, no stick keys held): Rare's read stands and the
    // pick follows it.
    const float sx = LoadF(base, input + kInputLeftStick), sy = LoadF(base, input + kInputLeftStick + 4);
    const bool ours = g.wrote && sx == g.wrote_x && sy == g.wrote_y;
    const bool keys_held = kx != 0.0 || ky != 0.0;
    if (!ours && !keys_held && std::hypot(sx, sy) > kControllerPush)
    {
      g.pick.radius = std::min(double(std::hypot(sx, sy)), 1.0);
      g.pick.angle = std::atan2(double(sy), double(sx));
      g.wrote = false;
      return;
    }
    if (!g.pick.used)
    {
      return;
    }
    g.wrote_x = float(g.pick.radius * std::cos(g.pick.angle));
    g.wrote_y = float(g.pick.radius * std::sin(g.pick.angle));
    g.wrote = true;
    StoreF(base, input + kInputLeftStick, g.wrote_x);
    StoreF(base, input + kInputLeftStick + 4, g.wrote_y);
  }
} // namespace

namespace locktopus_input
{
  void Attach(rex::ui::Window *window)
  {
    if (window)
    {
      window->AddInputListener(&g_keys, 1);
      window->AddListener(&g_keys);
    }
  }
} // namespace locktopus_input

REX_HOOK_RAW(sub_82639498)
{
  if (IsPointer(ctx.r3.u32))
  {
    BeforeLockbotUpdate(base, ctx.r3.u32);
  }
  __imp__sub_82639498(ctx, base);
}
