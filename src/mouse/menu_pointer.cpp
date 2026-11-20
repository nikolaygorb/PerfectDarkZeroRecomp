// menu_pointer.cpp - the mouse in the game's menus.
//
// Ported from Perfect-Dark-Zero-Reinstated (MIT): src/native_pointer.cpp (XUI hit test and focus, the render
// hook) and the menu part of src/mousehook.cpp (pointer, clicks, wheel, keystroke injection); TU0 addresses
// as ours. Without the inventory paging, campaign menu and HUD hiding of that fork.
// Menus = no gameplay look for user 0 for 300 ms (front end, pause, briefings, loading). There the cursor is
// free (MouseLook::SetMenuPointer), left click = point and select, right = back, middle = Start, side
// buttons = LB / RB, wheel = D-pad up / down (Shift: left / right, Ctrl: LB / RB).
#include "menu_pointer.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "../guest_memory.h"
#include "mouse_look.h"
#include "steer_look.h"

REXCVAR_DEFINE_BOOL(pdz_mouse_menu_pointer, true, "Mouse",
                    "Mouse in menus: point at a button and left-click to select it, right-click goes back, "
                    "middle-click is Start, the wheel moves up / down")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_827729F8); // XUI element render message (handle, message)
REX_EXTERN(__imp__sub_82AC4D18); // XInputGetKeystroke (user, flags, out)
REX_EXTERN(__imp__sub_82AC4CF8); // XamInputGetKeystrokeEx (&user, flags, out)
REX_EXTERN(sub_8276C0F8);        // element -> handle
REX_EXTERN(sub_82779A50);        // handle -> object of a registered class
REX_EXTERN(sub_82779D88);        // focus of a user
REX_EXTERN(sub_8277A0E8);        // list item index
REX_EXTERN(sub_8278ACA0);        // list select
REX_EXTERN(sub_82780C40);        // list focus
REX_EXTERN(sub_8277DD88);        // XM_WANT_FOCUS
REX_EXTERN(sub_8278C2A0);        // send message (hit test)
REX_EXTERN(sub_8277F5C8);        // XuiElementSetUserFocus
REX_EXTERN(sub_82792F10);        // tab select

namespace
{
  using namespace guest;

  // Registered XUI classes.
  constexpr uint32_t kClassElement = 0x82D356C4;
  constexpr uint32_t kClassTabScene = 0x82D356C0;
  constexpr uint32_t kClassScene = 0x82D356D4;
  constexpr uint32_t kClassList = 0x82D356B4;
  constexpr uint32_t kClassButton = 0x82D356EC;

  // XInput VK_PAD_* keystrokes.
  constexpr uint16_t kPadA = 0x5800, kPadB = 0x5801, kPadX = 0x5802, kPadRB = 0x5804, kPadLB = 0x5805,
                     kPadLT = 0x5806, kPadRT = 0x5807, kPadUp = 0x5810, kPadDown = 0x5811, kPadLeft = 0x5812,
                     kPadRight = 0x5813, kPadStart = 0x5814;
  constexpr uint32_t kErrorEmpty = 4306;
  constexpr int64_t kMenuAfterNs = 300'000'000;

  int64_t NowMs() { return NowNs() / 1'000'000; }

  std::atomic<bool> g_menu{false};

  struct Pointer
  {
    float x, y;
    uint64_t motion, click;
    bool active;
  };

  class Listener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener
  {
  public:
    rex::ui::Window *window = nullptr;
    std::mutex mutex;
    std::atomic<bool> focused{false};
    std::deque<std::pair<uint16_t, uint16_t>> menu_keys;
    bool title_start_pending = true;
    bool shift = false, ctrl = false;
    float pointer_x = -1, pointer_y = -1;
    uint64_t pointer_motion = 0, pointer_click = 0, accepted_click = 0;
    int64_t pointer_click_time = 0;
    float click_x = -1, click_y = -1;

    bool Active() const
    {
      return REXCVAR_GET(pdz_mouse_menu_pointer) && focused && g_menu.load() &&
             rex::cvar::GetFlagByName("mnk_mode") == "true" && !pdz::mouse::MouseLook::Get().OverlayActive();
    }

