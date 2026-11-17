/* NessStudio public C ABI. See the library README for ownership, lifecycle
 * and build instructions. CPU/GPU capture, audio, camera, previews, scenes
 * and media jobs are accessed through versioned structures and opaque handles. */
#ifndef NESSSTUDIO_CONTRACT_H
#define NESSSTUDIO_CONTRACT_H
#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#define NSS_CALL __cdecl
#else
#define NSS_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define NSS_ABI_MAJOR 1u
#define NSS_ABI_MINOR 8u
#define NSS_PREVIEW_CPU 1u
#define NSS_PREVIEW_GPU_D3D9 2u
#define NSS_PREVIEW_BGRA32 1u
#define NSS_PREVIEW_TIMESTAMP_ESTIMATED 1u
#define NSS_CONTRACT_REVISION 1u
#define NSS_STRUCT_VERSION 1u
#define NSS_MAX_JSON_BYTES 1048576u
#define NSS_OK 0
#define NSS_ACCEPTED 1
#define NSS_ALREADY 2
#define NSS_TIMEOUT 3
#define NSS_E_ARGUMENT (-1)
#define NSS_E_VERSION (-2)
#define NSS_E_STATE (-3)
#define NSS_E_UNAVAILABLE (-4)
#define NSS_E_BUSY (-5)
#define NSS_E_IO (-6)
#define NSS_E_BACKPRESSURE (-7)
#define NSS_E_CANCELLED (-8)
#define NSS_E_INTERNAL (-9)
#define NSS_E_TIMEOUT (-10)
#define NSS_E_EVENT_BACKPRESSURE (-11)

/* Use uint32_t fields for these values; no compiler-sized C enums in structs. */
#define NSS_CREATED 0u
#define NSS_PREPARING 1u
#define NSS_READY 2u
#define NSS_STARTING 3u
#define NSS_RECORDING 4u
#define NSS_PAUSING 5u
#define NSS_PAUSED 6u
#define NSS_RESUMING 7u
#define NSS_STOPPING 8u
#define NSS_FINALIZING 9u
#define NSS_COMPLETED 10u
#define NSS_FAILED 11u
#define NSS_DISPOSED 12u
#define NSS_COMMAND_PREPARE 1u
#define NSS_COMMAND_START 2u
#define NSS_COMMAND_PAUSE 3u
#define NSS_COMMAND_RESUME 4u
#define NSS_COMMAND_STOP 5u
#define NSS_EVENT_RESULT 1u
#define NSS_EVENT_FAULT 2u
#define NSS_PIXEL_BGRA8_OPAQUE 1u
#define NSS_PACKET_KEYFRAME 1u
#define NSS_PREVIEW_ESTIMATED_TIME 1u
#define NSS_PREVIEW_CPU 1u
#define NSS_PREVIEW_GPU_D3D9_SURFACE 2u
#define NSS_STAGE_NONE 0u
#define NSS_STAGE_BOOTSTRAP 1u
#define NSS_STAGE_PLAN 2u
#define NSS_STAGE_CAPTURE 3u
#define NSS_STAGE_PROCESS 4u
#define NSS_STAGE_ENCODER 5u
#define NSS_STAGE_AUDIO 6u
#define NSS_STAGE_OUTPUT 7u
#define NSS_STAGE_STORAGE 8u
#define NSS_STAGE_PREVIEW 9u
#define NSS_STAGE_JOB 10u
#define NSS_STAGE_CONTROL 11u
#define NSS_SOURCE_SCREEN 1u
#define NSS_SOURCE_CAMERA 2u
#define NSS_SOURCE_MICROPHONE 3u
#define NSS_SOURCE_LOOPBACK 4u
#define NSS_JOB_CREATED 100u
#define NSS_JOB_RUNNING 101u
#define NSS_JOB_FINALIZING 102u
#define NSS_JOB_CANCELLING 103u
#define NSS_JOB_CANCELLED 104u
#define NSS_JOB_COMPLETED 105u
#define NSS_JOB_FAILED 106u

typedef int32_t nss_result;
typedef struct nss_engine nss_engine;
typedef struct nss_session nss_session;
typedef struct nss_job nss_job;
typedef struct nss_preview nss_preview;
typedef struct nss_preview_lease nss_preview_lease;
typedef struct nss_blob nss_blob;

/* Immutable borrowed bytes. Inputs are copied before return, no embedded NUL. */
typedef struct nss_utf8_view {
    const uint8_t* data;
    uint64_t bytes;
} nss_utf8_view;

typedef struct nss_version_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t pointer_bits;
    uint32_t contract_revision;
    uint8_t build_utf8[64];
} nss_version_v1;

typedef struct nss_error_v1 {
    uint32_t size;
    uint32_t version;
    int32_t code;
    uint32_t stage;
    int32_t hresult;
    uint32_t reserved;
    uint8_t message_utf8[256];
} nss_error_v1;

/* Versioned control JSON. The envelope does not own the input view. */
typedef struct nss_json_v1 {
    uint32_t size;
    uint32_t version;
    nss_utf8_view json;
} nss_json_v1;

typedef struct nss_event_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t sequence;
    uint64_t command_id;
    uint64_t object_id;
    int64_t active_time_hns;
    uint32_t kind;
    uint32_t state;
    int32_t status;
    uint32_t stage;
    int32_t hresult;
    uint32_t reserved;
    uint8_t message_utf8[256];
} nss_event_v1;

typedef struct nss_preview_config_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t source_id;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t slots;
    uint32_t channel;
    uint32_t reserved;
} nss_preview_config_v1;

