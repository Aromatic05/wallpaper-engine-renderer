#include "vulkanrender/FinalOutputMsaa.hpp"

#include "render/vulkanrender/ClearPass.hpp"
#include "render/vulkanrender/FinPass.hpp"
#include "render/vulkanrender/Resource.hpp"
#include "backend/scene/internal/SpecTexs.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "render/vulkan/include/vulkan/Device.hpp"
#include "render/vulkan/include/vulkan/Instance.hpp"
#include "render/vulkan/include/vulkan/StagingBuffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace
{
namespace vk = wallpaper::vulkan;

constexpr std::uint32_t kWidth { 16 };
constexpr std::uint32_t kHeight { 16 };

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "final-output-msaa-integration-test: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

struct VulkanFixture {
    vk::Instance instance;
    vk::Device   device;

    VulkanFixture() {
        const std::array instance_extensions {
            vk::Extension { false, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME },
        };
        const std::array<vk::InstanceLayer, 0> instance_layers {};
        const std::array device_extensions {
            vk::Extension { true, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
            vk::Extension { true, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME },
            vk::Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME },
            vk::Extension { true, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME },
            vk::Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME },
        };

        Require(vk::Instance::Create(instance, instance_extensions, instance_layers),
                "failed to create Vulkan instance");
        Require(instance.ChoosePhysicalDevice([&](auto gpu) {
                    return vk::Device::CheckGPU(gpu, device_extensions, {});
                }),
                "failed to choose Vulkan physical device");
        Require(vk::Device::Create(instance,
                                   device_extensions,
                                   VkExtent2D { kWidth, kHeight },
                                   device,
                                   {},
                                   true),
                "failed to create Vulkan device");
    }
};

