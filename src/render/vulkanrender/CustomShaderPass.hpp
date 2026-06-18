#pragma once
#include "VulkanPass.hpp"
#include <functional>
#include <string>
#include <vector>

#include "vulkan/Device.hpp"
#include "scene/Scene.h"
#include "vulkan/StagingBuffer.hpp"
#include "vulkan/GraphicsPipeline.hpp"
#include "scene/SpriteAnimation.hpp"
#include "interface/IShaderValueUpdater.h"

namespace wallpaper
{

namespace vulkan
{

class CustomShaderPass : public VulkanPass {
public:
    struct Desc {
        // in
        SceneNode*               node { nullptr };
        std::vector<std::string> textures;
        std::string              output;
        std::string              cameraOverride;
        bool                     clearBeforeDraw { false };
        bool                     forceAlphaWrite { false };
        bool                     premultipliedSourceBlend { false };
        std::function<bool()>    should_execute;
        sprite_map_t             sprites_map;

        // -----prepared
        // vulkan texs
        std::vector<ImageSlotsRef> vk_textures;
        std::vector<i32>           vk_tex_binding;
        ImageParameters            vk_output;

        // bufs
        bool                          dyn_vertex { false };
        std::vector<StagingBufferRef> vertex_bufs;
        StagingBufferRef              index_buf;
        StagingBufferRef              ubo_buf;

        // pipeline
        VkClearValue       clear_value;
        bool               blending { false };
        vvk::Framebuffer   fb;
        PipelineParameters pipeline;
        u32                draw_count { 0 };

        // uniforms
        std::function<void()> update_op;
    };

    CustomShaderPass(const Desc&);
    virtual ~CustomShaderPass();

    void setDescTex(u32 index, std::string_view tex_key);

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;
    std::string residencyKey() const override;
    const Desc& desc() const { return m_desc; }

private:
    Desc m_desc;
};

} // namespace vulkan
} // namespace wallpaper
