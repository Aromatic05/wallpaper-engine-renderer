#pragma once

#include "VulkanPass.hpp"

#include <string>

#include "scene/Scene.h"
#include "vulkan/Device.hpp"

namespace wallpaper
{
namespace vulkan
{

class ClearPass : public VulkanPass {
public:
    struct Desc {
        std::string     target;
        ImageParameters vk_target;
        VkClearValue    clear_value {};
    };

    explicit ClearPass(const Desc&);
    ~ClearPass() override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

    const Desc& desc() const { return m_desc; }

private:
    Desc m_desc;
};

} // namespace vulkan
} // namespace wallpaper
