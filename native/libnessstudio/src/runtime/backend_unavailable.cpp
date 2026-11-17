#include "session.hpp"
#include "recording.hpp"
namespace nss {
std::unique_ptr<Backend> make_backend(const Json& request, Fault& error) {
    if (request.contains("testFixture") || (!request.contains("capture") && !request.contains("camera") && !request.contains("audio") && !request.contains("previewOnly"))) {
        error = {NSS_E_UNAVAILABLE, NSS_STAGE_PLAN, 0, "An explicit physical capture/encoder/output plan is required; synthetic media is unavailable"};
        return {};
    }
    if (!request.contains("scene") && (request.contains("previewOnly") || (!request.contains("capture") && !request.contains("camera") && request.contains("audio"))))
        return make_auxiliary_backend(request, error);
    return make_recording_backend(request, error);
}
bool test_backend_build() noexcept { return false; }
}
