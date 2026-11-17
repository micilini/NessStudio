#include "dshow_source.hpp"
#include "dshow_timestamp.hpp"
#include <wincodec.h>
#include <cstring>
#include <future>
#include <limits>

namespace nss::camera::detail {
namespace {
void jpeg(IWICImagingFactory *factory,const VideoFormat &format,
          std::span<const std::byte> input,std::span<std::byte> output) {
  if(input.size()<4 || input.size()>format.maximum_bytes)
    throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,VFW_E_INVALIDMEDIATYPE,"Camera JPEG sample exceeds its negotiated byte budget");
  ComPtr<IWICStream> stream;
  checked(factory->CreateStream(&stream),"Camera JPEG stream allocation failed");
  checked(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(const_cast<std::byte *>(input.data())),static_cast<DWORD>(input.size())),
    "Camera JPEG stream initialization failed");
  ComPtr<IWICBitmapDecoder> decoder;
  // Explicit system JPEG decoder. Do not discover arbitrary installed codecs.
  checked(CoCreateInstance(CLSID_WICJpegDecoder,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&decoder)),"Windows JPEG decoder is unavailable");
  checked(decoder->Initialize(stream.Get(),WICDecodeMetadataCacheOnDemand),"Camera MJPEG payload is invalid");
  ComPtr<IWICBitmapFrameDecode> frame;checked(decoder->GetFrame(0,&frame),"Camera JPEG frame is unavailable");
  UINT width{},height{};checked(frame->GetSize(&width,&height),"Camera JPEG geometry is unavailable");
  if(width!=format.width || height!=format.height)
    throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,VFW_E_INVALIDMEDIATYPE,"Camera JPEG silently changed dimensions");
  ComPtr<IWICFormatConverter> converter;
  checked(factory->CreateFormatConverter(&converter),"Camera JPEG BGRA converter allocation failed");
  checked(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),
    "Camera JPEG BGRA conversion is unavailable");
  checked(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(output.size()),reinterpret_cast<BYTE *>(output.data())),
    "Camera JPEG pixel decode failed");
}
}
SampleReceiver::SampleReceiver(Plan plan,const VideoFormat &format)
  :plan_(std::move(plan)),format_(format),raw_(4,format.maximum_bytes),decoded_(3,size_t(format.width)*format.height*4) {
  ready_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  if(!ready_) checked(HRESULT_FROM_WIN32(GetLastError()),"Camera delivery event is unavailable");
  try {worker_=std::thread([this] {process();});}
  catch(...) {CloseHandle(ready_);throw;}
}
SampleReceiver::~SampleReceiver() {close();CloseHandle(ready_);}
void SampleReceiver::fail_locked(HRESULT status,const char *message) noexcept {
  if(FAILED(failure_) || closing_) return;
  failure_=FAILED(status)?status:E_FAIL;
  try {failure_message_=message;}catch(...) {failure_message_.clear();}
  changed_.notify_all();SetEvent(ready_);
}
void SampleReceiver::fail(HRESULT status,const char *message) noexcept {
  std::lock_guard lock(gate_);fail_locked(status,message);
}
HRESULT SampleReceiver::run(REFERENCE_TIME start) noexcept {
  std::lock_guard lock(gate_);
  if(closing_) return VFW_E_WRONG_STATE;
  if(start<=0 || (samples_ && stream_zero_!=start)) {
    fail_locked(VFW_E_SAMPLE_TIME_NOT_SET,"Camera graph changed its QPC reference origin");return failure_;
  }
  stream_zero_=start;return S_OK;
}
HRESULT SampleReceiver::receive(IMediaSample *sample) noexcept {
  if(!sample) return E_POINTER;
  // Timestamp before our queue lock, media inspection, copies or conversion.
  int64_t arrival{};
  try {arrival=qpc_hns();} catch(...) {fail(E_FAIL,"Camera callback QPC unavailable");return E_FAIL;}
  std::lock_guard lock(gate_);
  ++callbacks_;
  if(closing_) return S_FALSE;
  if(FAILED(failure_)) return failure_;
  try {
    AM_MEDIA_TYPE *change{};
    const auto change_status=sample->GetMediaType(&change);
    if(change) {
      bool valid{};
      try {const auto actual=video_format(*change);valid=same_format(format_,actual) && actual.maximum_bytes<=format_.maximum_bytes;}
      catch(...) {free_type(change);throw;}
      free_type(change);
      if(!valid) throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,VFW_E_INVALIDMEDIATYPE,"Camera changed format during recording");
    }
    checked(change_status,"Camera sample media type is unavailable");
    if(sample->IsPreroll()==S_OK) return S_OK;
    REFERENCE_TIME begin{},end{};
    checked(sample->GetTime(&begin,&end),"Camera sample has no stream timestamp");
    const auto timing=camera_timestamp(plan_.timestamp_mode,stream_zero_,begin,arrival,
                                      samples_!=0,last_raw_timestamp_,last_arrival_);
    const auto timestamp=timing.capture_qpc;
    const auto size=sample->GetActualDataLength(),capacity=sample->GetSize();
    if(size<static_cast<LONG>(format_.minimum_bytes) || size>capacity || size>static_cast<LONG>(format_.maximum_bytes))
      throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,VFW_E_BUFFER_UNDERFLOW,"Camera sample is shorter than its layout or exceeds its byte budget");
    BYTE *pixels{};checked(sample->GetPointer(&pixels),"Camera sample pointer is unavailable");
    if(!pixels) throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,E_POINTER,"Camera sample contains a null pixel pointer");
    ++samples_;raw_bytes_+=static_cast<uint32_t>(size);
    if(sample->IsDiscontinuity()==S_OK) ++discontinuities_;
    if(samples_==1) {first_timestamp_=timestamp;first_arrival_=arrival;first_raw_timestamp_=begin;}
    last_timestamp_=timestamp;last_arrival_=arrival;last_raw_timestamp_=begin;
    max_delay_=std::max(max_delay_,arrival-timing.graph_projection);
    maximum_raw_future_=std::max(maximum_raw_future_,timing.graph_projection-arrival);
    if(timing.graph_projection-arrival>hns_per_second || arrival-timing.graph_projection>5*hns_per_second)
      ++uncorrelated_raw_samples_;
    if(timestamp_count_<16 || samples_%300==0) {
      size_t index=timestamp_count_;
      if(index==timestamps_.size()) {
        std::move(timestamps_.begin()+17,timestamps_.end(),timestamps_.begin()+16);index=31;
      } else ++timestamp_count_;
      timestamps_[index]={static_cast<int64_t>(samples_),timestamp,arrival,begin,timing.graph_projection};
    }
    auto bytes=raw_.acquire();
    if(!bytes || count_==queue_.size()) {++raw_drops_;return S_OK;}
    std::memcpy(bytes->writable().data(),pixels,static_cast<size_t>(size));bytes->seal();
    queue_[(head_+count_)%queue_.size()].emplace(Input{std::move(*bytes),static_cast<size_t>(size),samples_,generation_,timestamp,arrival});
    ++count_;peak_=std::max(peak_,count_);changed_.notify_one();return S_OK;
  } catch(const capture::Error &e) {fail_locked(e.native,e.what());}
    catch(const std::bad_alloc &) {fail_locked(E_OUTOFMEMORY,"Camera packet handoff allocation failed");}
    catch(...) {fail_locked(E_UNEXPECTED,"Camera packet handoff failed");}
  return failure_;
}
void SampleReceiver::flush() noexcept {
  std::lock_guard lock(gate_);
  for(auto &entry:queue_) entry.reset();head_=count_=0;++generation_;latest_.reset();
}
void SampleReceiver::end_stream() noexcept {fail(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED),"Mandatory camera source ended or disconnected");}
void SampleReceiver::close() noexcept {
  {std::lock_guard lock(gate_);closing_=true;changed_.notify_all();SetEvent(ready_);}
  if(worker_.joinable()) worker_.join();
  flush();
}
void SampleReceiver::process() noexcept {
  try {
    ComRuntime runtime;
    ComPtr<IWICImagingFactory> factory;
    if(format_.pixels==Pixels::Mjpg)
      checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Windows JPEG conversion factory is unavailable");
    for(;;) {
      std::optional<Input> input;
      {
        std::unique_lock lock(gate_);changed_.wait(lock,[&] {return closing_ || FAILED(failure_) || count_;});
        if(closing_ || FAILED(failure_)) return;
        input=std::move(queue_[head_]);queue_[head_].reset();head_=(head_+1)%queue_.size();--count_;
      }
      auto target=decoded_.acquire();
      if(!target) {std::lock_guard lock(gate_);++decoded_drops_;continue;}
      const auto bytes=input->bytes.view().first(input->size);
      if(factory) {
        try {jpeg(factory.Get(),format_,bytes,target->writable());}
        catch(const capture::Error &error) {
          std::lock_guard lock(gate_);
          if(closing_) return;
          // Some UVC cameras send a partial JPEG while the stream starts. Wait
          // for real first content before Ready, without inventing pixels or
          // changing source time. Corruption after first content remains fatal.
          const bool malformed=error.native==WINCODEC_ERR_BADHEADER ||
            error.native==WINCODEC_ERR_BADIMAGE || error.native==WINCODEC_ERR_STREAMREAD;
          if(!malformed || frames_ || warmup_rejected_>=8 ||
             input->arrival-stream_zero_>hns_per_second) throw;
          ++warmup_rejected_;warmup_error_=error.native;warmup_error_bytes_=input->size;
          SetEvent(ready_);continue;
        }
      }
      else convert_raw(format_,bytes,target->writable());
      target->seal();
      capture::Frame frame;frame.surface=std::make_shared<CpuSurface>(std::move(*target));
      frame.id=input->id;frame.generation=input->generation;
      frame.begin_hns=frame.end_hns=frame.capture_hns=input->timestamp;
      frame.width=format_.width;frame.height=format_.height;frame.stride=format_.width*4;
      frame.format=capture::Frame::Format::Bgra;
      // Neither the delivery boundary nor a driver stream timestamp measures
      // the optical exposure boundary of an arbitrary USB camera.
      frame.estimated=true;
      {
        std::lock_guard lock(gate_);
        if(closing_ || input->generation!=generation_) continue;
        if(latest_) ++mailbox_drops_;
        latest_=std::move(frame);++frames_;SetEvent(ready_);
      }
    }
  } catch(const capture::Error &e) {fail(e.native,e.what());}
    catch(const std::bad_alloc &) {fail(E_OUTOFMEMORY,"Camera pixel worker exhausted memory");}
    catch(...) {fail(E_UNEXPECTED,"Camera pixel worker failed");}
}
std::optional<capture::Frame> SampleReceiver::poll() {
  std::lock_guard lock(gate_);
  if(FAILED(failure_)) throw capture::Error(NSS_E_IO,NSS_STAGE_CAPTURE,failure_,failure_message_.c_str());
  if(closing_) return {};
  if(last_arrival_ && qpc_hns()-last_arrival_>delivery_inactivity_hns)
    throw capture::Error(NSS_E_TIMEOUT,NSS_STAGE_CAPTURE,0,"Camera stopped delivering samples for two seconds");
  auto result=std::move(latest_);latest_.reset();return result;
}
Json SampleReceiver::metrics() const {
  std::lock_guard lock(gate_);Json timestamps=Json::array();
  for(size_t i=0;i<timestamp_count_;++i) timestamps.push_back({{"id",timestamps_[i][0]},
    {"captureQpcHns",timestamps_[i][1]},{"callbackQpcHns",timestamps_[i][2]},
    {"rawStreamHns",timestamps_[i][3]},{"graphProjectedSampleHns",timestamps_[i][4]}});
  const auto raw=raw_.stats(),decoded=decoded_.stats();
  return {{"requested",plan_json(plan_)},{"acceptedNativeInput",format_json(format_)},
    {"negotiatedDecodedOutput",{{"subtype","RGB32"},{"width",format_.width},{"height",format_.height}}},
    {"cameraSourceOpens",1},{"sourceFrames",samples_},{"decodedFrames",frames_},{"sourceCpuBytes",raw_bytes_},
    {"cameraGpuToCpuReadbackBytes",0},{"cameraCpuToGpuUploadBytes",0},{"hardwareDecoding",false},
    {"decoderMemory","CPU BGRA; raw native conversion or explicit Windows JPEG decoder"},
    {"startupMalformedJpegSamples",warmup_rejected_},{"startupLastJpegError",warmup_error_},
    {"startupLastJpegBytes",warmup_error_bytes_},{"startupMalformedLimit",8},
    {"startupMalformedWindowMs",1000},{"corruptionAfterFirstFrameFatal",true},
    {"timestampMode",plan_.timestamp_mode==TimestampMode::DeliveryQpc?"DeliveryQpc":"StrictStreamQpc"},
    {"timestampContract",plan_.timestamp_mode==TimestampMode::DeliveryQpc?
      "Measured callback-entry QPC; raw device timestamps retained separately; no rate fit, offset or CFR retiming":
      "Explicit strict driver-clock contract: QPC IReferenceClock + Run origin + raw stream timestamp"},
    {"timestampSamplesVersion",2},{"rawTimestampClockCorrelationProven",false},
    {"exposureBoundaryMeasured",false},{"graphStreamZeroQpcHns",stream_zero_},
    {"firstTimestampHns",first_timestamp_},{"lastTimestampHns",last_timestamp_},
    {"firstCallbackHns",first_arrival_},{"lastCallbackHns",last_arrival_},{"maxCallbackDelayHns",max_delay_},
    {"callbackDelayMetricBasis","Graph-projected raw device timestamp; not measured exposure latency"},
    {"firstRawStreamHns",first_raw_timestamp_},{"lastRawStreamHns",last_raw_timestamp_},
    {"maximumRawFutureLeadHns",maximum_raw_future_},{"rawSamplesOutsideGraphClockWindow",uncorrelated_raw_samples_},
    {"timestampSamples",timestamps},{"observedFps",samples_>1 && last_arrival_>first_arrival_?
      double(samples_-1)*hns_per_second/double(last_arrival_-first_arrival_):0.0},
    {"rawTimestampFps",samples_>1 && last_raw_timestamp_>first_raw_timestamp_?
      double(samples_-1)*hns_per_second/double(last_raw_timestamp_-first_raw_timestamp_):0.0},
    {"latestMailboxDrops",mailbox_drops_},{"sourcePoolDrops",decoded_drops_},
    {"sourceDiscontinuities",discontinuities_},{"sourceStreamTicks",0},{"sourceHresult",failure_},
    {"sourceClosed",closing_},{"callbackCalls",callbacks_},
    {"rawHandoff",{{"slots",4},{"queued",count_},{"peak",peak_},{"drops",raw_drops_},{"bytes",raw.bytes}}},
    {"pool",{{"bytes",decoded.bytes},{"inUse",decoded.in_use},{"peak",decoded.peak}}}};
}
} // namespace nss::camera::detail
