#pragma once

#include "rendergraph/RenderGraph.hpp"
#include "output/VulkanOutputInit.hpp"
#include "output/swapchain/ExSwapchain.hpp"
#include "Type.hpp"

#include <cstdio>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace wallpaper
{
class Scene;

namespace vulkan
{
class FinPass;

class VulkanRender {
public:
    VulkanRender();
    ~VulkanRender();

    bool init(RenderInitInfo);

    void destroy();

    void drawFrame(Scene&);
    void setPaused(bool paused);

    void clearLastRenderGraph(bool clear_scene_caches = false);
    void clearRenderGraphResources();
    void compileRenderGraph(Scene&, rg::RenderGraph&, bool refresh_resources_only = false);
    void warmupRenderGraphPipelines(Scene&, rg::RenderGraph&);
    void refreshImportedTextures(Scene&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);
    bool captureNextOffscreenFrame(std::string output_path, int32_t frame_number = 1,
                                   std::string* error_message = nullptr);

    ExSwapchain* exSwapchain() const;
    bool inited() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
} // namespace vulkan
} // namespace wallpaper
