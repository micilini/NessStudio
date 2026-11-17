#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "runtime/encoder_selection.hpp"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "json.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
template <class T> T fresh() { T v{}; v.size=sizeof(v); v.version=2; return v; }
void check(bool condition,const char *message) { if(!condition) throw std::runtime_error(message); }
int main(int argc,char **argv) {
  try {
    const bool absent=argc==2 && std::string_view(argv[1])=="--expect-no-x264";
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
      D3D11_SDK_VERSION,&device,nullptr,&context)),"Cannot create WARP test device");
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    check(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter->GetDesc(&desc)),"Cannot identify WARP device");
    auto gpu=fresh<ness_gpu_device_v2>();gpu.device=device.Get();
    gpu.adapter_luid=(uint64_t(static_cast<uint32_t>(desc.AdapterLuid.HighPart))<<32)|desc.AdapterLuid.LowPart;
    auto make_config=[] {
      auto c=fresh<ness_encoder_config_v2>();c.policy=NESS_V2_POLICY_REQUIRE_GPU;
      c.width=640;c.height=360;c.fps_num=30;c.fps_den=1;c.bitrate_bps=2000000;
      c.profile=NESS_V2_PROFILE_BASELINE;c.color=NESS_V2_BT709_LIMITED;c.rate_control=NESS_V2_CBR;
      c.max_pending_frames=4;c.max_packet_bytes=1024*1024;return c;
    };
    Json cases=Json::array();
    for(unsigned mode=0;mode<4;++mode) {
      auto c=make_config();auto error=fresh<ness_error_v2>();ness_encoder_v2 *raw{};
      std::vector<uint32_t> attempts;Json details=Json::array();
      if(mode==2) c.backend=NESS_V2_BACKEND_NVENC_D3D11;
      if(mode==3) c.width=0;
      const auto result=nss::select_encoder(c,&gpu,mode!=2,mode!=1,&raw,error,
        [&](uint32_t backend,int32_t code,const ness_error_v2 &e) {
          attempts.push_back(backend);details.push_back({{"backend",backend},{"code",code},
            {"reason",reinterpret_cast<const char *>(e.message_utf8)}});
        });
      std::unique_ptr<ness_encoder_v2,decltype(&ness_encoder_release_v2)> encoder(raw,ness_encoder_release_v2);
      if(mode==0) {
        check(attempts==std::vector<uint32_t>{NESS_V2_BACKEND_QSV_D3D11,NESS_V2_BACKEND_NVENC_D3D11,
          NESS_V2_BACKEND_AMF_D3D11,NESS_V2_BACKEND_X264},"Automatic capability probe order/fallback is wrong");
        check(c.policy==NESS_V2_POLICY_SOFTWARE,"Fallback did not select software input");
        if(absent) check(result==NESS_V2_E_UNAVAILABLE && !raw,"Missing x264 was not reported unavailable");
        else {
          check(result==NESS_V2_OK && raw,"Automatic x264 fallback failed");
          auto info=fresh<ness_encoder_info_v2>();
          check(ness_encoder_info_get_v2(raw,&info,&error)==NESS_V2_OK && info.backend==NESS_V2_BACKEND_X264 &&
            info.software && !info.gpu_input,"Fallback misreported its effective implementation");
        }
      } else if(mode==1) {
        check(result==NESS_V2_E_UNAVAILABLE && !raw && attempts.size()==3 &&
          c.policy==NESS_V2_POLICY_REQUIRE_GPU,"Strict hardware policy silently used software");
      } else if(mode==2) {
        check(result==NESS_V2_E_UNAVAILABLE && !raw && attempts==std::vector<uint32_t>{NESS_V2_BACKEND_NVENC_D3D11},
          "Explicit backend silently selected an alternative");
      } else check(result==NESS_V2_E_ARGUMENT && !raw && attempts.size()==1,
        "Malformed configuration was hidden by capability fallback");
      encoder.reset();
      auto stats=fresh<ness_runtime_stats_v2>();
      check(ness_runtime_stats_get_v2(&stats,&error)==NESS_V2_OK && !stats.encoders && !stats.packets && !stats.codecs,
        "Encoder selection leaked native resources");
      cases.push_back({{"case",mode},{"result",result},{"attempts",details},{"status","PASS"}});
    }
    std::cout<<Json{{"status","PASS"},{"cases",cases},{"physicalHardwareEncoding",false},
      {"x264AbsentExpected",absent}}.dump(2)<<'\n';return 0;
  } catch(const std::exception &e) {
    std::cerr<<Json{{"status","FAIL"},{"error",e.what()}}.dump()<<'\n';return 1;
  }
}
