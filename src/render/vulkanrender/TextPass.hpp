#pragma once

#include "VulkanPass.hpp"

#include <cstdint>
#include <string>

#include "scene/Scene.h"
#include "vulkan/Device.hpp"

namespace wallpaper
{
namespace vulkan
{

class TextPass : public VulkanPass {
public:
    struct Desc {
        SceneNode*  node { nullptr };
        int32_t     layer_id { 0 };
        std::string output;

        ImageParameters vk_output;
        VkClearValue    clear_value {};
        bool            clear_output { false };
    };

    explicit TextPass(const Desc&);
    ~TextPass() override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;
    std::string residencyKey() const override;
    bool canReuseForResidency(const VulkanPass& next_pass) const override;
    void absorbResidencyGraphState(const VulkanPass&) override;
    bool referencesRenderTarget(std::string_view) const override;
    bool referencesTextLayer(int32_t) const override;

    const Desc& desc() const { return m_desc; }
    bool executionDiagnosticEmitted() const { return m_execution_diagnostic_emitted; }

private:
    Desc m_desc;
    bool m_execution_diagnostic_emitted { false };
};

} // namespace vulkan
} // namespace wallpaper
