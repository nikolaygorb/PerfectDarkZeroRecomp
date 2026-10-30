// Field of view slider, opened in game with bind_fov_settings (F6).
#pragma once

namespace rex::ui
{
  class ImGuiDrawer;
} // namespace rex::ui

namespace fov_settings
{
  // Registers the dialog hotkey (bind_fov_settings, default F6).
  void Init(rex::ui::ImGuiDrawer *drawer);

  bool IsOpen();

  // Closes the dialog. Must run while the ImGui drawer is still alive.
  void Shutdown();
} // namespace fov_settings
