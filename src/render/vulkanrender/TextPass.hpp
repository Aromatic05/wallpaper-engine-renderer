#pragma once

#include "VulkanPass.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "scene/Scene.h"
#include "vulkan/GraphicsPipeline.hpp"
#include "vulkan/StagingBuffer.hpp"
#include "vulkan/Device.hpp"

namespace wallpaper
{
class SceneMesh;
class SceneTextPrimitive;

namespace vulkan
{

class TextPass : public VulkanPass {
public:
    struct Desc {
        Scene*      scene { nullptr };
        SceneNode*  node { nullptr };
        int32_t     layer_id { 0 };
        std::string output;

        ImageParameters            vk_output;
        vvk::Framebuffer           framebuffer;
        PipelineParameters         pipeline;
        StagingBufferRef           ubo_buf;
        ImageSlotsRef              background_texture;
        std::vector<ImageSlotsRef> page_textures;
        VkClearValue               clear_value {};
        bool                       clear_output { false };
    };

    explicit TextPass(const Desc&);
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

    const Desc& desc() const { return m_desc; }

private:
    struct MeshBuffers {
        std::vector<StagingBufferRef> vertex_bufs;
        StagingBufferRef              index_buf;
        uint32_t                      draw_count { 0 };
        bool                          force_upload { true };
    };

    SceneTextPrimitive* primitive() const;
    bool refreshTextures(const Device&);
    bool recreateFramebuffer(const Device&);
    bool ensureMeshBuffers(SceneMesh&, MeshBuffers&, RenderingResources&);

    Desc m_desc;
    MeshBuffers m_background_buffers;
    std::vector<MeshBuffers> m_page_buffers;
    uint32_t m_loaded_atlas_version { std::numeric_limits<uint32_t>::max() };
};

} // namespace vulkan
} // namespace wallpaper
