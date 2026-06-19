#include "GraphicsPipeline.hpp"
#include "Device.hpp"

#include "TextureCache.hpp"
#include "utils/Logging.h"
#include "utils/AutoDeletor.hpp"
#include "vvk/vulkan_wrapper.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

using namespace wallpaper::vulkan;

namespace
{

inline VkShaderStageFlagBits ToVkType(wallpaper::ShaderType stage) {
    using namespace wallpaper;
    switch (stage) {
    case ShaderType::VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderType::FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderType::GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
    default: assert(false); return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

inline std::optional<vvk::ShaderModule> CreateShaderModule(const vvk::Device& device,
                                                           ShaderSpv&         spv) {
    auto&                    data = spv.spirv;
    VkShaderModuleCreateInfo ci {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext    = nullptr,
        .codeSize = data.size() * sizeof(decltype(data.back())),
        .pCode    = data.data(),
    };
    vvk::ShaderModule sm;
    VVK_CHECK_ACT(return std::nullopt, device.CreateShaderModule(ci, sm));
    return sm;
}

const char* ShaderStageName(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vertex";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "fragment";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "geometry";
    default: return "unknown";
    }
}

template<typename T>
void HashValue(std::size_t& seed, const T& value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T> || std::is_floating_point_v<T> ||
                      std::is_same_v<T, bool>,
                  "HashValue only accepts scalar pipeline-key fields");
    const auto hash = std::hash<T> {}(value);
    seed ^= hash + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

void HashString(std::size_t& seed, std::string_view value) {
    const auto hash = std::hash<std::string_view> {}(value);
    seed ^= hash + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

void HashColorBlend(std::size_t& seed, const VkPipelineColorBlendAttachmentState& state) {
    HashValue(seed, state.blendEnable);
    HashValue(seed, state.srcColorBlendFactor);
    HashValue(seed, state.dstColorBlendFactor);
    HashValue(seed, state.colorBlendOp);
    HashValue(seed, state.srcAlphaBlendFactor);
    HashValue(seed, state.dstAlphaBlendFactor);
    HashValue(seed, state.alphaBlendOp);
    HashValue(seed, state.colorWriteMask);
}

void HashStencilOp(std::size_t& seed, const VkStencilOpState& state) {
    HashValue(seed, state.failOp);
    HashValue(seed, state.passOp);
    HashValue(seed, state.depthFailOp);
    HashValue(seed, state.compareOp);
    HashValue(seed, state.compareMask);
    HashValue(seed, state.writeMask);
    HashValue(seed, state.reference);
}

void HashDescriptorBinding(std::size_t& seed, const VkDescriptorSetLayoutBinding& binding) {
    HashValue(seed, binding.binding);
    HashValue(seed, binding.descriptorType);
    HashValue(seed, binding.descriptorCount);
    HashValue(seed, binding.stageFlags);
    HashValue(seed, binding.pImmutableSamplers != nullptr);
}

void HashShaderSpv(std::size_t& seed, const ShaderSpv& spv) {
    HashValue(seed, spv.stage);
    HashString(seed, spv.entry_point);
    HashValue(seed, spv.spirv.size());
    for (const auto word : spv.spirv) {
        HashValue(seed, word);
    }
}

} // namespace

void CachedGraphicsPipelineState::abandon() noexcept {
    handle.abandon();
    layout.abandon();
    pass.abandon();
    for (auto& descriptor_layout : descriptor_layouts) {
        descriptor_layout.abandon();
    }
    descriptor_layouts.clear();
}

std::shared_ptr<CachedGraphicsPipelineState>
GraphicsPipelineStateCache::find(std::string_view key) const {
    const auto it = m_states.find(std::string(key));
    return it == m_states.end() ? nullptr : it->second;
}

void GraphicsPipelineStateCache::store(std::string key,
                                       std::shared_ptr<CachedGraphicsPipelineState> state) {
    if (key.empty() || state == nullptr) return;
    m_states[std::move(key)] = std::move(state);
}

void GraphicsPipelineStateCache::clear() { m_states.clear(); }

void GraphicsPipelineStateCache::abandon() noexcept {
    for (auto& [_, state] : m_states) {
        if (state) state->abandon();
    }
    m_states.clear();
}

void PipelineParameters::bindCachedState(std::shared_ptr<CachedGraphicsPipelineState> state) {
    cached_state = std::move(state);
    descriptor_layouts.clear();
    if (!cached_state) {
        handle.reset();
        layout.reset();
        pass.reset();
        return;
    }
    handle.handle = cached_state->handle ? *cached_state->handle : VK_NULL_HANDLE;
    layout.handle = cached_state->layout ? *cached_state->layout : VK_NULL_HANDLE;
    pass.handle   = cached_state->pass ? *cached_state->pass : VK_NULL_HANDLE;
    descriptor_layouts.reserve(cached_state->descriptor_layouts.size());
    for (const auto& descriptor_layout : cached_state->descriptor_layouts) {
        descriptor_layouts.push_back(descriptor_layout ? *descriptor_layout : VK_NULL_HANDLE);
    }
}

void PipelineParameters::resetCachedState() {
    cached_state.reset();
    handle.reset();
    layout.reset();
    pass.reset();
    descriptor_layouts.clear();
}

GraphicsPipeline::GraphicsPipeline() { toDefault(); }
GraphicsPipeline::~GraphicsPipeline() {}

void GraphicsPipeline::toDefault() {
    m_view = VkPipelineViewportStateCreateInfo {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext         = nullptr,
        .viewportCount = 1,
        .scissorCount  = 1

    };
    multisample = VkPipelineMultisampleStateCreateInfo {
        .sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext                 = nullptr,
        .rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable   = false,
        .minSampleShading      = 1.0f,
        .alphaToCoverageEnable = false,
        .alphaToOneEnable      = false,
    };

    depth = VkPipelineDepthStencilStateCreateInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .pNext = nullptr
    };

    raster = VkPipelineRasterizationStateCreateInfo {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext            = nullptr,
        .depthClampEnable = false,
        .polygonMode      = VK_POLYGON_MODE_FILL,
        .cullMode         = VK_CULL_MODE_NONE,
        .frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable  = false,
        .lineWidth        = 1.0f,
    };
    m_color_attachments.clear();
    m_color_attachments.push_back(VkPipelineColorBlendAttachmentState {
        .blendEnable    = false,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT });
    m_color = VkPipelineColorBlendStateCreateInfo {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext           = nullptr,
        .logicOpEnable   = false,
        .logicOp         = VK_LOGIC_OP_COPY,
        .attachmentCount = (uint32_t)m_color_attachments.size(),
        .pAttachments    = m_color_attachments.data(),
    };
    m_dynamic_states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    m_input_assembly = VkPipelineInputAssemblyStateCreateInfo {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext                  = nullptr,
        .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        .primitiveRestartEnable = false
    };
}

ShaderSpv* GraphicsPipeline::getShaderSpv(VkShaderStageFlagBits stage) const {
    if (exists(m_stage_spv_map, stage)) return m_stage_spv_map.at(stage).get();
    return nullptr;
}

GraphicsPipeline&
GraphicsPipeline::setColorBlendStates(std::span<const VkPipelineColorBlendAttachmentState> stats) {
    m_color_attachments     = { stats.begin(), stats.end() };
    m_color.attachmentCount = (u32)m_color_attachments.size();
    m_color.pAttachments    = m_color_attachments.data();
    return *this;
}

GraphicsPipeline& GraphicsPipeline::setLogicOp(bool enable, VkLogicOp op) {
    m_color.logicOp       = op;
    m_color.logicOpEnable = enable;
    return *this;
}

GraphicsPipeline& GraphicsPipeline::setRenderPass(vvk::RenderPass pass) {
    m_pass = std::move(pass);
    return *this;
}

GraphicsPipeline& GraphicsPipeline::addStage(Uni_ShaderSpv&& spv) {
    VkShaderStageFlagBits stage = ::ToVkType(spv->stage);
    m_stage_spv_map[stage]      = std::move(spv);
    return *this;
}

GraphicsPipeline& GraphicsPipeline::addDescriptorSetInfo(std::span<const DescriptorSetInfo> info) {
    for (auto& i : info) {
        m_descriptor_set_infos.push_back(i);
    }
    return *this;
}

GraphicsPipeline& GraphicsPipeline::addInputAttributeDescription(
    std::span<const VkVertexInputAttributeDescription> attrs) {
    for (auto& a : attrs) {
        m_input_attr_descriptions.push_back(a);
    }
    return *this;
}
GraphicsPipeline& GraphicsPipeline::addInputBindingDescription(
    std::span<const VkVertexInputBindingDescription> binds) {
    for (auto& b : binds) {
        m_input_bind_descriptions.push_back(b);
    }
    return *this;
}
GraphicsPipeline& GraphicsPipeline::setTopology(VkPrimitiveTopology topology) {
    m_input_assembly.topology = topology;
    return *this;
}

std::string GraphicsPipeline::buildCacheKey(std::string_view compatibility_key) const {
    std::size_t seed = 0xcbf29ce484222325ull;
    HashString(seed, compatibility_key);

    for (const auto& [stage, spv] : m_stage_spv_map) {
        HashValue(seed, stage);
        if (spv) HashShaderSpv(seed, *spv);
    }
    for (const auto& info : m_descriptor_set_infos) {
        HashValue(seed, info.push_descriptor);
        HashValue(seed, info.bindings.size());
        for (const auto& binding : info.bindings) {
            HashDescriptorBinding(seed, binding);
        }
    }
    for (const auto& binding : m_input_bind_descriptions) {
        HashValue(seed, binding.binding);
        HashValue(seed, binding.stride);
        HashValue(seed, binding.inputRate);
    }
    for (const auto& attribute : m_input_attr_descriptions) {
        HashValue(seed, attribute.location);
        HashValue(seed, attribute.binding);
        HashValue(seed, attribute.format);
        HashValue(seed, attribute.offset);
    }
    HashValue(seed, m_view.viewportCount);
    HashValue(seed, m_view.scissorCount);
    HashValue(seed, m_dynamic_states.size());
    for (const auto dynamic_state : m_dynamic_states) {
        HashValue(seed, dynamic_state);
    }
    HashValue(seed, m_color_attachments.size());
    for (const auto& blend : m_color_attachments) {
        HashColorBlend(seed, blend);
    }
    HashValue(seed, m_color.logicOpEnable);
    HashValue(seed, m_color.logicOp);
    for (const auto blend_constant : m_color.blendConstants) {
        HashValue(seed, blend_constant);
    }
    HashValue(seed, m_input_assembly.topology);
    HashValue(seed, m_input_assembly.primitiveRestartEnable);
    HashValue(seed, raster.depthClampEnable);
    HashValue(seed, raster.rasterizerDiscardEnable);
    HashValue(seed, raster.polygonMode);
    HashValue(seed, raster.cullMode);
    HashValue(seed, raster.frontFace);
    HashValue(seed, raster.depthBiasEnable);
    HashValue(seed, raster.depthBiasConstantFactor);
    HashValue(seed, raster.depthBiasClamp);
    HashValue(seed, raster.depthBiasSlopeFactor);
    HashValue(seed, raster.lineWidth);
    HashValue(seed, depth.depthTestEnable);
    HashValue(seed, depth.depthWriteEnable);
    HashValue(seed, depth.depthCompareOp);
    HashValue(seed, depth.depthBoundsTestEnable);
    HashValue(seed, depth.stencilTestEnable);
    HashStencilOp(seed, depth.front);
    HashStencilOp(seed, depth.back);
    HashValue(seed, depth.minDepthBounds);
    HashValue(seed, depth.maxDepthBounds);
    HashValue(seed, multisample.rasterizationSamples);
    HashValue(seed, multisample.sampleShadingEnable);
    HashValue(seed, multisample.minSampleShading);
    HashValue(seed, multisample.alphaToCoverageEnable);
    HashValue(seed, multisample.alphaToOneEnable);
    HashValue(seed, multisample.pSampleMask != nullptr);
    if (multisample.pSampleMask != nullptr) {
        HashValue(seed, multisample.pSampleMask[0]);
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016zx", seed);
    return std::string(compatibility_key) + "|state=" + buffer;
}

bool GraphicsPipeline::create(const Device& device, vvk::RenderPass& pass,
                              PipelineParameters& pipeline,
                              GraphicsPipelineStateCache* cache) {
    const char* debug_name = pipeline.debug_name.empty() ? "(unnamed)" : pipeline.debug_name.c_str();
    const auto cache_key =
        pipeline.cache_key.empty() ? std::string {} : buildCacheKey(pipeline.cache_key);
    if (cache != nullptr && !cache_key.empty()) {
        if (auto cached_state = cache->find(cache_key)) {
            pipeline.bindCachedState(std::move(cached_state));
            LOG_VERBOSE("GraphicsPipelineCache: hit name=%s key=%s cached-states=%zu",
                     debug_name,
                     cache_key.c_str(),
                     cache->size());
            return true;
        }
    }

    LOG_VERBOSE("GraphicsPipeline: create begin name=%s existingDescriptorLayouts=%zu existingLayout=%s existingPipeline=%s stageCount=%zu descriptorSetInfos=%zu",
             debug_name,
             pipeline.descriptor_layouts.size(),
             pipeline.layout ? "true" : "false",
             pipeline.handle ? "true" : "false",
             m_stage_spv_map.size(),
             m_descriptor_set_infos.size());
    if (pipeline.handle || pipeline.layout || pipeline.pass || !pipeline.descriptor_layouts.empty()) {
        LOG_VERBOSE("GraphicsPipeline: reset stale pipeline state name=%s descriptorLayouts=%zu layout=%s pipeline=%s pass=%s",
                 debug_name,
                 pipeline.descriptor_layouts.size(),
                 pipeline.layout ? "true" : "false",
                 pipeline.handle ? "true" : "false",
                 pipeline.pass ? "true" : "false");
        pipeline.resetCachedState();
    }
    for (const auto& [stage, spv] : m_stage_spv_map) {
        if (!spv) continue;
        LOG_VERBOSE("GraphicsPipeline: stage name=%s stage=%s entry=%s spirvWords=%zu",
                 debug_name,
                 ShaderStageName(stage),
                 spv->entry_point.c_str(),
                 spv->spirv.size());
    }
    VkPipelineDynamicStateCreateInfo dynamic_info {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext             = nullptr,
        .dynamicStateCount = (uint32_t)m_dynamic_states.size(),
        .pDynamicStates    = m_dynamic_states.data()
    };
    for (size_t info_index = 0; info_index < m_descriptor_set_infos.size(); ++info_index) {
        auto& info = m_descriptor_set_infos[info_index];
        LOG_VERBOSE("GraphicsPipeline: descriptor-set-info name=%s index=%zu push=%s bindingCount=%zu",
                 debug_name,
                 info_index,
                 info.push_descriptor ? "true" : "false",
                 info.bindings.size());
        for (size_t binding_index = 0; binding_index < info.bindings.size(); ++binding_index) {
            const auto& binding = info.bindings[binding_index];
            LOG_VERBOSE("GraphicsPipeline: descriptor-binding name=%s set=%zu bindingIndex=%zu binding=%u type=%u count=%u stageFlags=0x%x",
                     debug_name,
                     info_index,
                     binding_index,
                     binding.binding,
                     binding.descriptorType,
                     binding.descriptorCount,
                     binding.stageFlags);
        }
        VkDescriptorSetLayoutCreateInfo create_info {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = nullptr
        };
        VkDescriptorSetLayoutCreateFlags flags {};
        if (info.push_descriptor) flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;

        create_info.bindingCount = (u32)info.bindings.size();
        create_info.pBindings    = info.bindings.data();
        create_info.flags        = flags;
        vvk::DescriptorSetLayout layout;
        VVK_CHECK(device.handle().CreateDescriptorSetLayout(create_info, layout));
        if (!pipeline.cached_state) pipeline.cached_state = std::make_shared<CachedGraphicsPipelineState>();
        pipeline.cached_state->descriptor_layouts.emplace_back(std::move(layout));
        pipeline.descriptor_layouts.push_back(*pipeline.cached_state->descriptor_layouts.back());
        LOG_VERBOSE("GraphicsPipeline: descriptor-layout-created name=%s set=%zu handle=%p totalLayouts=%zu",
                 debug_name,
                 info_index,
                 reinterpret_cast<void*>(pipeline.descriptor_layouts.back()),
                 pipeline.descriptor_layouts.size());
    }
    {
        const auto& layouts = pipeline.descriptor_layouts;
        LOG_VERBOSE("GraphicsPipeline: create pipeline layout name=%s layoutCount=%zu renderPass=%p",
                 debug_name,
                 layouts.size(),
                 reinterpret_cast<void*>(*pass));
        for (size_t layout_index = 0; layout_index < layouts.size(); ++layout_index) {
            LOG_VERBOSE("GraphicsPipeline: pipeline-layout-entry name=%s index=%zu handle=%p",
                     debug_name,
                     layout_index,
                     reinterpret_cast<void*>(layouts[layout_index]));
        }

        VkPipelineLayoutCreateInfo ci {
            .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext          = nullptr,
            .setLayoutCount = (uint32_t)layouts.size(),
            .pSetLayouts    = layouts.data(),
        };
        vvk::PipelineLayout layout;
        VVK_CHECK(device.handle().CreatePipelineLayout(ci, layout));
        if (!pipeline.cached_state) pipeline.cached_state = std::make_shared<CachedGraphicsPipelineState>();
        pipeline.cached_state->layout = std::move(layout);
        pipeline.layout.handle = *pipeline.cached_state->layout;
        LOG_VERBOSE("GraphicsPipeline: create pipeline layout success name=%s layout=%p",
                 debug_name,
                 reinterpret_cast<void*>(*pipeline.cached_state->layout));
    }

    std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
    std::vector<vvk::ShaderModule>               shader_modules;
    for (auto& item : m_stage_spv_map) {
        auto&                           spv = item.second;
        VkPipelineShaderStageCreateInfo info {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .stage = ::ToVkType(spv->stage),
            .pName = spv->entry_point.c_str()
        };
        if (auto opt = CreateShaderModule(device.handle(), *spv); opt.has_value()) {
            shader_modules.emplace_back(std::move(opt.value()));
            info.module = *shader_modules.back();
        }

        shaderStages.push_back(info);
    }
    /*
    AUTO_DELETER(shadermodule, ([&shaderStages, &device]() {
                     for (auto& sha : shaderStages) {
                         device.handle().destroyShaderModule(sha.module);
                     }
                 }));
    */

    VkPipelineVertexInputStateCreateInfo input {
        .sType                         = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext                         = nullptr,
        .vertexBindingDescriptionCount = (uint32_t)m_input_bind_descriptions.size(),
        .pVertexBindingDescriptions    = m_input_bind_descriptions.data(),
        .vertexAttributeDescriptionCount = (uint32_t)m_input_attr_descriptions.size(),
        .pVertexAttributeDescriptions    = m_input_attr_descriptions.data()
    };

    VkGraphicsPipelineCreateInfo create {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = nullptr,
        .stageCount          = (uint32_t)shaderStages.size(),
        .pStages             = shaderStages.data(),
        .pVertexInputState   = &input,
        .pInputAssemblyState = &m_input_assembly,
        .pViewportState      = &m_view,
        .pRasterizationState = &raster,
        .pMultisampleState   = &multisample,
        .pDepthStencilState  = &depth,
        .pColorBlendState    = &m_color,
        .pDynamicState       = &dynamic_info,
        .layout              = *pipeline.layout,
        .renderPass          = *pass,
    };
    vvk::Pipeline handle;
    VVK_CHECK_BOOL_RE(device.handle().CreateGraphicsPipeline(create, handle));
    if (!pipeline.cached_state) pipeline.cached_state = std::make_shared<CachedGraphicsPipelineState>();
    pipeline.cached_state->handle = std::move(handle);
    pipeline.handle.handle = *pipeline.cached_state->handle;
    LOG_VERBOSE("GraphicsPipeline: create graphics pipeline success name=%s pipeline=%p",
             debug_name,
             reinterpret_cast<void*>(*pipeline.cached_state->handle));
    pipeline.cached_state->pass = std::move(pass);
    pipeline.pass.handle = *pipeline.cached_state->pass;
    if (cache != nullptr && !cache_key.empty()) {
        cache->store(cache_key, pipeline.cached_state);
        LOG_VERBOSE("GraphicsPipelineCache: store name=%s key=%s cached-states=%zu",
                 debug_name,
                 cache_key.c_str(),
                 cache->size());
    }
    return true;
}
