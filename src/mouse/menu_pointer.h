// menu_pointer.h - the mouse in the game's menus: point, click, wheel.
#pragma once

namespace rex::ui
{
  class Window;
} // namespace rex::ui

namespace menu_pointer
{
  // Once, after the main window has opened.
  void Attach(rex::ui::Window *window);
} // namespace menu_pointer
