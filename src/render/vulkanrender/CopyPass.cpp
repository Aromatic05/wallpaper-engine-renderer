#include "CopyPass.hpp"
#include "SpecTexs.hpp"
#include "utils/Logging.h"
#include "utils/AutoDeletor.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"

using namespace wallpaper::vulkan;

CopyPass::CopyPass(const Desc& desc): m_desc(desc) {}

CopyPass::~CopyPass() {};

std::string CopyPass::residencyKey() const {
    return "CopyPass|src=" + m_desc.src + "|dst=" + m_desc.dst;
}

bool CopyPass::canReuseForResidency(const VulkanPass& next_pass) const {
    const auto* next = dynamic_cast<const CopyPass*>(&next_pass);
    return next != nullptr && residencyKey() == next->residencyKey() &&
           m_desc.track_source_extent == next->m_desc.track_source_extent;
}

void CopyPass::absorbResidencyGraphState(const VulkanPass& next_pass) {
    VulkanPass::absorbResidencyGraphState(next_pass);
    const auto* next = dynamic_cast<const CopyPass*>(&next_pass);
    if (next == nullptr) return;
    m_desc.should_execute = next->m_desc.should_execute;
}

bool CopyPass::referencesRenderTarget(std::string_view render_target) const {
    return m_desc.src == render_target || m_desc.dst == render_target;
}

bool CopyPass::synchronizeDestinationTarget(Scene& scene) {
    const auto source = scene.renderTargets.find(m_desc.src);
    if (source == scene.renderTargets.end()) return false;

    auto destination = scene.renderTargets.find(m_desc.dst);
    if (destination == scene.renderTargets.end()) {
        auto inherited       = source->second;
        inherited.allowReuse = true;
        scene.renderTargets.insert_or_assign(m_desc.dst, std::move(inherited));
    } else if (m_desc.track_source_extent) {
        destination->second.width      = source->second.width;
        destination->second.height     = source->second.height;
        destination->second.mapWidth   = source->second.mapWidth;
        destination->second.mapHeight  = source->second.mapHeight;
        destination->second.allowReuse = true;
    }
    return true;
}

void CopyPass::prepare(Scene& scene, const Device& device, RenderingResources&) {
    if (! synchronizeDestinationTarget(scene)) {
        LOG_ERROR("%s not found", m_desc.src.c_str());
        return;
    }

    std::array<std::string, 2>      textures    = { m_desc.src, m_desc.dst };
    std::array<ImageParameters*, 2> vk_textures = { &m_desc.vk_src, &m_desc.vk_dst };
    for (usize i = 0; i < textures.size(); i++) {
        auto& tex_name = textures[i];
        if (tex_name.empty()) continue;

        ImageParameters img;
        if (IsSpecTex(tex_name)) {
            auto& rt  = scene.renderTargets.at(tex_name);
            auto  opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            if (opt.has_value())
                img = opt.value();
            else
                LOG_ERROR("query image from cache failed");
        } else {
            LOG_ERROR("can't copy image source");
            return;
        }
        *vk_textures[i] = img;
    }

    // Copy passes are prepared before shader passes so graph dependencies exist before deferred
    // residency begins. Releasing a final-read source here would let a later producer bind a new
    // physical image for the same logical render target while this pass keeps the old handle. Keep
    // the source resident until this pass reaches its actual final-read boundary in execute().
    setPrepared();
};

void CopyPass::refreshResources(Scene& scene, const Device& device, RenderingResources&) {
    if (! synchronizeDestinationTarget(scene)) {
        setPrepared(false);
        return;
    }
    std::array<std::string, 2>      textures    = { m_desc.src, m_desc.dst };
    std::array<ImageParameters*, 2> vk_textures = { &m_desc.vk_src, &m_desc.vk_dst };
    for (usize i = 0; i < textures.size(); i++) {
        auto& tex_name = textures[i];
        if (tex_name.empty()) continue;

        if (scene.renderTargets.count(tex_name) == 0) {
            setPrepared(false);
            return;
        }
        auto& rt  = scene.renderTargets.at(tex_name);
        auto  opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
        if (!opt.has_value()) {
            setPrepared(false);
            return;
        }
        *vk_textures[i] = opt.value();
    }
}
void CopyPass::execute(const Device& device, RenderingResources& rr) {
    if (m_desc.should_execute && ! m_desc.should_execute()) {
        releaseFinalReadTexs(device);
        return;
    }

    auto& cmd = rr.command;
    auto& src = m_desc.vk_src;
    auto& dst = m_desc.vk_dst;

    if (! (src.handle && dst.handle)) {
        assert(src.handle && dst.handle);
        releaseFinalReadTexs(device);
        return;
    }

    if (src.extent.width > dst.extent.width || src.extent.height > dst.extent.height ||
        src.extent.depth > dst.extent.depth) {
        LOG_ERROR("copy image extent exceeds destination: src='%s' %ux%ux%u dst='%s' %ux%ux%u",
                  m_desc.src.c_str(),
                  src.extent.width,
                  src.extent.height,
                  src.extent.depth,
                  m_desc.dst.c_str(),
                  dst.extent.width,
                  dst.extent.height,
                  dst.extent.depth);
        releaseFinalReadTexs(device);
        return;
    }

    VkImageSubresourceRange srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,

    };
    VkImageCopy copy {
        .srcSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = srang.aspectMask,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .dstSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = srang.aspectMask,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .extent = { src.extent.width, src.extent.height, 1 },
    };
    {
        VkImageMemoryBarrier in_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image            = src.handle,
            .subresourceRange = srang,
        };
        VkImageMemoryBarrier out_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image            = dst.handle,
            .subresourceRange = srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            {},
                            {},
                            std::array { in_bar, out_bar });
    }
    cmd.CopyImage(src.handle,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  dst.handle,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  copy);
    {
        VkImageMemoryBarrier in_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = src.handle,
            .subresourceRange = srang,
        };
        VkImageMemoryBarrier out_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = dst.handle,
            .subresourceRange = srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            {},
                            {},
                            std::array { in_bar, out_bar });
    }

    if (dst.mipmap_level > 1) {
        device.tex_cache().RecGenerateMipmaps(cmd, dst);
    }
    releaseFinalReadTexs(device);
};
void CopyPass::destory(const Device&, RenderingResources&) {}
