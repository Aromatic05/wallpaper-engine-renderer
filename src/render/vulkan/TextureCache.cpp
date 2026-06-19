
#include "Shader.hpp"
#include "Swapchain.hpp"
#include "TextureCache.hpp"
#include "Device.hpp"
#include "Util.hpp"

#include "Image.hpp"
#include "core/MapSet.hpp"
#include "core/ArrayHelper.hpp"
#include "utils/AutoDeletor.hpp"
#include "utils/Hash.h"
#include "Parameters.hpp"
#include "vvk/vulkan_wrapper.hpp"

#include <drm/drm_fourcc.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>

using namespace wallpaper;
using namespace wallpaper::vulkan;

namespace wallpaper
{
namespace vulkan
{
VkFormat ToVkType(TextureFormat tf) {
    switch (tf) {
    case TextureFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case TextureFormat::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case TextureFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case TextureFormat::R8: return VK_FORMAT_R8_UNORM;
    case TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::RGB8: return VK_FORMAT_R8G8B8_UNORM;
    case TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    default: assert(false); return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

VkSamplerAddressMode ToVkType(wallpaper::TextureWrap sam) {
    using namespace wallpaper;
    switch (sam) {
    case TextureWrap::CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::REPEAT:
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}
VkFilter ToVkType(wallpaper::TextureFilter sam) {
    using namespace wallpaper;
    switch (sam) {
    case TextureFilter::LINEAR: return VK_FILTER_LINEAR;
    case TextureFilter::NEAREST:
    default: return VK_FILTER_NEAREST;
    }
}
} // namespace vulkan
} // namespace wallpaper

namespace
{
constexpr uint32_t kDefaultDmabufFourcc = DRM_FORMAT_ABGR8888;
constexpr VkFormatFeatureFlags2 kRequiredDmabufFeatures =
    static_cast<VkFormatFeatureFlags2>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) |
    static_cast<VkFormatFeatureFlags2>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) |
    static_cast<VkFormatFeatureFlags2>(VK_FORMAT_FEATURE_TRANSFER_DST_BIT);

std::vector<uint64_t> FilterSupportedDrmModifiers(const vvk::PhysicalDevice& gpu,
                                                  VkFormat                   format,
                                                  std::span<const uint64_t>  preferred_modifiers) {
    if (preferred_modifiers.empty())
        return {};

    const auto supported_modifiers = gpu.GetDrmFormatModifierProperties2(format);
    std::vector<uint64_t> usable_modifiers;
    usable_modifiers.reserve(preferred_modifiers.size());

    for (const auto preferred_modifier : preferred_modifiers) {
        const auto it =
            std::find_if(supported_modifiers.begin(),
                         supported_modifiers.end(),
                         [preferred_modifier](const VkDrmFormatModifierProperties2EXT& candidate) {
                             return candidate.drmFormatModifier == preferred_modifier &&
                                 (candidate.drmFormatModifierTilingFeatures &
                                  kRequiredDmabufFeatures) == kRequiredDmabufFeatures;
                         });
        if (it != supported_modifiers.end())
            usable_modifiers.push_back(preferred_modifier);
    }

    return usable_modifiers;
}

VkSamplerCreateInfo GenSamplerInfo(TextureKey key) {
    auto& sam = key.sample;

    VkSamplerCreateInfo sampler_info { .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                       .pNext            = nullptr,
                                       .magFilter        = ToVkType(sam.magFilter),
                                       .minFilter        = (ToVkType(sam.minFilter)),
                                       .mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                       .addressModeU     = (ToVkType(sam.wrapS)),
                                       .addressModeV     = (ToVkType(sam.wrapS)),
                                       .addressModeW     = (ToVkType(sam.wrapT)),
                                       .anisotropyEnable = (false),
                                       .maxAnisotropy    = (1.0f),
                                       .compareEnable    = (false),
                                       .compareOp        = VK_COMPARE_OP_NEVER,
                                       .minLod           = (0.0f),
                                       .maxLod           = (1.0f),
                                       .borderColor      = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
                                       .unnormalizedCoordinates = (false) };
    return sampler_info;
}

VkResult TransImgLayout(const vvk::Queue& queue, vvk::CommandBuffer& cmd,
                        const ImageParameters& image, VkImageLayout layout) {
    VkResult result;
    do {
        result = cmd.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        });
        if (result != VK_SUCCESS) break;

        VkImageSubresourceRange subresourceRange {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount     = VK_REMAINING_ARRAY_LAYERS,
        };
        {
            VkImageMemoryBarrier out_bar {
                .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext            = nullptr,
                .srcAccessMask    = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout        = layout,
                .image            = image.handle,
                .subresourceRange = subresourceRange,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                out_bar);
        }
        result = cmd.End();
        if (result != VK_SUCCESS) break;

        VkSubmitInfo sub_info {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = cmd.address(),
        };
        result = queue.Submit(sub_info);
    } while (false);
    return result;
}

