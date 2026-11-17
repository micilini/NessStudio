#include "dshow_types.hpp"
#include <dxva.h>
#include <cstring>
#include <array>
#include <cstdlib>
#include <numeric>
#include <utility>

namespace nss::camera::detail {
void checked(HRESULT value,const char *operation) {
  capture::checked(value,NSS_STAGE_CAPTURE,operation);
}
ComRuntime::ComRuntime() {
  checked(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"DirectShow COM MTA initialization failed");
}
ComRuntime::~ComRuntime() {CoUninitialize();}
void MediaType::clear() noexcept {
  CoTaskMemFree(value.pbFormat);
  if(value.pUnk) value.pUnk->Release();
  value={};
}
HRESULT MediaType::copy_to(AM_MEDIA_TYPE *out) const noexcept {
  if(!out) return E_POINTER;
  *out={};
  if(value.cbFormat>65536 || (value.cbFormat && !value.pbFormat)) return VFW_E_INVALIDMEDIATYPE;
  auto copy=value;
  copy.pbFormat=nullptr;
  if(value.cbFormat) {
    copy.pbFormat=static_cast<BYTE *>(CoTaskMemAlloc(value.cbFormat));
    if(!copy.pbFormat) return E_OUTOFMEMORY;
    std::memcpy(copy.pbFormat,value.pbFormat,value.cbFormat);
  }
  if(copy.pUnk) copy.pUnk->AddRef();
  *out=copy;return S_OK;
}
MediaType::MediaType(const AM_MEDIA_TYPE &input) {
  MediaType borrowed;borrowed.value=input;
  const auto result=borrowed.copy_to(&value);borrowed.value={};
  checked(result,"Cannot retain DirectShow media type");
}
MediaType::MediaType(MediaType &&other) noexcept:value(std::exchange(other.value,{})) {}
MediaType &MediaType::operator=(MediaType &&other) noexcept {
  if(this!=&other) {clear();value=std::exchange(other.value,{});}return *this;
}
MediaType::~MediaType() {clear();}
void free_type(AM_MEDIA_TYPE *type) noexcept {
  if(!type) return;
  MediaType owner;owner.value=*type;CoTaskMemFree(type);
}
namespace {
[[noreturn]] void invalid(const char *reason) {
  throw capture::Error(NSS_E_UNAVAILABLE,NSS_STAGE_CAPTURE,VFW_E_INVALIDMEDIATYPE,reason);
}
}
VideoFormat video_format(const AM_MEDIA_TYPE &type) {
  if(type.majortype!=MEDIATYPE_Video || !type.pbFormat || type.cbFormat>65536)
    invalid("DirectShow camera requires a bounded video media type");
  BITMAPINFOHEADER bitmap{};RECT crop{},target{};REFERENCE_TIME interval{};
  DWORD controls{},interlace{};
  if(type.formattype==FORMAT_VideoInfo && type.cbFormat>=sizeof(VIDEOINFOHEADER)) {
    VIDEOINFOHEADER info{};std::memcpy(&info,type.pbFormat,sizeof(info));
    bitmap=info.bmiHeader;crop=info.rcSource;target=info.rcTarget;interval=info.AvgTimePerFrame;
  } else if(type.formattype==FORMAT_VideoInfo2 && type.cbFormat>=sizeof(VIDEOINFOHEADER2)) {
    VIDEOINFOHEADER2 info{};std::memcpy(&info,type.pbFormat,sizeof(info));
    bitmap=info.bmiHeader;crop=info.rcSource;target=info.rcTarget;interval=info.AvgTimePerFrame;
    controls=info.dwControlFlags;interlace=info.dwInterlaceFlags;
  } else invalid("Camera requires VIDEOINFOHEADER or VIDEOINFOHEADER2");
  const auto bitmap_offset=type.formattype==FORMAT_VideoInfo?offsetof(VIDEOINFOHEADER,bmiHeader):offsetof(VIDEOINFOHEADER2,bmiHeader);
  if(bitmap.biSize<sizeof(BITMAPINFOHEADER) || bitmap.biSize>type.cbFormat-bitmap_offset ||
     bitmap.biPlanes!=1 || bitmap.biWidth<=0 ||
     bitmap.biWidth>1920 || bitmap.biHeight==0 || bitmap.biHeight<-1080 || bitmap.biHeight>1080 ||
     (bitmap.biWidth&1) || (bitmap.biHeight&1) || interval<166666 || interval>100000000 ||
     (interlace&AMINTERLACE_IsInterlaced))
    invalid("Camera geometry/cadence or interlaced input is unsupported");
  VideoFormat f;f.width=static_cast<uint32_t>(bitmap.biWidth);
  f.height=static_cast<uint32_t>(std::abs(bitmap.biHeight));f.interval=interval;
  auto full_rect=[&](const RECT &r) {return (!r.left && !r.top && !r.right && !r.bottom) ||
    (r.left==0 && r.top==0 && r.right==bitmap.biWidth && r.bottom==static_cast<LONG>(f.height));};
  if(!full_rect(crop) || !full_rect(target)) invalid("Camera driver requested an unsupported implicit crop");
  if(type.subtype==MEDIASUBTYPE_RGB32 && bitmap.biBitCount==32 && bitmap.biCompression==BI_RGB) {
    f.pixels=Pixels::Bgra;f.stride=f.width*4;f.bottom_up=bitmap.biHeight>0;
  } else if(type.subtype==MEDIASUBTYPE_YUY2 && bitmap.biBitCount==16 && bitmap.biCompression==MEDIASUBTYPE_YUY2.Data1) {
    f.pixels=Pixels::Yuy2;f.stride=f.width*2;
  } else if(type.subtype==MEDIASUBTYPE_NV12 && bitmap.biBitCount==12 && bitmap.biCompression==MEDIASUBTYPE_NV12.Data1) {
    f.pixels=Pixels::Nv12;f.stride=f.width;
  } else if(type.subtype==MEDIASUBTYPE_MJPG && bitmap.biCompression==MEDIASUBTYPE_MJPG.Data1) f.pixels=Pixels::Mjpg;
  else invalid("Camera subtype/bit depth is unsupported");
  if(f.pixels==Pixels::Mjpg) {f.minimum_bytes=4;f.maximum_bytes=f.width*f.height*4+65536;}
  else {
    const auto rows=f.pixels==Pixels::Nv12?f.height*3/2:f.height;
    // For uncompressed VIDEOINFO the bitmap width defines the pitch. A driver
    // needing another pitch must advertise that width; trailing allocation
    // padding is tolerated, but never interpreted as visible pixels.
    f.minimum_bytes=f.stride*rows;
    f.maximum_bytes=std::max(f.minimum_bytes,static_cast<uint32_t>(bitmap.biSizeImage));
    if(f.maximum_bytes>f.minimum_bytes+65536) invalid("Camera sample allocation exceeds its negotiated budget");
  }
  if((controls&(AMCONTROL_USED|AMCONTROL_COLORINFO_PRESENT))==(AMCONTROL_USED|AMCONTROL_COLORINFO_PRESENT)) {
    DXVA_ExtendedFormat color{};std::memcpy(&color,&controls,sizeof(color));
    if(color.VideoTransferMatrix==DXVA_VideoTransferMatrix_BT709) f.bt709=true;
    else if(color.VideoTransferMatrix!=DXVA_VideoTransferMatrix_Unknown &&
            color.VideoTransferMatrix!=DXVA_VideoTransferMatrix_BT601)
      invalid("Camera YUV transfer matrix is unsupported");
    if(color.NominalRange==DXVA_NominalRange_0_255) f.full_range=true;
    else if(color.NominalRange!=DXVA_NominalRange_Unknown && color.NominalRange!=DXVA_NominalRange_16_235)
      invalid("Camera nominal range is unsupported");
    f.color_declared=color.VideoTransferMatrix!=DXVA_VideoTransferMatrix_Unknown &&
      color.NominalRange!=DXVA_NominalRange_Unknown;
  }
  return f;
}
const char *subtype_name(Pixels value) noexcept {
  switch(value) {case Pixels::Bgra:return "RGB32";case Pixels::Yuy2:return "YUY2";
    case Pixels::Nv12:return "NV12";case Pixels::Mjpg:return "MJPG";}
  return "Unknown";
}
Json format_json(const VideoFormat &f,uint32_t index) {
  uint64_t num=10000000,den=static_cast<uint64_t>(f.interval);
  // HNS quantization is at most one tick for the common rational cadences.
  for(const auto ratio:std::array<std::pair<uint32_t,uint32_t>,8>{{{60,1},{60000,1001},{30,1},{30000,1001},{25,1},{24,1},{15,1},{5,1}}})
    if(std::abs(f.interval-static_cast<int64_t>(10000000ULL*ratio.second/ratio.first))<=1) {
      num=ratio.first;den=ratio.second;break;
    }
  const auto gcd=std::gcd(num,den);
  return {{"index",index},{"subtype",subtype_name(f.pixels)},{"width",f.width},{"height",f.height},
    {"fpsNum",num/gcd},{"fpsDen",den/gcd},{"intervalHns",f.interval},{"supportedInput",true},
    {"memory","DirectShow CPU samples"},{"stride",f.stride},{"bottomUp",f.bottom_up},
    {"yuvMatrix",f.bt709?"BT709":"BT601"},{"range",f.full_range?"full":"limited"},
    {"colorDeclaredByDriver",f.color_declared},{"unspecifiedYuvColorPolicy","BT601 limited"}};
}
void set_interval(AM_MEDIA_TYPE &type,int64_t interval) {
  (void)video_format(type);
  if(type.formattype==FORMAT_VideoInfo)
    reinterpret_cast<VIDEOINFOHEADER *>(type.pbFormat)->AvgTimePerFrame=interval;
  else reinterpret_cast<VIDEOINFOHEADER2 *>(type.pbFormat)->AvgTimePerFrame=interval;
}
bool same_format(const VideoFormat &a,const VideoFormat &b) noexcept {
  return a.pixels==b.pixels && a.width==b.width && a.height==b.height && a.stride==b.stride &&
    std::abs(a.interval-b.interval)<=1 && a.bottom_up==b.bottom_up && a.bt709==b.bt709 && a.full_range==b.full_range;
}
void convert_raw(const VideoFormat &f,std::span<const std::byte> input,std::span<std::byte> output) {
  if(f.pixels==Pixels::Mjpg || input.size()<f.minimum_bytes || input.size()>f.maximum_bytes ||
     output.size()<uint64_t(f.width)*f.height*4)
    throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,VFW_E_BUFFER_UNDERFLOW,"Camera pixel buffer violates its negotiated layout");
  const auto *src=reinterpret_cast<const uint8_t *>(input.data());
  auto *dst=reinterpret_cast<uint8_t *>(output.data());
  auto pixel=[&](uint8_t *d,int y,int u,int v) {
    const int c=f.full_range?y:std::max(0,y-16),cb=u-128,cr=v-128;
    const int yy=c*(f.full_range?256:298);
    const int r=(yy+(f.bt709?(f.full_range?403:459):(f.full_range?359:409))*cr+128)>>8;
    const int g=(yy-(f.bt709?(f.full_range?48:55):(f.full_range?88:100))*cb-
      (f.bt709?(f.full_range?120:136):(f.full_range?183:208))*cr+128)>>8;
    const int b=(yy+(f.bt709?(f.full_range?475:541):(f.full_range?454:516))*cb+128)>>8;
    d[0]=static_cast<uint8_t>(std::clamp(b,0,255));d[1]=static_cast<uint8_t>(std::clamp(g,0,255));
    d[2]=static_cast<uint8_t>(std::clamp(r,0,255));d[3]=255;
  };
  for(uint32_t row=0;row<f.height;++row) {
    auto *d=dst+size_t(row)*f.width*4;
    const auto *s=src+size_t(f.bottom_up?f.height-1-row:row)*f.stride;
    if(f.pixels==Pixels::Bgra) {
      std::memcpy(d,s,size_t(f.width)*4);
      for(uint32_t x=0;x<f.width;++x) d[x*4+3]=255;
    } else if(f.pixels==Pixels::Yuy2) {
      for(uint32_t x=0;x<f.width;x+=2) {pixel(d+x*4,s[x*2],s[x*2+1],s[x*2+3]);pixel(d+(x+1)*4,s[x*2+2],s[x*2+1],s[x*2+3]);}
    } else {
      const auto *uv=src+size_t(f.stride)*f.height+size_t(row/2)*f.stride;
      for(uint32_t x=0;x<f.width;++x) pixel(d+x*4,s[x],uv[x&~1u],uv[(x&~1u)+1]);
    }
  }
}
std::string utf8(std::wstring_view value) {
  if(value.empty()) return {};
  const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
  if(!size) checked(HRESULT_FROM_WIN32(GetLastError()),"Camera UTF8 conversion failed");
  std::string result(static_cast<size_t>(size),'\0');
  WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr);return result;
}
std::wstring wide(std::string_view value) {
  if(value.empty()) return {};
  const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
  if(!size) checked(HRESULT_FROM_WIN32(GetLastError()),"Camera device UTF8 identity is invalid");
  std::wstring result(static_cast<size_t>(size),L'\0');
  MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size);return result;
}
namespace {
constexpr std::wstring_view direct_suffix=L"#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global";
constexpr std::wstring_view mf_suffix=L"#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global";
bool equal_id(std::wstring_view a,std::wstring_view b) {
  return CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;
}
bool usb_interface(std::wstring_view value,std::wstring_view suffix) {
  constexpr std::wstring_view prefix=L"\\\\?\\usb#";
  return value.size()>prefix.size()+suffix.size() && equal_id(value.substr(0,prefix.size()),prefix) &&
    equal_id(value.substr(value.size()-suffix.size()),suffix);
}
std::wstring canonical_id(std::string_view value) {
  auto text=wide(value);
  if(usb_interface(text,direct_suffix) || usb_interface(text,mf_suffix)) {
    text.resize(text.size()-direct_suffix.size());text.append(L"#{normalized-camera-interface}");
  }
  return text;
}
}
bool same_device(std::string_view a,std::string_view b) {
  // Restrict compatibility to the two documented interface categories of
  // the same complete USB instance (including MI/function and instance ID).
  const auto left=canonical_id(a),right=canonical_id(b);
  return CompareStringOrdinal(left.data(),static_cast<int>(left.size()),right.data(),static_cast<int>(right.size()),TRUE)==CSTR_EQUAL;
}
Json identity_aliases(std::string_view value) {
  auto text=wide(value);Json aliases=Json::array();
  if(usb_interface(text,direct_suffix)) {
    text.resize(text.size()-direct_suffix.size());text.append(mf_suffix);aliases.push_back(utf8(text));
  }
  return aliases;
}
} // namespace nss::camera::detail
