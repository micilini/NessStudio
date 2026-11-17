/* Declaration/layout review only. Does not provide fake engine functions. */
#include "nessstudio/nessstudio.h"
#include <stdio.h>

#ifdef __cplusplus
#define CHECK static_assert
#define ALIGNMENT alignof
#else
#define CHECK _Static_assert
#define ALIGNMENT _Alignof
#endif

CHECK(sizeof(void*) == 8, "Contract requires an x64 process");
CHECK(sizeof(nss_utf8_view) == 16, "UTF8 view layout");
CHECK(sizeof(nss_version_v1) == 88, "Version layout");
CHECK(sizeof(nss_error_v1) == 280, "Error layout");
CHECK(sizeof(nss_json_v1) == 24, "JSON layout");
CHECK(sizeof(nss_event_v1) == 320, "Event layout");
CHECK(sizeof(nss_preview_config_v1) == 48, "Preview config layout");
CHECK(sizeof(nss_preview_frame_v1) == 96, "CPU preview layout");
CHECK(sizeof(nss_gpu_surface_v1) == 80, "GPU preview layout");
CHECK(sizeof(nss_packet_info_v1) == 48, "Packet layout");
CHECK(offsetof(nss_event_v1, message_utf8) == 64, "Event payload boundary");
CHECK(offsetof(nss_preview_frame_v1, data) == 64, "CPU preview pointer boundary");

#define BEGIN(T) printf("\"%s\":{\"size\":%zu,\"alignment\":%zu,\"fields\":{", #T, sizeof(T), (size_t)ALIGNMENT(T))
#define FIRST(T,F) printf("\"%s\":%zu", #F, offsetof(T,F))
#define FIELD(T,F) printf(",\"%s\":%zu", #F, offsetof(T,F))
#define ENVELOPE(T) FIRST(T,size); FIELD(T,version)
#define END() printf("}}")
#define NEXT() printf(",")

int main(void)
{
    printf("{\"pointer_bits\":%zu,\"abi_major\":%u,\"abi_minor\":%u,\"contract_revision\":%u,\"layouts\":{", sizeof(void*) * 8, NSS_ABI_MAJOR, NSS_ABI_MINOR, NSS_CONTRACT_REVISION);
    BEGIN(nss_utf8_view); FIRST(nss_utf8_view,data); FIELD(nss_utf8_view,bytes); END(); NEXT();
    BEGIN(nss_version_v1); ENVELOPE(nss_version_v1); FIELD(nss_version_v1,abi_major); FIELD(nss_version_v1,abi_minor); FIELD(nss_version_v1,pointer_bits); FIELD(nss_version_v1,contract_revision); FIELD(nss_version_v1,build_utf8); END(); NEXT();
    BEGIN(nss_error_v1); ENVELOPE(nss_error_v1); FIELD(nss_error_v1,code); FIELD(nss_error_v1,stage); FIELD(nss_error_v1,hresult); FIELD(nss_error_v1,reserved); FIELD(nss_error_v1,message_utf8); END(); NEXT();
    BEGIN(nss_json_v1); ENVELOPE(nss_json_v1); FIELD(nss_json_v1,json); END(); NEXT();
    BEGIN(nss_event_v1); ENVELOPE(nss_event_v1); FIELD(nss_event_v1,sequence); FIELD(nss_event_v1,command_id); FIELD(nss_event_v1,object_id); FIELD(nss_event_v1,active_time_hns); FIELD(nss_event_v1,kind); FIELD(nss_event_v1,state); FIELD(nss_event_v1,status); FIELD(nss_event_v1,stage); FIELD(nss_event_v1,hresult); FIELD(nss_event_v1,reserved); FIELD(nss_event_v1,message_utf8); END(); NEXT();
    BEGIN(nss_preview_config_v1); ENVELOPE(nss_preview_config_v1); FIELD(nss_preview_config_v1,source_id); FIELD(nss_preview_config_v1,max_width); FIELD(nss_preview_config_v1,max_height); FIELD(nss_preview_config_v1,fps_num); FIELD(nss_preview_config_v1,fps_den); FIELD(nss_preview_config_v1,slots); FIELD(nss_preview_config_v1,channel); FIELD(nss_preview_config_v1,reserved); END(); NEXT();
    BEGIN(nss_preview_frame_v1); ENVELOPE(nss_preview_frame_v1); FIELD(nss_preview_frame_v1,session_id); FIELD(nss_preview_frame_v1,source_id); FIELD(nss_preview_frame_v1,generation); FIELD(nss_preview_frame_v1,frame_id); FIELD(nss_preview_frame_v1,capture_time_hns); FIELD(nss_preview_frame_v1,acquisition_uncertainty_hns); FIELD(nss_preview_frame_v1,bytes); FIELD(nss_preview_frame_v1,data); FIELD(nss_preview_frame_v1,width); FIELD(nss_preview_frame_v1,height); FIELD(nss_preview_frame_v1,stride); FIELD(nss_preview_frame_v1,pixel_format); FIELD(nss_preview_frame_v1,flags); FIELD(nss_preview_frame_v1,reserved); END(); NEXT();
    BEGIN(nss_gpu_surface_v1); ENVELOPE(nss_gpu_surface_v1); FIELD(nss_gpu_surface_v1,session_id); FIELD(nss_gpu_surface_v1,source_id); FIELD(nss_gpu_surface_v1,generation); FIELD(nss_gpu_surface_v1,frame_id); FIELD(nss_gpu_surface_v1,capture_time_hns); FIELD(nss_gpu_surface_v1,surface_d3d9); FIELD(nss_gpu_surface_v1,adapter_luid); FIELD(nss_gpu_surface_v1,width); FIELD(nss_gpu_surface_v1,height); FIELD(nss_gpu_surface_v1,flags); FIELD(nss_gpu_surface_v1,reserved); END(); NEXT();
    BEGIN(nss_packet_info_v1); ENVELOPE(nss_packet_info_v1); FIELD(nss_packet_info_v1,track_id); FIELD(nss_packet_info_v1,flags); FIELD(nss_packet_info_v1,pts_hns); FIELD(nss_packet_info_v1,dts_hns); FIELD(nss_packet_info_v1,duration_hns); FIELD(nss_packet_info_v1,bytes); END();
    printf("}}\n");
    return 0;
}
