// game_constants.h - Game-specific constants
#pragma once

#include <cstdint>

namespace GameConstants
{
  constexpr uint32_t kCodeBase = 0x82140000;
  constexpr uint32_t kCodeEnd = 0x82CE62AC;
}

namespace GameConstants::PatchConstants
{
  struct Patch
  {
    std::uintptr_t address;
    std::uint32_t value;
  };

  constexpr Patch Fps60()
  {
    return Patch{
        0x826CFB77,
        0x01};
  }

  constexpr Patch AspectRatio()
  {
    return Patch{
        0x820EC158,
        0x4018E38E};
  }
}