// cutscene_frame_cap_hook.cpp - 60 FPS cap for cutscenes, scripted cameras and menus.
//
// Author: 2023PerfectDark (https://github.com/2023PerfectDark/Perfect-Dark-Zero-Reinstated),
// ported from PaceFrame() in its mousehook.cpp. TU0 camera addresses there are adapted from
// marinesciencedude/xenia-canary-mousehook (PerfectDarkZero.cc, Copyright 2023 Ben Vanik,
// BSD-3-Clause).
//
// Cutscenes, scripted cameras and the front end advance per frame and play too fast above
// 60 FPS. sub_826CFC10 is the D3D swap (it calls VdSwap) on the render thread, reached every
// frame even when the game stops polling input. Before it, frames are paced to 60 FPS unless
// the local player's gameplay camera is in control; normal gameplay stays unrestricted.
#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "../render/engine/render_hooks.h"
#include "../render/engine/scene_probe.h"

REX_EXTERN(__imp__sub_826CFC10);

namespace
{
  // Local player character and the viewport camera slots.
  constexpr uint32_t kLocalPlayer = 0x82D2AD38;
  constexpr uint32_t kViewportCameras = 0x82D68320;
  constexpr uint32_t kViewportCount = 4;
  // Mission session; its first byte is nonzero while CamSpy has no control.
  constexpr uint32_t kSession = 0x82D6B2B0;

  // Camera: attached character, device object and mode, gameplay mode object (id at +11).
  constexpr uint32_t kCameraCharacter = 8;
  constexpr uint32_t kCameraDevice = 16;
  constexpr uint32_t kCameraDeviceMode = 20;
  constexpr uint32_t kCameraModeObject = 0x53C;
  constexpr uint32_t kModeId = 11;

  // Character: type 2 = player; nonzero +0x16A3 = camera locked by a script.
  constexpr uint32_t kCharacterType = 12;
  constexpr uint32_t kTypePlayer = 2;
  constexpr uint32_t kPlayerSize = 0x16D2;
  constexpr uint32_t kPlayerCameraLocked = 0x16A3;

  // DP_CamSpy (constructor 8252EE80, camera setup 82530FD0).
  constexpr uint32_t kCamSpyVtable = 0x820C1090;
  constexpr uint32_t kCamSpyType = 50;
  constexpr uint32_t kCamSpyOwner = 1536;
  constexpr uint32_t kCamSpyMode = 1560;
  constexpr uint32_t kCamSpyActive = 1571;
  constexpr uint32_t kCamSpySize = 1574;

  constexpr std::chrono::nanoseconds kFrameInterval{16'666'667};

  // Every page is checked before following guest pointers (level transitions free them).
  bool Readable(uint32_t address, uint32_t size)
  {
    auto *rt = rex::Runtime::instance();
    auto *memory = rt ? rt->memory() : nullptr;
    if (!memory || !address || uint64_t(address) + size > 0x100000000ull)
    {
      return false;
    }
    for (uint64_t at = address; at < uint64_t(address) + size;)
    {
      auto *heap = memory->LookupHeap(uint32_t(at));
      uint32_t protect = 0;
      if (!heap || !heap->QueryProtect(uint32_t(at), &protect) ||
          !(protect & rex::memory::kMemoryProtectRead))
      {
        return false;
      }
      at = (at / heap->page_size() + 1) * heap->page_size();
    }
    return true;
  }

  uint32_t Read32(const uint8_t *base, uint32_t address)
  {
    if (!Readable(address, 4))
    {
      return 0;
    }
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  uint8_t Read8(const uint8_t *base, uint32_t address)
  {
    return Readable(address, 1) ? base[address] : 0;
  }

  // CamSpy device driving this camera, or 0.
  uint32_t CamSpy(const uint8_t *base, uint32_t camera)
  {
    if (!camera || !Readable(camera, 24))
    {
      return 0;
    }
    const uint32_t object = Read32(base, camera + kCameraDevice);
    if (!object || !Readable(object, kCamSpySize))
    {
      return 0;
    }
    return Read32(base, object) == kCamSpyVtable &&
                   Read32(base, object + kCharacterType) == kCamSpyType &&
                   Read32(base, object + kCamSpyMode) == Read32(base, camera + kCameraDeviceMode) &&
                   Read8(base, object + kCamSpyActive)
               ? object
               : 0;
  }

  // CamSpy moves the player's view to another viewport slot.
  uint32_t PlayerCamera(const uint8_t *base)
  {
    const uint32_t player = Read32(base, kLocalPlayer);
    for (uint32_t i = 0; i < kViewportCount; ++i)
    {
      const uint32_t camera = Read32(base, kViewportCameras + i * 4);
      const uint32_t camspy = CamSpy(base, camera);
      if (camspy && player && Read32(base, camspy + kCamSpyOwner) == player)
      {
        return camera;
      }
    }
    return Read32(base, kViewportCameras);
  }

  bool IsPlayer(const uint8_t *base, uint32_t character)
  {
    return character && Readable(character, kPlayerSize) &&
           Read32(base, character + kCharacterType) == kTypePlayer;
  }

  uint32_t CameraPlayer(const uint8_t *base, uint32_t camera, uint32_t camspy)
  {
    const uint32_t attached = camera ? Read32(base, camera + kCameraCharacter) : 0;
    if (IsPlayer(base, attached))
    {
      return attached;
    }
    const uint32_t owner = camspy ? Read32(base, camspy + kCamSpyOwner) : 0;
    if (IsPlayer(base, owner))
    {
      return owner;
    }
    const uint32_t player = Read32(base, kLocalPlayer);
    return IsPlayer(base, player) ? player : 0;
  }

  bool GameplayCameraActive(const uint8_t *base)
  {
    const uint32_t camera = PlayerCamera(base);
    const uint32_t camspy = CamSpy(base, camera);
    const uint32_t player = CameraPlayer(base, camera, camspy);
    if (!player)
    {
      return false;
    }
    if (camspy)
    {
      const uint32_t session = Read32(base, kSession);
      return Read32(base, camspy + kCamSpyMode) && Readable(session, 1) &&
             Read8(base, session) == 0;
    }

    const uint32_t mode = Readable(camera, kCameraModeObject + 4)
                              ? Read32(base, camera + kCameraModeObject)
                              : 0;
    if (!mode || !Readable(mode, kModeId + 1))
    {
      return false;
    }
    // Gameplay camera modes; 5 is CamSpy, handled above.
    switch (Read8(base, mode + kModeId))
    {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 6:
      return Read8(base, player + kPlayerCameraLocked) == 0;
    default:
      return false;
    }
  }
} // namespace

REX_HOOK_RAW(sub_826CFC10)
{
  static thread_local std::chrono::steady_clock::time_point previous{};

  const bool gameplay = !base || GameplayCameraActive(base);
  // Player control <-> cutscene/scripted camera is a temporal history cut.
  pdz::engine::scene::NoteGameplayCamera(gameplay);
  if (gameplay)
  {
    previous = {};
  }
  else
  {
    if (previous != std::chrono::steady_clock::time_point{})
    {
      std::this_thread::sleep_until(previous + kFrameInterval);
    }
    previous = std::chrono::steady_clock::now();
  }

  pdz::engine::OnSwap(ctx, base);
}
