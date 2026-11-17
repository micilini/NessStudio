#pragma once
#include <cstdint>
#if defined(_M_IX86) || defined(_M_X64)
#include <intrin.h>
#endif

namespace nss::cpu {
struct Capabilities {
  bool sse2{}, avx{}, osxsave{}, avx2{};
  uint64_t xcr0{};
};
inline Capabilities capabilities() {
  static const Capabilities value = [] {
    Capabilities result;
#if defined(_M_IX86) || defined(_M_X64)
    int registers[4]{};
    __cpuidex(registers, 0, 0);
    const int maximum = registers[0];
    if (maximum < 1) return result;
    __cpuidex(registers, 1, 0);
    result.sse2 = (registers[3] & (1 << 26)) != 0;
    result.avx = (registers[2] & (1 << 28)) != 0;
    result.osxsave = (registers[2] & (1 << 27)) != 0;
    // XGETBV itself requires OSXSAVE. CPUID's AVX2 bit alone does not
    // establish that this process can use the XMM/YMM register state.
    if (result.avx && result.osxsave) result.xcr0 = _xgetbv(0);
    if (maximum >= 7) {
      __cpuidex(registers, 7, 0);
      result.avx2 = (registers[1] & (1 << 5)) != 0;
    }
#endif
    return result;
  }();
  return value;
}
inline bool has_avx2(Capabilities value) noexcept {
  return value.sse2 && value.avx && value.osxsave && value.avx2 &&
         (value.xcr0 & 6) == 6;
}
} // namespace nss::cpu
