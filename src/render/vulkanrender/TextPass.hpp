#pragma once

#include "VulkanPass.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "vulkan/Device.hpp"
#include "vulkan/GraphicsPipeline.hpp"
#include "vulkan/StagingBuffer.hpp"

namespace wallpaper
{
class SceneTextPrimitive;
class SceneNode;
class SceneMesh;

namespace vulkan
{

class TextPass : public VulkanPass {
public:
    struct Desc {
        Scene*      scene { nullptr };
        SceneNode*  node { nullptr };
        // Authored layer ownership is separate from the SceneNode pointer because runtime text
        // rebuilds swap primitives under stable nodes; the layer id remains the durable refresh key.
        int32_t     layer_id { 0 };
        bool        execute_when_hidden { false };
        std::string output;

        ImageParameters          vk_output;
        vvk::Framebuffer         framebuffer;
        PipelineParameters       pipeline;
        StagingBufferRef         ubo_buf;
        ImageSlotsRef            background_texture;
        std::vector<ImageSlotsRef> page_textures;
        VkClearValue             clear_value {};
        bool                     clear_output { false };
    };

    TextPass(const Desc&);
    ~TextPass() override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void refreshResources(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;
    bool warmupPipeline(Scene&, const Device&, RenderingResources&) override;
    std::string residencyKey() const override;
    bool canReuseForResidency(const VulkanPass& next_pass) const override;
    void absorbResidencyGraphState(const VulkanPass&) override;
    bool referencesRenderTarget(std::string_view) const override;
    bool referencesTextLayer(int32_t) const override;

private:
    struct MeshBuffers {
        std::vector<StagingBufferRef> vertex_bufs;
        StagingBufferRef              index_buf;
        uint32_t                      draw_count { 0 };
        bool                          force_upload { true };
    };

    // The direct text pass owns its own dynamic mesh uploads because text geometry can change
    // without changing render-graph topology. Keeping the buffers inside the pass lets a single
    // pass instance absorb atlas page count and quad changes in place.
    bool ensureMeshBuffers(SceneMesh&, MeshBuffers&, RenderingResources&);
    bool refreshTextures(const Device&);
    bool recreateFramebuffer(const Device&);

    Desc m_desc;
    MeshBuffers m_background_buffers;
    std::vector<MeshBuffers> m_page_buffers;
    uint32_t m_loaded_atlas_version { std::numeric_limits<uint32_t>::max() };
};

} // namespace vulkan
} // namespace wallpaper
