// mouse_look.h - mouse motion for the native mouse look hooks (mouse_look_hook.cpp).
//
// The SDK's keyboard/mouse driver (mnk) turns the mouse into the right stick, which the game
// then runs through its dead zone, response curve and speed cap. While the look hooks run,
// MouseLook takes the mouse from mnk (rex::input::mnk::SetMouseLookActive(false)), locks the
// pointer itself and hands the raw motion to the hooks as an angle. When the hooks stop
// running (menus, cutscenes, vehicles) a watchdog gives the mouse back to mnk, so the
// emulated stick takes over there. Ported from GoldenEye007ReloadedRecomp (same SDK).
#pragma once

#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace rex::ui
{
  class ImGuiDrawer;
  class WindowedAppContext;
} // namespace rex::ui

namespace pdz::mouse
{

  // Degrees, in pointer direction: the hook maps them onto the game's angles.
  struct Turn
  {
    double yaw;   // + = turn right
    double pitch; // + = look down
  };

  class MouseLook final : public rex::ui::WindowInputListener, public rex::ui::WindowListener
  {
  public:
    static MouseLook &Get();

    MouseLook(const MouseLook &) = delete;
    MouseLook &operator=(const MouseLook &) = delete;

    // Call once, after the main window has opened. imgui is optional: when
    // given, the mouse is handed back while an overlay wants it. overlay_open
    // is optional too: while it returns true the pointer stays free, since a
    // locked pointer can't reach an overlay to make it want the mouse.
    void Attach(rex::ui::Window *window, rex::ui::ImGuiDrawer *imgui = nullptr,
                std::function<bool()> overlay_open = nullptr);

    // pdz_mouse_look is on. Cheap; lets a hook skip its own work.
    bool Enabled() const;

    // Called by the look hook once per look update (guest thread). Drives the
    // hand-over from mnk and returns the mouse turn since the last call, or
    // nullopt while the pointer is not ours yet. extra_scale multiplies the
    // turn (zoom).
    std::optional<Turn> Consume(double extra_scale);

    // Steering something else than the player's view (steer_look.cpp; from PerfectDarkZeroRestored
    // src/mod/mouse_look.cpp): keeps the pointer claimed; the motion is kept for TakeSteerStick, or left
    // pending for ConsumeSteer when direct.
    void ConsumeForSteering(bool direct);
    // Direct steering (a mounted turret): the mouse turn since the last call.
    Turn ConsumeSteer(double extra_scale);
    // The mouse as a right stick, x / y in -1..1 (y + = up), smoothed; zero when steering is false.
    struct SteerStick
    {
      double x = 0.0, y = 0.0;
    };
    SteerStick TakeSteerStick(bool steering);

    // Raw mouse motion since the last call, whoever owns the pointer (the Locktopus).
    void TakeRawMotion(double &dx, double &dy);

    // An ImGui overlay wants the mouse.
    bool OverlayActive() const;

    // The menu pointer (menu_pointer.cpp) wants a free, visible cursor: mnk is not given the mouse back.
    void SetMenuPointer(bool active);

    // rex::ui::WindowInputListener
    void OnMouseMove(rex::ui::MouseEvent &e) override;

    // rex::ui::WindowListener
    void OnLostFocus(rex::ui::UISetupEvent &) override;
    void OnGotFocus(rex::ui::UISetupEvent &) override;
    void OnClosing(rex::ui::UIEvent &) override;

  private:
    MouseLook() = default;
    ~MouseLook();

    enum class Phase
    {
      kIdle,      // mnk owns the mouse
      kHandoff,   // mnk told to let go, waiting for it to do so
      kEngaging,  // capture request posted to the UI thread
      kEngaged,   // pointer locked, deltas flowing
      kReleasing, // release request posted to the UI thread
    };

    using Clock = std::chrono::steady_clock;
    using UiTask = std::function<void()>;

    UiTask BeginReleaseLocked();
    UiTask BeginEngageLocked();
    // The claim's state machine (idle -> handoff -> engaging -> engaged); true when engaged.
    bool AdvanceClaimLocked(Clock::time_point now, UiTask &task);
    Turn TakeTurnLocked(double extra_scale);
    static Turn TurnFromCounts(double dx, double dy, double extra_scale);
    bool OverlayWantsMouse() const;
    // Hands the mouse back to mnk unless the menu pointer keeps it free.
    void GiveBackToMnk();

    // Call without mu_ held.
    void Post(UiTask task);

    void WatchdogLoop();

    rex::ui::Window *window_ = nullptr;
    rex::ui::WindowedAppContext *ui_ = nullptr;
    rex::ui::ImGuiDrawer *imgui_ = nullptr;
    std::function<bool()> overlay_open_;

    mutable std::mutex mu_;
    Phase phase_ = Phase::kIdle;
    bool has_focus_ = true;
    // Set by the UI-thread engage task
    bool captured_ = false;
    rex::ui::Window::CursorVisibility saved_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
    Clock::time_point handoff_since_{};
    Clock::time_point last_consume_{};

    double pending_dx_ = 0.0;
    double pending_dy_ = 0.0;
    // ConsumeForSteering / TakeSteerStick: motion kept for the steered thing and its smoothed speed.
    double steer_dx_ = 0.0, steer_dy_ = 0.0, steer_vx_ = 0.0, steer_vy_ = 0.0;
    Clock::time_point last_stick_{};
    double raw_dx_ = 0.0, raw_dy_ = 0.0;
    std::atomic<bool> menu_pointer_{false};

    std::thread watchdog_;
    std::condition_variable watchdog_cv_;
    bool watchdog_stop_ = false;
  };

} // namespace pdz::mouse
