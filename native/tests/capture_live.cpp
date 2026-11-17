#include "capture_fixture.hpp"
#include "capture_crash.hpp"
#include "runtime/time.hpp"
#ifndef NSS_CPU_CAPTURE_ONLY
#include "capture/readback.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#endif
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include "capture_api.hpp"
using namespace nss::capture;
using fixture::check;
#ifndef NSS_CPU_CAPTURE_ONLY
namespace nss::capture {
std::unique_ptr<Pipeline> make_gpu_for_test_window(const Plan &, void *);
void set_qualification_arrival(std::function<void()>);
void set_qualification_completion_event_failure(bool);
void set_qualification_fence_failure(bool);
void set_qualification_shader_view_failure(bool);
void set_qualification_compute_view_failure(bool);
void set_qualification_readback_packed_failure(bool);
void set_qualification_output5_unavailable(bool);
} // namespace nss::capture
#else
namespace nss::capture {
std::unique_ptr<Pipeline> make_gpu(const Plan &) {
    throw Error(NSS_E_UNAVAILABLE, NSS_STAGE_CAPTURE, 0,
                "GPU/WGC deliberately absent from this CPU-only qualification binary");
}
} // namespace nss::capture
#endif
namespace {
uint64_t inspection_bytes{}, inspection_calls{};
std::filesystem::path artifact_directory;
void artifact(const std::string &name, const std::vector<uint8_t> &bytes) {
    if (artifact_directory.empty())
        return;
    std::filesystem::create_directories(artifact_directory);
    const auto path = artifact_directory / name;
    check(!std::filesystem::exists(path), "Capture artifact already exists");
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(file), "Capture artifact write failed");
}
Frame next(Pipeline &source, uint64_t after = 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto frame = source.poll();
        source.flush_work();
        if (frame && frame->id > after)
            return std::move(*frame);
        check(std::chrono::steady_clock::now() < deadline, "No completed captured frame before deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
std::vector<uint8_t> inspect(const Frame &frame) {
    if (const auto pixels = frame.surface->cpu_pixels(); !pixels.empty())
        return {reinterpret_cast<const uint8_t *>(pixels.data()),
                reinterpret_cast<const uint8_t *>(pixels.data()) + pixels.size()};
#ifndef NSS_CPU_CAPTURE_ONLY
    using Microsoft::WRL::ComPtr;
    auto *texture = static_cast<ID3D11Texture2D *>(frame.surface->gpu_texture());
    check(texture && frame.surface->gpu_device(), "GPU lease lacks texture/device descriptor");
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    check(device.Get() == frame.surface->gpu_device(), "GPU lease device differs from texture owner");
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    check(desc.Format == DXGI_FORMAT_NV12 && desc.Width == frame.width && desc.Height == frame.height &&
              desc.Usage == D3D11_USAGE_DEFAULT,
          "GPU output is not an owned default NV12 texture");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    checked(device->CreateTexture2D(&desc, nullptr, &staging), NSS_STAGE_PROCESS,
            "Diagnostic staging allocation failed");
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> done;
    checked(device->CreateQuery(&query, &done), NSS_STAGE_PROCESS, "Diagnostic query failed");
    context->CopyResource(staging.Get(), texture);
    context->End(done.Get());
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        BOOL ready{};
        const auto status =
            context->GetData(done.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        checked(status, NSS_STAGE_PROCESS, "Diagnostic completion failed");
        if (status == S_OK && ready)
            break;
        check(std::chrono::steady_clock::now() < deadline, "Diagnostic GPU wait timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), NSS_STAGE_PROCESS,
            "Diagnostic-only Map failed");
    struct unmap {
        ID3D11DeviceContext *context;
        ID3D11Resource *resource;
        ~unmap() { context->Unmap(resource, 0); }
    } release{context.Get(), staging.Get()};
    check(mapped.RowPitch >= frame.width, "Diagnostic row pitch too small");
    std::vector<uint8_t> pixels(size_t(frame.width) * frame.height * 3 / 2);
    const auto *bytes = static_cast<const uint8_t *>(mapped.pData);
    for (uint32_t row = 0; row < frame.height + frame.height / 2; ++row)
        std::memcpy(pixels.data() + size_t(row) * frame.width, bytes + size_t(row) * mapped.RowPitch,
                    frame.width);
    ++inspection_calls;
    inspection_bytes += pixels.size();
    return pixels;
#else
    throw std::runtime_error("CPU-only binary received GPU pixels");
#endif
}
void bars(const Frame &frame, const std::vector<uint8_t> &pixels) {
    const uint8_t expected[8][3] = {{16, 128, 128}, {235, 128, 128}, {63, 102, 240}, {173, 42, 26},
                                    {32, 240, 118}, {219, 16, 138},  {188, 154, 16}, {78, 214, 230}};
    for (uint32_t i = 0; i < 8; ++i) {
        const auto x = static_cast<uint32_t>(uint64_t(frame.width) * (i * 2 + 1) / 16) & ~1u;
        // The 320x240 window places its diagnostic text at source row 80.
        // Sample the middle of the known bars, outside that opaque text and
        // the lower grayscale ramp, for both window and desktop fixtures.
        const auto y = (frame.height / 2) & ~1u;
        const auto at = size_t(y) * frame.width + x,
                   uv = size_t(frame.width) * frame.height + size_t(y / 2) * frame.width + x;
        if (abs(int(pixels[at]) - expected[i][0]) > 3 || abs(int(pixels[uv]) - expected[i][1]) > 3 ||
            abs(int(pixels[uv + 1]) - expected[i][2]) > 3)
            throw std::runtime_error("Real capture BT709 bar " + std::to_string(i) +
                " differs at " + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                ": YUV=" + std::to_string(pixels[at]) + "," + std::to_string(pixels[uv]) +
                "," + std::to_string(pixels[uv + 1]));
    }
}
Json live(bool gpu, const Display &display, fixture::Window &window) {
    window.pattern(0);
    auto config = plan(Json{{"version", 1},
                            {"mode", gpu ? "RequireGpuResidentHardware" : "FullSoftware"},
                            {"cursor", false},
                            {"slots", 3},
                            {"trace", true},
                            {"output", {{"width", 640}, {"height", 400}}}});
    Json result;
    std::vector<Frame> held;
    auto source = gpu ? make_gpu(config) : make_cpu(config);
    for (int i = 0; i < 3; ++i)
        held.push_back(next(*source));
    auto pixels = inspect(held[0]);
    bars(held[0], pixels);
    artifact(gpu ? "gpu-bars-640x400.nv12" : "cpu-bars-640x400.nv12", pixels);
    artifact(gpu ? "gpu-owned-reference-1920x1200.bgra" : "cpu-owned-reference-1920x1200.bgra",
             window.reference());
    check(held[0].estimated != gpu, "Source timestamp precision was misclassified");
    if (gpu)
        check(held[0].surface->adapter_luid() == display.adapter_luid, "Frame adapter identity mismatch");
    const auto retained = inspect(held[0]);
    for (int i = 1; i <= 6; ++i) {
        window.pattern(0, static_cast<uint32_t>(i));
        for (int p = 0; p < 20; ++p) {
            (void)source->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    check(inspect(held[0]) == retained, "Slow consumer's retained frame was overwritten");
    result["pressureBeforeRelease"] = source->metrics();
    check((gpu ? source->metrics().at("poolPressureDrops") : source->metrics().at("outputPoolPressureDrops"))
                  .get<uint64_t>() > 0,
          "Small output pool did not apply backpressure");
    held.erase(held.begin() + 1, held.end());
    auto advanced = next(*source, held[0].id);
    check(advanced.id > held[0].id, "Capture did not recover after lease release");
    const auto numbered = inspect(advanced);
    for (uint32_t bit = 0; bit < 16; ++bit) {
        const auto x = static_cast<uint32_t>(config.viewport.x + int64_t(bit * 24 + 12) *
                                                                     config.viewport.width /
                                                                     display.bounds.width),
                   y = static_cast<uint32_t>(config.viewport.y +
                                             int64_t(48) * config.viewport.height / display.bounds.height);
        const int expected = (6u & (1u << bit)) ? 235 : 16;
        check(abs(int(numbered[size_t(y) * advanced.width + x]) - expected) <= 3,
              "Frame identity advanced without the latest numbered pixel content");
    }
    advanced.surface.reset();
    source->close();
    source.reset();
    check(inspect(held[0]) == retained, "Capture stop invalidated an owned frame");
    held.clear();
    window.pattern(1);
    config.crop = {96, 128, 333, 241};
    config.full_source = false;
    config.requested_width = 333;
    config.requested_height = 241;
    config.width = 334;
    config.height = 242;
    config.pad_even = true;
    config.viewport = fit_viewport(333, 241, 334, 242);
    source = gpu ? make_gpu(config) : make_cpu(config);
    auto odd = next(*source);
    const auto cropped = inspect(odd);
    const auto chroma = size_t(odd.width) * odd.height;
    for (size_t i = chroma; i < cropped.size(); ++i)
        check(abs(int(cropped[i]) - 128) <= 3, "Odd crop included the exterior magenta marker");
    const auto center = size_t(odd.height / 2) * odd.width + odd.width / 2;
    check(abs(int(cropped[center]) - 235) <= 3, "Odd crop lost the white selected content");
    const auto viewport = config.viewport;
    for (uint32_t y = 0; y < odd.height; ++y)
        for (uint32_t x = 0; x < odd.width; ++x)
            if (x >= static_cast<uint32_t>(viewport.x + viewport.width) ||
                y >= static_cast<uint32_t>(viewport.y + viewport.height))
                check(abs(int(cropped[size_t(y) * odd.width + x]) - 16) <= 3,
                      "Output padding was not limited-range black");
    artifact(gpu ? "gpu-odd-334x242.nv12" : "cpu-odd-334x242.nv12", cropped);
    odd.surface.reset();
    source->close();
    result["cropMetrics"] = source->metrics();
    source.reset();
    result["status"] = "PASS";
    result["route"] = gpu ? "GPU" : "FullSoftware";
    result["display"] = display_json(display);
    result["barsVerified"] = 8;
    result["oddCropOutsideMarkerExcluded"] = true;
    result["slowConsumerStable"] = true;
    result["leaseSurvivedOwner"] = true;
    return result;
}
Json cpu_lifetime(const Plan &config) {
    const auto start = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 20; ++i) {
        auto source = make_cpu(config);
        auto frame = next(*source);
        source->close();
        source.reset();
        check(!frame.surface->cpu_pixels().empty(), "CPU lease lost its backing pool");
    }
    const auto end = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    check(end <= start + 2, "GDI handles leaked across capture cycles");
    {
        auto raw = std::make_unique<GdiSource>(config);
        std::vector<GdiSource::Image> images;
        for (uint32_t i = 0; i < config.slots; ++i) {
            auto image = raw->acquire();
            check(image.has_value(), "DIB pool did not deliver initial slots");
            images.push_back(std::move(*image));
        }
        check(!raw->acquire(), "DIB pool overwrote borrowed pixels");
        const auto first = images[0].pixels.data[0];
        raw.reset();
        check(images[0].pixels.data[0] == first, "DIB lease did not outlive source");
    }
    return {
        {"cycles", 20}, {"gdiBefore", start}, {"gdiAfter", end}, {"rawDibPressureAndOwnerLifetime", true}};
}
Json cpu_deferred(const Display &display, fixture::Window &window) {
    auto config = plan(Json{{"version", 1}, {"mode", "FullSoftware"},
                            {"cursor", false}, {"slots", 3},
                            {"output", {{"width", 640}, {"height", 400}}}});
    config.defer_conversion = true;
    auto source = make_cpu(config);
    const auto queued_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (source->metrics().at("acquisitionQueued").get<size_t>() < 2) {
        check(std::chrono::steady_clock::now() < queued_deadline, "Ordered GDI queue did not fill");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        const auto first = next(*source, 0);
        const auto second = next(*source, first.id);
        check(first.id == 1 && second.id == 2 && first.end_hns <= second.begin_hns &&
                  source->metrics().at("acquisitionDecimation") == 0,
              "GDI overwrote an unexamined acquisition while its consumer was delayed");
    }
    Frame selected;
    uint64_t last_id{};
    for (uint32_t number = 0; number < 8; ++number) {
        window.pattern(0, number);
        auto raw = next(*source, last_id);
        last_id = raw.id;
        check(raw.format == Frame::Format::Bgra && raw.surface->cpu_pixels().empty(),
              "Unselected CPU acquisition exposed unprepared NV12");
        selected = std::move(raw);
    }
    check(source->metrics().at("converted") == 0,
          "Unselected CPU acquisitions performed unnecessary conversion");
    const auto begin = selected.begin_hns, end = selected.end_hns,
               timestamp = selected.capture_hns;
    auto shared_selection = selected;
    check(source->prepare_frame(selected), "Selected CPU conversion was not ready");
    check(selected.begin_hns == begin && selected.end_hns == end && selected.capture_hns == timestamp &&
              selected.width == 640 && selected.height == 400 && selected.stride == 640 &&
              selected.format == Frame::Format::Nv12,
          "Selected CPU conversion changed acquisition timing or geometry");
    const auto pixels = inspect(selected);
    bars(selected, pixels);
    for (int repetition = 0; repetition < 16; ++repetition)
        check(source->prepare_frame(shared_selection), "Shared CPU selection did not reuse its conversion");
    check(source->metrics().at("converted") == 1,
          "Reusing a CPU frame repeated its NV12 conversion");
    std::vector<Frame> retained{selected};
    for (uint32_t i = 1; i < config.slots; ++i) {
        auto frame = next(*source, last_id);
        last_id = frame.id;
        check(source->prepare_frame(frame), "CPU output pool exhausted before its budget");
        retained.push_back(std::move(frame));
    }
    auto waiting = next(*source, last_id);
    last_id = waiting.id;
    check(!source->prepare_frame(waiting) && waiting.format == Frame::Format::Bgra &&
              waiting.surface->cpu_pixels().empty(),
          "Selected CPU conversion overwrote a borrowed output slot");
    check(inspect(selected) == pixels, "CPU backpressure changed retained pixels");
    retained.pop_back();
    check(source->prepare_frame(waiting), "CPU conversion did not recover when an output lease returned");
    {
        auto foreign = make_cpu(config);
        bool rejected = false;
        try { (void)foreign->prepare_frame(selected); }
        catch (const Error &error) { rejected = error.code == NSS_E_ARGUMENT; }
        check(rejected, "A CPU pipeline accepted another pipeline's selection");
        foreign->close();
    }
    bool wrong_thread_rejected = false;
    std::thread foreign_thread([&] {
        try { (void)source->prepare_frame(shared_selection); }
        catch (const Error &error) { wrong_thread_rejected = error.code == NSS_E_STATE; }
    });
    foreign_thread.join();
    check(wrong_thread_rejected, "CPU preparation accepted a foreign thread");
    auto abandoned = next(*source, last_id);
    source->close();
    bool closed_rejected = false;
    try { (void)source->prepare_frame(abandoned); }
    catch (const Error &error) { closed_rejected = error.code == NSS_E_STATE; }
    check(closed_rejected, "CPU conversion started after capture closed");
    const auto metrics = source->metrics();
    check(metrics.at("converted") == 4 && metrics.at("outputPool").at("peak") == config.slots &&
              metrics.at("selectedConversionPressureRetries") == 1 && metrics.at("outputPoolPressureDrops") == 0 &&
              metrics.at("acquisitionThreadJoined"),
          "Deferred CPU conversion exceeded its pool or did not join acquisition");
    check(metrics.at("frames").get<uint64_t>() >= abandoned.id &&
              metrics.at("frames") == metrics.at("gdiFlushes") &&
              metrics.at("acquisitionQueued") == 0,
          "Final acquisition diagnostics lost completed frames or retained a cleared queue");
    source.reset();
    check(inspect(selected) == pixels && inspect(shared_selection) == pixels,
          "Converted CPU pixels did not survive their owner");
    return {{"status", "PASS"}, {"unselectedAcquisitions", 10}, {"selectedReuseChecks", 16},
            {"delayedConsumerPreservedOldestAcquisitions", true},
            {"barsVerified", 8}, {"originalAcquisitionIntervalPreserved", true},
            {"boundedOutputBackpressure", true}, {"foreignPipelineRejected", true},
            {"foreignThreadRejected", true}, {"closedConversionRejected", true},
            {"selectedLeaseSurvivedOwner", true}, {"pipeline", metrics}, {"display", display_json(display)}};
}
Json cursor(bool gpu, const Display &display, fixture::Window &window) {
    POINT original{};
    check(GetPhysicalCursorPos(&original) != 0, "Cursor position unavailable");
    CURSORINFO old{};
    old.cbSize = sizeof(old);
    check(GetCursorInfo(&old) != 0, "Original cursor unavailable");
    std::array<uint8_t, 32> mask{}, white{};
    white.fill(255);
    HCURSOR icon = CreateCursor(GetModuleHandleW(nullptr), 3, 5, 16, 16, mask.data(), white.data());
    check(icon != nullptr, "Owned cursor creation failed");
    struct restore {
        fixture::Window &window;
        POINT position;
        HCURSOR old, owned;
        ~restore() {
            window.cursor(old);
            SetPhysicalCursorPos(position.x, position.y);
            DestroyCursor(owned);
        }
    } cleanup{window, original, old.hCursor, icon};
    window.pattern(2);
    window.cursor(icon);
    check(SetPhysicalCursorPos(display.bounds.x + 160, display.bounds.y + 180) != 0,
          "Owned cursor placement failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto config = plan(Json{{"version", 1},
                            {"mode", gpu ? "RequireGpuResidentHardware" : "FullSoftware"},
                            {"cursor", true},
                            {"crop", {{"x", 96}, {"y", 128}, {"width", 128}, {"height", 128}}},
                            {"output", {{"width", 128}, {"height", 128}}}});
    auto source = gpu ? make_gpu(config) : make_cpu(config);
    Frame frame;std::vector<uint8_t> pixels;uint64_t bright=0,outside=0,observed=0;
    const auto cursor_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    for(;;){frame=next(*source);pixels=inspect(frame);bright=outside=0;++observed;
        for(uint32_t y=0;y<128;++y)for(uint32_t x=0;x<128;++x)if(pixels[y*128+x]>200){++bright;if(x<61||x>=77||y<47||y>=63)++outside;}
        if(bright==256&&outside==0)break;
        if(std::chrono::steady_clock::now()>=cursor_deadline)throw std::runtime_error("Cursor did not converge to its exact shape/hotspot: white="+std::to_string(bright)+" outside="+std::to_string(outside));
        frame.surface.reset();
    }
    artifact(gpu ? "gpu-cursor-128x128.nv12" : "cpu-cursor-128x128.nv12", pixels);
    frame.surface.reset();
    source->close();
    return {{"status", "PASS"},
            {"whitePixels", bright},
            {"cursorFramesObserved",observed},
            {"outsideExpectedHotspot", outside},
            {"physicalPositionX", display.bounds.x + 160},
            {"physicalPositionY", display.bounds.y + 180}};
}
#ifndef NSS_CPU_CAPTURE_ONLY
Frame next_without_flush(Pipeline &source, uint64_t after) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        auto frame = source.poll();
        if (frame && frame->id > after) return std::move(*frame);
        check(std::chrono::steady_clock::now() < deadline,
              "No duplicated acquisition before the poll-only deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
Json deferred_selection(const Plan &config, const std::function<void(uint32_t)> &paint,
                        bool verify_bars) {
    auto source = make_gpu(config);
    Frame selected;
    uint64_t last_id{};
    for (uint32_t number = 0; number < 32; ++number) {
        if (paint) paint(number);
        auto raw = config.desktop_duplication && number == 31
            ? next_without_flush(*source, last_id) : next(*source, last_id);
        last_id = raw.id;
        check(raw.format == Frame::Format::Bgra && !raw.surface->gpu_texture(),
              "Unselected acquisition exposed an unprepared NV12 texture");
        selected = std::move(raw);
    }
    const auto before = source->metrics();
    if (config.desktop_duplication)
        check(before.at("gpuCopies") == 32 && before.at("converted") == 0 &&
                  before.at("acquisitionsReturnedAfterCopySubmission") == 31 &&
                  before.at("copiedDesktopReturnsBeforeAcquire").get<uint64_t>() +
                      before.at("copiedDesktopReturnsAfterCompletion").get<uint64_t>() == 31 &&
                  before.at("ownerBatchFlushes") == 31 &&
                  before.at("heldDesktopAcquisition") == true &&
                  before.at("retainedUnreturnedAcquisitions") == 1 &&
                  before.at("gpuCompletionEventRegistrations") == 0 &&
                  !source->wait_hint().processing,
              "Duplicated capture did not retain exactly its last acquisition or converted unselected frames");
    else check(before.at("gpuCopies") == 0 && before.at("converted") == 0 &&
              before.at("acquisitionsReturnedWithoutGpuCopy").get<uint64_t>() >= 30 &&
              before.at("retainedUnreturnedAcquisitions").get<uint64_t>() <= 2,
          "Unselected acquisitions copied pixels or exhausted the source pool");
    const auto timestamp = selected.capture_hns;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    check(!source->prepare_frame(selected), "New selected conversion unexpectedly published unfinished output");
    const auto submitted = source->metrics();
    if(config.desktop_duplication && submitted.at("device").at("timelineFenceSupported")) {
        check(submitted.at("timelineFenceSignals")==31 &&
              submitted.at("timelineFenceReservedValues")==32 &&
              submitted.at("timelineFenceCoalescedMarkers")==1 &&
              submitted.at("timelineFenceUnsubmittedValue")==32,
              "Copy and selected conversion did not retain one unsubmitted completion marker");
        // Even if the driver submits other work implicitly, this reserved
        // completion value cannot publish the unflushed conversion.
        for(int attempt=0;attempt<4;++attempt)
            check(!source->prepare_frame(selected) && !selected.surface->gpu_texture(),
                  "An unsubmitted batch marker was treated as GPU completion");
    }
    const uint64_t selected_copies = config.desktop_duplication ? 32 : 1;
    check(submitted.at("acquisitionsReturnedAfterCopySubmission") ==
              selected_copies - (config.desktop_duplication ? 1 : 0) &&
              (config.desktop_duplication ? submitted.at("retainedUnreturnedAcquisitions") == 1 :
              submitted.at("retainedUnreturnedAcquisitions").get<uint64_t>() + 1 ==
                  before.at("retainedUnreturnedAcquisitions").get<uint64_t>()) &&
              !selected.surface->gpu_texture(),
          "Selected source ownership changed unexpectedly or exposed unfinished NV12");
    // Repaint the source before explicitly flushing this owner's queued copy.
    // Old source numbers are below 32; the new frames all set marker bit six.
    // The selected image must retain the old bit despite subsequent repaints.
    if (paint && verify_bars)
        for (uint32_t number = 64; number < 72; ++number) paint(number);
    source->flush_work();
    if (config.desktop_duplication)
        check(source->metrics().at("ownerBatchFlushes") == 32 &&
                  source->metrics().at("heldDesktopAcquisition") == true,
              "Owned copy and conversion did not share a submission while retaining the last desktop");
    while (!source->prepare_frame(selected)) {
        source->flush_work();
        check(std::chrono::steady_clock::now() < deadline, "Selected conversion did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(selected.capture_hns == timestamp && selected.width == config.width &&
              selected.height == config.height && selected.format == Frame::Format::Nv12,
          "Selected conversion changed the source timestamp or output geometry");
    const auto pixels = inspect(selected);
    if (verify_bars) {
        if (paint) {
            const auto x = uint64_t(6 * 24 + 12) * selected.width / config.crop.width;
            const auto y = uint64_t(48) * selected.height / config.crop.height;
            check(x < selected.width && y < selected.height &&
                      pixels[size_t(y) * selected.width + size_t(x)] < 48,
                  "Returned source changed the selected pixels before queued copy completed");
        }
        artifact("deferred-selected-" + std::to_string(selected.width) + "x" +
                 std::to_string(selected.height) + ".nv12", pixels);
        bars(selected, pixels);
    }
    source->retire_completed_work();
    check(source->metrics().at("retainedUnreturnedAcquisitions") == 0,
          "Post-wakeup retirement changed the bounded source ownership");
    for (int repetition = 0; repetition < 16; ++repetition)
        check(source->prepare_frame(selected), "Prepared frame ceased to be ready");
    check(source->metrics().at("converted") == 1 && source->metrics().at("gpuCopies") == selected_copies,
          "Reusing a selected immutable frame repeated its copy or conversion");
    if (paint) paint(99);
    auto abandoned = next(*source, selected.id);
    (void)source->prepare_frame(abandoned);
    abandoned.surface.reset();
    if (paint) paint(100);
    auto unused_at_close = config.desktop_duplication
        ? next_without_flush(*source, last_id) : next(*source, last_id);
    if (config.desktop_duplication)
        check(source->metrics().at("heldDesktopAcquisition") == true &&
                  source->metrics().at("inflightCopies").get<uint64_t>() > 0,
              "Close test did not leave an unflushed owned copy and one held desktop");
    source->close();
    const auto metrics = source->metrics();
    const uint64_t final_copies = config.desktop_duplication ? 34 : 2;
    check(metrics.at("timelineFenceUnsubmittedValue")==0 &&
          metrics.at("timelineFenceSignals")==metrics.at("timelineFenceReservedValues"),
          "Capture close stranded a reserved completion marker");
    check(metrics.at("inflightCopies") == 0 && metrics.at("inflightConversions") == 0 &&
              metrics.at("retainedUnreturnedAcquisitions") == 0 &&
              metrics.at("gpuCopies") == final_copies && metrics.at("converted") == 2 &&
              metrics.at("acquisitionsReturnedAfterCopySubmission") == final_copies,
          "Abandoned or unselected acquisitions did not quiesce during close");
    if (config.desktop_duplication)
        check(metrics.at("heldDesktopAcquisition") == false,
              "Close retained the last borrowed desktop");
    source.reset();
    check(inspect(selected) == pixels, "Selected GPU lease did not survive its owner");
    selected.surface.reset();
    unused_at_close.surface.reset();
    return {{"status", "PASS"}, {"unselectedAcquisitions", 32},
            {"selectedReuseChecks", 16}, {"barsVerified", verify_bars ? 8 : 0},
            {"originalTimestampPreserved", true}, {"abandonedConversionQuiesced", true},
            {"sourceReturnedBeforeNv12Publication", !config.desktop_duplication},
            {"desktopReturnedAfterCopyCompletionOrNextAcquisition", config.desktop_duplication},
            {"unflushedOwnedCopyDrainedDuringClose", config.desktop_duplication},
            {"selectedPixelsSurvivedSourceRepaints", bool(paint) && verify_bars},
            {"selectedLeaseSurvivedOwner", true}, {"pipeline", metrics}};
}
Json duplication_completion_return(fixture::Window &desktop) {
    auto config = plan(Json{{"version", 1}, {"cursor", false}, {"slots", 6},
        {"sourceApi", "DesktopDuplication"}, {"output", {{"width", 1280}, {"height", 800}}}});
    config.defer_conversion = true;
    auto source = make_gpu(config);
    desktop.pattern(0, 1);
    auto first = next_without_flush(*source, 0);
    const auto before = source->metrics();
    check(before.at("heldDesktopAcquisition") == true && before.at("gpuCopies") == 1 &&
              before.at("acquisitionsReturnedAfterCopySubmission") == 0 &&
              before.at("ownerBatchFlushes") == 0,
          "Copied desktop was returned before the next acquisition request");
    const auto first_timestamp = first.capture_hns;
    desktop.pattern(0, 99);
    auto second = next_without_flush(*source, first.id);
    const auto turned = source->metrics();
    check(turned.at("heldDesktopAcquisition") == true && turned.at("gpuCopies") == 2 &&
              turned.at("acquisitionsReturnedAfterCopySubmission") == 1 &&
              turned.at("ownerBatchFlushes") == 0,
          "Next acquisition did not return exactly the preceding copied desktop");
    const auto second_timestamp = second.capture_hns;
    check(second_timestamp > first_timestamp, "Held desktop prevented a newer source acquisition");
    source->retire_completed_work();
    // An unsignalled reserved fence cannot complete. Event queries may finish
    // after an implicit driver submission, even before our explicit Flush;
    // their correctness is checked by the retained pixels and final returns.
    if (before.at("device").at("timelineFenceSupported"))
        check(source->metrics().at("heldDesktopAcquisition") == true &&
                  source->metrics().at("acquisitionsReturnedAfterCopySubmission") == 1,
              "Unsubmitted fence marker was mistaken for completed GPU work");
    const auto prepare = [&](Frame &frame) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!source->prepare_frame(frame)) {
            source->flush_work();
            check(std::chrono::steady_clock::now() < deadline, "Held desktop conversion timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    prepare(first);
    prepare(second);
    const auto old_pixels = inspect(first), new_pixels = inspect(second);
    bars(first, old_pixels); bars(second, new_pixels);
    const auto x = uint64_t(6 * 24 + 12) * first.width / config.crop.width;
    const auto y = uint64_t(48) * first.height / config.crop.height;
    check(old_pixels[size_t(y) * first.width + size_t(x)] < 48 &&
              new_pixels[size_t(y) * second.width + size_t(x)] > 200 &&
              first.capture_hns == first_timestamp && second.capture_hns == second_timestamp,
          "Returning a desktop before explicit flush corrupted queued pixels or source time");
    source->retire_completed_work();
    check(source->metrics().at("heldDesktopAcquisition") == false &&
              source->metrics().at("copiedDesktopReturnsAfterCompletion") == 1 &&
              source->metrics().at("acquisitionsReturnedAfterCopySubmission") == 2,
          "Completed GPU copy retained the desktop until another acquisition");
    source->close();
    const auto result = source->metrics();
    check(result.at("heldDesktopAcquisition") == false &&
              result.at("acquisitionsReturnedAfterCopySubmission") == 2 &&
              result.at("inflightCopies") == 0 && result.at("inflightConversions") == 0 &&
              result.at("timelineFenceUnsubmittedValue") == 0,
          "Capture close retained the final borrowed desktop or unfinished work");
    source.reset();
    check(inspect(first) == old_pixels && inspect(second) == new_pixels,
          "Held desktop invalidated owned images after close");
    return {{"status", "PASS"}, {"turnoverBeforeExplicitFlush", true},
            {"oldAndNewMarkerPixelsVerified", true}, {"originalTimestampsPreserved", true},
            {"lastAcquisitionReleasedAfterGpuCompletion", true}, {"ownedLeasesSurvivedClose", true},
            {"pipeline", result}};
}
Json duplication_window_filter(fixture::Window &desktop) {
    auto config = plan(Json{{"version", 1}, {"cursor", false}, {"slots", 6},
        {"sourceApi", "DesktopDuplication"}, {"output", {{"width", 1280}, {"height", 800}}}});
    config.defer_conversion = true;
    auto source = make_gpu(config);
    desktop.pattern(0, 1);
    auto retained = next(*source);
    const auto prepare = [&](Frame &frame) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!source->prepare_frame(frame)) {
            source->flush_work();
            check(std::chrono::steady_clock::now() < deadline, "Filtered capture conversion timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    prepare(retained);
    const auto original = inspect(retained);
    bars(retained, original);
    const auto before = source->metrics();
    // Deliberately place the hint beyond all real timestamps. This exercises
    // returning eight genuine desktop acquisitions without copying or marking
    // GPU work; a retained older output must remain immutable throughout.
    for (uint32_t number = 64; number < 72; ++number) {
        const auto count = source->metrics().at("servedWindowAcquisitionsReturnedWithoutCopy").get<uint64_t>();
        desktop.pattern(0, number);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            check(!source->poll_after((std::numeric_limits<int64_t>::max)()),
                  "Already served source image was delivered despite the filter");
            source->flush_work();
            check(std::chrono::steady_clock::now() < deadline, "No real acquisition exercised the copy filter");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (source->metrics().at("servedWindowAcquisitionsReturnedWithoutCopy").get<uint64_t>() == count);
    }
    const auto rejected = source->metrics();
    check(rejected.at("servedWindowAcquisitionsReturnedWithoutCopy").get<uint64_t>() >= 8 &&
              rejected.at("acquisitionsReturnedWithoutGpuCopy") == rejected.at("servedWindowAcquisitionsReturnedWithoutCopy") &&
              rejected.at("gpuCopies") == before.at("gpuCopies") &&
              rejected.at("converted") == before.at("converted") &&
              rejected.at("ownerBatchFlushes") == before.at("ownerBatchFlushes") &&
              rejected.at("timelineFenceReservedValues") == before.at("timelineFenceReservedValues") &&
              rejected.at("heldDesktopAcquisition") == false,
          "Rejected desktop images copied pixels, submitted work or retained a borrowed resource");
    const auto minimum = nss::qpc_hns();
    desktop.pattern(0, 99);
    std::optional<Frame> accepted;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!(accepted = source->poll_after(minimum))) {
        source->flush_work();
        check(std::chrono::steady_clock::now() < deadline, "A newer eligible source image was suppressed");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(accepted->capture_hns > minimum && accepted->id > retained.id,
          "Copy filtering changed source time or reused an old identity");
    const auto timestamp = accepted->capture_hns;
    prepare(*accepted);
    const auto pixels = inspect(*accepted);
    bars(*accepted, pixels);
    const auto x = uint64_t(6 * 24 + 12) * accepted->width / config.crop.width;
    const auto y = uint64_t(48) * accepted->height / config.crop.height;
    check(original[size_t(y) * retained.width + size_t(x)] < 48 &&
              pixels[size_t(y) * accepted->width + size_t(x)] > 200 && accepted->capture_hns == timestamp,
          "Filtered turnover corrupted retained pixels or failed to capture the new marker");
    source->close();
    const auto result = source->metrics();
    check(result.at("gpuCopies") == 2 && result.at("converted") == 2 &&
              result.at("inflightCopies") == 0 && result.at("inflightConversions") == 0 &&
              result.at("heldDesktopAcquisition") == false && result.at("timelineFenceUnsubmittedValue") == 0,
          "Filtered capture did not drain its two accepted images");
    source.reset();
    check(inspect(retained) == original && inspect(*accepted) == pixels,
          "Copy filtering invalidated an owned lease after capture closed");
    return {{"status", "PASS"}, {"filteredAcquisitionsAtLeast", 8}, {"newerSourceAccepted", true},
            {"retainedPixelsVerified", true}, {"originalTimestampPreserved", true}, {"pipeline", result}};
}
Json duplication_eager(fixture::Window &desktop) {
    const auto config = plan(Json{{"version", 1}, {"cursor", false},
        {"sourceApi", "DesktopDuplication"}, {"output", {{"width", 640}, {"height", 400}}}});
    auto source = make_gpu(config);
    desktop.pattern(0, 0);
    auto selected = next_without_flush(*source, 0);
    check(selected.format == Frame::Format::Nv12 && selected.surface->gpu_texture(),
          "Eager duplication published incomplete output");
    const auto pixels = inspect(selected);
    bars(selected, pixels);
    auto last_id = selected.id;
    for (uint32_t number = 1; number < 16; ++number) {
        desktop.pattern(0, number);
        auto current = next_without_flush(*source, last_id);
        last_id = current.id;
        check(current.capture_hns > selected.capture_hns && current.surface->gpu_texture(),
              "Eager duplication did not progress with completed output");
    }
    source->close();
    const auto metrics = source->metrics();
    check(metrics.at("heldDesktopAcquisition") == false && metrics.at("inflightCopies") == 0 &&
              metrics.at("inflightConversions") == 0,
          "Eager duplication did not drain");
    source.reset();
    check(inspect(selected) == pixels, "Eager duplication overwrote a retained frame");
    return {{"status", "PASS"}, {"retainedPixelsVerified", true},
            {"eagerProgressWithoutExternalFlush", true}, {"pipeline", metrics}};
}
Json ordered_readback_selection(const Plan &config, fixture::Window &window) {
    window.pattern(0,0);
    auto source=make_gpu(config);
    Readback readback(false,config.prefer_query_completion);
    auto selected=config.desktop_duplication?next_without_flush(*source,0):next(*source);
    auto gpu_reference=selected;
    check(!selected.surface->gpu_texture(),"Ordered test did not begin with an unconverted acquisition");
    const auto timestamp=selected.capture_hns;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;) {
        const auto result=source->prepare_readback(selected,readback);
        check(result!=Pipeline::ReadbackPreparation::Unsupported,"Ordered pipeline was not selected");
        source->flush_work();
        if(result==Pipeline::ReadbackPreparation::Ready) break;
        check(std::chrono::steady_clock::now()<deadline,"Ordered conversion/readback did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(selected.capture_hns==timestamp && selected.format==Frame::Format::Nv12 &&
          selected.width==config.width && selected.height==config.height &&
          !selected.surface->cpu_pixels().empty(),"Ordered readback changed identity or geometry");
    const auto pixels=inspect(selected);
    const bool packed=readback.metrics().at("packedBufferCopies")==1;
    if(config.desktop_duplication && source->metrics().at("device").at("timelineFenceSelected"))
        check(source->metrics().at("timelineFenceSignals")==1 &&
              source->metrics().at("timelineFenceCoalescedMarkers")==1 &&
              source->metrics().at("timelineFenceUnsubmittedValue")==0,
              "Readback did not seal the shared copy/conversion marker before its staging fence");
    if(packed) {
        check(!gpu_reference.surface->gpu_texture(),"Packed CPU output exposed an unwritten NV12 texture");
        while(!source->prepare_frame(gpu_reference)) {
            source->flush_work();
            check(std::chrono::steady_clock::now()<deadline,"Independent GPU reference did not complete");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else check(source->prepare_frame(gpu_reference),"Readback completed before its source conversion");
    check(inspect(gpu_reference)==pixels,"Ordered readback differs from completed NV12 GPU pixels");
    bars(selected,pixels);
    artifact("ordered-readback-"+std::to_string(selected.width)+"x"+
             std::to_string(selected.height)+".nv12",pixels);
    for(int repetition=0;repetition<16;++repetition) {
        auto repeated=gpu_reference;
        check(source->prepare_readback(repeated,readback)==Pipeline::ReadbackPreparation::Ready,
              "Ordered readback missed its immutable frame cache");
        check(inspect(repeated)==pixels,"Cached ordered readback pixels changed");
    }
    check(readback.metrics().at("orderedCopies")==1 && source->metrics().at("gpuCopies")==1 &&
          source->metrics().at("converted")==1+int(packed),"Repeated ordered frame queued extra copies/conversions");
    window.pattern(0,99);
    auto abandoned=next(*source,selected.id);
    check(source->prepare_readback(abandoned,readback)==Pipeline::ReadbackPreparation::Pending,
          "Abandoned ordered copy unexpectedly completed inline");
    source->flush_work();abandoned={};
    source->close();
    const auto close_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!readback.quiesce()) {
        check(std::chrono::steady_clock::now()<close_deadline,"Ordered readback close did not quiesce");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto metrics=source->metrics();
    check(metrics.at("inflightCopies")==0 && metrics.at("inflightConversions")==0 &&
          metrics.at("retainedUnreturnedAcquisitions")==0 && metrics.at("gpuCopies")==2 &&
          metrics.at("converted")==2+int(packed) && readback.metrics().at("orderedCopies")==2,
          "Ordered capture/readback did not preserve bounded work and cleanup");
    gpu_reference={};source.reset();
    check(inspect(selected)==pixels,"Ordered CPU pixels did not survive the GPU owner");
    return {{"status","PASS"},{"nv12BytesEqualCompletedGpu",true},{"barsVerified",8},
            {"originalTimestampPreserved",true},{"cachedReuseChecks",16},
            {"abandonedCopyQuiesced",true},{"readback",readback.metrics()},{"pipeline",metrics}};
}
Json deferred_odd_crop(bool duplication, fixture::Window &window) {
    window.pattern(1);
    auto config = plan(Json{{"version", 1}, {"cursor", false}, {"slots", 6},
        {"sourceApi", duplication ? "DesktopDuplication" : "WindowsGraphicsCapture"},
        {"crop", {{"x", 96}, {"y", 128}, {"width", 333}, {"height", 241}}},
        {"output", {{"width", 334}, {"height", 242}}}});
    config.defer_conversion = true;
    auto source = make_gpu(config);
    auto selected = next(*source);
    const auto timestamp = selected.capture_hns;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!source->prepare_frame(selected)) {
        source->flush_work();
        check(std::chrono::steady_clock::now() < deadline, "Odd shader crop did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto pixels = inspect(selected);
    const auto chroma = size_t(selected.width) * selected.height;
    for (size_t i = chroma; i < pixels.size(); ++i)
        check(abs(int(pixels[i]) - 128) <= 3, "Odd shader crop included exterior chroma");
    check(abs(int(pixels[size_t(selected.height / 2) * selected.width + selected.width / 2]) - 235) <= 3,
          "Odd shader crop lost selected white content");
    for (uint32_t y = 0; y < selected.height; ++y)
        for (uint32_t x = 0; x < selected.width; ++x)
            if (x >= static_cast<uint32_t>(config.viewport.x + config.viewport.width) ||
                y >= static_cast<uint32_t>(config.viewport.y + config.viewport.height))
                check(abs(int(pixels[size_t(y) * selected.width + x]) - 16) <= 3,
                      "Odd shader output padding is not limited-range black");
    check(selected.capture_hns == timestamp, "Odd crop changed the acquisition timestamp");
    artifact("deferred-odd-334x242.nv12", pixels);
    source->close();
    const auto metrics = source->metrics();
    check(metrics.at("computeConversionSelected") == true &&
              metrics.at("ownedBgraShaderOnly") == false &&
              metrics.at("videoProcessorViewsSelected") == false &&
              metrics.at("paddingClears").get<uint64_t>() > 0,
          "Odd shader input did not retain the padding clear path");
    source.reset();
    check(inspect(selected) == pixels, "Odd crop lease changed after capture closed");
    return {{"status", "PASS"}, {"oddCropOutsideMarkerExcluded", true},
            {"limitedRangePaddingVerified", true}, {"timestampPreserved", true},
            {"leaseSurvivedOwner", true}, {"pipeline", metrics}};
}
Json deferred_resize() {
    fixture::Window window({64, 96, 320, 240});
    auto config = plan(Json{{"version", 1}, {"cursor", false},
                           {"output", {{"width", 320}, {"height", 240}}}});
    config.defer_conversion = true;
    auto source = make_gpu_for_test_window(config, window.handle());
    auto retained = next(*source);
    const auto timestamp = retained.capture_hns;
    check(!retained.surface->gpu_texture(), "Resize test did not retain an unselected acquisition");
    window.resize(640, 360, 64);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool resized{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto frame = source->poll();
        source->flush_work();
        if (frame && frame->generation > retained.generation) {
            resized = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(resized && source->prepare_frame(retained),
          "Source resize did not materialize the retained old generation");
    check(retained.capture_hns == timestamp, "Source resize changed retained acquisition time");
    const auto pixels = inspect(retained);
    artifact("deferred-resize-retained-320x240.nv12", pixels);
    bars(retained, pixels);
    source->close();
    const auto metrics = source->metrics();
    check(metrics.at("retainedUnreturnedAcquisitions") == 0,
          "Source resize leaked an old acquisition");
    source.reset();
    check(inspect(retained) == pixels, "Source resize corrupted a retained old texture");
    return {{"status", "PASS"}, {"retainedOldGenerationPixelsVerified", true},
            {"originalTimestampPreserved", true}, {"pipeline", metrics}};
}
Json lifecycle(const Plan &config) {
    fixture::Window window({64, 96, 320, 240});
    auto source = make_gpu_for_test_window(config, window.handle());
    auto first = next(*source);
    auto retained = inspect(first);
    window.resize(640, 360, -128);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool resized = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto frame = source->poll(); frame && frame->generation > first.generation) {
            resized = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!resized) {
        RECT actual{};
        GetWindowRect(window.handle(), &actual);
        throw std::runtime_error("WGC resize did not create a new generation: " + source->metrics().dump() +
                                 " windowWidth=" + std::to_string(actual.right - actual.left) +
                                 " windowHeight=" + std::to_string(actual.bottom - actual.top));
    }
    check(inspect(first) == retained, "Resize overwrote an old generation lease");
    window.close();
    bool closed = false;
    const auto close_limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < close_limit) {
        try {
            (void)source->poll();
        } catch (const Error &e) {
            closed = e.stage == NSS_STAGE_CAPTURE && e.code == NSS_E_UNAVAILABLE;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(closed, "WGC source Closed was not propagated");
    source->close();
    auto report = source->metrics();
    source.reset();
    check(inspect(first) == retained, "Source Closed invalidated retained texture");
    first.surface.reset();
    for (int i = 0; i < 40; ++i) {
        fixture::Window temporary({32, 32, 320, 240});
        auto capture = make_gpu_for_test_window(config, temporary.handle());
        auto frame = next(*capture);
        capture->close();
        const auto done = capture->metrics();
        check(done.at("callbacksActive") == 0 && done.at("inflightCopies") == 0,
              "Callbacks/copies remained after stop");
    }
    struct Barrier {
        std::atomic<bool> enabled{}, entered{}, proceed{};
    };
    auto barrier = std::make_shared<Barrier>();
    set_qualification_arrival([barrier] {
        if (barrier->enabled && !barrier->entered.exchange(true))
            while (!barrier->proceed)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    Json overlap;
    {
        fixture::Window target({64, 96, 320, 240});
        auto capture = make_gpu_for_test_window(config, target.handle());
        auto image = next(*capture);
        barrier->enabled = true;
        target.pattern(0, 123);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!barrier->entered && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!barrier->entered) {
            barrier->proceed = true;
            throw std::runtime_error("No real WGC callback entered the stop-overlap barrier");
        }
        std::jthread release([barrier] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            barrier->proceed = true;
        });
        capture->close();
        overlap = capture->metrics();
        check(overlap.at("callbacksActiveAtStop").get<uint32_t>() >= 1 && overlap.at("callbacksActive") == 0,
              "Stop did not overlap and quiesce the real callback");
    }
    set_qualification_arrival({});
    return {{"status", "PASS"},
            {"scope", "lifecycle"},
            {"actualWindowResize", true},
            {"actualNegativeWindowPosition", -128},
            {"actualSourceDestroyed", true},
            {"closedByItemEvent", report.at("closedByItemEvent")},
            {"closedByWin32Lifetime", report.at("closedByWin32Lifetime")},
            {"stopCycles", 40},
            {"pipeline", report},
            {"stopOverlappedRealCallback", true},
            {"overlap", overlap}};
}
#endif
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        SetUnhandledExceptionFilter(capture_exception);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const std::wstring scope = argc > 1 ? argv[1] : L"cpu";
        if (argc > 2)
            artifact_directory = argv[2];
        const auto monitors = displays();
        check(!monitors.empty(), "Interactive monitor is required for hardware qualification");
        const auto display = monitors.front();
        if (scope == L"cpu-wake-existing") {
            auto config = plan(Json{{"version", 1}, {"mode", "FullSoftware"},
                {"cursor", false}, {"slots", 6}, {"output", {{"width", 640}, {"height", 400}}}});
            config.defer_conversion = true;
            auto source = make_cpu(config);
            const auto hint = source->wait_hint();
            check(hint.event != nullptr && !hint.processing, "CPU source omitted its acquisition notification");
            uint64_t last_id{}, notifications{}, completed_frames{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
            while (completed_frames < 24 && std::chrono::steady_clock::now() < deadline) {
                check(WaitForSingleObject(hint.event, 2000) == WAIT_OBJECT_0,
                      "CPU acquisition or a queued successor failed to wake its consumer");
                ++notifications;
                auto frame = source->poll();
                if (!frame) continue; // A consumed readiness signal may precede a bounded poll.
                check(frame->id > last_id, "CPU notification reordered acquisitions");
                check(frame->surface->cpu_pixels().empty(),
                      "Deferred screen frame exposed an unprepared raw alpha plane");
                last_id = frame->id;
                check(source->prepare_frame(*frame), "Synchronous CPU conversion did not finish");
                check(frame->format == Frame::Format::Nv12 &&
                      frame->surface->cpu_pixels().size() == uint64_t(frame->width) * frame->height * 3 / 2,
                      "Deferred screen frame failed to expose its sealed NV12 plane");
                ++completed_frames;
                if (completed_frames % 3 == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(90)); // Fill the two-entry queue.
            }
            check(completed_frames == 24, "CPU event delivery stalled under a delayed consumer");
            source->close();
            check(source->wait_hint().event == nullptr, "Closed CPU pipeline offered a readiness handle");
            const auto metrics = source->metrics();
            check(metrics.at("acquisitionQueuePeak").get<uint64_t>() <= 2 &&
                  metrics.at("acquisitionThreadJoined") == true, "CPU acquisition exceeded its queue or lifetime budget");
            source.reset();
            check(live_pipelines == 0 && live_surfaces == 0 && live_capture_bytes == 0 &&
                  nss::live_frame_leases == 0 && nss::live_pool_bytes == 0,
                  "CPU event probe retained capture resources");
            std::cout << Json{{"status", "PASS"}, {"scope", "cpu-wake-existing"},
                {"notifications", notifications}, {"completedFrames", completed_frames},
                {"createdDesktopFixture", false}, {"closedHintCleared", true},
                {"quiescent", true}, {"pipeline", metrics}}.dump() << '\n';
            return 0;
        }
#ifndef NSS_CPU_CAPTURE_ONLY
        if (scope == L"deferred-existing" || scope == L"compute-existing" || scope == L"compute-fallback-existing") {
            const bool shader = scope != L"deferred-existing";
            const bool fallback = scope == L"compute-fallback-existing";
            set_qualification_compute_view_failure(fallback);
            auto config = plan(Json{{"version", 1}, {"cursor", false}, {"slots", 6},
                                   {"output", {{"width", shader ? 1280 : 640}, {"height", shader ? 800 : 400}}}});
            config.defer_conversion = true;
            auto result = deferred_selection(config, {}, false);
            check(live_pipelines == 0 && live_surfaces == 0 && live_capture_bytes == 0 &&
                      nss::live_frame_leases == 0 && nss::live_pool_bytes == 0,
                  "Deferred source resources did not quiesce");
            result["quiescent"] = true;
            result["createdDesktopFixture"] = false;
            if (shader) {
                check(result.at("pipeline").at("computeConversionSelected") == !fallback,
                      "Compute selection or forced raster fallback was not exercised");
                check(result.at("pipeline").at("shaderConversionSelected") == true,
                      "Shader fallback unexpectedly selected the video processor");
                result["injectedUnsupportedComputeViews"] = fallback;
            }
            set_qualification_compute_view_failure(false);
            std::cout << result.dump() << '\n';
            return 0;
        }
#endif
        fixture::Window desktop(display.bounds);
        Json result;
        if (scope == L"cpu") {
            result = live(false, display, desktop);
            const auto config = plan(Json{{"version", 1},
                                          {"mode", "FullSoftware"},
                                          {"cursor", false},
                                          {"output", {{"width", 320}, {"height", 200}}}});
            result["lifetime"] = cpu_lifetime(config);
            result["cursor"] = cursor(false, display, desktop);
        } else if (scope == L"cpu-deferred") {
            result = cpu_deferred(display, desktop);
        }
#ifndef NSS_CPU_CAPTURE_ONLY
        else if (scope == L"gpu") {
            result = live(true, display, desktop);
            result["cursor"] = cursor(true, display, desktop);
        } else if (scope == L"duplication-completion-return" || scope == L"duplication-completion-return-query") {
            const bool query = scope.ends_with(L"query");
            set_qualification_fence_failure(query);
            result = duplication_completion_return(desktop);
            if (query)
                check(result.at("pipeline").at("eventQueryReads").get<uint64_t>() > 0,
                      "Completed desktop return did not exercise event-query completion");
            set_qualification_fence_failure(false);
        } else if (scope == L"duplication-window-filter" || scope == L"duplication-window-filter-query") {
            const bool query = scope.ends_with(L"query");
            set_qualification_fence_failure(query);
            result = duplication_window_filter(desktop);
            if (query)
                check(result.at("pipeline").at("eventQueryReads").get<uint64_t>() > 0,
                      "Copy filter did not exercise event-query completion");
            set_qualification_fence_failure(false);
        } else if (scope == L"duplication-eager") {
            result = duplication_eager(desktop);
        } else if (scope == L"deferred-odd" || scope == L"duplication-odd") {
            result = deferred_odd_crop(scope == L"duplication-odd", desktop);
        } else if (scope == L"deferred" || scope == L"deferred-shader" || scope == L"deferred-shader-fallback" || scope == L"deferred-compute-fallback" ||
                   scope == L"duplication" || scope == L"duplication-shader" || scope == L"duplication-shader-fallback" || scope == L"duplication-compute-fallback" || scope == L"duplication-query" || scope == L"duplication-legacy-factory" || scope == L"duplication-original-policy") {
            const bool duplication = scope.starts_with(L"duplication");
            const bool query = scope == L"duplication-query";
            const bool legacy = scope == L"duplication-legacy-factory";
            const bool original_policy = scope == L"duplication-original-policy";
            const bool shader = scope != L"deferred" && scope != L"duplication" && !query && !legacy && !original_policy;
            const bool fallback = scope == L"deferred-shader-fallback" || scope == L"duplication-shader-fallback";
            set_qualification_fence_failure(query);
            set_qualification_shader_view_failure(fallback);
            set_qualification_compute_view_failure(scope == L"deferred-compute-fallback" || scope == L"duplication-compute-fallback");
            set_qualification_output5_unavailable(legacy);
            auto config = plan(Json{{"version", 1}, {"cursor", false}, {"slots", 6},
                                     {"sourceApi", duplication ? "DesktopDuplication" : "WindowsGraphicsCapture"},
                                     {"output", {{"width", shader ? 1280 : 640}, {"height", shader ? 800 : 400}}}});
            config.defer_conversion = true;
            config.prefer_legacy_duplication = original_policy;
            config.trace = duplication;
            result = deferred_selection(config, [&](uint32_t number) { desktop.pattern(0, number); }, true);
            if (shader) {
                check(result.at("pipeline").at("shaderConversionSelected") == !fallback &&
                      result.at("pipeline").at("shaderConvertedFrames") == (fallback ? 0 : 2),
                      "The selected shader or its unsupported-view fallback was not exercised");
                result["injectedUnsupportedPlaneViews"] = fallback;
            }
            if (!duplication) result["resize"] = deferred_resize();
            else {
                const auto &pipeline = result.at("pipeline");
                const bool modern = pipeline.at("output5ProbeResult").get<int64_t>() >= 0 && !original_policy;
                check(pipeline.at("duplicationFactory") == (modern ? "DuplicateOutput1" : "DuplicateOutput") &&
                          pipeline.at("originalDuplicationFactoryRequested") == original_policy &&
                          (!legacy || (!modern && pipeline.at("output5ProbeResult").get<int64_t>() == E_NOINTERFACE)),
                      "Duplication factory capability fallback was not exercised correctly");
                bool rejected{};
                try { (void)make_gpu_for_test_window(config, desktop.handle()); }
                catch (const Error &error) { rejected = error.code == NSS_E_ARGUMENT; }
                check(rejected, "Monitor-only duplication accepted a window source");
                result["windowSourceRejected"] = true;
                if (query)
                    check(result.at("pipeline").at("completionMechanism") == "D3D11 event query" &&
                              result.at("pipeline").at("eventQueryReads").get<uint64_t>() > 0,
                          "Duplication did not exercise real event-query completion");
            }
            set_qualification_fence_failure(false);
            set_qualification_shader_view_failure(false);
            set_qualification_compute_view_failure(false);
            set_qualification_output5_unavailable(false);
        } else if (scope.starts_with(L"ordered-readback")) {
            const bool duplication=scope.find(L"duplication")!=std::wstring::npos;
            const bool small_viewport=scope==L"ordered-readback";
            const bool raster=scope.ends_with(L"raster");
            const bool fallback=scope.ends_with(L"video-fallback");
            const bool hybrid_query=scope.find(L"hybrid-query")!=std::wstring::npos;
            const bool packed_fallback=scope.ends_with(L"packed-fallback");
            const bool legacy=scope.ends_with(L"legacy-factory");
            const bool original_policy=scope.ends_with(L"original-policy");
            const bool query=scope.ends_with(L"query") && !hybrid_query;
            set_qualification_compute_view_failure(raster);
            set_qualification_shader_view_failure(fallback);
            set_qualification_fence_failure(query);
            set_qualification_readback_packed_failure(packed_fallback);
            set_qualification_output5_unavailable(legacy);
            auto config=plan(Json{{"version",1},{"cursor",false},{"slots",6},
                {"sourceApi",duplication?"DesktopDuplication":"WindowsGraphicsCapture"},
                {"output",{{"width",small_viewport?640:1280},{"height",small_viewport?400:800}}}});
            config.defer_conversion=true;
            config.prefer_legacy_duplication=original_policy;
            config.prefer_query_completion=hybrid_query;
            result=ordered_readback_selection(config,desktop);
            if (duplication) {
                const auto &pipeline = result.at("pipeline");
                const bool modern = pipeline.at("output5ProbeResult").get<int64_t>() >= 0 && !original_policy;
                check(pipeline.at("duplicationFactory") == (modern ? "DuplicateOutput1" : "DuplicateOutput") &&
                          pipeline.at("originalDuplicationFactoryRequested") == original_policy &&
                          (!legacy || (!modern && pipeline.at("output5ProbeResult").get<int64_t>() == E_NOINTERFACE)),
                      "Ordered readback did not preserve the selected duplication factory");
            }
            check(result.at("readback").at("packedBufferCopies")==
                  (duplication && !small_viewport && !raster && !fallback && !packed_fallback?2:0),
                  "Packed readback selection or its texture fallback was not exercised");
            check(result.at("pipeline").at("computeConversionSelected")==(!small_viewport && !raster && !fallback) &&
                  result.at("pipeline").at("shaderConversionSelected")==(!small_viewport && !fallback),
                  "The requested ordered conversion backend was not exercised");
            check(result.at("pipeline").at("ownedBgraShaderOnly")==(!small_viewport && !fallback) &&
                  result.at("pipeline").at("videoProcessorViewsSelected")== (small_viewport || fallback),
                  "Owned BGRA or video fallback views do not match the selected conversion");
            if(query) check(result.at("pipeline").at("eventQueryReads").get<uint64_t>()>0,
                            "Ordered query fallback was not exercised");
            if(hybrid_query) check(result.at("pipeline").at("eventQueryReads").get<uint64_t>()>0 &&
                  result.at("pipeline").at("timelineFenceSignals")==0 &&
                  result.at("pipeline").at("device").at("queryCompletionPreferred")==true &&
                  result.at("readback").at("completion")=="D3D11 event query",
                  "Hybrid query policy did not cover both capture and staging without timeline signals");
            set_qualification_compute_view_failure(false);
            set_qualification_shader_view_failure(false);
            set_qualification_fence_failure(false);
            set_qualification_readback_packed_failure(false);
            set_qualification_output5_unavailable(false);
        } else if (scope == L"gpu-completion-events" || scope == L"gpu-completion-event-fallback") {
            const bool fallback = scope == L"gpu-completion-event-fallback";
            set_qualification_completion_event_failure(fallback);
            auto config = plan(Json{{"version", 1}, {"cursor", false}, {"slots", 6},
                                    {"output", {{"width", 640}, {"height", 400}}}});
            config.defer_conversion = true;
            auto source = make_gpu_for_test_window(config, desktop.handle());
            check(source->metrics().at("device").at("timelineFenceSupported"),
                  "This live qualification requires a fence-capable adapter");
            uint64_t last_id{}, notifications{};
            Frame retained;
            for (uint32_t number = 1; number <= 32; ++number) {
                desktop.pattern(0, number);
                auto frame = next(*source, last_id);
                last_id = frame.id;
                const auto timestamp = frame.capture_hns;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!source->prepare_frame(frame)) {
                    source->flush_work();
                    const auto hint = source->wait_hint();
                    check(hint.processing, "Pending GPU conversion was not reported");
                    if (fallback) {
                        check(!hint.completion_event, "Failed notification registration did not fall back");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    } else {
                        check(hint.completion_event != nullptr, "Pending GPU work has no completion event");
                        check(WaitForSingleObject(hint.completion_event, 5000) == WAIT_OBJECT_0,
                              "GPU work did not signal completion");
                        // Manual reset preserves the signal until the owner
                        // observes it and retires this one registration.
                        check(WaitForSingleObject(hint.completion_event, 0) == WAIT_OBJECT_0,
                              "Waiting consumed the owner's completion evidence");
                        ++notifications;
                    }
                    check(std::chrono::steady_clock::now() < deadline, "GPU conversion wait exceeded its bound");
                }
                check(frame.capture_hns == timestamp && frame.format == Frame::Format::Nv12,
                      "Completion notification changed frame time or format");
                bars(frame, inspect(frame));
                retained = std::move(frame);
            }
            desktop.pattern(0, 99);
            auto abandoned = next(*source, last_id);
            (void)source->prepare_frame(abandoned);
            source->flush_work();
            abandoned.surface.reset();
            source->close();
            const auto metrics = source->metrics();
            check(!source->wait_hint().event && !source->wait_hint().completion_event,
                  "Closed pipeline still exposes wait handles");
            check(metrics.at("inflightCopies") == 0 && metrics.at("inflightConversions") == 0 &&
                      metrics.at("gpuCompletionEventPendingValue") == 0,
                  "Closing abandoned work left a pending GPU operation or notification");
            if (fallback) {
                check(metrics.at("gpuCompletionEventRegistrationFailures") == 1 &&
                          metrics.at("gpuCompletionEventRegistrations") == 0,
                      "Failed registration was retried or recorded as a success");
            } else {
                check(notifications > 0 && metrics.at("gpuCompletionEventRegistrations") > 0 &&
                          metrics.at("gpuCompletionEventRegistrationFailures") == 0,
                      "Live GPU notifications were not exercised");
                check(metrics.at("gpuCompletionEventRegistrations") <= metrics.at("timelineFenceSignals"),
                      "A completed fence was registered repeatedly, causing a wakeup loop");
            }
            source.reset();
            bars(retained, inspect(retained));
            retained.surface.reset();
            set_qualification_completion_event_failure(false);
            result = {{"status", "PASS"}, {"frames", 32}, {"notifications", notifications},
                      {"injectedRegistrationFailure", fallback}, {"timestampPreserved", true},
                      {"leaseSurvivedOwner", true}, {"pipeline", metrics}};
        } else if (scope == L"wakeups") {
            const auto config = plan(Json{{"version", 1}, {"cursor", false},
                                          {"output", {{"width", 320}, {"height", 200}}}});
            auto source = make_gpu_for_test_window(config, desktop.handle());
            const auto hint = source->wait_hint();
            check(hint.event != nullptr, "WGC did not expose its owned readiness event");
            uint64_t last_id{}, notifications{};
            for (uint32_t number = 1; number <= 6; ++number) {
                // Consume an earlier notification before producing new content;
                // auto-reset preserves arrivals between this read and the wait.
                (void)WaitForSingleObject(hint.event, 0);
                desktop.pattern(0, number);
                check(WaitForSingleObject(hint.event, 5000) == WAIT_OBJECT_0,
                      "New WGC content did not wake its waiting consumer");
                ++notifications;
                const auto frame = next(*source, last_id);
                last_id = frame.id;
                check(source->wait_hint().event == hint.event,
                      "WGC readiness event changed while the source was alive");
            }
            source->close();
            check(source->wait_hint().event == nullptr,
                  "Closed pipeline still offered a readiness handle");
            source.reset();
            result = {{"status", "PASS"}, {"notifications", notifications},
                      {"lastCompletedFrame", last_id}, {"closedHintCleared", true}};
        } else if (scope == L"lifecycle") {
            auto config =
                plan(Json{{"version", 1}, {"cursor", false}, {"output", {{"width", 320}, {"height", 240}}}});
            result = lifecycle(config);
        }
#endif
        else if (scope == L"api-gpu" || scope == L"api-cpu")
            result = capture_api_probe(scope == L"api-gpu");
        else if (scope == L"fixture") {
            check(argc > 2 && artifact_directory.is_absolute(),
                  "Owned fixture requires an absolute stop-marker path");
            if (argc > 3) {
                const auto pixels=desktop.reference();
                std::ofstream reference(std::filesystem::path(argv[3]),std::ios::binary);
                reference.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
                check(bool(reference),"Owned fixture reference write failed");
            }
            std::cout << Json{{"ready", true},
                              {"processId", GetCurrentProcessId()},
                              {"display", display_json(display)}}
                             .dump()
                      << std::endl;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
            while (!std::filesystem::exists(artifact_directory) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(std::filesystem::exists(artifact_directory), "Owned fixture stop deadline reached");
            result = {{"status", "PASS"}, {"scope", "owned-fixture"}};
        } else if (scope == L"static-gpu" || scope == L"static-cpu") {
            const bool gpu = scope == L"static-gpu";
            result = probe(Json{{"version", 1},
                                {"mode", gpu ? "RequireGpuResidentHardware" : "FullSoftware"},
                                {"cursor", false},
                                {"durationMs", 60000},
                                {"output", {{"width", gpu ? 1920 : 1280}, {"height", gpu ? 1080 : 720}}}});
            check(result.at("slots") == 1800 && result.at("missingSlots") == 0 &&
                      result.at("lastPtsHns") == 599666666 && result.at("lastDurationHns") == 333334,
                  "60 second CFR timeline has gaps or truncation drift");
            result["ownedStaticDesktop"] = true;
        } else
            throw std::runtime_error("Unknown live capture scope");
        result["inspection"] = {
            {"purpose",
             "diagnostic-only test target; production capture library contains no staging/Map code"},
            {"gpuReadbackCalls", inspection_calls},
            {"gpuReadbackBytes", inspection_bytes}};
        check(live_pipelines == 0 && live_surfaces == 0 && live_capture_bytes == 0 &&
                  nss::live_frame_leases == 0 && nss::live_pool_bytes == 0,
              "Native capture resources did not quiesce");
        result["quiescent"] = true;
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const Error &e) {
        std::cout << Json{{"status", "FAIL"},
                          {"code", e.code},
                          {"stage", e.stage},
                          {"hresult", e.native},
                          {"message", e.what()}}
                         .dump()
                  << '\n';
        return 1;
    } catch (const std::exception &e) {
        std::cout << Json{{"status", "FAIL"}, {"message", e.what()}}.dump() << '\n';
        return 1;
    }
}