vk::VmaImageParameters CreatePresentImage(const vk::Device& device, VkExtent2D extent) {
    vk::VmaImageParameters image;
    VkImageCreateInfo image_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = VkExtent3D { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    image.extent = image_info.extent;

    VmaAllocationCreateInfo allocation_info {};
    allocation_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    Require(vvk::CreateImage(device.vma_allocator(),
                             image_info,
                             allocation_info,
                             image.handle) == VK_SUCCESS,
            "failed to create resolve image");

    VkImageViewCreateInfo view_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .image = *image.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_info.format,
        .components = {},
        .subresourceRange = VkImageSubresourceRange {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    Require(device.handle().CreateImageView(view_info, image.view) == VK_SUCCESS,
            "failed to create resolve image view");
    return image;
}

vk::VmaBufferParameters CreateReadbackBuffer(const vk::Device& device, VkExtent2D extent) {
    vk::VmaBufferParameters buffer;
    VkBufferCreateInfo buffer_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4u,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    buffer.req_size = buffer_info.size;
    VmaAllocationCreateInfo allocation_info {};
    allocation_info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    Require(vvk::CreateBuffer(device.vma_allocator(),
                              buffer_info,
                              allocation_info,
                              buffer.handle) == VK_SUCCESS,
            "failed to create readback buffer");
    return buffer;
}

std::array<std::uint8_t, 4> RenderCase(vk::Device& device,
                                       VkSampleCountFlagBits sample_count,
                                       VkExtent2D extent) {
    wallpaper::Scene scene;
    scene.renderTargets[wallpaper::SpecTex_Default.data()] = wallpaper::SceneRenderTarget {
        .width = static_cast<wallpaper::i32>(extent.width),
        .height = static_cast<wallpaper::i32>(extent.height),
        .allowReuse = false,
    };
    scene.clearColor = { 0.0f, 0.0f, 0.0f };

    vk::StagingBuffer vertex_buffer(device, 4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    Require(vertex_buffer.allocate(), "failed to allocate shared vertex buffer");

    vk::RenderingResources resources {};
    resources.vertex_buf = &vertex_buffer;
    resources.dyn_buf = &vertex_buffer;

    vk::ClearPass clear_pass(vk::ClearPass::Desc {
        .target = wallpaper::SpecTex_Default.data(),
        .clear_value = VkClearValue { .color = { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } } },
    });
    vk::FinPass final_pass(vk::FinPass::Desc {
        .sample_count = sample_count,
    });

    auto present_image = CreatePresentImage(device, extent);
    final_pass.setPresent(vk::ImageParameters(present_image));
    final_pass.setPresentFormat(VK_FORMAT_R8G8B8A8_UNORM);
    final_pass.setPresentLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    final_pass.setPresentQueueIndex(device.graphics_queue().family_index);

    clear_pass.prepare(scene, device, resources);
    final_pass.prepare(scene, device, resources);
    Require(clear_pass.prepared(), "clear pass did not prepare");
    Require(final_pass.prepared(), "final pass did not prepare");

    vvk::CommandBuffers command_buffers;
    Require(device.cmd_pool().Allocate(1,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       command_buffers) == VK_SUCCESS,
            "failed to allocate command buffer");
    resources.command = vvk::CommandBuffer(command_buffers[0], device.handle().Dispatch());
    Require(resources.command.Begin(VkCommandBufferBeginInfo {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                .pInheritanceInfo = nullptr,
            }) == VK_SUCCESS,
            "failed to begin command buffer");

    Require(vertex_buffer.recordUpload(resources.command), "failed to upload final-pass vertices");
    clear_pass.execute(device, resources);
    final_pass.execute(device, resources);

    VkImageMemoryBarrier image_ready {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *present_image.handle,
        .subresourceRange = VkImageSubresourceRange {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    resources.command.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      0,
                                      image_ready);

    auto readback = CreateReadbackBuffer(device, extent);
    VkBufferImageCopy copy_region {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = VkImageSubresourceLayers {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = present_image.extent,
    };
    std::array copy_regions { copy_region };
    resources.command.CopyImageToBuffer(*present_image.handle,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        *readback.handle,
                                        copy_regions);

    VkBufferMemoryBarrier buffer_ready {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = *readback.handle,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    resources.command.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_HOST_BIT,
                                      0,
                                      buffer_ready);
    Require(resources.command.End() == VK_SUCCESS, "failed to end command buffer");

    vvk::Fence fence;
    Require(device.handle().CreateFence(VkFenceCreateInfo {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            },
            fence) == VK_SUCCESS,
            "failed to create fence");
    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = resources.command.address(),
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    Require(device.graphics_queue().handle.Submit(submit_info, *fence) == VK_SUCCESS,
            "failed to submit final output pass");
    Require(fence.Wait() == VK_SUCCESS, "final output pass did not complete");

    void* mapped = nullptr;
    Require(readback.handle.MapMemory(&mapped) == VK_SUCCESS,
            "failed to map final output readback");
    const auto* rgba = static_cast<const std::uint8_t*>(mapped);
    const std::size_t center =
        ((static_cast<std::size_t>(extent.height) / 2u) * extent.width +
         (static_cast<std::size_t>(extent.width) / 2u)) * 4u;
    const std::array<std::uint8_t, 4> result {
        rgba[center + 0], rgba[center + 1], rgba[center + 2], rgba[center + 3]
    };
    readback.handle.UnMapMemory();

    final_pass.destory(device, resources);
    clear_pass.destory(device, resources);
    vertex_buffer.destroy();
    return result;
}

void RequireRed(const std::array<std::uint8_t, 4>& rgba, std::string_view path) {
    Require(rgba[0] >= 250u, path == "1x" ? "1x red channel is incorrect"
                                          : "MSAA red channel is incorrect");
    Require(rgba[1] <= 5u, path == "1x" ? "1x green channel is incorrect"
                                        : "MSAA green channel is incorrect");
    Require(rgba[2] <= 5u, path == "1x" ? "1x blue channel is incorrect"
                                        : "MSAA blue channel is incorrect");
    Require(rgba[3] >= 250u, path == "1x" ? "1x alpha channel is incorrect"
                                          : "MSAA alpha channel is incorrect");
}

} // namespace

int main() {
    VulkanFixture fixture;
    auto& device = fixture.device;

    VkImageFormatProperties format_properties {};
    Require(device.gpu().GetImageFormatProperties(VK_FORMAT_R8G8B8A8_UNORM,
                                                   VK_IMAGE_TYPE_2D,
                                                   VK_IMAGE_TILING_OPTIMAL,
                                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                   0,
                                                   format_properties) == VK_SUCCESS,
            "failed to query final-output format sample counts");
    const VkSampleCountFlags supported_sample_counts =
        device.limits().framebufferColorSampleCounts & format_properties.sampleCounts;
    const auto msaa_sample_count = vk::ResolveFinalOutputSampleCount(
        4,
        supported_sample_counts,
        device.sampleRateShadingEnabled());
    std::fprintf(stderr,
                 "final-output-msaa-integration-test: selected-samples=%u format-mask=0x%x "
                 "sample-rate-shading=%s\n",
                 static_cast<unsigned>(msaa_sample_count),
                 static_cast<unsigned>(format_properties.sampleCounts),
                 device.sampleRateShadingEnabled() ? "enabled" : "unsupported");
    Require(msaa_sample_count == VK_SAMPLE_COUNT_1_BIT ||
                device.sampleRateShadingEnabled(),
            "multisample output selected without sample-rate shading support");

    const VkExtent2D native_extent { kWidth, kHeight };
    const auto single_sample = RenderCase(device, VK_SAMPLE_COUNT_1_BIT, native_extent);
    const auto multisample = RenderCase(device, msaa_sample_count, native_extent);
    const auto resized_multisample =
        RenderCase(device, msaa_sample_count, VkExtent2D { 24, 12 });
    RequireRed(single_sample, "1x");
    RequireRed(multisample, "MSAA");
    RequireRed(resized_multisample, "MSAA-resized");
    Require(single_sample == multisample,
            "resolved MSAA center pixel differs from the legacy 1x output");
    return 0;
}
