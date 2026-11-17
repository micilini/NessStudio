#include <nessstudio/nessstudio.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"ABI failure at line %d\n",__LINE__); return 1; } } while(0)
int main(void) {
    nss_version_v1 version = {0}; nss_error_v1 error = {0};
    version.size = sizeof(version); version.version = NSS_STRUCT_VERSION;
    error.size = sizeof(error); error.version = NSS_STRUCT_VERSION;
    CHECK(nss_get_version(&version) == NSS_OK && version.pointer_bits == 64);
    CHECK(sizeof(nss_version_v1) == 88 && sizeof(nss_error_v1) == 280 && sizeof(nss_json_v1) == 24);
    CHECK(nss_get_version(NULL) == NSS_E_ARGUMENT);
    version.size = 8; CHECK(nss_get_version(&version) == NSS_E_VERSION);
    {
        const char* text = "{\"version\":1,\"clientName\":\"C-consumer\"}";
        nss_json_v1 config = {sizeof(nss_json_v1),1,{(const uint8_t*)text,0}};
        nss_engine* engine = NULL; nss_blob* blob = NULL; nss_utf8_view view = {0};
        config.json.bytes = strlen(text);
        CHECK(nss_engine_create(&config,&engine,&error) == NSS_OK && engine);
        CHECK(nss_runtime_quiesce(0,&error) == NSS_TIMEOUT);
        CHECK(nss_engine_capabilities(engine,&blob,&error) == NSS_OK && blob);
        nss_engine_release(engine);
        CHECK(nss_blob_view(blob,&view) == NSS_OK && view.bytes > 0);
        nss_blob_release(blob);
    }
    CHECK(nss_runtime_quiesce(0,&error) == NSS_OK);
    printf("{\"status\":\"PASS\",\"consumer\":\"C11\",\"versionSize\":%zu,\"errorSize\":%zu,\"jsonSize\":%zu}\n",sizeof(nss_version_v1),sizeof(nss_error_v1),sizeof(nss_json_v1));
    return 0;
}
