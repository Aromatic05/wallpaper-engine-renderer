#include "ClearPass.hpp"

#include "PassCommon.hpp"
#include "Resource.hpp"
#include "utils/Logging.h"

using namespace wallpaper::vulkan;

ClearPass::ClearPass(const Desc& desc): m_desc(desc) {}

ClearPass::~ClearPass() = default;

void ClearPass::prepare(Scene& scene, const Device& device, RenderingResources&) {
    if (scene.renderTargets.count(m_desc.target) == 0) {
        LOG_ERROR("ClearPass: target render target not found: %s", m_desc.target.c_str());
        return;
    }

    const auto& rt = scene.renderTargets.at(m_desc.target);
    if (auto opt = device.tex_cache().Query(m_desc.target, ToTexKey(rt), ! rt.allowReuse);
        opt.has_value()) {
        m_desc.vk_target = opt.value();
    } else {
        LOG_ERROR("ClearPass: query image from cache failed: %s", m_desc.target.c_str());
        return;
    }

    setPrepared();
}

void ClearPass::execute(const Device&, RenderingResources& rr) {
    auto& img = m_desc.vk_target;
    if (! img.handle) {
        setPrepared(false);
        return;
    }

    VkImageSubresourceRange range {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_MIP_LEVELS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_ARRAY_LAYERS,
    };

    VkImageMemoryBarrier to_transfer {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext            = nullptr,
        .srcAccessMask    = 0,
        .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image            = img.handle,
        .subresourceRange = range,
    };

    auto& cmd = rr.command;
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        to_transfer);

    cmd.ClearColorImage(img.handle,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &m_desc.clear_value.color,
                        range);

    VkImageMemoryBarrier to_shader_read {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext            = nullptr,
        .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask    = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image            = img.handle,
        .subresourceRange = range,
    };

    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        to_shader_read);
}

void ClearPass::destory(const Device&, RenderingResources&) {
    setPrepared(false);
    clearReleaseTexs();
}