void RecordClearNewRenderTargetToTransparentBlack(vvk::CommandBuffer& cmd,
                                                  const ImageParameters& image) {
    VkImageSubresourceRange subresourceRange {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_MIP_LEVELS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_ARRAY_LAYERS,
    };
    {
        VkImageMemoryBarrier to_transfer {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = 0,
            .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image            = image.handle,
            .subresourceRange = subresourceRange,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            to_transfer);
    }

    const VkClearColorValue transparent_black { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    cmd.ClearColorImage(image.handle,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &transparent_black,
                        subresourceRange);

    {
        VkImageMemoryBarrier to_shader_read {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = image.handle,
            .subresourceRange = subresourceRange,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            to_shader_read);
    }
}

std::size_t ImageAllocationBytes(const VmaImageParameters& image) {
    return image.handle ? static_cast<std::size_t>(image.handle.AllocationSize()) : 0u;
}

std::size_t ImageSlotsAllocationBytes(const ImageSlots& slots) {
    std::size_t total = 0;
    for (const auto& image : slots.slots) {
        total += ImageAllocationBytes(image);
    }
    return total;
}

std::size_t ImageMipmapUploadBytes(const ImageData& mipmap) {
    if (mipmap.size > 0) {
        return static_cast<std::size_t>(mipmap.size);
    }
    return static_cast<std::size_t>(std::max(mipmap.width, 0)) *
           static_cast<std::size_t>(std::max(mipmap.height, 0)) * 4u;
}

std::size_t ImageSlotUploadBytes(const Image::Slot& slot) {
    std::size_t total = 0;
    for (const auto& mipmap : slot.mipmaps) {
        total += ImageMipmapUploadBytes(mipmap);
    }
    return total;
}

std::size_t PendingImageUploadBytes(
    std::span<const TextureCachePendingImageUpload> uploads) {
    std::size_t total = 0;
    for (const auto& upload : uploads) {
        for (const auto& stage : upload.stage_bufs) {
            total += stage.req_size;
        }
    }
    return total;
}

bool TextureSlotsResident(const ImageSlots& slots) {
    if (slots.slots.empty()) return false;
    for (const auto& slot : slots.slots) {
        if (!slot.handle || !slot.view || !slot.sampler) return false;
    }
    return true;
}

std::optional<vvk::DeviceMemory> AllocateMemory(const vvk::Device& device, vvk::PhysicalDevice gpu,
                                                VkMemoryRequirements  reqs,
                                                VkMemoryPropertyFlags property,
                                                void*                 pNext = NULL) {
    VkPhysicalDeviceMemoryProperties pros = gpu.GetMemoryProperties().memoryProperties;
    for (uint32_t i = 0; i < pros.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1 << i)) && (pros.memoryTypes[i].propertyFlags & property)) {
            VkMemoryAllocateInfo memory_allocate_info { .sType =
                                                            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .pNext           = pNext,
                                                        .allocationSize  = reqs.size,
                                                        .memoryTypeIndex = i };
            vvk::DeviceMemory    mem;
            VkResult             res = device.AllocateMemory(memory_allocate_info, mem);
            if (res == VK_SUCCESS) {
                return mem;
            } else {
                VVK_CHECK(res);
                return std::nullopt;
            }
        }
    }
    LOG_ERROR("vulkan allocate memory failed, no memory match requires");
    return std::nullopt;
}

std::optional<ExImageParameters> CreateExImage(uint32_t width, uint32_t height, VkFormat format,
                                               VkImageTiling       tiling,
                                               VkSamplerCreateInfo sampler_info,
                                               VkImageUsageFlags usage, const vvk::Device& device,
                                               const vvk::PhysicalDevice& gpu,
                                               ExternalFrameExportMode export_mode,
                                               uint32_t export_drm_fourcc,
                                               std::span<const uint64_t> export_drm_modifiers) {
    ExImageParameters image;
    do {
        const bool dmabuf_export = export_mode == ExternalFrameExportMode::DMA_BUF;
        const auto drm_fourcc = export_drm_fourcc != 0 ? export_drm_fourcc : kDefaultDmabufFourcc;
        const auto usable_drm_modifiers =
            dmabuf_export ? FilterSupportedDrmModifiers(gpu, format, export_drm_modifiers)
                          : std::vector<uint64_t> {};
        const bool use_modifier_tiling = !usable_drm_modifiers.empty();

        if (dmabuf_export && !use_modifier_tiling && tiling != VK_IMAGE_TILING_LINEAR) {
            LOG_ERROR("dma-buf export currently requires linear image tiling");
            break;
        }

        const auto handle_type = dmabuf_export
            ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
            : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalMemoryImageCreateInfo ex_info {
            .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext       = NULL,
            .handleTypes = handle_type
        };
        VkImageDrmFormatModifierListCreateInfoEXT drm_modifier_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .pNext = &ex_info,
            .drmFormatModifierCount = static_cast<uint32_t>(usable_drm_modifiers.size()),
            .pDrmFormatModifiers = usable_drm_modifiers.data(),
        };
        VkExportMemoryAllocateInfo ex_mem_info { .sType =
                                                     VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                                 .pNext = NULL,
                                                 .handleTypes = handle_type };
        VkImageCreateInfo          info {
                     .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                     .pNext       = use_modifier_tiling
                        ? static_cast<const void*>(&drm_modifier_info)
                        : static_cast<const void*>(&ex_info),
                     .imageType   = VK_IMAGE_TYPE_2D,
                     .format      = format,
                     .extent      = VkExtent3D { .width = width, .height = height, .depth = 1 },
                     .mipLevels   = 1,
                     .arrayLayers = 1,
                     .samples     = VK_SAMPLE_COUNT_1_BIT,
                     .tiling      = use_modifier_tiling
                        ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                        : tiling,
                     .usage       = usage,
                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                     .queueFamilyIndexCount = 0,
                     .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        image.extent = info.extent;
        image.handle_type = dmabuf_export
            ? ExternalFrameHandleType::DMA_BUF
            : ExternalFrameHandleType::OPAQUE_FD;

        VVK_CHECK_ACT(break, device.CreateImage(info, image.handle));

        image.mem_reqs = device.GetImageMemoryRequirements(*image.handle);

        if (auto opt = AllocateMemory(
                device, gpu, image.mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ex_mem_info);
            opt.has_value()) {
            image.mem = std::move(opt.value());
        } else
            break;

        VVK_CHECK_ACT(break, image.handle.BindMemory(*image.mem, 0));
        {
            VkImageViewCreateInfo createinfo {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext    = nullptr,
                .image    = *image.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = format,
                .subresourceRange =
                    VkImageSubresourceRange {
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel   = 0,
                        .levelCount     = 1,
                        .baseArrayLayer = 0,
                        .layerCount     = 1,
                    },
            };
            VVK_CHECK_ACT(break, device.CreateImageView(createinfo, image.view));
        }
        VVK_CHECK_ACT(break, device.CreateSampler(sampler_info, image.sampler));
        VVK_CHECK_ACT(break, image.mem.GetMemoryFdKHR(handle_type, &image.fd));

        if (dmabuf_export) {
            if (format != VK_FORMAT_R8G8B8A8_UNORM) {
                LOG_ERROR("unsupported dma-buf export format: %d", static_cast<int>(format));
                break;
            }

            const VkImageSubresource subresource {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .arrayLayer = 0,
            };
            const auto layout = image.handle.GetSubresourceLayout(subresource);

            image.drm_fourcc = drm_fourcc;
            if (use_modifier_tiling) {
                VkImageDrmFormatModifierPropertiesEXT modifier_properties {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
                };
                VVK_CHECK_ACT(break, image.handle.GetDrmFormatModifierProperties(modifier_properties));
                image.drm_modifier = modifier_properties.drmFormatModifier;
            } else {
                image.drm_modifier = DRM_FORMAT_MOD_LINEAR;
            }
            image.n_planes = 1;
            image.planes[0].fd = image.fd;
            image.planes[0].offset = static_cast<uint32_t>(layout.offset);
            image.planes[0].stride = static_cast<uint32_t>(layout.rowPitch);
        }

        return image;

    } while (false);
    return std::nullopt;
}

