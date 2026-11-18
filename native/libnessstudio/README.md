# libnessstudio

libnessstudio is NessStudio's recording engine. It exposes a **C ABI** from a Windows x64 DLL; its implementation uses C++20 and native Windows APIs. Applications can use it without WPF or .NET.

## Components and supported routes

| Area | Implementation |
|---|---|
| Screen capture | Windows Graphics Capture, Desktop Duplication and explicit software capture; monitor/region/cursor support |
| Camera | DirectShow device enumeration, format negotiation, native sample receiver and pixel conversion |
| Audio | WASAPI microphone/loopback capture, timestamp handling, resampling and WAV tracks |
| Processing | D3D11 textures/shaders and CPU pixel/audio routines with capability-checked SSE2/AVX2 paths |
| Encoding/muxing | Separate NessMuxer dependency, statically linked; direct Quick Sync/NVENC/AMF and CPU x264 |
| Scenes/previews | Scene composition, CPU preview leases and shared GPU presentation surfaces |
| Sessions | Asynchronous commands/events, pause/resume, bounded queues, manifests and recovery checkpoints |
| Media jobs | Explicit FFmpeg jobs for export, finalization and related project operations |

Windows x64 is the implemented platform. A portable C header does not imply Linux/macOS capture support. The engine has one active session and one active job per configured engine budget. CPU mode can operate without a hardware video encoder, but still needs a functioning Windows capture source and the x264 runtime.

## Build and package

From the **NessStudio repository root**, with Visual Studio 2026 C++ x64/v145 and Windows SDK 10.0.26100.0 installed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/Build-Native.ps1 -Configuration Release -StageDirectory .out/libnessstudio-sdk -RunTests -Sdk
```

Use `-Configuration Debug` for development, or `-NativeBuildDirectory <absolute-directory>` to select a different compiler output directory. The script finds Visual Studio's CMake, resolves hash-pinned dependencies, builds, runs CTest when requested, and stages:

- `libnessstudio.dll`, `nessstudio-cli.exe`, `libx264-164.dll` and matching C++ runtime DLLs;
- `include/nessstudio/nessstudio.h` and `lib/libnessstudio.lib`;
- `native-build.json`, runtime/dependency manifests, licenses and the supplied x264 source archive.

Place the runtime files beside your executable. Link a C or C++ consumer to `libnessstudio.lib` and add `include/` to its include path. NessMuxer is incorporated into this DLL, so a separate NessMuxer DLL is unnecessary. Export jobs additionally require the pinned FFmpeg executable at the path supplied by the host. The [Development Guide](../../DEVELOPMENT.md#generate-the-native-dlls-and-place-them-in-the-app) explains application staging and standalone NessMuxer builds.

The GitHub Actions workflow publishes the same native SDK after tests. It uses `-SkipHardwareTests` to exclude the three scene tests labeled `hardware`, because hosted runners lack a D3D11 video device. Local `-RunTests` includes these tests by default; add `-SkipHardwareTests` on a machine without that device. Test fixtures and synthetic RGB windows are not shipped in this package.

## Minimal C consumer

This program queries the engine's capabilities without opening a capture device:

```c
#include <nessstudio/nessstudio.h>
#include <stdio.h>