    void Position(const rex::ui::MouseEvent &e)
    {
      if (!window)
      {
        return;
      }
      pointer_x = float(e.x()) / float(std::max(1u, window->GetActualPhysicalWidth()));
      pointer_y = float(e.y()) / float(std::max(1u, window->GetActualPhysicalHeight()));
      ++pointer_motion;
    }

    void MenuKey(uint16_t vk)
    {
      if (menu_keys.size() > 30)
      {
        return;
      }
      menu_keys.emplace_back(vk, 1); // down
      menu_keys.emplace_back(vk, 2); // up
    }

    void Clear()
    {
      std::lock_guard lock(mutex);
      menu_keys.clear();
      accepted_click = pointer_click;
      pointer_x = pointer_y = -1;
    }

    void OnKeyDown(rex::ui::KeyEvent &e) override { Modifier(e, true); }
    void OnKeyUp(rex::ui::KeyEvent &e) override { Modifier(e, false); }

    void OnMouseDown(rex::ui::MouseEvent &e) override
    {
      if (!Active())
      {
        return;
      }
      std::lock_guard lock(mutex);
      Position(e);
      using B = rex::ui::MouseEvent::Button;
      switch (e.button())
      {
      case B::kLeft:
        if (title_start_pending)
        {
          MenuKey(kPadStart); // PRESS START
        }
        else
        {
          ++pointer_click;
          pointer_click_time = NowMs();
          click_x = pointer_x;
          click_y = pointer_y;
        }
        title_start_pending = false;
        break;
      case B::kRight: MenuKey(kPadB); break;
      case B::kMiddle:
        MenuKey(kPadStart);
        title_start_pending = false;
        break;
      case B::kX1: MenuKey(kPadLB); break;
      case B::kX2: MenuKey(kPadRB); break;
      default: return;
      }
      e.set_handled(true); // not mnk's fire / aim binds
    }

    void OnMouseWheel(rex::ui::MouseEvent &e) override
    {
      if (!Active() || !e.scroll_y())
      {
        return;
      }
      std::lock_guard lock(mutex);
      const int detents = std::clamp(e.scroll_y() / 120, -16, 16);
      for (int i = 0; i < std::abs(detents); ++i)
      {
        MenuKey(ctrl    ? (detents > 0 ? kPadLB : kPadRB)
                : shift ? (detents > 0 ? kPadLeft : kPadRight)
                        : (detents > 0 ? kPadUp : kPadDown));
      }
      e.set_handled(true);
    }

    void OnMouseMove(rex::ui::MouseEvent &e) override
    {
      if (!Active())
      {
        return;
      }
      std::lock_guard lock(mutex);
      Position(e);
    }

    void OnLostFocus(rex::ui::UISetupEvent &) override
    {
      focused = false;
      Clear();
    }
    void OnGotFocus(rex::ui::UISetupEvent &) override
    {
      Clear();
      focused = true;
    }

  private:
    void Modifier(const rex::ui::KeyEvent &e, bool down)
    {
      using VK = rex::ui::VirtualKey;
      const VK vk = e.virtual_key();
      std::lock_guard lock(mutex);
      if (vk == VK::kShift || vk == VK::kLShift || vk == VK::kRShift)
      {
        shift = down;
      }
      else if (vk == VK::kControl || vk == VK::kLControl || vk == VK::kRControl)
      {
        ctrl = down;
      }
      else if (vk == VK::kReturn && down)
      {
        title_start_pending = false;
      }
    }
  };
  Listener g_listener;

  void UpdateMode()
  {
    const bool menu = REXCVAR_GET(pdz_mouse_menu_pointer) &&
                      NowNs() - pdz::mouse::LastGameplayLookNs() > kMenuAfterNs;
    if (g_menu.exchange(menu) != menu)
    {
      g_listener.Clear();
      if (!menu)
      {
        std::lock_guard lock(g_listener.mutex);
        g_listener.title_start_pending = false; // past the title screen
      }
      pdz::mouse::MouseLook::Get().SetMenuPointer(menu);
    }
  }

