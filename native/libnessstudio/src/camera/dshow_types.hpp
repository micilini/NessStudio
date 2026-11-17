#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "camera.hpp"
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>

namespace nss::camera::detail {
using Microsoft::WRL::ComPtr;
void checked(HRESULT value,const char *operation);
struct ComRuntime {
  ComRuntime();
  ~ComRuntime();
  ComRuntime(const ComRuntime &)=delete;
  ComRuntime &operator=(const ComRuntime &)=delete;
};
struct MediaType final {
  AM_MEDIA_TYPE value{};
  MediaType()=default;
  explicit MediaType(const AM_MEDIA_TYPE &);
  MediaType(MediaType &&) noexcept;
  MediaType &operator=(MediaType &&) noexcept;
  MediaType(const MediaType &)=delete;
  MediaType &operator=(const MediaType &)=delete;
  ~MediaType();
  void clear() noexcept;
  HRESULT copy_to(AM_MEDIA_TYPE *) const noexcept;
};
void free_type(AM_MEDIA_TYPE *) noexcept;
enum class Pixels { Bgra,Yuy2,Nv12,Mjpg };
struct VideoFormat {
  Pixels pixels{};
  uint32_t width{},height{},stride{},minimum_bytes{},maximum_bytes{};
  int64_t interval{};
  bool bottom_up{},bt709{},full_range{},color_declared{};
};
VideoFormat video_format(const AM_MEDIA_TYPE &);
const char *subtype_name(Pixels) noexcept;
Json format_json(const VideoFormat &,uint32_t index=0);
void set_interval(AM_MEDIA_TYPE &,int64_t interval);
bool same_format(const VideoFormat &,const VideoFormat &) noexcept;
void convert_raw(const VideoFormat &,std::span<const std::byte>,std::span<std::byte>);
std::string utf8(std::wstring_view);
std::wstring wide(std::string_view);
bool same_device(std::string_view,std::string_view);
Json identity_aliases(std::string_view);
} // namespace nss::camera::detail
