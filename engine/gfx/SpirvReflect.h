#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gfx {

/**
 * @brief Just enough SPIR-V parsing to check a module against a hand-written layout.
 *
 * **Must not be used to generate anything.** Descriptor layouts stay hand-written in
 * `Renderer::createDescriptorLayouts`; a generated layout agrees with whatever the shader
 * said and so catches nothing -- including the `sampler2DMS`-versus-`sampler2D` mismatch
 * this exists to catch.
 *
 * Anything it does not understand it skips, and a module it cannot parse yields an empty
 * result the caller reads as "nothing to check". Aborting on an unknown opcode would turn
 * a safety net into a liability.
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
    /// Every `constant_id` the module declares, ascending. A pipeline must supply a value
    /// for each -- an unspecialised constant silently keeps its GLSL default, which is
    /// invariably the "feature on" case.
    std::vector<uint32_t> specConstantIds;
};

ReflectedModule reflectSpirv(const std::vector<uint32_t>& code);

} // namespace gfx
