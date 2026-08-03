#pragma once

#include <volk.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gfx {

struct VulkanContext;

/// Read a compiled SPIR-V module from SUBSTRATE_SHADER_DIR. Aborts if it is missing.
std::vector<uint32_t> readShaderBinary(const std::string& name);

/**
 * @brief Make `spirv`'s bytes this process's answer for `name`, ahead of any file.
 *
 * Hot reload's one way to publish what it just compiled. The bytes are read here and
 * held in memory; the file is the caller's to delete, and nothing writes them back out.
 * That is the whole point — a build directory then holds only what the build put there,
 * so a cold start can only run SPIR-V CMake produced, and a reloaded module cannot
 * outlive the session that compiled it.
 *
 * Keyed by the same name `readShaderBinary` takes, so every consumer of a module picks
 * the new bytes up without knowing this exists: `loadShader` for the pipelines and
 * `verifyShaderBindings` for the Debug reflection check.
 *
 * False if the file could not be read, which the caller reports — it has the shader's
 * name and the compiler's output and this does not.
 */
bool overrideShaderBinary(const std::string& name, const std::filesystem::path& spirv);

/// Load a compiled SPIR-V module from SUBSTRATE_SHADER_DIR.
VkShaderModule loadShader(const VulkanContext& ctx, const std::string& name);

/**
 * @brief The fields of VkGraphicsPipelineCreateInfo that actually differ per pass.
 *
 * A plain parameter struct, not a builder — assign the fields you care about and
 * pass it to createGraphicsPipeline(). Everything not listed here is identical
 * across every pass and lives in the .cpp.
 *
 * This exists only because three passes would otherwise each copy ~120 lines of
 * identical struct-filling.
 */
struct GraphicsPipelineDesc {
    std::string vertexShader;
    std::string fragmentShader;

    std::vector<VkVertexInputBindingDescription> vertexBindings;
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;

    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    /// Every pass in this engine draws triangles except one: S4.5's debug lines. A field
    /// with a default rather than a second pipeline builder, because that is all the
    /// difference amounts to -- the line pipeline shares every other piece of state a
    /// fullscreen triangle pipeline uses.
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    bool depthTest = true;
    bool depthWrite = true;
    /// Reverse-Z: near is 1, far is 0, so "closer" means greater.
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;

    /**
     * @brief How the fragment combines with what is already in the attachment.
     *
     * A selector rather than two booleans, for the reason TONEMAP_OPERATOR is one: the
     * modes are mutually exclusive, and a pair of flags would spell two states that
     * cannot both be true and one that means nothing.
     */
    enum class Blend {
        None,      ///< Overwrite. Every geometry pass.
        AlphaOver, ///< Straight src-alpha over dst. The overlay and the forward pass.
        Additive,  ///< src + dst. Screen-space reflections composite this way, because
                   ///< a reflection is radiance the surface adds rather than replaces.
        /// src + dst * (1 - src.a), with src.rgb already multiplied through. Volumetric
        /// fog composites this way: in-scattered light is added *and* what is behind it
        /// is attenuated, and additive alone would give the fog a glow with nothing
        /// hidden behind it.
        PremultipliedOver,
    };
    Blend blend = Blend::None;

    /**
     * @brief Fragment-stage specialisation constants, indexed by `constant_id`.
     *
     * `constants[i]` is the value of `layout(constant_id = i)` in the fragment shader,
     * so the vector index *is* the id and there is nothing to keep in sync but the
     * order. Every value is a `uint32_t`, which covers the boolean case too: SPIR-V
     * specialises a `bool` from a `VkBool32`, and 0/1 in a `uint32_t` is exactly that.
     *
     * Empty leaves every constant at the default the shader declares. A list shorter
     * than the shader declares is fine; the trailing constants keep their defaults.
     *
     * Only the fragment stage gets these. Every feature constant in this engine gates
     * shading, and attaching the same values to a vertex shader that declares none
     * would be noise in the create-info rather than a second place to look.
     */
    std::vector<uint32_t> constants;
};

VkPipeline createGraphicsPipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                  const GraphicsPipelineDesc& desc);

/**
 * @brief Build a compute pipeline from a shader name and a layout.
 *
 * Deliberately one function and no more. There is no compute-pass type, no dispatch
 * wrapper and no descriptor plumbing here: each site calls `vkCmdDispatch` directly,
 * exactly as the graphics passes call `vkCmdDraw`.
 *
 * **No specialisation constants, and that is not an omission.** The validation layers in
 * SDK 1.3.280 fold spec constants themselves and reject the SPIR-V 1.6
 * `OpExecutionModeId LocalSizeId` glslang emits for every compute shader in this tree --
 * so a specialised compute stage logs `does not contain valid spirv` on a module
 * `spirv-val` accepts. Anything a compute pass needs to vary goes in a push constant.
 */
VkPipeline createComputePipeline(const VulkanContext& ctx, VkPipelineLayout layout, const std::string& shaderName);

} // namespace gfx
