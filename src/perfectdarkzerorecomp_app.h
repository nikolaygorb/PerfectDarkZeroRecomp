// perfectdarkzerorecomp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/gpu_plugin.h>

namespace GameConstants
{
  constexpr uint32_t kCodeBase = 0x82000000;
  constexpr uint32_t kCodeEnd = 0x82E6C540;
}

REXCVAR_DEFINE_STRING(graphics_backend, "any", "GPU",
                      "Graphics API backend: any, d3d12, vulkan")
    .allowed({"any", "d3d12", "vulkan"});

// Experimental ports of the community xenia-canary "60 FPS" / "Aspect Ratio"
// Both off by default to match upstream.
REXCVAR_DEFINE_BOOL(pdz_fps60_unlock, false, "Gameplay",
                    "Experimental: unlock 60 FPS update rate (ported from xenia-canary "
                    "game-patches, may affect physics/animation timing)");
REXCVAR_DEFINE_BOOL(pdz_aspect_ratio_16_9, false, "Gameplay",
                    "Experimental: force a 16:9 aspect ratio constant (ported from "
                    "xenia-canary game-patches)");

class PerfectdarkzerorecompApp : public rex::ReXApp
{
public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext &ctx)
  {
    return std::unique_ptr<PerfectdarkzerorecompApp>(new PerfectdarkzerorecompApp(ctx, "perfectdarkzerorecomp",
                                                                                  PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  void OnPreSetup(rex::RuntimeConfig &config) override
  {
    std::string backend = REXCVAR_GET(graphics_backend);
    if (backend != "any" && !config.gpu_plugin.empty())
    {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      if (!config.graphics)
      {
        REXLOG_WARN("graphics_backend '{}' unavailable, falling back to automatic selection",
                    backend);
      }
    }
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  void OnPostLoadXexImage() override
  {
    if (!REXCVAR_GET(pdz_aspect_ratio_16_9))
    {
      return;
    }
    auto *rt = rex::Runtime::instance();
    uint8_t *base = rt ? rt->virtual_membase() : nullptr;
    if (!base)
    {
      return;
    }
    constexpr uint32_t kAspectRatioAddr = 0x820EC158;
    constexpr uint32_t kAspectRatioValue = 0x4018E38E;
    uint8_t *p = base + kAspectRatioAddr;
    // The constant lives in a read-only-mapped section (write access
    // violation without this) - unprotect just long enough to patch it,
    // then restore whatever access it had before.
    rex::memory::PageAccess old_access{};
    rex::memory::Protect(p, 4, rex::memory::PageAccess::kReadWrite, &old_access);
    p[0] = static_cast<uint8_t>(kAspectRatioValue >> 24);
    p[1] = static_cast<uint8_t>(kAspectRatioValue >> 16);
    p[2] = static_cast<uint8_t>(kAspectRatioValue >> 8);
    p[3] = static_cast<uint8_t>(kAspectRatioValue);
    rex::memory::Protect(p, 4, old_access, nullptr);
  }
  void OnPostSetup() override
  {
    LoadSettingsFiles();

    if (const char *e = std::getenv("PDZ_NO_STUB_SWEEP"); e && *e == '1')
    {
      return;
    }
    PerformStubSweep();
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  void OnConfigurePaths(rex::PathConfig &paths) override
  {
    // --game_data_root / REX_GAME_DATA_ROOT still win: this cvar is read
    // before any config file loads, so it's already non-empty here if set.
    if (paths.game_data_root.empty())
    {
      paths.game_data_root = RepoRoot() / "assets";
    }

    // Hardware/rendering settings become the SDK's primary config file, so
    // the in-game Settings overlay's "Save to config" also lands here.
    paths.config_path = SettingsDir() / "hardware.toml";

    // Input mapping lives in its own file, loaded manually below (the SDK
    // only auto-loads the single path above).
    LoadSettingsFiles();
  }

private:
  static std::filesystem::path RepoRoot()
  {
    namespace fs = std::filesystem;
    fs::path dir = rex::filesystem::GetExecutableFolder();
    for (int i = 0; i < 6 && !dir.empty() && dir.has_parent_path(); ++i)
    {
      if (fs::exists(dir / "perfectdarkzerorecomp_manifest.toml") || fs::exists(dir / "assets"))
      {
        break;
      }
      dir = dir.parent_path();
    }
    return dir;
  }

  static std::filesystem::path SettingsDir() { return RepoRoot() / "settings"; }

  static void LoadSettingsFiles()
  {
    namespace fs = std::filesystem;
    fs::path dir = SettingsDir();
    for (const char *file : {"hardware.toml", "mapping.toml"})
    {
      if (fs::path p = dir / file; fs::exists(p))
      {
        rex::cvar::LoadConfig(p);
      }
    }
  }

  void PerformStubSweep()
  {
    auto *rt = rex::Runtime::instance();
    auto *fd = rt ? rt->function_dispatcher() : nullptr;
    uint8_t *base = rt ? rt->virtual_membase() : nullptr;
    if (!fd || !base)
    {
      return;
    }

    static FILE *stub_log = std::fopen("logs/stub_sweep.txt", "w");
    static std::mutex stub_mutex;
    static std::unordered_map<uint32_t, uint32_t> stub_hits;

    static PPCFunc *stub = [](PPCContext &ctx, uint8_t *) noexcept
    {
      uint32_t addr = ctx.ctr.u32;
      uint32_t lr = ctx.lr;
      std::lock_guard<std::mutex> lock(stub_mutex);
      uint32_t &count = stub_hits[addr];
      if (count == 0 && stub_log)
      {
        std::fprintf(stub_log,
                     "[stub] addr=0x%08X LR=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                     addr, lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
        std::fflush(stub_log);
      }
      ++count;
    };

    uint32_t stubbed = 0;
    for (uint32_t addr = GameConstants::kCodeBase; addr < GameConstants::kCodeEnd; addr += 4)
    {
      if (!fd->GetFunction(addr))
      {
        fd->SetFunction(addr, stub);
        ++stubbed;
      }
    }
    if (stub_log)
    {
      std::fprintf(stub_log, "=== stub sweep: scanned %u addresses, stubbed %u ===\n",
                   (GameConstants::kCodeEnd - GameConstants::kCodeBase) / 4, stubbed);
      std::fflush(stub_log);
    }
  }
};
