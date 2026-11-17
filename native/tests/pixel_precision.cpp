#include "capture/pixel_guarded_avx2.hpp"
#include "cpu_features.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <vector>
#include <xmmintrin.h>

using namespace nss::capture;
using nss::capture::detail::ScaleTap;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static uint32_t next(uint32_t& state) { return state = state * 1664525U + 1013904223U; }
struct Taps {
  std::vector<ScaleTap> x, y;
  Taps(Rect crop, Rect viewport) {
    if (crop.width == viewport.width && crop.height == viewport.height) return;
    const auto axis = [](int origin, int source, int count) {
      std::vector<ScaleTap> result;
      for (int i = 0; i < count; ++i) {
        const auto value = std::clamp(origin + (double(i) + .5) * source / count - .5,
                                       double(origin), double(origin + source - 1));
        const auto first = static_cast<uint32_t>(value);
        result.push_back({first,(std::min)(first + 1,static_cast<uint32_t>(origin + source - 1)),value - first});
      }
      return result;
    };
    x = axis(crop.x,crop.width,viewport.width); y = axis(crop.y,crop.height,viewport.height);
  }
};
static void compare(BgraView source, Rect crop, Rect viewport, uint32_t width, uint32_t height,
                    size_t alignment, detail::PixelPrecisionStats& stats) {
  Taps taps(crop,viewport);
  const auto bytes = size_t(width) * height * 3 / 2;
  std::vector<uint8_t> reference(bytes,0xa5), guarded(bytes + alignment + 32,0xa5);
  const auto* x = taps.x.empty() ? nullptr : taps.x.data();
  const auto* y = taps.y.empty() ? nullptr : taps.y.data();
  detail::render_pixels_avx2(source,crop,viewport,width,height,reference.data(),viewport,x,y);
  detail::render_pixels_guarded_avx2(source,crop,viewport,width,height,guarded.data()+alignment,viewport,x,y,&stats);
  require(std::equal(reference.begin(),reference.end(),guarded.begin()+alignment),"Float guard failed byte equivalence");
  require(std::all_of(guarded.begin(),guarded.begin()+alignment,[](uint8_t b){return b==0xa5;}) &&
          std::all_of(guarded.begin()+alignment+bytes,guarded.end(),[](uint8_t b){return b==0xa5;}),"Output guard changed");
  // Exercise the exact counter-free production instantiation over disjoint
  // areas. Both two-pixel tails and nonzero tap offsets must match a single
  // full-viewport pass; bytes outside each rectangle stay untouched.
  std::fill(guarded.begin(),guarded.end(),uint8_t{0xa5});
  const auto split_x = (viewport.width / 4) * 2;
  const auto split_y = (viewport.height / 4) * 2;
  for (const auto area : {Rect{viewport.x,viewport.y,split_x,split_y},
                          Rect{viewport.x+split_x,viewport.y,viewport.width-split_x,split_y},
                          Rect{viewport.x,viewport.y+split_y,split_x,viewport.height-split_y},
                          Rect{viewport.x+split_x,viewport.y+split_y,viewport.width-split_x,viewport.height-split_y}}) {
    if (area.width && area.height)
      detail::render_pixels_guarded_avx2(source,crop,viewport,width,height,guarded.data()+alignment,area,x,y);
  }
  require(std::equal(reference.begin(),reference.end(),guarded.begin()+alignment),"Tiled production kernel changed a byte");
  require(std::all_of(guarded.begin(),guarded.begin()+alignment,[](uint8_t b){return b==0xa5;}) &&
          std::all_of(guarded.begin()+alignment+bytes,guarded.end(),[](uint8_t b){return b==0xa5;}),"Tiled kernel overwrote output guards");
}
static Json verify() {
  const auto original = _mm_getcsr();
  struct Restore { unsigned value; ~Restore() { _mm_setcsr(value); } } restore{original};
  uint32_t random = 903717;
  detail::PixelPrecisionStats stats;
  uint64_t cases{};
  for (const auto rounding : {_MM_ROUND_NEAREST,_MM_ROUND_DOWN,_MM_ROUND_UP,_MM_ROUND_TOWARD_ZERO}) {
    _MM_SET_ROUNDING_MODE(rounding);
    for (int i = 0; i < 3000; ++i) {
      const auto sw = 1 + next(random) % 159, sh = 1 + next(random) % 101;
      const auto stride = sw * 4 + next(random) % 31;
      const auto cx = next(random) % sw, cy = next(random) % sh;
      const auto cw = 1 + next(random) % (sw-cx), ch = 1 + next(random) % (sh-cy);
      const auto vw = 2 * (1 + next(random) % 97), vh = 2 * (1 + next(random) % 47);
      const auto pad = 2 * (next(random) % 3);
      const auto alignment = next(random) % 32;
      const auto bytes = size_t(sh-1)*stride + size_t(sw)*4;
      std::vector<uint8_t> pixels(bytes+alignment);
      for (auto& p:pixels) p=static_cast<uint8_t>(next(random)>>24);
      const auto before=pixels;
      compare({pixels.data()+alignment,sw,sh,stride,bytes},
              {int(cx),int(cy),int(cw),int(ch)},{int(pad),int(pad),int(vw),int(vh)},
              vw+2*pad,vh+2*pad,alignment,stats);
      require(pixels==before,"Source pixels changed");
      ++cases;
    }
  }
  _MM_SET_ROUNDING_MODE(_MM_ROUND_NEAREST);
  // Every 24-bit identity color, with a constant-color 2x2 chroma block.
  constexpr uint32_t width=4096;
  std::vector<uint8_t> pixels(size_t(width)*2*4);
  for (uint32_t base=0;base<0x1000000;base+=width/2) {
    for (uint32_t x=0;x<width;++x) {
      const auto color=base+x/2;
      for (uint32_t y=0;y<2;++y) {
        auto* p=pixels.data()+(size_t(y)*width+x)*4;
        p[0]=uint8_t(color);p[1]=uint8_t(color>>8);p[2]=uint8_t(color>>16);p[3]=uint8_t(color>>4);
      }
    }
    compare({pixels.data(),width,2,width*4,pixels.size()},{0,0,int(width),2},{0,0,int(width),2},width,2,7,stats);
  }
  require(stats.precise_blocks>0 && stats.precise_blocks<stats.blocks,"Quantization guard was not exercised selectively");
  return {{"status","PASS"},{"randomCases",cases},{"exhaustiveIdentityColors",0x1000000},
          {"roundingModes",4},{"eightPixelBlocks",stats.blocks},{"preciseFallbackBlocks",stats.precise_blocks},
          {"byteExact",true},{"productionKernel",true},{"disjointAreas",true},{"counterFreePath",true}};
}
static Json benchmark() {
  const std::array<std::array<uint32_t,6>,3> shapes{{{1280,720,1280,720,1280,720},
      {1920,1200,1280,720,1152,720},{1920,1200,1920,1080,1728,1080}}};
  uint32_t random=771;
  Json rows=Json::array();
  for (const auto& shape:shapes) {
    const auto sw=shape[0],sh=shape[1],width=shape[2],height=shape[3],vw=shape[4],vh=shape[5];
    std::vector<uint8_t> input(size_t(sw)*sh*4),reference(size_t(width)*height*3/2),candidate(reference.size());
    for(auto& value:input)value=uint8_t(next(random)>>24);
    const BgraView source{input.data(),sw,sh,sw*4,input.size()};
    const Rect crop{0,0,int(sw),int(sh)},viewport{int((width-vw)/2),int((height-vh)/2),int(vw),int(vh)};
    Taps taps(crop,viewport);
    const auto* x=taps.x.empty()?nullptr:taps.x.data();const auto* y=taps.y.empty()?nullptr:taps.y.data();
    Json pairs=Json::array();detail::PixelPrecisionStats stats;
    for(int pair=-3;pair<20;++pair) {
      std::array<double,2> elapsed{};
      for(int at=0;at<2;++at) {
        const auto index=(pair&1)?1-at:at;
        const auto start=std::chrono::steady_clock::now();
        if(index)detail::render_pixels_guarded_avx2(source,crop,viewport,width,height,candidate.data(),viewport,x,y,&stats);
        else detail::render_pixels_avx2(source,crop,viewport,width,height,reference.data(),viewport,x,y);
        elapsed[size_t(index)]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      }
      require(reference==candidate,"Benchmark pixels differ");
      if(pair>=0)pairs.push_back({{"doubleMs",elapsed[0]},{"guardedFloatMs",elapsed[1]}});
    }
    rows.push_back({{"sourceWidth",sw},{"sourceHeight",sh},{"outputWidth",width},{"outputHeight",height},
                    {"pairs",pairs},{"blocks",stats.blocks},{"preciseFallbackBlocks",stats.precise_blocks}});
  }
  return {{"status","PASS"},{"scope","Isolated production pixel kernel precision"},{"cases",rows},{"byteExact",true}};
}
int main(int argc,char** argv) {
  try {
    if(!nss::cpu::has_avx2(nss::cpu::capabilities())) {std::cout<<"{\"status\":\"UNAVAILABLE\"}\n";return 77;}
    const std::string mode=argc>1?argv[1]:"verify";
    require(mode=="verify" || mode=="benchmark","Unknown experiment mode");
    std::cout<<(mode=="verify"?verify():benchmark()).dump()<<'\n';return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