  Pointer MenuPointer()
  {
    std::lock_guard lock(g_listener.mutex);
    const auto &l = g_listener;
    const bool click = l.pointer_click > l.accepted_click && NowMs() - l.pointer_click_time < 300 &&
                       std::fabs(l.pointer_x - l.click_x) < .01f && std::fabs(l.pointer_y - l.click_y) < .01f;
    return {l.pointer_x, l.pointer_y, l.pointer_motion, click ? l.pointer_click : 0,
            l.Active() && l.pointer_x >= 0};
  }

  void PointerAccept(uint64_t click, uint16_t key = kPadA)
  {
    if (!click)
    {
      return;
    }
    std::lock_guard lock(g_listener.mutex);
    if (click <= g_listener.accepted_click)
    {
      return;
    }
    g_listener.accepted_click = click;
    if (key)
    {
      g_listener.MenuKey(key);
    }
  }

  bool MenuKeystroke(uint8_t *base, uint32_t user, uint32_t address)
  {
    if (user != 0 && (user & 0xFF) != 0xFF)
    {
      return false;
    }
    if (!g_listener.Active() || !Readable(address, 8, true))
    {
      return false;
    }
    std::lock_guard lock(g_listener.mutex);
    if (g_listener.menu_keys.empty())
    {
      return false;
    }
    const auto [key, flags] = g_listener.menu_keys.front();
    g_listener.menu_keys.pop_front();
    std::memset(base + address, 0, 8);
    Store16(base, address, key);
    Store16(base, address + 4, flags);
    return true;
  }

  // ---- XUI ----
  bool Visible(const uint8_t *base, uint32_t element)
  {
    for (int depth = 0; element && depth < 64; ++depth)
    {
      if (!Readable(element, 240) || !(Load32(base, element + 236) & 1))
      {
        return false;
      }
      element = Load32(base, element + 40);
      if (!element)
      {
        return true;
      }
    }
    return false;
  }

  std::u16string Id(const uint8_t *base, uint32_t element)
  {
    std::u16string id;
    if (!Readable(element, 8))
    {
      return id;
    }
    const uint32_t p = Load32(base, element + 4);
    for (uint32_t i = 0; p && i < 96 && Readable(p + i * 2, 2); ++i)
    {
      const uint16_t c = Load16(base, p + i * 2);
      if (!c)
      {
        break;
      }
      id += char16_t(c);
    }
    return id;
  }

  uint32_t PauseOwner(const uint8_t *base, uint32_t element)
  {
    for (unsigned i = 0; element && i < 64 && Readable(element, 44); ++i)
    {
      if (Id(base, element) == u"aid_darkxui_ingame_large_solo_start")
      {
        return element;
      }
      element = Load32(base, element + 40);
    }
    return 0;
  }

  float F(const uint8_t *base, uint32_t address) { return LoadF(base, address); }

