#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pdz::engine {

inline constexpr uint32_t kDeviceBytes = 0x5080;
inline constexpr uint32_t kConstantBegin = 1920;  // VS float constants (big-endian).

inline uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void PutBe32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (24 - 8 * i));
}

// Host-order values of the device state the producer checks.
struct DeviceSnapshot {
  uint32_t tiling = 0;
  std::array<uint32_t, 6> viewport{};  // x, y, width, height, min z, max z.
};
inline bool SnapshotDevice(std::span<const uint8_t> bytes, DeviceSnapshot& out) {
  if (bytes.size() < kDeviceBytes) return false;
  out.tiling = (bytes[10432] >> 2) & 1;
  for (uint32_t i = 0; i < out.viewport.size(); ++i) out.viewport[i] = Be32(bytes.data() + 12808 + 4 * i);
  return true;
}

}  // namespace pdz::engine
