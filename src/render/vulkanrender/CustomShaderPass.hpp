#pragma once
#include "VulkanPass.hpp"
#include <functional>
#include <string>
#include <vector>

#include "vulkan/Device.hpp"
#include "scene/Scene.h"
#include "vulkan/StagingBuffer.hpp"
#include "vulkan/GraphicsPipeline.hpp"
#include "SpriteAnimation.hpp"
#include "interface/IShaderValueUpdater.h"

namespace wallpaper
{

namespace vulkan
{

class CustomShaderPass : public VulkanPass {
public:
    struct Desc {
        // in
        // Keeping a scene pointer inside the prepared pass description lets execution-time
        // diagnostics inspect the live render-target metadata that the pass is sampling from and
        // writing to. That is essential for tracking text/effect bugs where physical Vulkan image
        // sizes diverge from the authored logical content rectangle.
        Scene*                   scene { nullptr };
        SceneNode*               node { nullptr };
        int32_t                  layer_id { 0 };
        bool                     execute_when_hidden { false };
        // Optional runtime gate for topology-stable helper passes. This is used for the synthetic
        // hidden-final-effect composite: the pass exists in the graph, but it must only draw on
        // frames where the final authored effect is locally hidden.
        std::function<bool()>    should_execute;
        std::vector<std::string> textures;
        std::string              output;
        bool                     force_alpha_write { false };
        // Some helper passes publish an offscreen target whose RGB is already premultiplied by the
        // producer's translucent blend. The pass still uses the material's authored blend mode, but
        // the RGB source factor must be ONE to avoid multiplying color by alpha twice.
        bool                     premultiplied_source_blend { false };
        // Some render-order composition routes seed a private effect source before publishing the
        // resolved result into a parent compose layer. Those source targets must start from
        // transparent pixels, but later authored effect passes still keep their normal load policy.
        bool                     clear_before_draw { false };
        // Render-order composition routes can draw an otherwise ordinary scene node into an offscreen
        // composition source. The node must keep its authored camera binding for graph ownership, but
        // this particular pass needs to project through the composition layer's source camera so the
        // target texture uses the layer's own aspect ratio instead of the global framebuffer aspect.
        std::string              camera_override;
        // Framebuffer-seeded compose layers are the inverse: the pass writes an offscreen source
        // target, while uniforms such as g_ModelViewProjectionMatrix must describe the active screen
        // camera so the composelayer shader samples the current framebuffer region that the final
        // world-space writer will replace.
        bool                     use_active_camera_for_uniforms { false };
        bool                     use_active_camera_for_parallax { false };
        sprite_map_t             sprites_map;

        // -----prepared
        // vulkan texs
        std::vector<ImageSlotsRef> vk_textures;
        std::vector<i32>           vk_tex_binding;
        ImageParameters            vk_output;

        // bufs
        bool                          dyn_vertex { false };
        bool                          force_dyn_upload { false };
        std::vector<StagingBufferRef> vertex_bufs;
        StagingBufferRef              index_buf;
        StagingBufferRef              ubo_buf;

        // pipeline
        VkClearValue       clear_value;
        VkClearValue       depth_clear_value;
        bool               blending { false };
        bool               model_pass { false };
        bool               depth_test { false };
        bool               depth_write { false };
        bool               clear_depth { true };
        vvk::Framebuffer   fb;
        VmaImageParameters* depth_image_ref { nullptr };
        PipelineParameters pipeline;
        u32                draw_count { 0 };

        // Dynamic mesh bytes are uploaded before command recording, while uniforms and sprite
        // frame selection remain on the historical execute-time path for text/effect stability.
        std::function<void()> update_dynamic_mesh_op;
        // uniforms
        std::function<void()> update_op;
    };

    CustomShaderPass(const Desc&);
    virtual ~CustomShaderPass();

    const Desc& desc() const { return m_desc; }
    void setDescTex(u32 index, std::string_view tex_key);

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void prepareDeferred(Scene&, const Device&, RenderingResources&) override;
    void refreshResources(Scene&, const Device&, RenderingResources&) override;
    void updateBeforeUpload() override;
    DeferredPrepareResourcesState requestDeferredPrepareResources(Scene&, const Device&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;
    bool warmupPipeline(Scene&, const Device&, RenderingResources&) override;
    std::string residencyKey() const override;
    bool canReuseForResidency(const VulkanPass& next_pass) const override;
    void absorbResidencyGraphState(const VulkanPass&) override;
    bool referencesRenderTarget(std::string_view) const override;

private:
    Desc m_desc;
};

} // namespace vulkan
} // namespace wallpaper
