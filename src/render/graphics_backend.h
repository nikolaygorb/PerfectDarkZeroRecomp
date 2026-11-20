#pragma once

namespace rex
{
  struct RuntimeConfig;
} // namespace rex

namespace graphics_backend
{
  // Loads the GPU plugin with the backend from graphics_backend, before the
  // SDK's own auto-load (which only ever asks for "any"). Runs in OnPreSetup.
  void Select(rex::RuntimeConfig &config);
} // namespace graphics_backend
