#define NOMINMAX
#include "audio/audio.hpp"
#include "audio/sinc_kernel.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <windows.h>

using namespace nss::audio;
static void check(bool good, const char *message) { if (!good) throw std::runtime_error(message); }
static std::vector<dsp::Isa> available() {
  std::vector<dsp::Isa> result{dsp::Isa::Scalar};
  const auto caps = dsp::capabilities();
  if (caps.sse2) result.push_back(dsp::Isa::Sse2);
  if (dsp::select(caps) == dsp::Isa::Avx2) result.push_back(dsp::Isa::Avx2);
  return result;
}
static nss::Json qualify() {
  check(dsp::select({}) == dsp::Isa::Scalar, "Unavailable vector ISA did not select scalar");
  check(dsp::select({false,true,true,true,6}) == dsp::Isa::Scalar, "Invalid capabilities selected vector code");
  check(dsp::select({true,false,true,true,6}) == dsp::Isa::Sse2, "AVX2 selected without AVX");
  check(dsp::select({true,true,false,true,6}) == dsp::Isa::Sse2, "AVX2 selected without OSXSAVE");
  check(dsp::select({true,true,true,false,6}) == dsp::Isa::Sse2, "AVX2 selected without its CPU bit");
  for (uint64_t xcr0 : {0u,1u,2u,3u,4u,5u})
    check(dsp::select({true,true,true,true,xcr0}) == dsp::Isa::Sse2, "AVX2 selected with incomplete saved state");
  check(dsp::select({true,true,true,true,6}) == dsp::Isa::Avx2, "Complete AVX2 state not selected");
  uint64_t coefficients{}, samples{};
  nss::Json names = nss::Json::array();
  for (const auto isa : available()) {
    if (isa == dsp::Isa::Scalar) continue;
    names.push_back(dsp::kernel(isa).name);
    for (unsigned rate : {8000u,11025u,16000u,22050u,32000u,44100u,48000u,88200u,96000u,192000u}) {
      const double cutoff = std::min(1.0, 48000.0/rate) * .94;
      const SincKernel scalar(cutoff, dsp::Isa::Scalar), vector(cutoff, isa);
      alignas(32) std::array<dsp::Stereo, 101> storage{};
      uint32_t state = 0x45e591u;
      for (auto &sample : storage) for (auto &value : sample) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        value = static_cast<float>(static_cast<int32_t>(state)) / 2147483648.f;
      }
      storage[1] = {std::numeric_limits<float>::denorm_min(), -0.f};
      storage[2] = {std::nextafter(.5f/32768, 0.f), std::nextafter(.5f/32768, 1.f)};
      storage[3] = {1.f, -1.f};
      for (size_t phase = 0; phase <= 4093; ++phase) {
        const double fraction = double(phase)/4093;
        const auto expected = scalar.weights(fraction), actual = vector.weights(fraction);
        check(std::memcmp(expected.data(), actual.data(), sizeof(expected)) == 0, "SIMD coefficient bits differ from scalar");
        coefficients += expected.size();
        if (phase % 41) continue;
        for (size_t count : {size_t{1},size_t{2},size_t{17},size_t{31},size_t{32},size_t{33},size_t{97}})
          for (int64_t first = -17; first <= static_cast<int64_t>(count)+1; first += 3) {
            const auto reference = scalar.filter(storage.data()+1, count, first, expected);
            const auto filtered = vector.filter(storage.data()+1, count, first, actual);
            check(std::memcmp(reference.data(), filtered.data(), sizeof(reference)) == 0, "SIMD sample bits differ at a fractional phase or clamped edge");
            for (size_t channel=0; channel<2; ++channel)
              check(std::lround(std::clamp(double(reference[channel]),-1.,32767./32768)*32768) ==
                    std::lround(std::clamp(double(filtered[channel]),-1.,32767./32768)*32768), "SIMD changed quantized PCM16");
            samples += 2;
          }
      }
    }
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    auto *memory = static_cast<uint8_t *>(VirtualAlloc(nullptr, size_t(system.dwPageSize)*2, MEM_RESERVE, PAGE_NOACCESS));
    check(memory != nullptr, "Guard-page reservation failed");
    struct Release { uint8_t *value; ~Release() { VirtualFree(value,0,MEM_RELEASE); } } release{memory};
    check(VirtualAlloc(memory,system.dwPageSize,MEM_COMMIT,PAGE_READWRITE) == memory, "Guard-page commit failed");
    SincKernel scalar(.94,dsp::Isa::Scalar), vector(.94,isa);
    const auto weights = scalar.weights(.123);
    for (size_t count : {size_t{1},size_t{31},size_t{32},size_t{33}}) {
      auto *input = reinterpret_cast<dsp::Stereo *>(memory + system.dwPageSize - count*sizeof(dsp::Stereo));
      for (size_t i=0;i<count;++i) new (input+i) dsp::Stereo{float(i)/33, -float(i)/33};
      for (int64_t first : {-15,0,1,17}) {
        const auto a = scalar.filter(input,count,first,weights), b = vector.filter(input,count,first,weights);
        check(std::memcmp(a.data(),b.data(),sizeof(a)) == 0, "SIMD guard-page edge differs");
      }
    }
  }
  return {{"status","PASS"},{"implementedKernels",names},{"effectiveIsa",dsp::kernel().name},
          {"coefficientValuesBitCompared",coefficients},{"stereoChannelSamplesBitCompared",samples},
          {"guardPageEdgesPassed",true},{"capabilityFallbackCasesPassed",12},
          {"pcm16Exact",true},{"wholeLibraryAvx2Required",false}};
}
static nss::Json benchmark() {
  constexpr size_t iterations=500000, repeats=7;
  const auto choices=available();
  nss::Json timings=nss::Json::object();
  alignas(32) std::array<dsp::Stereo,96> input;
  for (size_t i=0;i<input.size();++i) input[i]={float(std::sin(double(i))),float(std::cos(double(i)*.31))};
  double checksum{};
  for (size_t repetition=0;repetition<repeats;++repetition)
    for (size_t offset=0;offset<choices.size();++offset) {
      const auto isa=choices[(offset+repetition)%choices.size()];
      const SincKernel kernel(.94,isa);
      const auto begin=std::chrono::steady_clock::now();
      for (size_t i=0;i<iterations;++i) {
        const auto weights=kernel.weights(double(i%4093)/4093);
        const auto sample=kernel.filter(input.data(),input.size(),16,weights);
        checksum+=sample[0];
      }
      const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
      timings[kernel.instruction_set()].push_back(ms);
    }
  nss::Json results=nss::Json::object();
  for (const auto &entry:timings.items()) {
    auto ordered=entry.value().get<std::vector<double>>();std::sort(ordered.begin(),ordered.end());
    results[entry.key()]={{"elapsedMs",entry.value()},{"medianMs",ordered[ordered.size()/2]},
        {"medianNanosecondsPerStereoOutput",ordered[ordered.size()/2]*1e6/iterations}};
  }
  return {{"status","MEASURED"},{"iterationsPerRun",iterations},{"repetitions",repeats},
      {"cases",results},{"checksum",checksum},{"scope","32-tap interpolation and stereo FIR numerical equivalence"}};
}
int main(int argc,char **argv) {
  try {
    std::cout << (argc==2 && std::string(argv[1])=="--benchmark" ? benchmark() : qualify()).dump() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cout << nss::Json{{"status","FAIL"},{"message",error.what()}}.dump() << '\n';return 1;
  }
}
