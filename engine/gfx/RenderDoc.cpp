#include "gfx/RenderDoc.h"

#include "core/Logger.h"

#include <renderdoc_app.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gfx {
namespace {

RENDERDOC_API_1_6_0* api = nullptr;
bool attachAttempted = false;

} // namespace

bool renderDocAttach(const std::filesystem::path& pathTemplate) {
    if (attachAttempted) return api != nullptr;
    attachAttempted = true;

    // RTLD_NOLOAD, and GetModuleHandle rather than LoadLibrary, are load-bearing: both
    // return a handle only for a module *already* in the process. A plain dlopen or
    // LoadLibrary would pull in a second, unhooked copy of RenderDoc behind the Vulkan
    // loader's back.
#ifdef _WIN32
    HMODULE lib = GetModuleHandleA("renderdoc.dll");
#else
    void* lib = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
#endif
    if (lib == nullptr) {
        core::Logger::status(core::LogCategory::Render,
                       "RenderDoc: capture layer not in this process; set ENABLE_VULKAN_RENDERDOC_CAPTURE=1 "
                       "(scripts/rdoc.sh does)");
        return false;
    }

#ifdef _WIN32
    // Through void*: GetProcAddress returns a function pointer of a different type, and
    // casting between the two directly is what -Wcast-function-type exists to catch.
    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(
        reinterpret_cast<void*>(GetProcAddress(lib, "RENDERDOC_GetAPI")));
#else
    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(lib, "RENDERDOC_GetAPI"));
#endif
    if (getApi == nullptr || getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) != 1) {
        core::Logger::warn(core::LogCategory::Render, "RenderDoc: the capture library is loaded but offers no 1.6.0 API");
        api = nullptr;
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(pathTemplate.parent_path(), ec);
    api->SetCaptureFilePathTemplate(pathTemplate.string().c_str());

    // Unbind RenderDoc's own hotkey: it defaults to F12, which this application already
    // uses for the PNG capture, so leaving both bound makes one keypress write two files.
    api->SetCaptureKeys(nullptr, 0);

    core::Logger::status(core::LogCategory::Render, "RenderDoc: attached, captures go to %s_frameNNNN.rdc",
                   pathTemplate.string().c_str());
    return true;
}

bool renderDocAttached() { return api != nullptr; }

void renderDocTrigger(uint32_t frames) {
    if (api == nullptr) {
        core::Logger::warn(core::LogCategory::Render, "RenderDoc: capture requested but the layer is not loaded; none written");
        return;
    }
    api->TriggerMultiFrameCapture(frames);
    core::Logger::status(core::LogCategory::Render, "RenderDoc: capturing %u frame%s", frames, frames == 1 ? "" : "s");
}

uint32_t renderDocCaptureCount() { return api != nullptr ? api->GetNumCaptures() : 0; }

} // namespace gfx
