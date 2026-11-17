#pragma once
#include "dshow_types.hpp"
#include <limits>

namespace nss::camera::detail {
struct CameraTimestamp {
  int64_t capture_qpc{}, graph_projection{};
};
// A graph reference clock does not prove that a UVC driver timestamps its
// samples with that clock. DeliveryQpc records the measured callback boundary;
// the exposure boundary remains unknown. It never estimates a frequency from
// output CFR, fits an A/V offset, or changes the raw driver timestamp.
inline CameraTimestamp camera_timestamp(TimestampMode mode, int64_t origin,
    int64_t raw_stream, int64_t arrival, bool have_previous,
    int64_t previous_raw, int64_t previous_arrival) {
  if(origin<=0 || arrival<=0 || raw_stream<0 ||
     raw_stream>std::numeric_limits<int64_t>::max()-origin)
    throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,VFW_E_SAMPLE_TIME_NOT_SET,
                         "Camera timestamp has an invalid origin or range");
  if(have_previous && (raw_stream<=previous_raw || arrival<=previous_arrival))
    throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,VFW_E_SAMPLE_TIME_NOT_SET,
                         "Camera raw timestamp or callback QPC is nonmonotonic");
  const auto projected=origin+raw_stream;
  if(mode==TimestampMode::StrictStreamQpc) {
    if(projected-arrival>hns_per_second || arrival-projected>5*hns_per_second)
      throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,VFW_E_SAMPLE_TIME_NOT_SET,
                           "Camera stream timestamp is outside the QPC correlation window");
    return {projected,projected};
  }
  if(mode!=TimestampMode::DeliveryQpc)
    throw capture::Error(NSS_E_ARGUMENT,NSS_STAGE_PLAN,0,"Invalid camera timestamp mode");
  return {arrival,projected};
}
} // namespace nss::camera::detail
