#include "gfx/Pipeline.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "gfx/Resources.h"
#include "gfx/VulkanContext.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

#ifndef SUBSTRATE_SHADER_DIR
#define SUBSTRATE_SHADER_DIR "shaders"
#endif
#ifndef SUBSTRATE_GAME_SHADER_DIR
#define SUBSTRATE_GAME_SHADER_DIR "shaders/game"
#endif

namespace gfx {

namespace {

/// What this process compiled for itself, keyed the way `readShaderBinary` is called.
/// Dies with the process; see `overrideShaderBinary`.
std::unordered_map<std::string, std::vector<uint32_t>> g_shaderOverrides;

/// Whole-file read of a `.spv`. The stream must already be open `ate | binary` -- the
/// size comes from where that left the get pointer.
std::vector<uint32_t> readSpirv(std::ifstream& file) {
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
    return code;
}

}  // namespace

bool overrideShaderBinary(const std::string& name, const std::filesystem::path& spirv) {
    std::ifstream file(spirv, std::ios::ate | std::ios::binary);
    if (!file.is_open()) return false;
    g_shaderOverrides[name] = readSpirv(file);
    return true;
}

std::vector<uint32_t> readShaderBinary(const std::string& name) {
    // Hot reload's output, ahead of both directories because it is by definition newer.
    if (const auto it = g_shaderOverrides.find(name); it != g_shaderOverrides.end()) return it->second;

    const std::string leaf = name + ".spv";

    // `operator/` discards its left operand when the right one is absolute, so this one
    // expression covers both builds: absolute macros in a dev build resolve to the build
    // directories, and the relative fallbacks above put a package's SPIR-V beside the
    // executable. See engine/core/Paths.h.
    //
    // The game's tree first: a game shader sharing an engine shader's name must win, and
    // it wins by being found first rather than by overwriting anything.
    const std::filesystem::path gamePath = core::executableDir() / SUBSTRATE_GAME_SHADER_DIR / leaf;
    const std::filesystem::path enginePath = core::executableDir() / SUBSTRATE_SHADER_DIR / leaf;

    std::ifstream file(gamePath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) file.open(enginePath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        // The resolved paths, not the macros: in a package the macro is "shaders" and says
        // nothing about where that was looked for.
        core::Logger::critical(core::LogCategory::Render, "Cannot open shader %s in either %s or %s", leaf.c_str(),
                         gamePath.string().c_str(), enginePath.string().c_str());
    }

    return readSpirv(file);
}

VkShaderModule loadShader(const VulkanContext& ctx, const std::string& name) {
    const std::vector<uint32_t> code = readShaderBinary(name);

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(ctx.device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

VkPipeline createGraphicsPipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                  const GraphicsPipelineDesc& desc) {
    VkShaderModule vert = loadShader(ctx, desc.vertexShader);
    // An empty fragment shader name is a depth-only pipeline, not a mistake: binding any
    // module containing `discard` -- `shadow.frag` does, for foliage -- costs early-Z and
    // the double-rate depth path whether or not the discard is reached.
    VkShaderModule frag = desc.fragmentShader.empty() ? VK_NULL_HANDLE : loadShader(ctx, desc.fragmentShader);

    // One entry per constant, id == index, with `pData` pointing at `desc.constants`
    // itself -- so the offsets are the array offsets and `desc` must outlive the create.
    std::vector<VkSpecializationMapEntry> specEntries(desc.constants.size());
    for (uint32_t i = 0; i < specEntries.size(); ++i) {
        specEntries[i] = {i, i * static_cast<uint32_t>(sizeof(uint32_t)), sizeof(uint32_t)};
    }

    VkSpecializationInfo specInfo{};
    specInfo.mapEntryCount = static_cast<uint32_t>(specEntries.size());
    specInfo.pMapEntries = specEntries.data();
    specInfo.dataSize = desc.constants.size() * sizeof(uint32_t);
    specInfo.pData = desc.constants.data();

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";

    const uint32_t stageCount = frag != VK_NULL_HANDLE ? 2u : 1u;
    if (frag != VK_NULL_HANDLE) {
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        if (!desc.constants.empty()) stages[1].pSpecializationInfo = &specInfo;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(desc.vertexBindings.size());
    vertexInput.pVertexBindingDescriptions = desc.vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(desc.vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = desc.vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = desc.topology;

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cullMode;
    raster.frontFace = desc.frontFace;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    // `sampleShadingEnable` stays false: the fragment shader runs once per pixel and
    // writes every covered sample. Enabling it moves per-sample cost into every geometry
    // pass, where the lighting pass is the one that wants it.
    multisample.rasterizationSamples = desc.samples;

    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = desc.depthCompare;

    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(desc.colorFormats.size());
    for (auto& b : blendAttachments) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
        b.blendEnable = desc.blend != GraphicsPipelineDesc::Blend::None ? VK_TRUE : VK_FALSE;
        b.colorBlendOp = VK_BLEND_OP_ADD;
        b.alphaBlendOp = VK_BLEND_OP_ADD;
        if (desc.blend == GraphicsPipelineDesc::Blend::Additive) {
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        } else if (desc.blend == GraphicsPipelineDesc::Blend::PremultipliedOver) {
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        } else {
            b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
    }

    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    blend.pAttachments = blendAttachments.data();

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    // Dynamic rendering: formats are declared here instead of in a VkRenderPass.
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(desc.colorFormats.size());
    renderingInfo.pColorAttachmentFormats = desc.colorFormats.data();
    renderingInfo.depthAttachmentFormat = desc.depthFormat;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &renderingInfo;
    info.stageCount = stageCount;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCheck(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");

    // Derived from the shader pair rather than passed in, so a capture's pipeline name
    // cannot drift from the shader it actually runs.
    const std::string pipelineName =
        desc.vertexShader + " + " + (desc.fragmentShader.empty() ? "(depth only)" : desc.fragmentShader);
    setObjectName(ctx, reinterpret_cast<uint64_t>(pipeline), VK_OBJECT_TYPE_PIPELINE, pipelineName.c_str());

    vkDestroyShaderModule(ctx.device, vert, nullptr);
    if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(ctx.device, frag, nullptr);
    return pipeline;
}

VkPipeline createComputePipeline(const VulkanContext& ctx, VkPipelineLayout layout, const std::string& shaderName) {
    VkShaderModule module = loadShader(ctx, shaderName);

    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCheck(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateComputePipelines");

    setObjectName(ctx, reinterpret_cast<uint64_t>(pipeline), VK_OBJECT_TYPE_PIPELINE, shaderName.c_str());

    vkDestroyShaderModule(ctx.device, module, nullptr);
    return pipeline;
}

} // namespace gfx
