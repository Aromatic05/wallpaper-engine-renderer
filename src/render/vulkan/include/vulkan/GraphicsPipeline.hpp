#pragma once
#include "Instance.hpp"
#include "core/MapSet.hpp"
#include "Spv.hpp"
#include <memory>
#include <string>
#include <unordered_map>

namespace wallpaper
{
namespace vulkan
{

template<typename HandleType>
struct BorrowedPipelineHandle {
    HandleType handle { VK_NULL_HANDLE };

    explicit operator bool() const noexcept { return handle != VK_NULL_HANDLE; }
    const HandleType& operator*() const noexcept { return handle; }
    void reset() noexcept { handle = VK_NULL_HANDLE; }
    void abandon() noexcept { handle = VK_NULL_HANDLE; }
};

struct CachedGraphicsPipelineState {
    vvk::Pipeline       handle;
    vvk::PipelineLayout layout;
    vvk::RenderPass     pass;
    std::vector<vvk::DescriptorSetLayout> descriptor_layouts;

    void abandon() noexcept;
};

class GraphicsPipelineStateCache {
public:
    std::shared_ptr<CachedGraphicsPipelineState> find(std::string_view key) const;
    void store(std::string key, std::shared_ptr<CachedGraphicsPipelineState> state);
    void clear();
    void abandon() noexcept;
    std::size_t size() const noexcept { return m_states.size(); }

private:
    std::unordered_map<std::string, std::shared_ptr<CachedGraphicsPipelineState>> m_states;
};

struct PipelineParameters {
    BorrowedPipelineHandle<VkPipeline>       handle;
    BorrowedPipelineHandle<VkPipelineLayout> layout;
    BorrowedPipelineHandle<VkRenderPass>     pass;

    std::string debug_name;
    // A caller-supplied semantic key that describes the render-pass compatibility contract
    // (attachment format/load operation/depth mode). GraphicsPipeline mixes this with reflected
    // shader, descriptor, vertex-input, and fixed-function state before touching the cache,
    // mirroring game-engine PSO caches where visibility/resource residency is separate from
    // pipeline lifetime.
    std::string cache_key;
    std::vector<VkDescriptorSetLayout> descriptor_layouts;
    std::shared_ptr<CachedGraphicsPipelineState> cached_state;

    void bindCachedState(std::shared_ptr<CachedGraphicsPipelineState>);
    void resetCachedState();
};

struct DescriptorSetInfo {
    bool push_descriptor { false };

    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

class Device;

class GraphicsPipeline : NoCopy, NoMove {
public:
    GraphicsPipeline();
    ~GraphicsPipeline();

    void toDefault();
    bool create(const Device&, vvk::RenderPass&, PipelineParameters&,
                GraphicsPipelineStateCache* cache = nullptr);

    VkPipelineMultisampleStateCreateInfo   multisample {};
    VkPipelineRasterizationStateCreateInfo raster {};
    VkPipelineDepthStencilStateCreateInfo  depth {};

    ShaderSpv*  getShaderSpv(VkShaderStageFlagBits) const;
    const auto& pass() const { return m_pass; }

    GraphicsPipeline& setColorBlendStates(std::span<const VkPipelineColorBlendAttachmentState>);
    GraphicsPipeline& setLogicOp(bool enable, VkLogicOp);

    // required after default
    GraphicsPipeline& setRenderPass(vvk::RenderPass);
    GraphicsPipeline& addDescriptorSetInfo(std::span<const DescriptorSetInfo>);
    GraphicsPipeline& addStage(Uni_ShaderSpv&&);
    GraphicsPipeline& addInputAttributeDescription(std::span<const VkVertexInputAttributeDescription>);
    GraphicsPipeline& addInputBindingDescription(std::span<const VkVertexInputBindingDescription>);
    GraphicsPipeline& setTopology(VkPrimitiveTopology);

private:
    std::string buildCacheKey(std::string_view compatibility_key) const;

    vvk::RenderPass m_pass;

    VkPipelineInputAssemblyStateCreateInfo         m_input_assembly {};
    std::vector<VkVertexInputBindingDescription>   m_input_bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> m_input_attr_descriptions;

    VkPipelineViewportStateCreateInfo                m_view;
    VkPipelineColorBlendStateCreateInfo              m_color;
    std::vector<VkDynamicState>                      m_dynamic_states;
    std::vector<VkPipelineColorBlendAttachmentState> m_color_attachments;
    std::vector<DescriptorSetInfo>                   m_descriptor_set_infos;
    Map<VkShaderStageFlagBits, Uni_ShaderSpv>        m_stage_spv_map;
};

} // namespace vulkan
} // namespace wallpaper
