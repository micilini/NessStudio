#pragma once
#include "session.hpp"
namespace nss {
std::unique_ptr<Backend> make_recording_backend(const Json &, Fault &);
std::unique_ptr<Backend> make_auxiliary_backend(const Json &, Fault &);
}