  // The pointer over an element of a rendered scene: focus it, and on a click select it.
  void PointAt(PPCContext &ctx, uint8_t *base, uint32_t render_handle)
  {
    const Pointer pointer = MenuPointer();
    if (!pointer.active)
    {
      return;
    }
    // A stationary mouse must not reclaim focus during keyboard / pad navigation.
    static uint64_t last_motion = 0;
    if (last_motion == pointer.motion && !pointer.click)
    {
      return;
    }
    // Guest calls on the UI thread, below the caller's frame.
    PPCContext call = ctx;
    call.r1.u32 -= 256;
    const uint32_t scratch = call.r1.u32 + 80;
    if (!Readable(call.r1.u32, 256, true))
    {
      return;
    }
    call.r3.u64 = render_handle;
    sub_8276C0F8(call, base);
    const uint32_t handle = call.r3.u32;
    call.r3.u64 = handle;
    call.r4.u64 = Load32(base, kClassElement);
    sub_82779A50(call, base);
    const uint32_t element = call.r3.u32;
    if (!Visible(base, element))
    {
      return;
    }
    uint32_t focus_user = 255;
    call.r3.u64 = focus_user;
    sub_82779D88(call, base);
    if (!call.r3.u32)
    {
      focus_user = 0; // in-game pause menu: the local player's focus slot
      call.r3.u64 = focus_user;
      sub_82779D88(call, base);
    }
    if (!call.r3.u32)
    {
      return;
    }
    const uint32_t focused_handle = call.r3.u32;
    call.r4.u64 = Load32(base, kClassElement);
    sub_82779A50(call, base);
    const uint32_t focused_root = call.r3.u32;
    const uint32_t target_root = element;

    // Pause menu tab labels and footer legends are not focusable buttons.
    const uint32_t pause = PauseOwner(base, element);
    const std::u16string id = Id(base, element);
    if (pause && (id == u"A button" || id == u"B button" || id == u"XuiNavButton1" || id == u"XuiBackButton1"))
    {
      return;
    }
    const bool tab = id == u"TabLabel";
    const uint16_t legend = id == u"A_Button"       ? kPadA
                            : id == u"B_Button"     ? kPadB
                            : id == u"Start_button" ? kPadStart
                                                    : 0;
    if (pause && pause == PauseOwner(base, focused_root) && (tab || legend))
    {
      if (!pointer.click)
      {
        return;
      }
      const float width = F(base, element + 12), height = F(base, element + 16);
      const float x = pointer.x * 1280.f - F(base, element + 208);
      const float y = pointer.y * 720.f - F(base, element + 212);
      // Footer legends occupy the last 200 units of their wide asset.
      if (!std::isfinite(x) || !std::isfinite(y) || x < (legend ? std::max(0.f, width - 200.f) : 0.f) ||
          x > width || y < 0 || y > std::min(height, 40.f))
      {
        return;
      }
      if (legend)
      {
        PointerAccept(pointer.click, legend);
        return;
      }
      const std::u16string parent_id = Id(base, Load32(base, element + 40));
      const int index = parent_id == u"Button1" ? 0 : parent_id == u"Button2" ? 1 : parent_id == u"Button3" ? 2 : -1;
      if (index < 0)
      {
        return;
      }
      call.r3.u64 = Load32(base, pause);
      sub_8276C0F8(call, base);
      call.r4.u64 = Load32(base, kClassTabScene);
      sub_82779A50(call, base);
      if (!Readable(call.r3.u32, 24) || Load32(base, call.r3.u32 + 8) <= uint32_t(index))
      {
        return;
      }
      PointerAccept(pointer.click, 0);
      call.r4.u64 = uint32_t(index);
      call.r5.u64 = 1;
      sub_82792F10(call, base);
      return;
    }
    // Nearest XuiScene (cached scenes share the background root during transitions).
    const auto root = [&call, base](uint32_t p) {
      for (unsigned depth = 0; p && depth < 64; ++depth)
      {
        if (!Readable(p, 44))
        {
          return uint32_t(0);
        }
        call.r3.u64 = Load32(base, p);
        sub_8276C0F8(call, base);
        call.r4.u64 = Load32(base, kClassScene);
        sub_82779A50(call, base);
        if (call.r3.u32)
        {
          return p;
        }
        const uint32_t parent = Load32(base, p + 40);
        if (!parent)
        {
          return p;
        }
        p = parent;
      }
      return uint32_t(0);
    };
    // Multiplayer value rows have their own XuiScene: compare the owning multiplayer page.
    const auto multiplayer_page = [base](uint32_t p) {
      for (unsigned depth = 0; p && depth < 64 && Readable(p, 44); ++depth)
      {
        if (Id(base, p).starts_with(u"aid_darkxui_frontend_multi_"))
        {
          return p;
        }
        p = Load32(base, p + 40);
      }
      return uint32_t(0);
    };
    const uint32_t multiplayer = multiplayer_page(target_root);
    if (!focused_root || (multiplayer ? multiplayer_page(focused_root) != multiplayer
                                      : root(focused_root) != root(target_root)))
    {
      return;
    }
    const bool postgame = multiplayer && Id(base, multiplayer) == u"aid_darkxui_frontend_multi_statspostgame";
    const bool owner_lobby = multiplayer && Id(base, multiplayer) == u"aid_darkxui_frontend_multi_gamelobby_owner";
    if ((postgame || owner_lobby) && id == u"TabLabel_large")
    {
      if (postgame && !pointer.click)
      {
        return;
      }
      const float x = pointer.x * 1280.f - F(base, element + 208);
      const float y = pointer.y * 720.f - F(base, element + 212);
      if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > F(base, element + 12) ||
          y > F(base, element + 16))
      {
        return;
      }
      const uint32_t parent = Load32(base, element + 40);
      if (!Readable(parent, 44))
      {
        return;
      }
      const std::u16string name = Id(base, parent);
      if (name.size() != 7 || !name.starts_with(u"Button") || name[6] < u'1' || name[6] > u'6')
      {
        return;
      }
      const uint32_t index = name[6] - u'1';
      const uint32_t owner = Load32(base, parent + 40);
      if (!Readable(owner, 44))
      {
        return;
      }
      if (postgame ? Id(base, owner) != u"resultTabscene"
                   : (Id(base, owner) != u"tabsceneGameDetails" || index > 1))
      {
        return;
      }
      call.r3.u64 = Load32(base, owner);
      sub_8276C0F8(call, base);
      call.r4.u64 = Load32(base, kClassTabScene);
      sub_82779A50(call, base);
      if (!Readable(call.r3.u32, 24) || Load32(base, call.r3.u32 + 8) <= index)
      {
        return;
      }
      last_motion = pointer.motion;
      PointerAccept(pointer.click, 0);
      call.r4.u64 = index;
      call.r5.u64 = 1;
      sub_82792F10(call, base);
      return;
    }
    if ((postgame || owner_lobby) && id.size() == 7 && id.starts_with(u"Button") && id[6] >= u'1' &&
        id[6] <= u'6')
    {
      return;
    }
    // Multiplayer footer parents would take the click as A before their labelled child.
    if (multiplayer && (id == u"buttonCreate" || id == u"buttonPlayerlist" || id == u"buttonBack" ||
                        id == u"buttonBack1" || id == u"BackButton" || id == u"buttonCustomGuns" ||
                        id == u"buttonFilterLocal" || id == u"buttonFilterRemote" || id == u"A button"))
    {
      return;
    }
    // Multiplayer footer legends: their pad action, focus stays on the selected row.
    const uint16_t multiplayer_action = id == u"A_Button"       ? kPadA
                                        : id == u"B_Button"     ? kPadB
                                        : id == u"X_Button"     ? kPadX
                                        : id == u"Start_button" ? kPadStart
                                        : id == u"L_TRIGGER"    ? kPadLT
                                        : id == u"R_TRIGGER"    ? kPadRT
                                                                : 0;
    if (multiplayer && multiplayer_action)
    {
      if (!pointer.click)
      {
        return;
      }
      const float width = F(base, element + 12), height = F(base, element + 16);
      const float x = pointer.x * 1280.f - F(base, element + 208);
      const float y = pointer.y * 720.f - F(base, element + 212);
      if (!std::isfinite(x) || !std::isfinite(y) || x < std::max(0.f, width - (postgame ? 165.f : 130.f)) ||
          x > width || y < 0 || y > std::min(height, 40.f))
      {
        return;
      }
      PointerAccept(pointer.click, multiplayer_action);
      return;
    }
    const bool list_item = id == u"control_ListItem";
    uint32_t list_handle = 0;
    int32_t list_index = -1;
    if (list_item)
    {
      const uint32_t parent = Load32(base, element + 40);
      if (!Readable(parent, 44))
      {
        return;
      }
      call.r3.u64 = Load32(base, parent);
      sub_8276C0F8(call, base);
      list_handle = call.r3.u32;
      call.r4.u64 = Load32(base, kClassList);
      sub_82779A50(call, base);
      if (!Readable(call.r3.u32, 92))
      {
        return;
      }
      const uint32_t count = Load32(base, call.r3.u32 + 88);
      call.r3.u64 = handle;
      sub_8277A0E8(call, base);
      list_index = call.r3.s32;
      if (list_index < 0 || uint32_t(list_index) >= count)
      {
        return;
      }
    }
    // Only buttons, list items and multiplayer value rows take pointer focus.
    call.r3.u64 = handle;
    call.r4.u64 = Load32(base, kClassButton);
    sub_82779A50(call, base);
    const bool value_control = multiplayer && (id == u"DarkXuiValueButton" || id == u"DarkXuiValueButton_simple");
    if (!call.r3.u32 && !list_item && !value_control)
    {
      return;
    }
    call.r3.u64 = list_item ? list_handle : handle;
    call.r4.u64 = scratch;
    Store32(base, scratch, 0);
    sub_8277DD88(call, base); // XM_WANT_FOCUS: excludes disabled controls
    if (call.r3.s32 < 0 || !Load32(base, scratch))
    {
      return;
    }
    // Native hit test with the element's animated transform and bounds.
    const uint32_t message = scratch + 32, payload = scratch + 64;
    Store32(base, message, 20);
    Store32(base, message + 4, 3);
    Store32(base, message + 8, 0);
    Store32(base, message + 12, 12);
    Store32(base, message + 16, payload);
    Store32(base, payload, uint32_t(pointer.x * 1280.f));
    Store32(base, payload + 4, uint32_t(pointer.y * 720.f));
    Store32(base, payload + 8, 0);
    call.r3.u64 = element;
    call.r4.u64 = message;
    sub_8278C2A0(call, base);
    if (!Load32(base, payload + 8))
    {
      return;
    }
    last_motion = pointer.motion;
    if (list_item)
    {
      call.r3.u64 = list_handle;
      call.r4.u64 = uint32_t(list_index);
      sub_8278ACA0(call, base);
      if (call.r3.s32 < 0)
      {
        return;
      }
      call.r3.u64 = list_handle;
      call.r4.u64 = focus_user;
      call.r5.u64 = 1;
      sub_82780C40(call, base);
      if (call.r3.s32 >= 0)
      {
        PointerAccept(pointer.click);
      }
      return;
    }
    if (focused_handle == handle)
    {
      PointerAccept(pointer.click);
      return;
    }
    call.r3.u64 = handle;
    call.r4.u64 = focus_user;
    sub_8277F5C8(call, base);
    if (call.r3.s32 >= 0)
    {
      PointerAccept(pointer.click);
    }
  }
} // namespace