int main(void) {
    static const uint8_t config_text[] = "{\"version\":1,\"clientName\":\"Example\"}";
    nss_json_v1 config = {sizeof(config), NSS_STRUCT_VERSION,
                         {config_text, sizeof(config_text) - 1}};
    nss_error_v1 error = {0};
    nss_engine *engine = NULL;
    nss_blob *capabilities = NULL;
    nss_utf8_view view = {0};
    error.size = sizeof(error);
    error.version = NSS_STRUCT_VERSION;
    nss_result result = nss_engine_create(&config, &engine, &error);
    if (result == NSS_OK)
        result = nss_engine_capabilities(engine, &capabilities, &error);
    if (result == NSS_OK)
        result = nss_blob_view(capabilities, &view);
    if (result == NSS_OK)
        fwrite(view.data, 1, (size_t)view.bytes, stdout);
    else
        fprintf(stderr, "Engine error %d: %s\n", result, (const char *)error.message_utf8);
    nss_blob_release(capabilities);
    nss_engine_release(engine);
    if (nss_runtime_quiesce(1000, &error) != NSS_OK)
        return 1;
    return result == NSS_OK ? 0 : 1;
}
```

In an x64 developer shell, with the example saved as `example.c`:

```powershell
cl /nologo /W4 /std:c17 /I .out/libnessstudio-sdk/include example.c /link /LIBPATH:.out/libnessstudio-sdk/lib libnessstudio.lib /OUT:.out/libnessstudio-sdk/example.exe
.out/libnessstudio-sdk/example.exe
```

## ABI, ownership and errors

- Include the public header; do not duplicate structure layouts or use packed structures. ABI major/minor and pointer width can be queried with `nss_get_version`. Product version 3.0.0 is separate from ABI major 1 and structure version 1.
- Initialize every versioned envelope with its exact `sizeof` and `NSS_STRUCT_VERSION`. Zero reserved fields. Text is bounded UTF-8 with explicit byte length, without embedded NULs. JSON input is copied before return and limited by `NSS_MAX_JSON_BYTES`.
- Release each owned opaque handle once through the corresponding release function. Release functions accept NULL. Views into blobs, packets or preview frames are borrowed and expire when their owner/lease is released.
- Keep preview leases short. Detach preview consumers and release outstanding leases before tearing down their host presentation resources. Pixel pointers must not be retained after release.
- Negative results carry an error code, stage and diagnostic. `NSS_ACCEPTED` means an asynchronous command was accepted, not completed. `NSS_TIMEOUT` from an event poll is not a recording failure. Respect backpressure instead of spinning or discarding accepted work silently.
- Release is not successful recording finalization. Wait for the terminal session/job result, then release its handles. `nss_runtime_quiesce` accepts a timeout from 0 to 1000 ms and checks that owned media resources are gone; retry after a timeout while coordinating shutdown. After asynchronous media use, the Windows module is pinned until process exit. Quiescence does not unpin the module.

## Recording lifecycle

1. Create an engine; query capabilities and devices with `nss_engine_capabilities` and `nss_engine_devices`. Use the returned device identifiers and camera formats.
2. Construct a version-1 recording JSON request and pass it to `nss_session_create`. The app's [`NativeRecordingService`](../../src/NessStudio.Infrastructure/Recording/NativeRecordingService.cs) is a complete host example; [`NativeRecordingSelection`](../../src/NessStudio.Infrastructure/Recording/NativeRecordingSelection.cs) maps policies and reads the effective route.
3. Send `NSS_COMMAND_PREPARE`, then poll events until its command ID completes in `NSS_READY` or fails. Preparation selects resources and reports the effective capture/encoder route.
4. Send `NSS_COMMAND_START` and observe `NSS_RECORDING`. Pause/resume are also commands with asynchronous completion. Use snapshots for metrics and scene revisions, rather than inferring success from a button click.
5. Send `NSS_COMMAND_STOP`; keep polling through finalization until `NSS_COMPLETED` or `NSS_FAILED`. Check the final snapshot/manifests before treating the media as complete.

Serialize control operations in the host, use a dedicated event consumer, and use interrupt-poll functions to wake that consumer during shutdown. Do not destroy handles while another host thread is using them.

The CLI supports `version --json`, capture diagnostics, configured recordings and exports. The [native tests](../tests) and [managed ABI probe](../../tests/NessStudio.AbiProbe) demonstrate session, preview, scene and job contracts.

## Encoder selection

`Auto` probes direct Quick Sync, NVENC and AMF on the selected D3D11 device, in that order. An unavailable capability allows the next candidate; where policy permits, x264 provides the software fallback. Configuration, allocation and driver errors retain their cause rather than being masked by another backend. Hardware-required mode fails when no compatible hardware encoder is available.

`GpuWithSoftwareEncoding` uses GPU capture/processing with x264. `FullSoftware` explicitly selects the CPU route. Capability discovery is based on the actual device/driver and requested format; product family names are insufficient. Native manifests report the route actually selected.

## Dependencies and licensing

Pins and integrity hashes live in `native/deps/dependencies.lock.json`. NessMuxer remains a separate repository and is consumed at a fixed revision. Its encoder SDK pins are in `native-backends.lock.json` inside that checkout. Build outputs carry fingerprints of native inputs and dependency revisions.

The engine's own source uses the repository's MIT license. The packaged encoder/runtime dependencies retain their own licenses; x264 is GPL. Keep all notices and corresponding source material included by the packaging script. See [Licenses and Redistributions](../../README.md#licenses-and-redistributions) for the application package.
