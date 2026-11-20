// locktopus_input.h - the Locktopus on the keyboard and mouse.
#pragma once

namespace rex::ui
{
  class Window;
} // namespace rex::ui

namespace locktopus_input
{
  // Once, after the main window has opened: starts tracking the stick keys.
  void Attach(rex::ui::Window *window);
} // namespace locktopus_input
