#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "preview.hpp"
#include <chrono>
#include <d3d11_4.h>
#include <d3d9.h>
#include <dxgi1_6.h>
#include <thread>
#include <windows.h>
#include <wrl/client.h>

namespace nss::preview {
namespace {
using Microsoft::WRL::ComPtr;
std::atomic<uint64_t> generations{};
void checked(HRESULT value, const char *message) {
  capture::checked(value, NSS_STAGE_PREVIEW, message);
}
struct Slot {
  bool in_use{};
  std::vector<uint8_t> pixels;
  ComPtr<IDirect3DTexture9> texture9;
  ComPtr<IDirect3DSurface9> surface9;
  ComPtr<ID3D11Texture2D> texture11;
  ComPtr<ID3D11VideoProcessorOutputView> output;
  ComPtr<ID3D11Query> done;
};
struct Pool {
  std::mutex gate;
  std::vector<Slot> slots;
  uint32_t width{}, height{};
  uint64_t generation{}, bytes{}, adapter{};
  bool gpu{};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11VideoDevice> video;
  ComPtr<ID3D11VideoContext1> video_context;
  ComPtr<ID3D11VideoProcessorEnumerator> enumeration;
  ComPtr<ID3D11VideoProcessor> processor;
  ComPtr<IDirect3D9Ex> d3d9;
  ComPtr<IDirect3DDevice9Ex> device9;
  Pool(uint32_t w, uint32_t h, uint32_t count, bool hardware)
      : slots(count), width(w), height(h), generation(++generations),
        bytes(uint64_t(w) * h * 4 * count), gpu(hardware) {
    live_bytes += bytes;
  }
  ~Pool() { live_bytes -= bytes; }
  void initialize(const capture::Frame &frame) {
    if (!gpu) {
      for (auto &slot : slots)
        slot.pixels.resize(size_t(width) * height * 4);
      return;
    }
    auto *original = static_cast<ID3D11Device *>(frame.surface->gpu_device());
    if (!original || !frame.surface->gpu_texture())
      throw capture::Error(NSS_E_UNAVAILABLE, NSS_STAGE_PREVIEW, 0,
                           "GPU preview requires an actual GPU source surface");
    device = original;
    adapter = frame.surface->adapter_luid();
    device->GetImmediateContext(&context);
    checked(device.As(&video), "GPU preview video device unavailable");
    checked(context.As(&video_context),
            "GPU preview video context unavailable");
    checked(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9),
            "D3D9Ex preview bridge unavailable");
    UINT selected = UINT_MAX;
    for (UINT index = 0; index < d3d9->GetAdapterCount(); ++index) {
      LUID value{};
      if (FAILED(d3d9->GetAdapterLUID(index, &value)))
        continue;
      const auto id = (uint64_t(static_cast<uint32_t>(value.HighPart)) << 32) |
                      value.LowPart;
      if (id == adapter) {
        selected = index;
        break;
      }
    }
    if (selected == UINT_MAX)
      throw capture::Error(
          NSS_E_UNAVAILABLE, NSS_STAGE_PREVIEW, 0,
          "WPF D3D9Ex bridge cannot resolve the recording adapter");
    D3DPRESENT_PARAMETERS presentation{};
    presentation.Windowed = TRUE;
    presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
    presentation.hDeviceWindow = GetDesktopWindow();
    presentation.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    presentation.BackBufferWidth = presentation.BackBufferHeight = 1;
    presentation.BackBufferFormat = D3DFMT_UNKNOWN;
    checked(d3d9->CreateDeviceEx(selected, D3DDEVTYPE_HAL, GetDesktopWindow(),
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                     D3DCREATE_MULTITHREADED |
                                     D3DCREATE_FPU_PRESERVE,
                                 &presentation, nullptr, &device9),
            "D3D9Ex hardware preview device failed");
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {30, 1};
    content.OutputFrameRate = {15, 1};
    content.InputWidth = frame.width;
    content.InputHeight = frame.height;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    checked(video->CreateVideoProcessorEnumerator(&content, &enumeration),
            "GPU preview scaler enumeration failed");
    checked(video->CreateVideoProcessor(enumeration.Get(), 0, &processor),
            "GPU preview scaler creation failed");
    auto *vp = processor.Get();
    video_context->VideoProcessorSetStreamFrameFormat(
        vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
    video_context->VideoProcessorSetStreamColorSpace1(
        vp, 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    video_context->VideoProcessorSetOutputColorSpace1(
        vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    video_context->VideoProcessorSetOutputAlphaFillMode(
        vp, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
    const RECT source{0, 0, static_cast<LONG>(frame.width),
                      static_cast<LONG>(frame.height)},
        destination{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    video_context->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &source);
    video_context->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &destination);
    video_context->VideoProcessorSetOutputTargetRect(vp, TRUE, &destination);
    for (auto &slot : slots) {
      HANDLE shared{};
      checked(device9->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                     D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                     &slot.texture9, &shared),
              "D3D9Ex shared BGRA preview texture failed");
      checked(slot.texture9->GetSurfaceLevel(0, &slot.surface9),
              "D3D9Ex preview surface unavailable");
      checked(device->OpenSharedResource(shared, IID_PPV_ARGS(&slot.texture11)),
              "D3D11 cannot open the D3D9Ex preview surface");
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output{};
      output.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      checked(video->CreateVideoProcessorOutputView(slot.texture11.Get(),
                                                    enumeration.Get(), &output,
                                                    &slot.output),
              "GPU preview output view failed");
      D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
      checked(device->CreateQuery(&query, &slot.done),
              "GPU preview completion query failed");
    }
  }
};
struct Storage {
  std::shared_ptr<Pool> pool;
  size_t index{};
  Storage(std::shared_ptr<Pool> value, size_t slot)
      : pool(std::move(value)), index(slot) {}
  ~Storage() {
    std::lock_guard lock(pool->gate);
    pool->slots[index].in_use = false;
  }
};
std::optional<size_t> reserve(Pool &pool) {
  std::lock_guard lock(pool.gate);
  for (size_t index = 0; index < pool.slots.size(); ++index)
    if (!pool.slots[index].in_use) {
      pool.slots[index].in_use = true;
      return index;
    }
  return {};
}
uint8_t clamp(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}
void nv12_to_bgra(const capture::Frame &source, Slot &destination,
                  uint32_t width, uint32_t height) {
  const auto data = source.surface->cpu_pixels();
  const auto stride = source.stride ? source.stride : source.width;
  if (data.size() < uint64_t(stride) * source.height * 3 / 2)
    throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PREVIEW, 0,
                         "CPU preview received an invalid NV12 span");
  const auto *pixels = reinterpret_cast<const uint8_t *>(data.data());
  const auto *uv = pixels + uint64_t(stride) * source.height;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t sy =
        static_cast<uint32_t>(uint64_t(y) * source.height / height);
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t sx =
          static_cast<uint32_t>(uint64_t(x) * source.width / width);
      const int yy =
          (std::max)(0, int(pixels[uint64_t(sy) * stride + sx]) - 16);
      const int u = int(uv[uint64_t(sy / 2) * stride + (sx & ~1U)]) - 128,
                v = int(uv[uint64_t(sy / 2) * stride + (sx & ~1U) + 1]) - 128;
      auto *output = destination.pixels.data() + (uint64_t(y) * width + x) * 4;
      output[0] = clamp((298 * yy + 541 * u + 128) >> 8);
      output[1] = clamp((298 * yy - 55 * u - 136 * v + 128) >> 8);
      output[2] = clamp((298 * yy + 459 * v + 128) >> 8);
      output[3] = 255;
    }
  }
}
} // namespace
struct Channel::Impl {
  mutable std::mutex gate;
  nss_preview_config_v1 config;
  uint64_t session{}, source{}, offered{}, produced{}, acquired{}, dropped{},
      copied_bytes{}, failures{};
  std::shared_ptr<Pool> pool;
  std::shared_ptr<Frame> latest, pending;
  std::shared_ptr<const capture::Surface> pending_source;
  ComPtr<ID3D11VideoProcessorInputView> pending_input;
  size_t pending_index{};
  int64_t next_hns{};
  bool attached{true}, enabled{true};
  std::optional<capture::Frame> queued_gpu;
  uint64_t dispatches{}, deferred_drops{};
  int64_t dispatch_max_hns{};
  std::string error;
  Impl(nss_preview_config_v1 value, uint64_t session_id, uint64_t source_id)
      : config(value), session(session_id), source(source_id) {}
  void complete() {
    if (!pending)
      return;
    BOOL ready{};
    const auto result =
        pool->context->GetData(pool->slots[pending_index].done.Get(), &ready,
                               sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    checked(result, "GPU preview completion query failed");
    if (result == S_FALSE || !ready)
      return;
    pending_source.reset();
    pending_input.Reset();
    if (attached && enabled) {
      if (latest)
        ++dropped;
      latest = std::move(pending);
      ++produced;
    } else
      pending.reset();
  }
  void offer(const capture::Frame &frame) {
    complete();
    if (!attached || !enabled || !error.empty())
      return;
    ++offered;
    const auto now = qpc_hns();
    if (now < next_hns)
      return;
    next_hns = now + static_cast<int64_t>(uint64_t(hns_per_second) *
                                          config.fps_den / config.fps_num);
    if (pending) {
      ++dropped;
      return;
    }
    const double ratio =
        (std::min)({double(config.max_width) / frame.width,
                    double(config.max_height) / frame.height, 1.0});
    const uint32_t width = (std::max)(1U, static_cast<uint32_t>(frame.width *
                                                                ratio)),
                   height = (std::max)(1U, static_cast<uint32_t>(frame.height *
                                                                 ratio));
    // Frontend dimensions are a bounded presentation target; recording geometry
    // never changes.
    if (!pool) {
      pool = std::make_shared<Pool>(width, height, config.slots,
                                    config.channel == 2);
      pool->initialize(frame);
    }
    auto index = reserve(*pool);
    if (!index) {
      ++dropped;
      return;
    }
    auto storage = std::make_shared<Storage>(pool, *index);
    auto output = std::make_shared<Frame>();
    output->storage = storage;
    output->cpu = {sizeof(nss_preview_frame_v1),
                   1,
                   session,
                   source,
                   pool->generation,
                   frame.id,
                   frame.capture_hns,
                   static_cast<uint64_t>(
                       (std::max)(int64_t{}, frame.end_hns - frame.begin_hns)),
                   uint64_t(width) * height * 4,
                   nullptr,
                   width,
                   height,
                   width * 4,
                   1,
                   frame.estimated ? 1U : 0U,
                   0};
    auto &slot = pool->slots[*index];
    if (!pool->gpu) {
      if (frame.surface->cpu_pixels().empty())
        throw capture::Error(NSS_E_UNAVAILABLE, NSS_STAGE_PREVIEW, 0,
                             "CPU preview of a GPU recording requires an "
                             "explicitly qualified readback route");
      nv12_to_bgra(frame, slot, width, height);
      output->cpu.data = slot.pixels.data();
      copied_bytes += output->cpu.bytes;
      if (latest)
        ++dropped;
      latest = std::move(output);
      ++produced;
      return;
    }
    output->gpu = {sizeof(nss_gpu_surface_v1),
                   1,
                   session,
                   source,
                   pool->generation,
                   frame.id,
                   frame.capture_hns,
                   slot.surface9.Get(),
                   pool->adapter,
                   width,
                   height,
                   frame.estimated ? 1U : 0U,
                   0};
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
    view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    checked(pool->video->CreateVideoProcessorInputView(
                static_cast<ID3D11Texture2D *>(frame.surface->gpu_texture()),
                pool->enumeration.Get(), &view, &pending_input),
            "GPU preview input view failed");
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = pending_input.Get();
    checked(pool->video_context->VideoProcessorBlt(
                pool->processor.Get(), slot.output.Get(), 0, 1, &stream),
            "GPU preview conversion/scaling failed");
    pool->context->End(slot.done.Get());
    pool->context->Flush();
    pending_source = frame.surface;
    pending_index = *index;
    pending = std::move(output);
  }
};
Channel::Channel(const nss_preview_config_v1 &request, uint64_t session,
                 uint64_t source)
    : impl_(std::make_unique<Impl>(request, session, source)) {
  ++live_channels;
}
Channel::~Channel() { --live_channels; }
std::shared_ptr<Frame> Channel::acquire(bool gpu) {
  std::unique_lock lock(impl_->gate, std::try_to_lock);
  if (!lock.owns_lock())
    return {};
  if ((impl_->config.channel == 2) != gpu)
    throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PREVIEW, 0,
                         "Preview acquire does not match the selected channel");
  if (!impl_->error.empty())
    throw capture::Error(NSS_E_UNAVAILABLE, NSS_STAGE_PREVIEW, 0,
                         impl_->error.c_str());
  if (!impl_->attached || !impl_->enabled)
    return {};
  auto result = std::move(impl_->latest);
  impl_->latest.reset();
  if (result)
    ++impl_->acquired;
  return result;
}
void Channel::offer(const capture::Frame &frame) {
  std::lock_guard lock(impl_->gate);
  try {
    impl_->offer(frame);
  } catch (const std::exception &error) {
    impl_->error = error.what();
    ++impl_->failures;
    impl_->attached = false;
    impl_->latest.reset();
  }
}
void Channel::set_enabled(bool enabled) {
  std::lock_guard lock(impl_->gate);
  impl_->enabled = enabled;
  if (!enabled) {
    impl_->latest.reset();
    impl_->queued_gpu.reset();
  }
}
bool Channel::active() const {
  std::lock_guard lock(impl_->gate);
  return impl_->attached && impl_->enabled && impl_->error.empty();
}
void Channel::queue_gpu(const capture::Frame &frame) {
  if (impl_->config.channel != 2) {
    offer(frame);
    return;
  }
  std::lock_guard lock(impl_->gate);
  if (!impl_->attached || !impl_->enabled || !impl_->error.empty())
    return;
  if (impl_->queued_gpu)
    ++impl_->deferred_drops;
  impl_->queued_gpu = frame;
}
bool Channel::dispatch_gpu() {
  std::lock_guard lock(impl_->gate);
  if (!impl_->queued_gpu || !impl_->attached || !impl_->enabled ||
      qpc_hns() < impl_->next_hns)
    return false;
  auto frame = std::move(*impl_->queued_gpu);
  impl_->queued_gpu.reset();
  const auto begin = qpc_hns();
  try {
    impl_->offer(frame);
  } catch (const std::exception &error) {
    impl_->error = error.what();
    ++impl_->failures;
    impl_->attached = false;
    impl_->latest.reset();
  }
  ++impl_->dispatches;
  impl_->dispatch_max_hns =
      (std::max)(impl_->dispatch_max_hns, qpc_hns() - begin);
  return true;
}
void Channel::pump() {
  std::lock_guard lock(impl_->gate);
  try {
    impl_->complete();
  } catch (const std::exception &error) {
    impl_->error = error.what();
    ++impl_->failures;
    impl_->attached = false;
  }
}
void Channel::detach() noexcept {
  std::lock_guard lock(impl_->gate);
  impl_->attached = false;
  impl_->latest.reset();
  impl_->queued_gpu.reset();
}
void Channel::close() {
  detach();
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (;;) {
    {
      std::lock_guard lock(impl_->gate);
      if (!impl_->pending)
        return;
      impl_->complete();
      if (!impl_->pending)
        return;
    }
    if (std::chrono::steady_clock::now() >= end)
      throw capture::Error(
          NSS_E_TIMEOUT, NSS_STAGE_PREVIEW, 0,
          "GPU preview work did not quiesce; owned source lease retained");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
Json Channel::metrics() const {
  std::lock_guard lock(impl_->gate);
  const auto &value = *impl_;
  return {{"sourceId", value.source},
          {"channel",
           value.config.channel == 2 ? "D3D11-to-D3D9Ex-D3DImage" : "CPU BGRA"},
          {"attached", value.attached},
          {"enabled", value.enabled},
          {"gpuDispatches", value.dispatches},
          {"deferredInputReplacements", value.deferred_drops},
          {"gpuDispatchMaxMs", double(value.dispatch_max_hns) / 10000},
          {"offered", value.offered},
          {"produced", value.produced},
          {"acquired", value.acquired},
          {"dropped", value.dropped},
          {"presentationCopyBytes", value.copied_bytes},
          {"previewGpuToCpuReadbackBytes", 0},
          {"failures", value.failures},
          {"error", value.error},
          {"poolBytes", value.pool ? value.pool->bytes : 0},
          {"generation", value.pool ? value.pool->generation : 0},
          {"slots", value.config.slots},
          {"maxFps", double(value.config.fps_num) / value.config.fps_den}};
}
bool Channel::retired() const {
  std::lock_guard lock(impl_->gate);
  return !impl_->attached && !impl_->pending;
}
void Hub::bind(uint64_t session, uint64_t source) {
  std::lock_guard lock(gate_);
  session_ = session;
  source_ = source;
}
std::shared_ptr<Channel> Hub::attach(const nss_preview_config_v1 &request) {
  std::lock_guard lock(gate_);
  if (ended_)
    throw capture::Error(NSS_E_STATE, NSS_STAGE_PREVIEW, 0,
                         "The source preview has ended");
  if (request.size != sizeof(request) || request.version != 1 ||
      request.source_id != source_ || request.reserved ||
      request.max_width == 0 || request.max_height == 0 ||
      request.max_width > 640 || request.max_height > 360 || !request.fps_num ||
      !request.fps_den || uint64_t(request.fps_num) > 15ULL * request.fps_den ||
      !request.slots || request.slots > 3 ||
      (request.channel != 1 && request.channel != 2))
    throw capture::Error(NSS_E_ARGUMENT, NSS_STAGE_PREVIEW, 0,
                         "Preview geometry/cadence/channel exceeds the bounded "
                         "presentation contract");
  channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                 [](const auto &channel) {
                                   return channel.use_count() == 1 &&
                                          channel->retired();
                                 }),
                  channels_.end());
  if (channels_.size() >= 2)
    throw capture::Error(NSS_E_BUSY, NSS_STAGE_PREVIEW, 0,
                         "Source preview consumer budget exhausted");
  auto channel = std::make_shared<Channel>(request, session_, source_);
  channels_.push_back(channel);
  return channel;
}
void Hub::offer(const capture::Frame &frame) {
  std::lock_guard lock(gate_);
  for (auto &channel : channels_)
    if (defer_gpu_)
      channel->queue_gpu(frame);
    else
      channel->offer(frame);
}
bool Hub::active() const {
  std::lock_guard lock(gate_);
  return std::any_of(channels_.begin(), channels_.end(),
                     [](const auto &channel) { return channel->active(); });
}
bool Hub::dispatch_gpu() {
  std::lock_guard lock(gate_);
  for (auto &channel : channels_)
    if (channel->dispatch_gpu())
      return true;
  return false;
}
void Hub::pump() {
  std::lock_guard lock(gate_);
  for (auto &channel : channels_)
    channel->pump();
}
void Hub::close() {
  std::lock_guard lock(gate_);
  ended_ = true;
  for (auto &channel : channels_)
    channel->close();
}
Json Hub::metrics() const {
  std::lock_guard lock(gate_);
  Json result = Json::array();
  for (auto &channel : channels_)
    result.push_back(channel->metrics());
  return result;
}
} // namespace nss::preview