/* data is immutable until lease_release, including after detach/session release. */
typedef struct nss_preview_frame_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t session_id;
    uint64_t source_id;
    uint64_t generation;
    uint64_t frame_id;
    int64_t capture_time_hns;
    uint64_t acquisition_uncertainty_hns;
    uint64_t bytes;
    const uint8_t* data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t reserved;
} nss_preview_frame_v1;

/* Borrowed IDirect3DSurface9*, not released by COM from the client.
 * Immutable completed content until lease release. WPF must unbind under
 * D3DImage lock before releasing the previous lease. See runtime.md. */
typedef struct nss_gpu_surface_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t session_id;
    uint64_t source_id;
    uint64_t generation;
    uint64_t frame_id;
    int64_t capture_time_hns;
    void* surface_d3d9;
    uint64_t adapter_luid;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t reserved;
} nss_gpu_surface_v1;

/* Packet metadata; payload/lease stays internal between encoder and output. */
typedef struct nss_packet_info_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t track_id;
    uint32_t flags;
    int64_t pts_hns;
    int64_t dts_hns;
    int64_t duration_hns;
    uint64_t bytes;
} nss_packet_info_v1;

/* All out envelopes must carry exact sizeof and version=1 on input.
 * out handles are set to NULL on failure. error may be NULL.
 * All release calls consume one owned reference, accept NULL, never throw.
 * release of live objects schedules quiescent teardown; it is NOT Stop success. */
nss_result NSS_CALL nss_get_version(nss_version_v1* out_version);
nss_result NSS_CALL nss_runtime_quiesce(uint32_t timeout_ms, nss_error_v1* error);
nss_result NSS_CALL nss_engine_create(const nss_json_v1* config, nss_engine** out_engine, nss_error_v1* error);
nss_result NSS_CALL nss_engine_capabilities(nss_engine* engine, nss_blob** out_json, nss_error_v1* error);
nss_result NSS_CALL nss_engine_devices(nss_engine* engine, uint32_t source_kind, nss_blob** out_json, nss_error_v1* error);
void NSS_CALL nss_engine_release(nss_engine* engine);
nss_result NSS_CALL nss_blob_view(const nss_blob* blob, nss_utf8_view* out_view);
void NSS_CALL nss_blob_release(nss_blob* blob);

nss_result NSS_CALL nss_session_create(nss_engine* engine, const nss_json_v1* request, nss_session** out_session, nss_error_v1* error);
nss_result NSS_CALL nss_session_command(nss_session* session, uint32_t command, uint64_t* out_command_id, nss_error_v1* error);
nss_result NSS_CALL nss_session_snapshot(nss_session* session, nss_blob** out_json, nss_error_v1* error);
/* Atomically accepts an immutable scene. Revision applies on the render worker;
   observe metrics.sharedScene.appliedRevision for application acknowledgement. */
nss_result NSS_CALL nss_session_update_scene(nss_session* session, nss_utf8_view scene_json, uint64_t* revision, nss_error_v1* error);
/* Suspends a presentation consumer while retaining its bounded GPU pool. */
nss_result NSS_CALL nss_preview_set_enabled(nss_preview* preview, uint32_t enabled, nss_error_v1* error);
nss_result NSS_CALL nss_session_poll_event(nss_session* session, uint32_t timeout_ms, nss_event_v1* out_event, nss_error_v1* error);
void NSS_CALL nss_session_interrupt_poll(nss_session* session);
void NSS_CALL nss_session_release(nss_session* session);

/* CPU presentation is the explicit, bounded exception to control-only pixels. */
nss_result NSS_CALL nss_preview_attach(nss_session* session, const nss_preview_config_v1* config, nss_preview** out_preview, nss_error_v1* error);
nss_result NSS_CALL nss_preview_acquire(nss_preview* preview, nss_preview_lease** out_lease, nss_preview_frame_v1* out_frame, nss_error_v1* error);
nss_result NSS_CALL nss_gpu_preview_acquire(nss_preview* preview, nss_preview_lease** out_lease, nss_gpu_surface_v1* out_surface, nss_error_v1* error);
nss_result NSS_CALL nss_preview_detach(nss_preview* preview, nss_error_v1* error);
void NSS_CALL nss_preview_lease_release(nss_preview_lease* lease);
void NSS_CALL nss_preview_release(nss_preview* preview);

nss_result NSS_CALL nss_job_create(nss_engine* engine, const nss_json_v1* request, nss_job** out_job, nss_error_v1* error);
nss_result NSS_CALL nss_job_start(nss_job* job, uint64_t* out_command_id, nss_error_v1* error);
nss_result NSS_CALL nss_job_cancel(nss_job* job, uint64_t* out_command_id, nss_error_v1* error);
nss_result NSS_CALL nss_job_snapshot(nss_job* job, nss_blob** out_json, nss_error_v1* error);
nss_result NSS_CALL nss_job_poll_event(nss_job* job, uint32_t timeout_ms, nss_event_v1* out_event, nss_error_v1* error);
void NSS_CALL nss_job_interrupt_poll(nss_job* job);
void NSS_CALL nss_job_release(nss_job* job);

nss_result NSS_CALL nss_probe_software_encoder(nss_engine* engine, nss_utf8_view output_path, nss_blob** out_report, nss_error_v1* error);
/* Synchronous bounded capture/process diagnostic, never an encoded recording.
 * Configuration is copied; the operation owns a native MTA worker and returns
 * metrics only. Use the session API for encoded recordings. */
nss_result NSS_CALL nss_probe_capture(nss_engine* engine, const nss_json_v1* request, nss_blob** out_report, nss_error_v1* error);

#ifdef __cplusplus
}
#endif
#endif
