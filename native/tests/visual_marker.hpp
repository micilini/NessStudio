#pragma once
#include <array>
#include <cstdint>

namespace fixture::marker {
inline constexpr const char *name = "gray32-crc16-moving-v1";
inline constexpr std::array<uint32_t, 8> rows{32, 96, 288, 416, 544, 672, 800, 992};
inline constexpr uint16_t checksum(uint32_t number) {
  uint16_t crc = 0xffff;
  for (unsigned byte = 0; byte < 4; ++byte) {
    crc ^= static_cast<uint16_t>(((number >> (byte * 8)) & 255) << 8);
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
  }
  return crc;
}
inline constexpr uint32_t row(uint32_t number) { return rows[(number / 120) % rows.size()]; }
inline constexpr uint8_t level(uint32_t number, uint32_t x) {
  const auto cell = (x / 24) % 80;
  const uint32_t gray = number ^ (number >> 1);
  const bool bit = cell < 64 ? (gray & (1u << (cell % 32))) != 0
                             : (checksum(number) & (1u << (cell - 64))) != 0;
  return bit ? 160 : 96;
}
} // namespace fixture::marker
