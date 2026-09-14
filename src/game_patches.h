// game_patches.h - Game-specific memory patches
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>

#include "game_cvars.h"
#include "game_constants.h"

namespace game_patches
{
  // Ported from xenia-canary game-patches for Perfect Dark Zero (4D5307D3)
  // https://github.com/xenia-canary/game-patches/blob/main/patches/4D5307D3%20-%20Perfect%20Dark%20Zero.patch.toml
  static void Fps60()
  {
    if (!REXCVAR_GET(pdz_fps60_unlock))
    {
      return;
    }

    auto *rt = rex::Runtime::instance();
    uint8_t *base = rt ? rt->virtual_membase() : nullptr;
    if (!base)
    {
      return;
    }
    auto memoryAddr = GameConstants::PatchConstants::Fps60().address;
    auto memoryValue =  GameConstants::PatchConstants::Fps60().value;
    uint8_t *p = base + memoryAddr;

    // The byte lives in a read-only-mapped section (write access violation
    // without this) - unprotect just long enough to patch it, then restore
    // whatever access it had before.
    rex::memory::PageAccess old_access{};
    rex::memory::Protect(p, 1, rex::memory::PageAccess::kReadWrite, &old_access);
    p[0] = memoryValue;
    rex::memory::Protect(p, 1, old_access, nullptr);
  }

  static void AspectRatio16_9()
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
    auto memoryAddr = GameConstants::PatchConstants::AspectRatio().address;
    auto memoryValue =  GameConstants::PatchConstants::AspectRatio().value;
    uint8_t *p = base + memoryAddr;

    // This is a be32 patch (4-byte big-endian float constant, guest is PPC) -
    // must write all 4 bytes in big-endian order, not just the low byte.
    rex::memory::PageAccess old_access{};
    rex::memory::Protect(p, sizeof(memoryValue), rex::memory::PageAccess::kReadWrite, &old_access);
    p[0] = static_cast<uint8_t>(memoryValue >> 24);
    p[1] = static_cast<uint8_t>(memoryValue >> 16);
    p[2] = static_cast<uint8_t>(memoryValue >> 8);
    p[3] = static_cast<uint8_t>(memoryValue);
    rex::memory::Protect(p, sizeof(memoryValue), old_access, nullptr);
  }
} // namespace game_patches
