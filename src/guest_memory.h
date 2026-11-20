// guest_memory.h - big-endian guest memory access shared by the hooks.
#pragma once

#include <bit>
#include <chrono>
#include <cstdint>

#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace guest
{
  inline bool IsPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  // Every page of [address, address + size) is mapped readable (and writable if asked).
  inline bool Readable(uint32_t address, uint32_t size, bool write = false)
  {
    auto *rt = rex::Runtime::instance();
    auto *memory = rt ? rt->memory() : nullptr;
    if (!memory || !IsPointer(address) || uint64_t(address) + size > 0x100000000ull)
    {
      return false;
    }
    const uint32_t required =
        rex::memory::kMemoryProtectRead | (write ? rex::memory::kMemoryProtectWrite : 0u);
    for (uint64_t at = address; at < uint64_t(address) + size;)
    {
      auto *heap = memory->LookupHeap(uint32_t(at));
      uint32_t protect = 0;
      if (!heap || !heap->QueryProtect(uint32_t(at), &protect) || (protect & required) != required)
      {
        return false;
      }
      at = (at / heap->page_size() + 1) * heap->page_size();
    }
    return true;
  }

  inline uint8_t *Base()
  {
    auto *rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
  }

  inline uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  inline void Store32(uint8_t *base, uint32_t address, uint32_t value)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  inline float LoadF(const uint8_t *base, uint32_t address)
  {
    return std::bit_cast<float>(Load32(base, address));
  }

  inline void StoreF(uint8_t *base, uint32_t address, float value)
  {
    Store32(base, address, std::bit_cast<uint32_t>(value));
  }

  inline uint16_t Load16(const uint8_t *base, uint32_t address)
  {
    return uint16_t((uint16_t(base[address]) << 8) | base[address + 1]);
  }

  inline void Store16(uint8_t *base, uint32_t address, uint16_t value)
  {
    base[address] = uint8_t(value >> 8);
    base[address + 1] = uint8_t(value);
  }

  // Checked read: 0 when the address is not mapped.
  inline uint32_t Read32(const uint8_t *base, uint32_t address)
  {
    return Readable(address, 4) ? Load32(base, address) : 0;
  }

  inline int64_t NowNs()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // The game's frame clock block (TU3 82D6BFD8); +120 = the frame's dt in seconds.
  constexpr uint32_t kFrameClock = 0x82D6B2B0;
  constexpr uint32_t kFrameDt = kFrameClock + 120;

  // Local player character class (launch).
  constexpr uint32_t kPlayerVtable = 0x820C93A0;
} // namespace guest
