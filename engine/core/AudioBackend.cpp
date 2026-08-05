#include "core/AudioBackend.h"

namespace core {

namespace {

constexpr core::Named<AudioBackend> kAudioBackends[] = {
    {"auto", AudioBackend::Auto},
    {"device", AudioBackend::Device},
    {"null", AudioBackend::Null},
};
static_assert(core::namesEveryValue(kAudioBackends), "a backend reachable from the enum and from no name");

} // namespace

core::Names<AudioBackend> audioBackendNames() {
    return kAudioBackends;
}

} // namespace core