namespace menu_pointer
{
  void Attach(rex::ui::Window *window)
  {
    if (!window)
    {
      return;
    }
    g_listener.window = window;
    g_listener.focused = window->HasFocus();
    window->AddListener(&g_listener);
    // Above mnk (0): its fire / aim binds don't see the clicks in menus. Below ImGui.
    window->AddInputListener(&g_listener, 32);
  }
} // namespace menu_pointer

REX_HOOK_RAW(sub_827729F8)
{
  const uint32_t render_handle = ctx.r3.u32;
  const bool render = Readable(ctx.r4.u32, 20) && Load32(base, ctx.r4.u32 + 4) == 0;
  if (render)
  {
    UpdateMode();
  }
  __imp__sub_827729F8(ctx, base);
  if (render)
  {
    PointAt(ctx, base, render_handle);
  }
}

REX_HOOK_RAW(sub_82AC4D18)
{
  const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_82AC4D18(ctx, base);
  UpdateMode();
  if (ctx.r3.u32 == kErrorEmpty && (!(flags & 0xFF) || (flags & 1)) &&
      MenuKeystroke(base, (flags & 0x40000000) ? 0 : user, out))
  {
    ctx.r3.u64 = 0;
  }
}

REX_HOOK_RAW(sub_82AC4CF8)
{
  const uint32_t user_ptr = ctx.r3.u32, flags = ctx.r4.u32, out = ctx.r5.u32;
  const uint32_t user = Readable(user_ptr, 4) ? Load32(base, user_ptr) : 0xFFu;
  __imp__sub_82AC4CF8(ctx, base);
  UpdateMode();
  if (ctx.r3.u32 == kErrorEmpty && (!(flags & 0xFF) || (flags & 1)) &&
      MenuKeystroke(base, (flags & 0x40000000) ? 0 : user, out))
  {
    if (Readable(user_ptr, 4, true))
    {
      Store32(base, user_ptr, 0);
    }
    ctx.r3.u64 = 0;
  }
}
