// mouse_look.cpp - see mouse_look.h.
#include "mouse_look.h"

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <utility>

REXCVAR_DEFINE_BOOL(pdz_mouse_look, true, "Mouse",
                    "Native mouse look: mouse motion turns the camera directly (1:1, no dead "
                    "zone, response curve or aim friction) instead of emulating the right stick. "
                    "Where the look hook does not run (menus, cutscenes, vehicles) the stick "
                    "emulation stays in charge. Needs mnk_mode and mnk_mouse")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_mouse_sensitivity, 2.0, "Mouse",
                      "Mouse look sensitivity: camera turn per mouse count = 0.022 deg x this "
                      "(as in Source games)")
    .range(0.01, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_mouse_invert_x, false, "Mouse", "Invert the horizontal mouse look axis")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_mouse_invert_y, false, "Mouse", "Invert the vertical mouse look axis")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::mouse
{

  namespace
  {
    // If Consume() stops being called (the look hook doesn't run in this game
    // state: menu, cutscene, vehicle...) the pointer must not stay hidden and
    // locked with nothing consuming its motion.
    constexpr std::chrono::milliseconds kConsumerTimeout{300};
    constexpr std::chrono::milliseconds kWatchdogPeriod{100};

    // After asking mnk to let go: never engage before the short one (a capture
    // update may already be queued on its side), and stop waiting for it to
    // release after the long one.
    constexpr std::chrono::milliseconds kHandoffMin{50};
    constexpr std::chrono::milliseconds kHandoffMax{300};

    constexpr double kDegreesPerCount = 0.022;

    // Guards against a stalled frame or a pointer teleport turning into one
    // huge snap: no single Consume() turns the camera by more than this.
    constexpr double kMaxDegreesPerCall = 85.0;

    // TakeSteerStick: the mouse speed's smoothing time constant (seconds).
    constexpr double kSteerSmoothing = 0.04;

    // mnk_sensitivity lives in the SDK DLL: read by name, twice a second.
    double SteerSensitivity()
    {
      static std::atomic<double> value{5.0};
      static std::atomic<int64_t> read_at{0};
      const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
      if (now - read_at.load(std::memory_order_relaxed) >= 500)
      {
        read_at.store(now, std::memory_order_relaxed);
        const double v = std::atof(rex::cvar::GetFlagByName("mnk_sensitivity").c_str());
        if (std::isfinite(v) && v > 0.0)
        {
          value.store(std::min(v, 100.0), std::memory_order_relaxed);
        }
      }
      return value.load(std::memory_order_relaxed);
    }
  } // namespace

  MouseLook &MouseLook::Get()
  {
    static MouseLook instance;
    return instance;
  }

  MouseLook::~MouseLook()
  {
    {
      std::lock_guard lock(mu_);
      watchdog_stop_ = true;
    }
    watchdog_cv_.notify_all();
    if (watchdog_.joinable())
    {
      watchdog_.join();
    }
  }

  void MouseLook::Attach(rex::ui::Window *window, rex::ui::ImGuiDrawer *imgui,
                         std::function<bool()> overlay_open)
  {
    if (!window)
    {
      return;
    }
    {
      std::lock_guard lock(mu_);
      if (window_)
      {
        return; // already attached
      }
      window_ = window;
      ui_ = &window->app_context();
      imgui_ = imgui;
      overlay_open_ = std::move(overlay_open);
      // Assumed focused until told otherwise, like the mnk driver does: the
      // window may not report its state yet and a focus event may never come.
      has_focus_ = true;
    }
    window->AddInputListener(this, 0);
    window->AddListener(this);
    watchdog_ = std::thread([this] { WatchdogLoop(); });
  }

  bool MouseLook::Enabled() const
  {
    return REXCVAR_GET(pdz_mouse_look);
  }

  void MouseLook::OnMouseMove(rex::ui::MouseEvent &e)
  {
    std::lock_guard lock(mu_);
    raw_dx_ += e.dx();
    raw_dy_ += e.dy();
    if (phase_ != Phase::kEngaged)
    {
      return;
    }
    pending_dx_ += e.dx();
    pending_dy_ += e.dy();
  }

  void MouseLook::OnLostFocus(rex::ui::UISetupEvent &)
  {
    UiTask task;
    {
      std::lock_guard lock(mu_);
      has_focus_ = false;
      task = BeginReleaseLocked();
    }
    Post(std::move(task));
  }

  void MouseLook::OnGotFocus(rex::ui::UISetupEvent &)
  {
    std::lock_guard lock(mu_);
    has_focus_ = true;
  }

  void MouseLook::OnClosing(rex::ui::UIEvent &)
  {
    bool give_back = false;
    {
      std::lock_guard lock(mu_);
      // The window is going away with its capture; nothing to undo on it, only
      // the mnk driver to hand the mouse back to.
      give_back = phase_ != Phase::kIdle;
      phase_ = Phase::kIdle;
      captured_ = false;
      window_ = nullptr;
      ui_ = nullptr;
      watchdog_stop_ = true;
    }
    watchdog_cv_.notify_all();
    if (give_back)
    {
      GiveBackToMnk();
    }
  }

  bool MouseLook::OverlayWantsMouse() const
  {
    return (overlay_open_ && overlay_open_()) || (imgui_ && imgui_->GetIO().WantCaptureMouse);
  }

  MouseLook::UiTask MouseLook::BeginEngageLocked()
  {
    phase_ = Phase::kEngaging;
    return [this]
    {
      rex::ui::Window *window = nullptr;
      {
        std::lock_guard lock(mu_);
        if (phase_ != Phase::kEngaging)
        {
          return; // released again before this ran
        }
        window = window_;
      }
      if (!window)
      {
        return;
      }

      // Window state is touched on the UI thread only, and outside the lock.
      const auto visibility = window->GetCursorVisibility();
      const auto restore_to = visibility == rex::ui::Window::CursorVisibility::kHidden
                                  ? rex::ui::Window::CursorVisibility::kVisible
                                  : visibility;
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      window->CaptureMouse();
      if (!window->SetRelativeMouseMode(true))
      {
        REXLOG_WARN("pdz_mouse_look: this window backend has no pointer lock, mouse motion is "
                    "taken from raw pointer deltas without re-centering");
      }

      std::lock_guard lock(mu_);
      captured_ = true;
      saved_cursor_ = restore_to;
      pending_dx_ = pending_dy_ = 0.0;
      // A release posted meanwhile keeps its phase and undoes the capture.
      if (phase_ == Phase::kEngaging)
      {
        phase_ = Phase::kEngaged;
      }
    };
  }

  MouseLook::UiTask MouseLook::BeginReleaseLocked()
  {
    if (phase_ == Phase::kIdle || phase_ == Phase::kReleasing)
    {
      return {};
    }
    phase_ = Phase::kReleasing;
    return [this]
    {
      rex::ui::Window *window = nullptr;
      bool captured = false;
      auto restore_to = rex::ui::Window::CursorVisibility::kVisible;
      {
        std::lock_guard lock(mu_);
        window = window_;
        captured = captured_;
        restore_to = saved_cursor_;
      }
      if (captured && window)
      {
        window->SetRelativeMouseMode(false);
        window->ReleaseMouse();
        window->SetCursorVisibility(restore_to);
      }
      {
        std::lock_guard lock(mu_);
        captured_ = false;
        pending_dx_ = pending_dy_ = 0.0;
        phase_ = Phase::kIdle;
      }
      // Only now: mnk re-captures on its next poll, after the pointer is free.
      GiveBackToMnk();
    };
  }

  void MouseLook::Post(UiTask task)
  {
    if (!task)
    {
      return;
    }
    rex::ui::WindowedAppContext *ui;
    {
      std::lock_guard lock(mu_);
      ui = ui_;
    }
    if (ui && ui->CallInUIThreadDeferred(std::move(task)))
    {
      return;
    }
    // No UI thread to run it on (shutting down): there is no window state left
    // worth undoing, just give the mouse back to mnk.
    {
      std::lock_guard lock(mu_);
      phase_ = Phase::kIdle;
      captured_ = false;
    }
    GiveBackToMnk();
  }

  void MouseLook::WatchdogLoop()
  {
    std::unique_lock lock(mu_);
    while (!watchdog_stop_)
    {
      watchdog_cv_.wait_for(lock, kWatchdogPeriod);
      if (watchdog_stop_)
      {
        break;
      }
      if (phase_ != Phase::kIdle && phase_ != Phase::kReleasing &&
          Clock::now() - last_consume_ > kConsumerTimeout)
      {
        UiTask task = BeginReleaseLocked();
        lock.unlock();
        Post(std::move(task));
        lock.lock();
      }
    }
  }

  bool MouseLook::AdvanceClaimLocked(Clock::time_point now, UiTask &task)
  {
    last_consume_ = now;
    switch (phase_)
    {
    case Phase::kIdle:
      // Claim: mnk stops mapping the mouse to the stick and releases its own capture on its next poll.
      rex::input::mnk::SetMouseLookActive(false);
      phase_ = Phase::kHandoff;
      handoff_since_ = now;
      break;

    case Phase::kHandoff:
    {
      const auto waited = now - handoff_since_;
      const bool mnk_let_go =
          window_->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kHidden;
      if (waited >= kHandoffMin && (mnk_let_go || waited >= kHandoffMax))
      {
        task = BeginEngageLocked();
      }
      break;
    }

    case Phase::kEngaging:
    case Phase::kReleasing:
      break;

    case Phase::kEngaged:
      return true;
    }
    return false;
  }

  std::optional<Turn> MouseLook::Consume(double extra_scale)
  {
    UiTask task;
    std::optional<Turn> result;
    {
      std::lock_guard lock(mu_);
      const bool want =
          REXCVAR_GET(pdz_mouse_look) && window_ && has_focus_ && !OverlayWantsMouse();
      if (!want)
      {
        last_consume_ = Clock::now();
        task = BeginReleaseLocked();
      }
      else if (AdvanceClaimLocked(Clock::now(), task))
      {
        result = TakeTurnLocked(extra_scale);
      }
    }
    Post(std::move(task));
    return result;
  }

  void MouseLook::ConsumeForSteering(bool direct)
  {
    UiTask task;
    {
      std::lock_guard lock(mu_);
      const bool want =
          REXCVAR_GET(pdz_mouse_look) && window_ && has_focus_ && !OverlayWantsMouse();
      if (!want)
      {
        last_consume_ = Clock::now();
        task = BeginReleaseLocked();
      }
      else if (AdvanceClaimLocked(Clock::now(), task) && !direct)
      {
        steer_dx_ += pending_dx_;
        steer_dy_ += pending_dy_;
        pending_dx_ = pending_dy_ = 0.0;
      }
    }
    Post(std::move(task));
  }

  Turn MouseLook::ConsumeSteer(double extra_scale)
  {
    UiTask task;
    Turn turn{0.0, 0.0};
    {
      std::lock_guard lock(mu_);
      const bool want =
          REXCVAR_GET(pdz_mouse_look) && window_ && has_focus_ && !OverlayWantsMouse();
      if (!want)
      {
        last_consume_ = Clock::now();
        task = BeginReleaseLocked();
      }
      else if (AdvanceClaimLocked(Clock::now(), task))
      {
        turn = TakeTurnLocked(extra_scale);
      }
    }
    Post(std::move(task));
    return turn;
  }

  MouseLook::SteerStick MouseLook::TakeSteerStick(bool steering)
  {
    std::lock_guard lock(mu_);
    const auto now = Clock::now();
    const double dt =
        std::clamp(std::chrono::duration<double>(now - last_stick_).count(), 1e-4, 0.1);
    last_stick_ = now;
    const double cx = steer_dx_, cy = steer_dy_;
    steer_dx_ = steer_dy_ = 0.0;
    SteerStick out;
    if (!steering)
    {
      steer_vx_ = steer_vy_ = 0.0;
      return out;
    }
    // Speed in counts per second, smoothed so a poll without motion doesn't drop the stick to zero.
    const double alpha = 1.0 - std::exp(-dt / kSteerSmoothing);
    steer_vx_ += (cx / dt - steer_vx_) * alpha;
    steer_vy_ += (cy / dt - steer_vy_) * alpha;
    // mnk's scale (mnk_sensitivity x 200 per count per 120 Hz poll) as a rate.
    const double per_count_per_s = SteerSensitivity() * 200.0 / 120.0 / 32767.0;
    const double y_sign = REXCVAR_GET(pdz_mouse_invert_y) ? -1.0 : 1.0;
    const double fx = std::clamp(steer_vx_ * per_count_per_s, -1.0, 1.0);
    const double fy = std::clamp(-steer_vy_ * per_count_per_s * y_sign, -1.0, 1.0);
    out.x = std::fabs(fx) < 0.003 ? 0.0 : fx;
    out.y = std::fabs(fy) < 0.003 ? 0.0 : fy;
    return out;
  }

  void MouseLook::TakeRawMotion(double &dx, double &dy)
  {
    std::lock_guard lock(mu_);
    dx = raw_dx_;
    dy = raw_dy_;
    raw_dx_ = raw_dy_ = 0.0;
  }

  bool MouseLook::OverlayActive() const
  {
    std::lock_guard lock(mu_);
    return OverlayWantsMouse();
  }

  void MouseLook::SetMenuPointer(bool active)
  {
    if (menu_pointer_.exchange(active) == active)
    {
      return;
    }
    std::lock_guard lock(mu_);
    if (phase_ == Phase::kIdle)
    {
      rex::input::mnk::SetMouseLookActive(!active);
    }
  }

  void MouseLook::GiveBackToMnk()
  {
    rex::input::mnk::SetMouseLookActive(!menu_pointer_.load());
  }

  Turn MouseLook::TakeTurnLocked(double extra_scale)
  {
    const double dx = pending_dx_;
    const double dy = pending_dy_;
    pending_dx_ = pending_dy_ = 0.0;
    return TurnFromCounts(dx, dy, extra_scale);
  }

  Turn MouseLook::TurnFromCounts(double dx, double dy, double extra_scale)
  {
    const double degrees_per_count =
        REXCVAR_GET(pdz_mouse_sensitivity) * kDegreesPerCount * extra_scale;
    const double x_sign = REXCVAR_GET(pdz_mouse_invert_x) ? -1.0 : 1.0;
    // Pointer y grows downward: + = look down.
    const double y_sign = REXCVAR_GET(pdz_mouse_invert_y) ? -1.0 : 1.0;

    const double yaw = dx * degrees_per_count * x_sign;
    const double pitch = dy * degrees_per_count * y_sign;
    if (!std::isfinite(yaw) || !std::isfinite(pitch))
    {
      return Turn{0.0, 0.0};
    }
    return Turn{std::clamp(yaw, -kMaxDegreesPerCall, kMaxDegreesPerCall),
                std::clamp(pitch, -kMaxDegreesPerCall, kMaxDegreesPerCall)};
  }

} // namespace pdz::mouse
