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
 * The bytes are held in memory and never written back, so a cold start can only run SPIR-V
 * CMake produced and a reloaded module cannot outlive the session that compiled it.
 * Keyed by the same name `readShaderBinary` takes, so `loadShader` and
 * `verifyShaderBindings` both pick the new bytes up without knowing this exists.
 *
 * False if the file could not be read; the caller reports, having the name and the
 * compiler's output that this does not.
 */
bool overrideShaderBinary(const std::string& name, const std::filesystem::path& spirv);

/// Load a compiled SPIR-V module from SUBSTRATE_SHADER_DIR.
VkShaderModule loadShader(const VulkanContext& ctx, const std::string& name);

/**
 * @brief The fields of VkGraphicsPipelineCreateInfo that actually differ per pass.
 *
 * A plain parameter struct, not a builder. Everything not listed here is identical across
 * every pass and lives in the .cpp.
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

    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    bool depthTest = true;
    bool depthWrite = true;
    /// Reverse-Z: near is 1, far is 0, so "closer" means greater.
    VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;

    /// @brief How the fragment combines with what is already in the attachment.
    enum class Blend {
        None,      ///< Overwrite.
        AlphaOver, ///< Straight src-alpha over dst.
        Additive,  ///< src + dst.
        /// src + dst * (1 - src.a), with src.rgb **already multiplied through** -- a
        /// shader writing unpremultiplied colour here gets a glow with nothing occluded.
        PremultipliedOver,
    };
    Blend blend = Blend::None;

    /**
     * @brief Fragment-stage specialisation constants, indexed by `constant_id`.
     *
     * `constants[i]` is the value of `layout(constant_id = i)`, so the vector index *is*
     * the id -- inserting an element renumbers every constant after it. A short list is
     * fine; the trailing constants keep the defaults the shader declares.
     *
     * **Fragment stage only.** A vertex shader declaring a `constant_id` will not receive
     * one from here.
     */
    std::vector<uint32_t> constants;
};

VkPipeline createGraphicsPipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                  const GraphicsPipelineDesc& desc);

/**
 * @brief Build a compute pipeline from a shader name and a layout.
 *
 * **Takes no specialisation constants.** The validation layers in SDK 1.3.280 fold spec
 * constants themselves and then reject the SPIR-V 1.6 `OpExecutionModeId LocalSizeId`
 * glslang emits for every compute shader here, logging `does not contain valid spirv` on a
 * module `spirv-val` accepts. Vary a compute pass through a push constant instead.
 */
VkPipeline createComputePipeline(const VulkanContext& ctx, VkPipelineLayout layout, const std::string& shaderName);

} // namespace gfx
