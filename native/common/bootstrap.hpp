#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <nessstudio/nessstudio.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace nss_bootstrap {
inline std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid Unicode argument");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
inline std::filesystem::path directory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("Cannot identify executable directory");
    path.resize(length); return std::filesystem::path(path).parent_path();
}
class module final {
    HMODULE value_{};
public:
    explicit module(const std::filesystem::path& library) {
        if (!library.is_absolute()) throw std::runtime_error("Native library path must be absolute");
        if (!std::filesystem::is_regular_file(library)) throw std::runtime_error("libnessstudio.dll is missing from the controlled application directory");
        std::ifstream image(library, std::ios::binary);
        IMAGE_DOS_HEADER dos{}; image.read(reinterpret_cast<char*>(&dos), sizeof(dos));
        if (!image || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) || dos.e_lfanew > 1024 * 1024)
            throw std::runtime_error("Invalid native PE image");
        image.seekg(dos.e_lfanew); DWORD signature{}; IMAGE_FILE_HEADER header{};
        image.read(reinterpret_cast<char*>(&signature), sizeof(signature)); image.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!image || signature != IMAGE_NT_SIGNATURE || header.Machine != IMAGE_FILE_MACHINE_AMD64)
            throw std::runtime_error("Native architecture mismatch: Windows x64 is required");
        value_ = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!value_) throw std::runtime_error("Native load failed; verify the packaged runtime and system dependencies (Win32 " + std::to_string(GetLastError()) + ")");
        try {
            nss_version_v1 version{}; version.size = sizeof(version); version.version = NSS_STRUCT_VERSION;
            auto get_version = get<decltype(&nss_get_version)>("nss_get_version");
            if (get_version(&version) != NSS_OK || version.abi_major != NSS_ABI_MAJOR || version.pointer_bits != 64 || version.contract_revision != NSS_CONTRACT_REVISION)
                throw std::runtime_error("Native ABI/contract revision is incompatible");
        } catch (...) { FreeLibrary(value_); value_ = nullptr; throw; }
    }
    module(const module&) = delete;
    module& operator=(const module&) = delete;
    ~module() { if (value_) FreeLibrary(value_); }
    template<class T> T get(const char* name) const {
        const auto entry = GetProcAddress(value_, name);
        if (!entry) throw std::runtime_error(std::string("Native ABI export missing: ") + name);
        return reinterpret_cast<T>(entry);
    }
};
}
