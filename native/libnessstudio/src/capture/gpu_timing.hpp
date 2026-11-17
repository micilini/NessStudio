#pragma once
#include "capture.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>

namespace nss::capture {
// Optional diagnostic only. Sample sparsely after warmup, never wait, flush or
// use timing evidence as a readiness signal. Storage and GPU queries are bounded.
class GpuTiming {
  using Query = Microsoft::WRL::ComPtr<ID3D11Query>;
  struct Sample {
    Query first, last, frequency;
    uint64_t sequence{};
    int64_t submitted{};
    bool pending{};
  };
  struct Record {
    uint64_t sequence{}, first{}, last{}, frequency{};
    int64_t submitted{};
    bool valid{};
  };
  struct State {
    std::array<Sample, 8> samples;
    std::array<Record, 256> records;
    size_t size{};
    uint64_t calls{}, submitted{}, pressure{};
    int64_t start{qpc_hns()};
  };
  bool enabled_{};
  std::unique_ptr<State> state_;
  void collect(ID3D11DeviceContext *context) {
    for (auto &sample : state_->samples) {
      if (!sample.pending) continue;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
      auto result = context->GetData(sample.frequency.Get(), &frequency, sizeof(frequency),
                                    D3D11_ASYNC_GETDATA_DONOTFLUSH);
      checked(result, NSS_STAGE_PROCESS, "GPU timing frequency read failed");
      if (result != S_OK) continue;
      uint64_t first{}, last{};
      result = context->GetData(sample.first.Get(), &first, sizeof(first), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      checked(result, NSS_STAGE_PROCESS, "GPU timing start read failed");
      if (result != S_OK) continue;
      result = context->GetData(sample.last.Get(), &last, sizeof(last), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      checked(result, NSS_STAGE_PROCESS, "GPU timing end read failed");
      if (result != S_OK) continue;
      state_->records[state_->size++] = {sample.sequence, first, last, frequency.Frequency,
          sample.submitted, !frequency.Disjoint && frequency.Frequency && last >= first};
      sample.pending = false;
    }
  }
public:
  explicit GpuTiming(bool enabled = false) : enabled_(enabled) {}
  int begin(ID3D11Device *device, ID3D11DeviceContext *context) {
    if (!enabled_) return -1;
    if (!state_) {
      state_ = std::make_unique<State>();
      for (auto &sample : state_->samples) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
        checked(device->CreateQuery(&desc, &sample.first), NSS_STAGE_PROCESS, "GPU timing start allocation failed");
        checked(device->CreateQuery(&desc, &sample.last), NSS_STAGE_PROCESS, "GPU timing end allocation failed");
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        checked(device->CreateQuery(&desc, &sample.frequency), NSS_STAGE_PROCESS, "GPU timing frequency allocation failed");
      }
    }
    // Once all accepted samples are collected, no more driver queries occur.
    if (state_->size == state_->records.size()) return -1;
    collect(context);
    const auto call = ++state_->calls;
    if (state_->submitted == state_->records.size() || call % 8 ||
        qpc_hns() - state_->start < 20 * hns_per_second) return -1;
    for (size_t index = 0; index < state_->samples.size(); ++index) {
      auto &sample = state_->samples[index];
      if (sample.pending) continue;
      sample.sequence = ++state_->submitted;
      sample.submitted = qpc_hns();
      sample.pending = true;
      context->Begin(sample.frequency.Get());
      context->End(sample.first.Get());
      return static_cast<int>(index);
    }
    ++state_->pressure;
    return -1;
  }
  void end(ID3D11DeviceContext *context, int index) {
    if (index < 0) return;
    auto &sample = state_->samples[static_cast<size_t>(index)];
    context->End(sample.last.Get());
    context->End(sample.frequency.Get());
  }
  Json metrics() const {
    Json result{{"enabled", enabled_}, {"sampleEveryCalls", 8}, {"warmupSeconds", 20},
                {"sampleLimit", 256}, {"scope", "GPU timestamp intervals; diagnostic instrumentation can affect scheduling"}};
    if (!state_) return result;
    result["calls"] = state_->calls;
    result["submitted"] = state_->submitted;
    result["collected"] = state_->size;
    result["pressure"] = state_->pressure;
    result["samples"] = Json::array();
    for (size_t i = 0; i < state_->size; ++i) {
      const auto &r = state_->records[i];
      result["samples"].push_back({{"sequence", r.sequence}, {"submittedQpcHns", r.submitted},
          {"first", r.first}, {"last", r.last}, {"frequency", r.frequency}, {"valid", r.valid},
          {"milliseconds", r.valid ? Json(double(r.last - r.first) * 1000.0 / double(r.frequency)) : Json(nullptr)}});
    }
    return result;
  }
};
}
