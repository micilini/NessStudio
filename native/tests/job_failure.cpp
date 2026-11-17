#define WIN32_LEAN_AND_MEAN
#include <array>
#include <windows.h>
// Negative child-process fixture only. Never linked into or staged with the
// product.
int main() {
  std::array<unsigned char, 4096> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<unsigned char>(i % 2 ? 0xff : 0xf0);
  for (int i = 0; i < 128; ++i) {
    DWORD written{};
    if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), bytes.data(),
                   static_cast<DWORD>(bytes.size()), &written, nullptr))
      return 2;
  }
  return 1;
}
