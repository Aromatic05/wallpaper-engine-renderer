#pragma once
#include "rendergraph/Pass.hpp"
#include <cstdint>
#include <span>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <unordered_set>

namespace wallpaper
{

class Scene;

namespace vulkan
{

class Device;
class RenderingResources;
class Resource;

enum class DeferredPrepareResourcesState
{
    Ready,
    Waiting,
};

class VulkanPass : public rg::Pass {
public:
    VulkanPass()                                                     = default;
    virtual ~VulkanPass()                                            = default;
    virtual void prepare(Scene&, const Device&, RenderingResources&) = 0;
    // Resource-only render-graph refreshes keep the pass topology and compiled pipelines alive,
    // and only need each pass to rebind texture-cache-backed images or recreate size-dependent
    // framebuffers. The default implementation is intentionally empty so passes without external
    // GPU resources can stay prepared and skip all extra work on minute-level text updates.
    virtual void refreshResources(Scene&, const Device&, RenderingResources&) {}
    // Dynamic passes write their current CPU-side vertex/index bytes into the shared staging
    // buffer. VulkanRender calls this before recording the frame's staging-buffer upload, so
    // particle systems that grow into newly allocated dynamic subranges after a reused-scene source
    // switch cannot draw from previous-frame or uninitialized GPU bytes. Uniform and sprite updates
    // intentionally stay on execute() because text/effect composites depend on the original pass
    // ordering for their transform and texture-projection state.
    virtual void updateBeforeUpload() {}
    // Deferred runtime preparation is split into an asset-streaming phase and a Vulkan residency
    // phase. A pass returns Waiting after it has queued background CPU work, allowing the renderer
    // to keep drawing prepared content instead of blocking the render thread on asset decoding.
    virtual DeferredPrepareResourcesState requestDeferredPrepareResources(Scene&, const Device&) {
        return DeferredPrepareResourcesState::Ready;
    }
    // Deferred preparation is used for runtime visibility changes after a resident graph already
    // exists. The default path is equivalent to ordinary prepare(), while heavier passes can
    // override it to enforce a non-blocking streaming contract before any synchronous fallback work
    // is allowed to run on the render thread.
    virtual void prepareDeferred(Scene& scene, const Device& device, RenderingResources& resources) {
        prepare(scene, device, resources);
    }
    virtual void execute(const Device&, RenderingResources&)         = 0;
    virtual void destory(const Device&, RenderingResources&)         = 0;
    // Pipeline warm-up mirrors PSO precompilation in game engines: build immutable pipeline state
    // without binding layer-owned textures, framebuffers, or mesh buffers. Hidden deferred layers
    // can therefore keep memory residency at zero while their future visible frame avoids
    // vkCreateGraphicsPipelines.
    virtual bool warmupPipeline(Scene&, const Device&, RenderingResources&) { return false; }
    virtual std::string residencyKey() const { return {}; }
    virtual bool canReuseForResidency(const VulkanPass& next_pass) const;
    virtual void absorbResidencyGraphState(const VulkanPass&) {}
    virtual bool referencesRenderTarget(std::string_view) const { return false; }
    virtual bool referencesTextLayer(int32_t) const { return false; }

    bool referencesAnyRenderTarget(const std::unordered_set<std::string>& render_targets) const {
        // Selective resource refreshes are driven by render-target keys. A pass only needs to run
        // when it writes one of those targets or samples one of them through a descriptor; otherwise
        // its prepared framebuffer, descriptors, and mesh uploads remain valid for this frame.
        for (const auto& render_target : render_targets) {
            if (referencesRenderTarget(render_target)) return true;
        }
        return false;
    }

    bool referencesAnyTextLayer(const std::unordered_set<int32_t>& text_layer_ids) const {
        // Dynamic text updates can replace glyph atlas pages and meshes without touching any
        // render-target size. This pass-level hook lets the resource-refresh path update only the
        // TextPass that owns the changed layer instead of falling back to a full graph refresh.
        for (const auto text_layer_id : text_layer_ids) {
            if (referencesTextLayer(text_layer_id)) return true;
        }
        return false;
    }

    void addReleaseTexs(std::span<const std::string_view> texs) {
        for (const auto tex : texs) {
            if (tex.empty()) continue;

            // Render-graph compilation owns the release list as declarative lifecycle metadata. Keep
            // this append operation idempotent so repeated graph assignment or duplicate final-read
            // edges cannot make one pass call TextureCache::MarkShareReady() more than once for the
            // same logical key.
            if (std::find(m_release_texs.begin(), m_release_texs.end(), tex) != m_release_texs.end()) {
                continue;
            }
            m_release_texs.emplace_back(tex);
        }
    }
    bool                         prepared() const { return m_prepared; }
    std::span<const std::string> releaseTexs() const { return m_release_texs; }
    void                         clearReleaseTexs() { m_release_texs.clear(); }

protected:
    void setPrepared(bool v = true) { m_prepared = v; }
    void releaseFinalReadTexs(const Device& device) const;

private:
    bool                     m_prepared { false };
    std::vector<std::string> m_release_texs;
};
} // namespace vulkan
} // namespace wallpaper