inline std::optional<VmaImageParameters>
CreateImage(const Device& device, VkExtent3D extent, u32 miplevel, VkFormat format,
            VkSamplerCreateInfo sampler_info, VkImageUsageFlags usage,
            VmaMemoryUsage mem_usage = VMA_MEMORY_USAGE_GPU_ONLY) {
    VmaImageParameters image;
    do {
        VkImageCreateInfo info {
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = format,
            .extent                = extent,
            .mipLevels             = miplevel,
            .arrayLayers           = 1,
            .samples               = VK_SAMPLE_COUNT_1_BIT,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            .usage                 = usage,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        image.extent = info.extent;
        VmaAllocationCreateInfo vma_info {};
        vma_info.usage = mem_usage;
        VVK_CHECK_ACT(break,
                      vvk::CreateImage(device.vma_allocator(), info, vma_info, image.handle));

        image.mipmap_level = miplevel;
        {
            VkImageViewCreateInfo createinfo {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext    = nullptr,
                .image    = *image.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = format,
                .subresourceRange =
                    VkImageSubresourceRange {
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel   = 0,
                        .levelCount     = miplevel,
                        .baseArrayLayer = 0,
                        .layerCount     = 1,
                    },
            };
            VVK_CHECK_ACT(break, device.handle().CreateImageView(createinfo, image.view));
        }
        VVK_CHECK_ACT(break, device.handle().CreateSampler(sampler_info, image.sampler));
        return image;
    } while (false);
    /*
    if (result != vk::Result::eSuccess) {
        device.DestroyImageParameters(image);
    }
    */
    return std::nullopt;
}

inline void RecordCopyImageData(std::span<const BufferParameters> in_bufs,
                                std::span<const VkExtent3D> in_exts,
                                vvk::CommandBuffer& cmd, const ImageParameters& image,
                                VkImageLayout old_layout) {
    VkImageSubresourceRange subresourceRange {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = (uint32_t)in_bufs.size(),
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    {
        VkImageMemoryBarrier in_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout        = old_layout,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image            = image.handle,
            .subresourceRange = subresourceRange,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            in_bar);
    }
    VkBufferImageCopy copy {
        .imageSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    for (usize i = 0; i < in_bufs.size(); i++) {
        copy.imageSubresource.mipLevel = (u32)i;
        copy.imageExtent               = in_exts[i];
        cmd.CopyBufferToImage(
            in_bufs[i].handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);
    }
    {
        VkImageMemoryBarrier out_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = image.handle,
            .subresourceRange = subresourceRange,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            out_bar);
    }
}

bool CanReuseTextureSlots(const ImageSlots& existing, const Image& image) {
    if (existing.slots.size() != image.slots.size()) return false;

    for (usize i = 0; i < image.slots.size(); i++) {
        const auto& cached_slot = existing.slots[i];
        const auto& next_slot = image.slots[i];
        if (cached_slot.extent.width != static_cast<uint32_t>(next_slot.width) ||
            cached_slot.extent.height != static_cast<uint32_t>(next_slot.height) ||
            cached_slot.mipmap_level != next_slot.mipmaps.size()) {
            return false;
        }
    }

    return true;
}

std::optional<TextureCachePendingImageUpload>
BuildImageUploadJob(const Device& device, std::string_view key, const Image& image,
                    const ImageSlots& slots, usize slot_index, VkImageLayout old_layout) {
    if (slot_index >= image.slots.size() || slot_index >= slots.slots.size()) return std::nullopt;

    const auto& image_slot = image.slots[slot_index];
    const auto& gpu_slot   = slots.slots[slot_index];
    TextureCachePendingImageUpload upload;
    upload.key        = std::string(key);
    upload.image      = ImageParameters(gpu_slot);
    upload.old_layout = old_layout;
    upload.stage_bufs.reserve(image_slot.mipmaps.size());
    upload.extents.reserve(image_slot.mipmaps.size());

    for (const auto& mipmap : image_slot.mipmaps) {
        VmaBufferParameters buf;
        if (!CreateStagingBuffer(device.vma_allocator(), static_cast<u32>(mipmap.size), buf)) {
            return std::nullopt;
        }
        void* mapped = nullptr;
        VVK_CHECK_ACT(return std::nullopt, buf.handle.MapMemory(&mapped));
        memcpy(mapped, mipmap.data.get(), static_cast<u32>(mipmap.size));
        buf.handle.UnMapMemory();
        upload.stage_bufs.emplace_back(std::move(buf));
        upload.extents.push_back(VkExtent3D {
            static_cast<u32>(mipmap.width),
            static_cast<u32>(mipmap.height),
            1,
        });
    }

    return upload;
}

bool QueueImageDataUploads(const Device& device, std::string_view key, const Image& image,
                           const ImageSlots& slots, VkImageLayout old_layout,
                           std::vector<TextureCachePendingImageUpload>& pending_uploads) {
    std::vector<TextureCachePendingImageUpload> queued_uploads;
    queued_uploads.reserve(image.slots.size());
    for (usize i = 0; i < image.slots.size(); i++) {
        auto upload = BuildImageUploadJob(device, key, image, slots, i, old_layout);
        if (!upload.has_value()) {
            return false;
        }
        queued_uploads.emplace_back(std::move(upload.value()));
    }

    for (auto& upload : queued_uploads) {
        pending_uploads.emplace_back(std::move(upload));
    }

    return true;
}

bool CreateImageSlotForTexture(const Device& device, const Image& image, usize slot_index,
                               VmaImageParameters& image_paras) {
    if (slot_index >= image.slots.size()) return false;

    const auto& sam = image.header.sample;
    const auto& image_slot = image.slots[slot_index];
    const auto  mipmap_levels = image_slot.mipmaps.size();

    if (image_slot.width <= 0 || image_slot.height <= 0 || image_slot.mipmaps.empty()) {
        return false;
    }
    VkSamplerCreateInfo sampler_info {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext                   = nullptr,
        .magFilter               = ToVkType(sam.magFilter),
        .minFilter               = (ToVkType(sam.minFilter)),
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU            = (ToVkType(sam.wrapS)),
        .addressModeV            = (ToVkType(sam.wrapS)),
        .addressModeW            = (ToVkType(sam.wrapT)),
        .anisotropyEnable        = (false),
        .maxAnisotropy           = (1.0f),
        .compareEnable           = (false),
        .compareOp               = VK_COMPARE_OP_NEVER,
        .minLod                  = (0.0f),
        .maxLod                  = (float)mipmap_levels,
        .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = (false),
    };
    VkFormat   format = ToVkType(image.header.format);
    VkExtent3D ext { (u32)image_slot.width, (u32)image_slot.height, 1 };

    if (auto opt = CreateImage(device,
                               ext,
                               (u32)mipmap_levels,
                               format,
                               sampler_info,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        opt.has_value()) {
        image_paras = std::move(opt.value());
        return true;
    }

    return false;
}

bool CreateImageSlotsForTexture(const Device& device, const Image& image, ImageSlots& img_slots) {
    ImageSlots next_slots;
    next_slots.slots.resize(image.slots.size());

    for (usize i = 0; i < image.slots.size(); i++) {
        if (!CreateImageSlotForTexture(device, image, i, next_slots.slots[i])) return false;
    }
    img_slots = std::move(next_slots);
    return true;
}
} // namespace

std::size_t TextureKey::HashValue(const TextureKey& k) {
    std::size_t seed { 0 };
    utils::hash_combine(seed, k.width);
    utils::hash_combine(seed, k.height);
    utils::hash_combine(seed, (int)k.usage);
    utils::hash_combine(seed, (int)k.format);
    utils::hash_combine(seed, (int)k.mipmap_level);

    utils::hash_combine(seed, (int)k.sample.wrapS);
    utils::hash_combine(seed, (int)k.sample.wrapT);
    utils::hash_combine(seed, (int)k.sample.magFilter);
    return seed;
}

std::optional<ExImageParameters> TextureCache::CreateExTex(uint32_t width, uint32_t height,
                                                           VkFormat format,
                                                           VkImageTiling tiling,
                                                           ExternalFrameExportMode export_mode,
                                                           uint32_t export_drm_fourcc,
                                                           std::span<const uint64_t> export_drm_modifiers) {
    if (export_mode == ExternalFrameExportMode::DMA_BUF &&
        ! m_device.supportExt(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
        LOG_ERROR("vulkan device missing %s for dma-buf export",
                  VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        return std::nullopt;
    }

    VkSamplerCreateInfo sampler_info {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext                   = nullptr,
        .magFilter               = VK_FILTER_NEAREST,
        .minFilter               = VK_FILTER_NEAREST,
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU            = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        .addressModeV            = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        .addressModeW            = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        .anisotropyEnable        = false,
        .maxAnisotropy           = 1.0f,
        .compareEnable           = false,
        .compareOp               = VK_COMPARE_OP_NEVER,
        .minLod                  = 0.0f,
        .maxLod                  = 1.0f,
        .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = false,
    };

    auto opt = CreateExImage(width,
                             height,
                             format,
                             tiling,
                             sampler_info,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             m_device.device(),
                             m_device.gpu(),
                             export_mode,
                             export_drm_fourcc,
                             m_device.supportExt(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)
                                 ? export_drm_modifiers
                                 : std::span<const uint64_t> {});
    if (opt.has_value()) {
        const auto& eximg = opt.value();

        if (! m_tex_cmd) allocateCmd();
        TransImgLayout(m_device.graphics_queue().handle, m_tex_cmd, eximg, VK_IMAGE_LAYOUT_GENERAL);
        VVK_CHECK(m_device.handle().WaitIdle());
    }
    return opt;
}

ImageSlotsRef TextureCache::CreateTex(Image& image) {
    m_streaming_tex_uploads.erase(image.key);
    if (exists(m_tex_map, image.key)) {
        auto& cached = m_tex_map.at(image.key);
        const auto cached_revision =
            exists(m_tex_revision_map, image.key) ? m_tex_revision_map.at(image.key) : 0;
        if (cached_revision == image.revision) {
            return cached;
        }

        purgeQueuedWorkForKey(image.key);
        if (CanReuseTextureSlots(cached, image) &&
            QueueImageDataUploads(m_device,
                                  image.key,
                                  image,
                                  cached,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  m_pending_image_uploads)) {
            m_tex_revision_map[image.key] = image.revision;
            LOG_VERBOSE("TextureCacheUploadQueued: key='%s' slots=%zu revision=%zu reuse=true",
                        image.key.c_str(),
                        image.slots.size(),
                        static_cast<size_t>(image.revision));
            return cached;
        }

        m_tex_map.erase(image.key);
        m_tex_revision_map.erase(image.key);
    }

    ImageSlots img_slots;
    if (!CreateImageSlotsForTexture(m_device, image, img_slots)) return {};

    if (!QueueImageDataUploads(
            m_device, image.key, image, img_slots, VK_IMAGE_LAYOUT_UNDEFINED, m_pending_image_uploads)) {
        return {};
    }
    m_tex_map[image.key] = std::move(img_slots);
    m_tex_revision_map[image.key] = image.revision;
    LOG_VERBOSE("TextureCacheUploadQueued: key='%s' slots=%zu revision=%zu reuse=false",
                image.key.c_str(),
                image.slots.size(),
                static_cast<size_t>(image.revision));
    return m_tex_map[image.key];
}

std::optional<ImageSlotsRef> TextureCache::FindTex(std::string_view key) const {
    const std::string key_string(key);
    if (m_streaming_tex_uploads.count(key_string) != 0) return std::nullopt;
    const auto        texture_it = m_tex_map.find(key_string);
    if (texture_it == m_tex_map.end()) return std::nullopt;
    if (!TextureSlotsResident(texture_it->second)) return std::nullopt;
    return ImageSlotsRef(texture_it->second);
}

TextureCacheStreamingState TextureCache::StagePendingTexUploads(std::string_view key,
                                                                std::size_t byte_budget) {
    const std::string key_string(key);
    auto              streaming_it = m_streaming_tex_uploads.find(key_string);
    if (streaming_it == m_streaming_tex_uploads.end()) {
        return m_tex_map.find(key_string) != m_tex_map.end()
                   ? TextureCacheStreamingState::Ready
                   : TextureCacheStreamingState::Failed;
    }

    auto& streaming = streaming_it->second;
    auto  slots_it  = m_tex_map.find(key_string);
    auto abort_streaming = [&]() {
        purgeQueuedWorkForKey(key_string);
        m_tex_map.erase(key_string);
        m_tex_revision_map.erase(key_string);
    };
    if (streaming.image == nullptr || slots_it == m_tex_map.end()) {
        abort_streaming();
        return TextureCacheStreamingState::Failed;
    }

    std::size_t staged_bytes = 0;
    std::size_t staged_slots = 0;
    while (!streaming.remaining_slots.empty()) {
        const auto slot_index = streaming.remaining_slots.front();
        if (slot_index >= streaming.image->slots.size()) {
            streaming.remaining_slots.pop_front();
            continue;
        }

        const auto slot_bytes = ImageSlotUploadBytes(streaming.image->slots[slot_index]);
        if (staged_slots != 0 && byte_budget != 0 && staged_bytes + slot_bytes > byte_budget) {
            break;
        }

        auto& gpu_slot = slots_it->second.slots[slot_index];
        if (!gpu_slot.handle || !gpu_slot.view || !gpu_slot.sampler) {
            // Allocate the backing VkImage only for the slot that is about to be streamed. The old
            // implementation created every sprite-frame image up front, which could still create a
            // visible hitch even though upload-buffer construction was byte-budgeted. This mirrors
            // the staged residency model in mature engines: CPU decode, GPU image allocation, and
            // staging-buffer copies all advance in small chunks while the old frame keeps drawing.
            if (!CreateImageSlotForTexture(m_device, *streaming.image, slot_index, gpu_slot)) {
                abort_streaming();
                return TextureCacheStreamingState::Failed;
            }
        }

        auto upload = BuildImageUploadJob(m_device,
                                          key_string,
                                          *streaming.image,
                                          slots_it->second,
                                          slot_index,
                                          streaming.old_layout);
        if (!upload.has_value()) {
            abort_streaming();
            return TextureCacheStreamingState::Failed;
        }
        m_pending_image_uploads.emplace_back(std::move(upload.value()));
        streaming.remaining_slots.pop_front();
        staged_bytes += slot_bytes;
        staged_slots++;
    }

    if (staged_slots != 0) {
        LOG_VERBOSE("TextureCacheStreamingStage: key='%s' staged-slots=%zu staged-bytes=%zu "
                    "remaining-slots=%zu",
                    key_string.c_str(),
                    staged_slots,
                    staged_bytes,
                    streaming.remaining_slots.size());
    }

    if (streaming.remaining_slots.empty()) {
        m_streaming_tex_uploads.erase(streaming_it);
        LOG_VERBOSE("TextureCacheStreamingReady: key='%s'", key_string.c_str());
        return TextureCacheStreamingState::Ready;
    }

    return TextureCacheStreamingState::Waiting;
}

TextureCacheStreamingState
TextureCache::StageTexUploads(std::shared_ptr<Image> image,
                              std::optional<usize> priority_slot,
                              std::size_t byte_budget) {
    if (image == nullptr || image->key.empty()) return TextureCacheStreamingState::Failed;
    const std::string key = image->key;

    auto cached_it = m_tex_map.find(key);
    if (cached_it != m_tex_map.end()) {
        const auto cached_revision =
            exists(m_tex_revision_map, key) ? m_tex_revision_map.at(key) : 0;
        if (cached_revision == image->revision && m_streaming_tex_uploads.count(key) == 0) {
            return TextureCacheStreamingState::Ready;
        }

        if (cached_revision != image->revision) {
            purgeQueuedWorkForKey(key);
            if (CanReuseTextureSlots(cached_it->second, *image)) {
                StreamingTexUpload streaming;
                streaming.image      = std::move(image);
                streaming.old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                for (usize i = 0; i < cached_it->second.slots.size(); i++) {
                    streaming.remaining_slots.push_back(i);
                }
                m_tex_revision_map[key] = streaming.image->revision;
                m_streaming_tex_uploads[key] = std::move(streaming);
            } else {
                m_tex_map.erase(key);
                m_tex_revision_map.erase(key);
                m_streaming_tex_uploads.erase(key);
            }
        }
    }

    if (m_tex_map.find(key) == m_tex_map.end()) {
        ImageSlots img_slots;
        img_slots.slots.resize(image->slots.size());

        StreamingTexUpload streaming;
        streaming.image = image;
        if (priority_slot.has_value() && *priority_slot < image->slots.size()) {
            streaming.remaining_slots.push_back(*priority_slot);
        }
        for (usize i = 0; i < image->slots.size(); i++) {
            if (priority_slot.has_value() && i == *priority_slot) continue;
            streaming.remaining_slots.push_back(i);
        }
        m_tex_map[key] = std::move(img_slots);
        m_tex_revision_map[key] = image->revision;
        m_streaming_tex_uploads[key] = std::move(streaming);
        const auto priority_label =
            priority_slot.has_value() ? std::to_string(*priority_slot) : std::string("none");
        LOG_VERBOSE("TextureCacheStreamingCreate: key='%s' slots=%zu revision=%zu priority=%s",
                    key.c_str(),
                    image->slots.size(),
                    static_cast<size_t>(image->revision),
                    priority_label.c_str());
    }

    return StagePendingTexUploads(key, byte_budget);
}

void TextureCache::allocateCmd() {
    const auto& pool = m_device.cmd_pool();
    VVK_CHECK(pool.Allocate(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_tex_cmds));
    m_tex_cmd = vvk::CommandBuffer(m_tex_cmds[0], m_device.handle().Dispatch());
}

std::optional<VmaImageParameters> TextureCache::CreateTex(TextureKey tex_key) {
    VmaImageParameters image_paras;
    do {
        VkSamplerCreateInfo sam_info = GenSamplerInfo(tex_key);
        VkFormat            format   = ToVkType(tex_key.format);
        VkExtent3D          ext { (u32)tex_key.width, (u32)tex_key.height, 1 };

        if (auto opt =
                CreateImage(m_device,
                            ext,
                            tex_key.mipmap_level,
                            format,
                            sam_info,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            opt.has_value()) {
            image_paras = std::move(opt.value());
        } else
            break;

        return image_paras;
    } while (false);
    return std::nullopt;
}

TextureCache::TextureCache(const Device& device): m_device(device) {}

TextureCache::~TextureCache() {};

void TextureCache::Clear() {
    m_tex_map.clear();
    m_tex_revision_map.clear();
    m_query_texs.clear();
    m_query_map.clear();
    m_streaming_tex_uploads.clear();
    m_pending_image_uploads.clear();
    m_inflight_image_uploads.clear();
    m_pending_render_target_clears.clear();
    m_deferred_graph_activation_depth = 0;
    m_deferred_share_ready_keys.clear();
}

bool TextureCache::ReleaseTexture(std::string_view key) {
    const std::string key_string(key);
    const auto        before_bytes = GetTrackedBytes();
    const auto        before_count = GetTrackedImageCount();

    purgeQueuedWorkForKey(key_string);
    const auto erased = m_tex_map.erase(key_string);
    m_tex_revision_map.erase(key_string);
    if (erased == 0) return false;

    LOG_INFO("TextureCacheRelease: static key='%s' bytes-before=%zu bytes-after=%zu "
             "images-before=%zu images-after=%zu",
             key_string.c_str(),
             before_bytes,
             GetTrackedBytes(),
             before_count,
             GetTrackedImageCount());
    return true;
}

bool TextureCache::ReleaseRenderTarget(std::string_view key) {
    const std::string key_string(key);
    const auto        query_it = m_query_map.find(key_string);
    if (query_it == m_query_map.end() || query_it->second == nullptr) return false;

    purgeQueuedWorkForKey(key_string);
    const auto before_bytes = GetTrackedBytes();
    const auto before_count = GetTrackedImageCount();
    auto*      query        = query_it->second;

    query->query_keys.erase(key_string);
    m_query_map.erase(query_it);

    if (! query->query_keys.empty()) {
        query->share_ready = false;
        LOG_INFO("TextureCacheRelease: render-target key='%s' detached shared-entry remaining=%zu "
                 "bytes=%zu images=%zu",
                 key_string.c_str(),
                 query->query_keys.size(),
                 GetTrackedBytes(),
                 GetTrackedImageCount());
        return true;
    }

    const auto query_ptr = query;
    for (auto iter = m_query_texs.begin(); iter != m_query_texs.end(); ++iter) {
        if (! *iter || iter->get() != query_ptr) continue;
        m_query_texs.erase(iter);
        LOG_INFO("TextureCacheRelease: render-target key='%s' bytes-before=%zu bytes-after=%zu "
                 "images-before=%zu images-after=%zu",
                 key_string.c_str(),
                 before_bytes,
                 GetTrackedBytes(),
                 before_count,
                 GetTrackedImageCount());
        return true;
    }

    LOG_ERROR("TextureCacheRelease: render-target key='%s' missing owning cache entry",
              key_string.c_str());
    return true;
}

std::optional<ImageParameters> TextureCache::Query(std::string_view key, TextureKey content_hash,
                                                   bool persist) {
    const std::string key_string(key);
    const TexHash     tex_hash = TextureKey::HashValue(content_hash);
    auto queue_initial_clear = [&](const VmaImageParameters& image) {
        // The render target may be sampled by a feedback pass before a writer touches it. Keep the
        // deterministic transparent-black bootstrap, but record it into the renderer's next frame
        // command buffer so visibility changes do not force a synchronous DeviceWaitIdle.
        purgeQueuedWorkForKey(key_string);
        m_pending_render_target_clears.push_back(TextureCachePendingRenderTargetClear {
            .key   = key_string,
            .image = ImageParameters(image),
        });
        LOG_VERBOSE("TextureCacheInitialClearQueued: key='%s' render-target=%dx%d mip-levels=%u "
                 "transparent-black=true",
                 key_string.c_str(),
                 content_hash.width,
                 content_hash.height,
                 content_hash.mipmap_level);
    };

    if (exists(m_query_map, key_string)) {
        auto* query = m_query_map.find(key_string)->second;

        if (query->content_hash != tex_hash) {
            // Resource-only render graph refreshes must behave like particle updates: keep the
            // graph and unrelated GPU resources alive, then resize only the specific offscreen
            // image whose render-target contract changed. The old cache returned stale images for
            // an existing key until the entire cache was cleared, which made minute-level text
            // bridge updates recreate every scene texture and caused visible hitches.
            LOG_VERBOSE("TextureCache: resize cached render target key='%s' previousHash=%zu nextHash=%zu "
                        "previousSize=[%u, %u] nextSize=[%d, %d]",
                        key_string.c_str(),
                        query->content_hash,
                        tex_hash,
                        query->image.extent.width,
                        query->image.extent.height,
                        content_hash.width,
                        content_hash.height);

            if (query->query_keys.size() > 1) {
                // A reusable image can be shared by multiple logical render-target keys. When only
                // one key changes size, detach that key into a fresh cache entry so the remaining
                // users keep sampling their still-valid image instead of being silently resized.
                query->query_keys.erase(key_string);
                m_query_map.erase(key_string);
            } else {
                if (auto opt = CreateTex(content_hash); opt.has_value()) {
                    query->image        = std::move(opt.value());
                    query->content_hash = tex_hash;
                    query->share_ready  = false;
                    query->persist      = persist;
                    query->query_keys.clear();
                    query->query_keys.insert(key_string);
                    m_query_map[key_string] = query;
                    queue_initial_clear(query->image);
                    return query->image;
                }
                return std::nullopt;
            }
        } else {
            query->share_ready = false;
            query->persist     = persist;

            return query->image;
        }
    };

    for (auto& query : m_query_texs) {
        if (! (query->share_ready)) continue;
        if (query->content_hash != tex_hash) continue;

        query->share_ready = false;
        query->persist     = persist;
        query->query_keys.insert(key_string);

        m_query_map[key_string] = &(*query);

        return query->image;
    }

    m_query_texs.emplace_back(std::make_unique<QueryTex>());
    auto& query                   = *m_query_texs.back();
    m_query_map[key_string] = &query;

    query.index        = (idx)m_query_texs.size() - 1;
    query.content_hash = tex_hash;
    query.query_keys.insert(key_string);
    query.persist = persist;
    if (auto opt = CreateTex(content_hash); opt.has_value()) {
        query.image = std::move(opt.value());
        queue_initial_clear(query.image);
        return query.image;
    }
    return std::nullopt;
}

void TextureCache::MarkShareReady(std::string_view key) {
    const std::string key_string(key);
    const auto query_it = m_query_map.find(key_string);
    if (query_it == m_query_map.end()) return;

    // Copy the cached QueryTex pointer before erasing the lookup entry. Holding a reference to the
    // map value across `erase()` leaves a dangling reference, and minute-rollover text bridge
    // refreshes hit that path when the final effect pass releases the resized clock source.
    auto* query = query_it->second;
    if (query == nullptr) {
        m_query_map.erase(query_it);
        return;
    }
    if (query->persist) return;

    if (m_deferred_graph_activation_depth != 0) {
        // Deferred render-graph activation is allowed to prepare passes across several frames, but
        // transient aliasing is not allowed to advance across that activation boundary. Feedback
        // effects such as cursor ripple bind ping-pong render targets from passes that may not be
        // prepared yet; letting an already-prepared bypass/copy pass mark those targets reusable
        // lets a later prepare reattach the same physical image to a different logical key. Mature
        // renderers keep transient attachment aliasing behind the graph/subgraph readiness fence,
        // so queue the release intent and replay it only after every deferred pass is resident.
        m_deferred_share_ready_keys.insert(key_string);
        return;
    }

    // A texture entry is reusable only after every logical key that still references it has reached
    // its last read. This makes per-key resize safe for shared render targets while preserving the
    // old transient-reuse behavior for unshared effect outputs.
    query->query_keys.erase(key_string);
    m_query_map.erase(query_it);
    query->share_ready = query->query_keys.empty();
}

void TextureCache::BeginDeferredGraphActivation() {
    m_deferred_graph_activation_depth++;
    LOG_INFO("TextureCacheDeferredGraphActivation: action=begin depth=%zu pending-share-ready=%zu",
             m_deferred_graph_activation_depth,
             m_deferred_share_ready_keys.size());
}

void TextureCache::EndDeferredGraphActivation() {
    if (m_deferred_graph_activation_depth == 0) return;
    m_deferred_graph_activation_depth--;
    if (m_deferred_graph_activation_depth != 0) return;

    auto deferred_keys = std::move(m_deferred_share_ready_keys);
    m_deferred_share_ready_keys.clear();
    LOG_INFO("TextureCacheDeferredGraphActivation: action=end replay-share-ready=%zu",
             deferred_keys.size());
    for (const auto& key : deferred_keys) {
        MarkShareReady(key);
    }
}

void TextureCache::CancelDeferredGraphActivation() {
    if (m_deferred_graph_activation_depth == 0 && m_deferred_share_ready_keys.empty()) return;
    LOG_INFO("TextureCacheDeferredGraphActivation: action=cancel depth=%zu "
             "drop-share-ready=%zu",
             m_deferred_graph_activation_depth,
             m_deferred_share_ready_keys.size());
    m_deferred_graph_activation_depth = 0;
    m_deferred_share_ready_keys.clear();
}

void TextureCache::purgeQueuedWorkForKey(std::string_view key) {
    const auto same_key = [key](const auto& work) {
        return work.key == key;
    };
    m_pending_image_uploads.erase(std::remove_if(m_pending_image_uploads.begin(),
                                                 m_pending_image_uploads.end(),
                                                 same_key),
                                  m_pending_image_uploads.end());
    m_inflight_image_uploads.erase(std::remove_if(m_inflight_image_uploads.begin(),
                                                  m_inflight_image_uploads.end(),
                                                  same_key),
                                   m_inflight_image_uploads.end());
    m_pending_render_target_clears.erase(
        std::remove_if(m_pending_render_target_clears.begin(),
                       m_pending_render_target_clears.end(),
                       same_key),
        m_pending_render_target_clears.end());
    m_streaming_tex_uploads.erase(std::string(key));
}

void TextureCache::RecordUploads(vvk::CommandBuffer& cmd) {
    if (m_pending_render_target_clears.empty() && m_pending_image_uploads.empty()) return;

    const auto started_at = std::chrono::steady_clock::now();
    const auto clear_count = m_pending_render_target_clears.size();
    const auto upload_count = m_pending_image_uploads.size();
    const auto upload_bytes = PendingImageUploadBytes(m_pending_image_uploads);

    for (const auto& clear : m_pending_render_target_clears) {
        RecordClearNewRenderTargetToTransparentBlack(cmd, clear.image);
    }
    m_pending_render_target_clears.clear();

    for (auto& upload : m_pending_image_uploads) {
        RecordCopyImageData(
            transform<VmaBufferParameters>(upload.stage_bufs,
                                           [](BufferParameters e) {
                                               return e;
                                           }),
            upload.extents,
            cmd,
            upload.image,
            upload.old_layout);
    }
    // Stage buffers must stay alive until the frame submission using this command buffer has
    // finished. VulkanRender waits and resets the frame fence before starting the next frame, so
    // RetireCompletedUploads() can drop this vector at the start of the following draw.
    for (auto& upload : m_pending_image_uploads) {
        m_inflight_image_uploads.emplace_back(std::move(upload));
    }
    m_pending_image_uploads.clear();

    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
    LOG_VERBOSE("TextureCacheRecordUploads: clears=%zu image-uploads=%zu upload-bytes=%zu "
                "duration=%.2fms",
                clear_count,
                upload_count,
                upload_bytes,
                static_cast<double>(elapsed_us) / 1000.0);
}

void TextureCache::RetireCompletedUploads() {
    if (m_inflight_image_uploads.empty()) return;

    const auto upload_bytes = PendingImageUploadBytes(m_inflight_image_uploads);
    LOG_VERBOSE("TextureCacheRetireUploads: image-uploads=%zu upload-bytes=%zu",
                m_inflight_image_uploads.size(),
                upload_bytes);
    m_inflight_image_uploads.clear();
}

std::size_t TextureCache::GetTrackedBytes() const {
    std::size_t total = 0;
    for (const auto& [_, slots] : m_tex_map) {
        total += ImageSlotsAllocationBytes(slots);
    }
    for (const auto& query : m_query_texs) {
        if (! query) continue;
        total += ImageAllocationBytes(query->image);
    }
    return total;
}

std::size_t TextureCache::GetTrackedImageCount() const {
    std::size_t total = 0;
    for (const auto& [_, slots] : m_tex_map) {
        total += slots.slots.size();
    }
    for (const auto& query : m_query_texs) {
        if (query && query->image.handle) total++;
    }
    return total;
}

void TextureCache::RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const {
    VkImageMemoryBarrier barrier {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext               = nullptr,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image.handle,
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    /*
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        out_bar);
        */

    i32 mipWidth  = (i32)image.extent.width;
    i32 mipHeight = (i32)image.extent.height;

    for (uint i = 1; i < image.mipmap_level; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout                     = i == 1 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        barrier.subresourceRange.baseMipLevel = i;
        barrier.oldLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        VkImageBlit blit {
            .srcSubresource =
                VkImageSubresourceLayers {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = i - 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
            .srcOffsets = { VkOffset3D { 0, 0, 0 }, VkOffset3D { mipWidth, mipHeight, 1 } },
            .dstOffsets = { VkOffset3D { 0, 0, 0 },
                            VkOffset3D { mipWidth > 1 ? mipWidth / 2 : 1,
                                         mipHeight > 1 ? mipHeight / 2 : 1,
                                         1 } },
        };
        blit.dstSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = blit.srcSubresource.aspectMask,
                .mipLevel       = blit.srcSubresource.mipLevel + 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },

        cmd.BlitImage(image.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      image.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      blit,
                      VK_FILTER_LINEAR);

        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    barrier.subresourceRange.baseMipLevel = image.mipmap_level - 1;
    barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;

    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        barrier);
}
