#include "engine.h"

#include "scene_probe.h"

#include <chrono>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_STRING(pdz_engine_report_dir, "", "Render/Engine",
                     "Scene diagnostics parent folder (producer report, SDK probe and frame exports); "
                     "each run creates a subfolder. Empty: no files.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace pdz::engine {
// The XEX is the one the code was generated from (the SDK's xex_image_check),
// so the hooked functions and PDZ's addresses hold.
void Initialize() {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return;
  std::filesystem::path directory;
  if (const auto requested = REXCVAR_GET(pdz_engine_report_dir); !requested.empty()) {
    directory = std::filesystem::path(requested) /
                fmt::format("scene-{}", std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count());
    std::error_code error;
    if (!std::filesystem::create_directories(directory, error) || error) {
      REXLOG_ERROR("PDZ engine: cannot create report directory {}", directory.string());
      directory.clear();
    }
  }
  scene::Initialize(*runtime, directory);
}

void Shutdown() { scene::Shutdown(); }
}  // namespace pdz::engine
