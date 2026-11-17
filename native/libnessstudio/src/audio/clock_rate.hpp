#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace nss::audio {
// Estimate rate from correlated device/QPC observations, not the difference
// from one noisy first timestamp. Centered rolling moments avoid subtracting
// large absolute QPC values and bound both memory and per-packet work.
class ClockRate final {
  struct Point {uint64_t position{};int64_t qpc{};};
  struct Coordinates {double seconds{},samples_seconds{};};
  std::array<Point, 640> points_{};
  size_t head_{}, count_{};
  uint64_t origin_position_{};
  int64_t origin_qpc_{}, last_qpc_{};
  uint32_t rate_{}, since_rebuild_{};
  double mean_x_{}, mean_y_{}, xx_{}, xy_{};
  bool initialized_{};
  Coordinates coordinates(const Point &point) const noexcept {
    return {double(point.qpc-origin_qpc_)/10000000.0,double(point.position-origin_position_)/rate_};
  }
  void add_moments(const Coordinates &point,size_t count) noexcept {
    const double dx=point.seconds-mean_x_,dy=point.samples_seconds-mean_y_;
    mean_x_+=dx/double(count);mean_y_+=dy/double(count);
    xx_+=dx*(point.seconds-mean_x_);xy_+=dx*(point.samples_seconds-mean_y_);
  }
  void rebuild() noexcept {
    // Sliding deletion slowly accumulates floating-point error even with
    // centered moments. Reanchor to an exact retained device/QPC pair and
    // rebuild periodically, bounding coordinates to this 30-second window.
    const auto &first=points_[head_];
    origin_position_=first.position;origin_qpc_=first.qpc;
    mean_x_=mean_y_=xx_=xy_=0;
    for(size_t i=0;i<count_;++i) add_moments(coordinates(points_[(head_+i)%points_.size()]),i+1);
    since_rebuild_=0;
  }
  void remove() noexcept {
    const auto point=coordinates(points_[head_]);
    if(count_==1) {mean_x_=mean_y_=xx_=xy_=0;}
    else {
      const auto next_x=mean_x_-(point.seconds-mean_x_)/double(count_-1);
      const auto next_y=mean_y_-(point.samples_seconds-mean_y_)/double(count_-1);
      xx_-=(point.seconds-mean_x_)*(point.seconds-next_x);
      xy_-=(point.seconds-mean_x_)*(point.samples_seconds-next_y);
      mean_x_=next_x;mean_y_=next_y;
    }
    head_=(head_+1)%points_.size();--count_;
  }
public:
  void reset() noexcept {*this=ClockRate{};}
  void observe(uint64_t position,int64_t qpc,uint32_t rate) {
    if(!rate || qpc<0) throw std::invalid_argument("Invalid audio clock observation");
    if(!initialized_) {origin_position_=position;origin_qpc_=qpc;rate_=rate;initialized_=true;}
    if(rate!=rate_) throw std::invalid_argument("Audio clock nominal rate changed without reset");
    if(position<origin_position_ || qpc<last_qpc_)
      throw std::invalid_argument("Audio clock observation moved backwards");
    if(count_ && qpc-last_qpc_<500000) return; // At most 20 observations/second.
    Point point{position,qpc};
    while(count_ && (qpc-points_[head_].qpc>300000000 || count_==points_.size())) remove();
    points_[(head_+count_)%points_.size()]=point;
    ++count_;last_qpc_=qpc;
    add_moments(coordinates(point),count_);
    if(++since_rebuild_>=256) rebuild();
  }
  double span_seconds() const noexcept {
    return count_?double(points_[(head_+count_-1)%points_.size()].qpc-points_[head_].qpc)/10000000.0:0;
  }
  size_t observations() const noexcept {return count_;}
  std::optional<double> ppm() const noexcept {
    if(count_<3 || span_seconds()<2 || xx_<=0) return {};
    return (xy_/xx_-1)*1000000.0;
  }
};
} // namespace nss::audio
