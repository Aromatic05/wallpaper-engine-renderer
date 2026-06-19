#pragma once
#include "rendergraph/Pass.hpp"
#include <span>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <cstdint>
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
    virtual void refreshResources(Scene&, const Device&, RenderingResources&);
    virtual DeferredPrepareResourcesState requestDeferredPrepareResources(Scene&, const Device&) {
        return DeferredPrepareResourcesState::Ready;
    }
    virtual void prepareDeferred(Scene&, const Device&, RenderingResources&);
    virtual void execute(const Device&, RenderingResources&)         = 0;
    virtual void destory(const Device&, RenderingResources&)         = 0;
    virtual bool warmupPipeline(Scene&, const Device&, RenderingResources&) { return false; }
    virtual std::string residencyKey() const { return {}; }
    virtual bool canReuseForResidency(const VulkanPass& next_pass) const;
    virtual void absorbResidencyGraphState(const VulkanPass&);
    virtual bool referencesRenderTarget(std::string_view) const { return false; }
    virtual bool referencesTextLayer(int32_t) const { return false; }

    bool referencesAnyRenderTarget(const std::unordered_set<std::string>& render_targets) const {
        for (const auto& render_target : render_targets) {
            if (referencesRenderTarget(render_target)) return true;
        }
        return false;
    }

    bool referencesAnyTextLayer(const std::unordered_set<int32_t>& text_layer_ids) const {
        for (const auto text_layer_id : text_layer_ids) {
            if (referencesTextLayer(text_layer_id)) return true;
        }
        return false;
    }

    void addReleaseTexs(std::span<const std::string_view> texs) {
        for (const auto tex : texs) {
            if (tex.empty()) continue;
            if (std::find(m_release_texs.begin(), m_release_texs.end(), tex) !=
                m_release_texs.end()) {
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

private:
    bool                     m_prepared { false };
    std::vector<std::string> m_release_texs;
};
} // namespace vulkan
} // namespace wallpaper
