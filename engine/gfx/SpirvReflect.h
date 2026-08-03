#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gfx {

/**
 * @brief Just enough SPIR-V parsing to check a module against a hand-written layout.
 *
 * Deliberately *not* a reflection library and deliberately not used to generate
 * anything. Descriptor layouts stay hand-written in `Renderer::createDescriptorLayouts`
 * where they can be read off the page; this walks the module and reports what it
 * actually declares so the two can be compared in Debug.
 *
 * Generating the layouts from reflection instead would remove the mismatch by removing
 * the ability to see what is bound, which is the wrong trade. It also would not have
 * caught the stage-5 `sampler2DMS`-versus-`sampler2D` bug, because a generated layout
 * would have agreed with whatever the shader said.
 *
 * Scope is the binary layout of the module header and the handful of opcodes below;
 * anything it does not understand it skips. A module it cannot parse produces an empty
 * result, which reads as "nothing to check" rather than as a failure -- this is a
 * safety net, and a safety net that aborts on an opcode it has not seen before is a
 * liability.
 */
struct ReflectedBinding {
    uint32_t set = 0;
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    /// Elements in the descriptor array. 0 means an unsized runtime array -- the
    /// descriptor-indexing case -- where the shader declares no count to check.
    uint32_t count = 1;
    /// From OpName, for the error message. Empty if the module was stripped.
    std::string name;
};

struct ReflectedModule {
    std::vector<ReflectedBinding> bindings;
    /// Every `constant_id` the module declares, ascending. A pipeline must supply a
    /// value for each: an unspecialised constant silently keeps its GLSL default, and
    /// that default is invariably the "feature on" case.
    std::vector<uint32_t> specConstantIds;
};

ReflectedModule reflectSpirv(const std::vector<uint32_t>& code);

} // namespace gfx
