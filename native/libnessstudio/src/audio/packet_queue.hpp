#pragma once
#include "capture/capture.hpp"
#include <array>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace nss::audio {
// Original bounded handoff between the WASAPI owner and DSP/file processing.
// Storage is allocated before capture. No borrowed endpoint pointer survives
// push(), and overload never evicts a packet or invents replacement samples.
class PacketQueue final {
public:
  struct Packet {
    uint32_t frames{}, flags{};
    uint64_t position{}, qpc{}, keepalive_frames{};
    int64_t received_hns{}, observed_idle_hns{};
    bool silent{};
  };
  struct Stats {
    uint64_t queued_frames{}, peak_frames{}, queued_packets{}, peak_packets{},
        pushed{}, popped{}, bytes{};
  };
private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<uint8_t> bytes_;
  std::array<Packet, 512> packets_{};
  size_t packet_head_{}, packet_count_{}, read_{}, write_{};
  uint32_t align_, max_frames_, max_packet_frames_;
  Stats stats_;
  bool closed_{};
  std::exception_ptr failure_;
  static size_t checked_bytes(uint32_t rate, uint32_t align) {
    if (rate < 8000 || rate > 192000 || !align || align > 8)
      throw std::invalid_argument("Invalid bounded audio queue format");
    return size_t(rate / 2) * align;
  }
public:
  PacketQueue(uint32_t rate, uint32_t align)
      : bytes_(checked_bytes(rate, align)), align_(align),
        max_frames_(rate / 2), max_packet_frames_(rate / 5) {
    stats_.bytes = bytes_.size() + sizeof(packets_);
  }
  PacketQueue(const PacketQueue &) = delete;
  PacketQueue &operator=(const PacketQueue &) = delete;
  void push(const uint8_t *source, Packet packet) {
    if (!packet.frames || packet.frames > max_packet_frames_ || (!source && !packet.silent))
      throw capture::Error(NSS_E_BACKPRESSURE, NSS_STAGE_AUDIO, 0,
                           "Audio packet exceeds the owned 200ms packet budget");
    std::unique_lock lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    if (closed_)
      throw capture::Error(NSS_E_STATE, NSS_STAGE_AUDIO, 0, "Audio packet queue is closed");
    if (packet_count_ == packets_.size() || packet.frames > max_frames_ - stats_.queued_frames)
      throw capture::Error(NSS_E_BACKPRESSURE, NSS_STAGE_AUDIO, 0,
                           "Audio processing queue exceeded its 500ms budget; no packet discarded");
    const size_t count = size_t(packet.frames) * align_;
    const auto first = (std::min)(count, bytes_.size() - write_);
    if (!packet.silent) {
      std::memcpy(bytes_.data() + write_, source, first);
      if (count > first) std::memcpy(bytes_.data(), source + first, count - first);
    }
    write_ = (write_ + count) % bytes_.size();
    packets_[(packet_head_ + packet_count_) % packets_.size()] = packet;
    ++packet_count_;
    stats_.queued_frames += packet.frames;
    stats_.peak_frames = (std::max)(stats_.peak_frames, stats_.queued_frames);
    stats_.queued_packets = packet_count_;
    stats_.peak_packets = (std::max)(stats_.peak_packets, stats_.queued_packets);
    ++stats_.pushed;
    lock.unlock();
    changed_.notify_one();
  }
  bool pop(std::vector<uint8_t> &destination, Packet &packet) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return packet_count_ || closed_ || failure_; });
    if (failure_) std::rethrow_exception(failure_);
    if (!packet_count_) return false;
    packet = packets_[packet_head_];
    const size_t count = size_t(packet.frames) * align_;
    destination.resize(count);
    const auto first = (std::min)(count, bytes_.size() - read_);
    if (!packet.silent) {
      std::memcpy(destination.data(), bytes_.data() + read_, first);
      if (count > first) std::memcpy(destination.data() + first, bytes_.data(), count - first);
    }
    read_ = (read_ + count) % bytes_.size();
    packet_head_ = (packet_head_ + 1) % packets_.size();
    --packet_count_;
    stats_.queued_packets = packet_count_;
    stats_.queued_frames -= packet.frames;
    ++stats_.popped;
    return true;
  }
  void close() noexcept {
    { std::lock_guard lock(mutex_); closed_ = true; }
    changed_.notify_all();
  }
  void fail(std::exception_ptr error) noexcept {
    { std::lock_guard lock(mutex_); if (!failure_) failure_ = error; closed_ = true; }
    changed_.notify_all();
  }
  void check() const {
    std::lock_guard lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
  }
  Stats stats() const { std::lock_guard lock(mutex_); return stats_; }
};

class PacketProcessor final {
  PacketQueue &queue_;
  std::thread thread_;
public:
  template<class Consumer>
  PacketProcessor(PacketQueue &queue, size_t maximum_packet_bytes, Consumer consume)
      : queue_(queue), thread_([&queue, maximum_packet_bytes, consume = std::move(consume)]() mutable {
          try {
            std::vector<uint8_t> copy;
            copy.reserve(maximum_packet_bytes);
            PacketQueue::Packet packet;
            while (queue.pop(copy, packet)) consume(copy, packet);
          } catch (...) { queue.fail(std::current_exception()); }
        }) {}
  PacketProcessor(const PacketProcessor &) = delete;
  PacketProcessor &operator=(const PacketProcessor &) = delete;
  ~PacketProcessor() { queue_.close(); if (thread_.joinable()) thread_.join(); }
  void drain() { queue_.close(); if (thread_.joinable()) thread_.join(); queue_.check(); }
};
} // namespace nss::audio
