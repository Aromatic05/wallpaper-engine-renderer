#pragma once
#include "VulkanPass.hpp"
#include <functional>
#include <string>

#include "vulkan/Device.hpp"
#include "scene/Scene.h"

namespace wallpaper
{
namespace vulkan
{

class CopyPass : public VulkanPass {
public:
    struct Desc {
        std::string src;
        std::string dst;

        ImageParameters vk_src;
        ImageParameters vk_dst;
        std::function<bool()> should_execute;
    };

    CopyPass(const Desc&);
    virtual ~CopyPass();

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

    const Desc& desc() const { return m_desc; }

private:
    Desc m_desc;
};

}
}
