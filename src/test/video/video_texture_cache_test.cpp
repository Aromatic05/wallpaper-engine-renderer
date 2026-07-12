#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "common/scene/Image.hpp"
#include "render/vulkan/include/vulkan/Device.hpp"
#include "render/vulkan/include/vulkan/Instance.hpp"
#include "render/vulkan/include/vulkan/VideoTextureCache.hpp"
#include "video_fixture.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <thread>

namespace
{
[[noreturn]] void Fail(const char* condition, int line) {
    std::fprintf(stderr,
                 "video-texture-cache-test:%d: requirement failed: %s\n",
                 line,
                 condition);
    std::abort();
}

void RequireImpl(bool condition, const char* expression, int line) {
    if (! condition) Fail(expression, line);
}

#define Require(condition) RequireImpl((condition), #condition, __LINE__)


struct VulkanFixture {
    wallpaper::vulkan::Instance instance;
    wallpaper::vulkan::Device   device;

    explicit VulkanFixture(
        bool enable_external_memory,
        wallpaper::vulkan::VideoTextureGpuPipeline pipeline_policy) {
        const std::array<wallpaper::vulkan::Extension, 0> instance_extensions {};
        const std::array<wallpaper::vulkan::InstanceLayer, 0> instance_layers {};
        const std::array<wallpaper::vulkan::Extension, 0> no_device_extensions {};
        const std::array device_extensions {
            wallpaper::vulkan::Extension {
                false, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME },
            wallpaper::vulkan::Extension {
                false, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME },
            wallpaper::vulkan::Extension {
                false, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME },
        };
        const std::span<const wallpaper::vulkan::Extension> selected_extensions =
            enable_external_memory
                ? std::span<const wallpaper::vulkan::Extension>(device_extensions)
                : std::span<const wallpaper::vulkan::Extension>(no_device_extensions);

        Require(wallpaper::vulkan::Instance::Create(
            instance, instance_extensions, instance_layers));
        Require(instance.ChoosePhysicalDevice([&](auto gpu) {
            return wallpaper::vulkan::Device::CheckGPU(gpu, selected_extensions, {});
        }));
        Require(wallpaper::vulkan::Device::Create(
            instance,
            selected_extensions,
            VkExtent2D { 64, 64 },
            device,
            wallpaper::vulkan::VideoTexturePipelineSettings {
                .gpu_pipeline = pipeline_policy,
            }));
    }
};

void FlushUploads(wallpaper::vulkan::Device& device,
                  wallpaper::vulkan::VideoTextureCache& cache) {
    vvk::CommandBuffers command_buffers;
    Require(device.cmd_pool().Allocate(
                1, VK_COMMAND_BUFFER_LEVEL_PRIMARY, command_buffers) == VK_SUCCESS);
    vvk::CommandBuffer command(command_buffers[0], device.handle().Dispatch());
    Require(command.Begin(VkCommandBufferBeginInfo {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                .pInheritanceInfo = nullptr,
            }) == VK_SUCCESS);
    cache.RecordUploads(command);
    Require(command.End() == VK_SUCCESS);

    vvk::Fence fence;
    Require(device.handle().CreateFence(VkFenceCreateInfo {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            },
            fence) == VK_SUCCESS);
    VkSubmitInfo submit {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = command.address(),
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    Require(device.graphics_queue().handle.Submit(submit, *fence) == VK_SUCCESS);
    Require(fence.Wait() == VK_SUCCESS);
}

std::unique_ptr<wallpaper::Image> MakeEmbeddedVideoImage() {
    const auto encoded = wallpaper::test::DecodeBase64(
        wallpaper::test::kFixtureVideoBase64);
    Require(!encoded.empty());

    auto image = std::make_unique<wallpaper::Image>();
    image->header.width = 64;
    image->header.height = 64;
    image->header.mapWidth = 64;
    image->header.mapHeight = 64;
    image->header.isVideoTexture = true;

    wallpaper::Image::Slot slot;
    slot.width = 64;
    slot.height = 64;

    wallpaper::ImageData data;
    data.width = 64;
    data.height = 64;
    data.size = static_cast<wallpaper::isize>(encoded.size());
    auto* bytes = new uint8_t[encoded.size()];
    std::memcpy(bytes, encoded.data(), encoded.size());
    data.data = wallpaper::ImageDataPtr(bytes, [](uint8_t* ptr) { delete[] ptr; });
    slot.mipmaps.emplace_back(std::move(data));
    image->slots.emplace_back(std::move(slot));
    return image;
}
} // namespace

int main() {
    wallpaper::Scene scene;
    scene.textures["movie"] = wallpaper::SceneTexture {
        .url = "movie",
        .sample =
            wallpaper::TextureSample {
                .wrapS = wallpaper::TextureWrap::CLAMP_TO_EDGE,
                .wrapT = wallpaper::TextureWrap::CLAMP_TO_EDGE,
                .magFilter = wallpaper::TextureFilter::LINEAR,
                .minFilter = wallpaper::TextureFilter::LINEAR,
            },
        .format = wallpaper::TextureFormat::RGBA8,
        .isVideo = true,
        .width = 64,
        .height = 64,
        .mapWidth = 64,
        .mapHeight = 64,
        .spriteAnim = {},
    };

    Require(scene.textures.at("movie").isVideo);
    VulkanFixture fixture(true, wallpaper::vulkan::VideoTextureGpuPipeline::Va);
    auto& cache = fixture.device.video_tex_cache();
    Require(cache.GetTrackedEntryCount() == 0);

    auto image = MakeEmbeddedVideoImage();
    const auto first = cache.Acquire("movie", scene.textures.at("movie"), *image);
    Require(first.slots.size() == 1);
    Require(first.slots[0].handle != VK_NULL_HANDLE);
    Require(first.slots[0].view != VK_NULL_HANDLE);
    Require(first.slots[0].sampler != VK_NULL_HANDLE);
    Require(cache.GetTrackedEntryCount() == 1);

    using namespace std::chrono_literals;
    std::optional<wallpaper::vulkan::VideoTextureStatus> first_sample_status;
    for (int attempt = 0; attempt < 300; ++attempt) {
        cache.Poll();
        first_sample_status = cache.GetStatus("movie");
        Require(first_sample_status.has_value());
        Require(!first_sample_status->pipeline_failed);
        if (first_sample_status->uploaded_sample_count > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    Require(first_sample_status.has_value());
    Require(first_sample_status->uploaded_sample_count > 0);
    const auto initial_upload_count = first_sample_status->uploaded_sample_count;
    const auto initial_loop_count = first_sample_status->loop_count;
    Require(first_sample_status->pipeline_mode ==
                wallpaper::vulkan::VideoTexturePipelineMode::VaMemoryBgra ||
            first_sample_status->pipeline_mode ==
                wallpaper::vulkan::VideoTexturePipelineMode::CpuRgba);
    Require(first_sample_status->upload_pending);
    FlushUploads(fixture.device, cache);
    Require(!cache.GetStatus("movie")->upload_pending);

    const auto second = cache.Acquire("movie", scene.textures.at("movie"), *image);
    Require(second.slots.size() == 1);
    Require(second.slots[0].handle == first.slots[0].handle);
    Require(second.slots[0].view == first.slots[0].view);
    Require(second.slots[0].sampler == first.slots[0].sampler);


    bool looped_frame_uploaded = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        cache.Poll();
        const auto status = cache.GetStatus("movie");
        Require(status.has_value());
        Require(!status->pipeline_failed);
        if (status->loop_count > initial_loop_count &&
            status->uploaded_sample_count > initial_upload_count &&
            !status->waiting_for_loop_sample) {
            looped_frame_uploaded = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    Require(looped_frame_uploaded);
    Require(cache.GetStatus("movie")->upload_pending);
    FlushUploads(fixture.device, cache);
    Require(!cache.GetStatus("movie")->upload_pending);

    const auto after_loop = cache.Acquire("movie", scene.textures.at("movie"), *image);
    Require(after_loop.slots.size() == 1);
    Require(after_loop.slots[0].handle == first.slots[0].handle);
    Require(after_loop.slots[0].view == first.slots[0].view);
    Require(after_loop.slots[0].sampler == first.slots[0].sampler);

    scene.videoTexturePaused["movie"] = true;
    cache.ApplyPlaybackStates(scene.videoTexturePaused, scene.videoTextureStopped);
    Require(cache.GetStatus("movie")->paused);

    scene.videoTexturePaused["movie"] = false;
    cache.ApplyPlaybackStates(scene.videoTexturePaused, scene.videoTextureStopped);
    Require(!cache.GetStatus("movie")->paused);

    scene.videoTextureStopped.insert("movie");
    cache.ApplyPlaybackStates(scene.videoTexturePaused, scene.videoTextureStopped);
    const auto stopped = cache.GetStatus("movie");
    Require(stopped.has_value() && stopped->stopped && stopped->paused);

    cache.Clear();
    Require(cache.GetTrackedEntryCount() == 0);
    Require(!cache.GetStatus("movie").has_value());

    VulkanFixture cpu_fixture(false, wallpaper::vulkan::VideoTextureGpuPipeline::Nvidia);
    auto& cpu_cache = cpu_fixture.device.video_tex_cache();
    const auto cpu_image_ref =
        cpu_cache.Acquire("movie-cpu", scene.textures.at("movie"), *image);
    Require(cpu_image_ref.slots.size() == 1);
    Require(cpu_image_ref.slots[0].handle != VK_NULL_HANDLE);

    std::optional<wallpaper::vulkan::VideoTextureStatus> cpu_status;
    for (int attempt = 0; attempt < 300; ++attempt) {
        cpu_cache.Poll();
        cpu_status = cpu_cache.GetStatus("movie-cpu");
        Require(cpu_status.has_value());
        Require(!cpu_status->pipeline_failed);
        if (cpu_status->uploaded_sample_count > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    Require(cpu_status.has_value());
    Require(cpu_status->pipeline_mode ==
            wallpaper::vulkan::VideoTexturePipelineMode::CpuRgba);
    Require(cpu_status->uploaded_sample_count > 0);
    Require(cpu_status->upload_pending);
    FlushUploads(cpu_fixture.device, cpu_cache);
    Require(!cpu_cache.GetStatus("movie-cpu")->upload_pending);
    cpu_cache.Clear();
    return 0;
}