#include "VulkanRender.hpp"
#include <typeinfo>

#include "utils/Logging.h"
#include "rendergraph/RenderGraph.hpp"
#include "scene/Scene.h"
#include "interface/IImageParser.h"
#include "interface/IShaderValueUpdater.h"

#include "utils/Algorism.h"

#include "vulkan/Device.hpp"
#include "vulkan/TextureCache.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VideoTextureCache.hpp"
#include "vulkan/VulkanExSwapchain.hpp"

#include "VulkanPass.hpp"
#include "PrePass.hpp"
#include "FinPass.hpp"
#include "CopyPass.hpp"
#include "Resource.hpp"

#include "core/ArrayHelper.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if ENABLE_RENDERDOC_API
#    include "RenderDoc.h"
#endif

using namespace wallpaper::vulkan;

constexpr uint64_t vk_wait_time { 10u * 1000u * 1000000u };
constexpr uint32_t vk_command_num { 1 };
constexpr std::size_t kDeferredPrepareMaxPassesPerFrame { 96 };
constexpr double      kDeferredPrepareFrameBudgetMs { 2.0 };

constexpr std::array base_inst_exts {
    Extension { false, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME },
};
constexpr std::array base_device_exts {
    Extension { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
    Extension { false, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME },
    Extension { false, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME },
    Extension { true, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME }
};

namespace
{

std::string MakeResidencyInstanceKey(
    const VulkanPass& pass, std::unordered_map<std::string, std::size_t>& occurrence_counts) {
    const auto base_key = pass.residencyKey();
    if (base_key.empty()) return {};

    const auto instance = occurrence_counts[base_key]++;
    return base_key + "|instance=" + std::to_string(instance);
}

void DestroyPassOnce(VulkanPass* pass, const Device& device, RenderingResources& resources,
                     std::unordered_set<VulkanPass*>& destroyed) {
    if (pass == nullptr || !destroyed.insert(pass).second) return;
    pass->destory(device, resources);
}

bool CreateReadbackBuffer(VmaAllocator allocator, std::size_t size,
                          VmaBufferParameters& buffer) {
    VkBufferCreateInfo ci {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    buffer.req_size = ci.size;

    VmaAllocationCreateInfo vma_info = {};
    vma_info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    VVK_CHECK_BOOL_RE(vvk::CreateBuffer(allocator, ci, vma_info, buffer.handle));
    return true;
}

constexpr unsigned char kPngSignature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

uint32_t Crc32(const uint8_t* data, std::size_t size) {
    uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

void AppendBigEndian32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

void AppendChunk(std::vector<uint8_t>& out, const char type[4], std::span<const uint8_t> data) {
    AppendBigEndian32(out, static_cast<uint32_t>(data.size()));
    const std::size_t type_offset = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = Crc32(out.data() + type_offset, 4 + data.size());
    AppendBigEndian32(out, crc);
}

uint32_t Adler32(std::span<const uint8_t> data) {
    constexpr uint32_t mod = 65521u;
    uint32_t a = 1u;
    uint32_t b = 0u;
    for (uint8_t byte : data) {
        a = (a + byte) % mod;
        b = (b + a) % mod;
    }
    return (b << 16u) | a;
}

bool WriteRgbaPng(const std::string& path, uint32_t width, uint32_t height,
                  std::span<const uint8_t> rgba, std::string* error_message) {
    if (width == 0 || height == 0) {
        if (error_message) *error_message = "cannot write png with zero-sized image";
        return false;
    }
    const std::size_t expected_size = static_cast<std::size_t>(width) * height * 4u;
    if (rgba.size() != expected_size) {
        if (error_message) {
            *error_message = "rgba buffer size does not match image dimensions";
        }
        return false;
    }

    std::vector<uint8_t> png;
    png.insert(png.end(), std::begin(kPngSignature), std::end(kPngSignature));

    std::vector<uint8_t> ihdr;
    ihdr.reserve(13);
    AppendBigEndian32(ihdr, width);
    AppendBigEndian32(ihdr, height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // rgba
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter
    ihdr.push_back(0); // interlace
    AppendChunk(png, "IHDR", ihdr);

    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    std::vector<uint8_t> filtered;
    filtered.reserve((stride + 1u) * height);
    for (uint32_t row = 0; row < height; ++row) {
        filtered.push_back(0); // no filter
        const auto row_offset = static_cast<std::size_t>(row) * stride;
        filtered.insert(filtered.end(), rgba.begin() + static_cast<std::ptrdiff_t>(row_offset),
                        rgba.begin() + static_cast<std::ptrdiff_t>(row_offset + stride));
    }

    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    std::size_t offset = 0;
    while (offset < filtered.size()) {
        const std::size_t remaining = filtered.size() - offset;
        const uint16_t block_size =
            static_cast<uint16_t>(std::min<std::size_t>(remaining, 65535u));
        const bool final_block = (offset + block_size) == filtered.size();
        zlib.push_back(final_block ? 0x01 : 0x00);
        zlib.push_back(static_cast<uint8_t>(block_size & 0xffu));
        zlib.push_back(static_cast<uint8_t>((block_size >> 8) & 0xffu));
        const uint16_t nlen = static_cast<uint16_t>(~block_size);
        zlib.push_back(static_cast<uint8_t>(nlen & 0xffu));
        zlib.push_back(static_cast<uint8_t>((nlen >> 8) & 0xffu));
        zlib.insert(zlib.end(), filtered.begin() + static_cast<std::ptrdiff_t>(offset),
                    filtered.begin() + static_cast<std::ptrdiff_t>(offset + block_size));
        offset += block_size;
    }
    AppendBigEndian32(zlib, Adler32(filtered));
    AppendChunk(png, "IDAT", zlib);
    AppendChunk(png, "IEND", {});

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        if (error_message) *error_message = "failed to open png output path";
        return false;
    }
    output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!output.good()) {
        if (error_message) *error_message = "failed while writing png output";
        return false;
    }
    return true;
}

} // namespace

struct VulkanRender::Impl {
    Impl()  = default;
    ~Impl() = default;

    bool init(RenderInitInfo);
    void destroy();

    void drawFrame(Scene&);
    void setPaused(bool paused);
    bool requestOffscreenFrameDump(std::string output_path, int32_t frame_number,
                                   std::string* error_message);

    bool CreateRenderingResource(RenderingResources&);
    void DestroyRenderingResource(RenderingResources&);

    void clearLastRenderGraph(bool clear_scene_caches);
    void clearRenderGraphResources();
    void releasePendingSceneResources(Scene&);
    void compileRenderGraph(Scene&, rg::RenderGraph&, bool refresh_resources_only);
    void warmupRenderGraphPipelines(Scene&, rg::RenderGraph&);
    void refreshImportedTextures(Scene&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);

    bool initRes();
    void drawFrameSwapchain();
    void drawFrameOffscreen();
    bool captureOffscreenFrameToPng(const ImageParameters& image, std::string_view output_path,
                                    std::string* error_message);
    void processDeferredGraphPreparation(Scene&);
    void setRenderTargetSize(Scene&, rg::RenderGraph&);
    bool isDeviceFaultResult(VkResult) const;
    bool checkVkResult(VkResult, const char* operation);
    void abandonDeviceOwnedResourcesAfterFault();

    Instance                m_instance;
    std::unique_ptr<Device> m_device;

    std::unique_ptr<PrePass> m_prepass { nullptr };
    std::unique_ptr<FinPass> m_finpass { nullptr };

    std::unique_ptr<FinPass> m_testpass { nullptr };
    ReDrawCB                 m_redraw_cb;

    std::unique_ptr<StagingBuffer> m_vertex_buf { nullptr };
    std::unique_ptr<StagingBuffer> m_dyn_buf { nullptr };

    vvk::CommandBuffers m_cmds;
    vvk::CommandBuffer  m_render_cmd;

    bool m_with_surface { false };
    bool m_inited { false };
    bool m_pass_loaded { false };
    bool m_device_faulted { false };
    bool m_device_fault_log_emitted { false };
    std::deque<std::size_t> m_deferred_prepare_indices;
    std::unordered_set<std::size_t> m_deferred_waiting_indices_logged;

    std::unique_ptr<VulkanExSwapchain> m_ex_swapchain;
    RenderingResources                 m_rendering_resources;

    std::vector<VulkanPass*> m_passes;
    std::vector<std::shared_ptr<rg::Pass>> m_compiled_pass_refs;

    struct PendingFrameCapture {
        std::string path;
        std::string last_error;
        int32_t target_frame { 1 };
        bool requested { false };
        bool completed { false };
        bool succeeded { false };
    };
    std::optional<PendingFrameCapture> m_pending_frame_capture;
    int32_t                            m_offscreen_frame_index { 0 };

};

VulkanRender::VulkanRender(): pImpl(std::make_unique<Impl>()) {}
VulkanRender::~VulkanRender() {};

bool VulkanRender::inited() const { return pImpl->m_inited; }

bool VulkanRender::init(RenderInitInfo info) { return pImpl->init(info); }
void VulkanRender::destroy() { pImpl->destroy(); }
void VulkanRender::drawFrame(Scene& scene) { pImpl->drawFrame(scene); };
void VulkanRender::setPaused(bool paused) { pImpl->setPaused(paused); };
void VulkanRender::clearLastRenderGraph(bool clear_scene_caches) {
    pImpl->clearLastRenderGraph(clear_scene_caches);
};
void VulkanRender::clearRenderGraphResources() { pImpl->clearRenderGraphResources(); };
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg, bool refresh_resources_only) {
    pImpl->compileRenderGraph(scene, rg, refresh_resources_only);
};
void VulkanRender::warmupRenderGraphPipelines(Scene& scene, rg::RenderGraph& rg) {
    pImpl->warmupRenderGraphPipelines(scene, rg);
};
void VulkanRender::refreshImportedTextures(Scene& scene) {
    pImpl->refreshImportedTextures(scene);
};
void VulkanRender::UpdateCameraFillMode(Scene& scene, wallpaper::FillMode fill) {
    pImpl->UpdateCameraFillMode(scene, fill);
};
bool VulkanRender::captureNextOffscreenFrame(std::string output_path, int32_t frame_number,
                                             std::string* error_message) {
    return pImpl->requestOffscreenFrameDump(std::move(output_path), frame_number, error_message);
}

wallpaper::ExSwapchain* VulkanRender::exSwapchain() const { return pImpl->m_ex_swapchain.get(); };

bool VulkanRender::Impl::init(RenderInitInfo info) {
    if (m_inited) return true;

    m_redraw_cb = info.redraw_callback;
    VkExtent2D extent { info.width, info.height };
    if (extent.width * extent.height < 500 * 500) {
        LOG_ERROR("too small swapchain image size: %dx%d", extent.width, extent.height);
    } else {
        LOG_INFO("set swapchain image size: %dx%d", extent.width, extent.height);
    }

    std::vector<Extension> inst_exts { base_inst_exts.begin(), base_inst_exts.end() };
    std::vector<Extension> device_exts { base_device_exts.begin(), base_device_exts.end() };

    if (! info.offscreen) {
        std::transform(info.surface_info.instanceExts.begin(),
                       info.surface_info.instanceExts.end(),
                       std::back_inserter(inst_exts),
                       [](const auto& s) {
                           return Extension { true, s.c_str() };
                       });
        device_exts.push_back({ true, VK_KHR_SWAPCHAIN_EXTENSION_NAME });
    }

    std::vector<InstanceLayer> inst_layers;
    // valid layer
    if (info.enable_valid_layer) {
        inst_layers.push_back({ true, VALIDATION_LAYER_NAME });
        LOG_INFO("vulkan valid layer \"%s\" enabled", VALIDATION_LAYER_NAME.data());
    }

    if (! Instance::Create(m_instance, inst_exts, inst_layers)) {
        LOG_ERROR("init vulkan failed");
        return false;
    }
    if (! info.offscreen) {
        VkSurfaceKHR surface;
        VVK_CHECK_ACT(
            {
                LOG_ERROR("create vulkan surface failed");
                return false;
            },
            info.surface_info.createSurfaceOp(*m_instance.inst(), &surface));
        m_instance.setSurface(VkSurfaceKHR(surface));
        m_with_surface = true;
    }
    {
        auto surface   = *m_instance.surface();
        auto check_gpu = [&device_exts, surface](const vvk::PhysicalDevice& gpu) {
            return Device::CheckGPU(gpu, device_exts, surface);
        };
        const auto preference =
            info.device_preference == wallpaper::VulkanDevicePreference::PreferIntegrated
                ? PhysicalDevicePreference::PreferIntegrated
                : (info.device_preference == wallpaper::VulkanDevicePreference::PreferDiscrete
                       ? PhysicalDevicePreference::PreferDiscrete
                       : PhysicalDevicePreference::Default);
        if (! m_instance.ChoosePhysicalDevice(check_gpu, info.uuid, preference)) return false;
    }

    {
        m_device = std::make_unique<Device>();
        const VideoTexturePipelineSettings video_texture_settings {
            .gpu_pipeline =
                info.gpu_pipeline_preference == wallpaper::GpuPipelinePreference::Va
                    ? VideoTextureGpuPipeline::Va
                    : (info.gpu_pipeline_preference == wallpaper::GpuPipelinePreference::NvidiaStateless
                           ? VideoTextureGpuPipeline::NvidiaStateless
                           : VideoTextureGpuPipeline::Nvidia),
        };
        if (! Device::Create(m_instance, device_exts, extent, *m_device, video_texture_settings)) {
            LOG_ERROR("init vulkan device failed");
            return false;
        }
    }

    if (info.offscreen) {
        m_ex_swapchain = CreateExSwapchain(*m_device,
                                           extent.width,
                                           extent.height,
                                           (info.offscreen_tiling == TexTiling::OPTIMAL
                                                ? VK_IMAGE_TILING_OPTIMAL
                                                : VK_IMAGE_TILING_LINEAR),
                                           info.export_mode,
                                           info.export_drm_fourcc,
                                           info.export_drm_modifiers);
        m_with_surface = false;
    }

    if (! initRes()) return false;
    ;

    m_inited = true;
    return m_inited;
}

bool VulkanRender::Impl::isDeviceFaultResult(VkResult result) const {
    return result == VK_ERROR_DEVICE_LOST || result == VK_TIMEOUT;
}

bool VulkanRender::Impl::checkVkResult(VkResult result, const char* operation) {
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) return true;

    const char* operation_name = operation ? operation : "unknown operation";
    if (isDeviceFaultResult(result)) {
        m_device_faulted = true;
        m_pass_loaded = false;
        if (!m_device_fault_log_emitted) {
            // Device loss is terminal for this VulkanRender instance.  Continuing to submit frames
            // only repeats VK_ERROR_DEVICE_LOST, and destroying every pipeline after an NVIDIA Xid
            // can enter driver teardown paths that have appeared in the Arsenal crash stacks.
            LOG_ERROR("HanabiScene Vulkan: device became unhealthy during %s (%s); "
                      "suppressing future frame submissions and abandoning deep Vulkan teardown",
                      operation_name,
                      vvk::ToString(result));
            m_device_fault_log_emitted = true;
        }
        return false;
    }

    LOG_ERROR("HanabiScene Vulkan: %s failed with %s", operation_name, vvk::ToString(result));
    return false;
}

bool VulkanRender::Impl::initRes() {
    m_prepass = std::make_unique<PrePass>(PrePass::Desc {});
    m_finpass = std::make_unique<FinPass>(FinPass::Desc {});
    if (m_with_surface) {
        m_finpass->setPresentFormat(m_device->swapchain().format());
        m_finpass->setPresentQueueIndex(m_device->present_queue().family_index);
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        m_finpass->setPresentFormat(m_ex_swapchain->format());
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_GENERAL);
        m_finpass->setPresentQueueIndex(VK_QUEUE_FAMILY_EXTERNAL);
    }
    /*
    m_testpass = std::make_unique<FinPass>(FinPass::Desc{});
    m_testpass->setPresentFormat(m_ex_swapchain->format());
    m_testpass->setPresentQueueIndex(m_device->graphics_queue().family_index);
    m_testpass->setPresentLayout(vk::ImageLayout::ePresentSrcKHR);
    */

    m_vertex_buf = std::make_unique<StagingBuffer>(*m_device,
                                                   2 * 1024 * 1024,
                                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_dyn_buf    = std::make_unique<StagingBuffer>(*m_device,
                                                2 * 1024 * 1024,
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (! m_vertex_buf->allocate()) return false;
    if (! m_dyn_buf->allocate()) return false;
    {
        auto& pool = m_device->cmd_pool();
        VVK_CHECK_BOOL_RE(pool.Allocate(vk_command_num, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_cmds));
        m_render_cmd = vvk::CommandBuffer(m_cmds[0], m_device->handle().Dispatch());
    }
    if (! CreateRenderingResource(m_rendering_resources)) return false;

#if ENABLE_RENDERDOC_API
    load_renderdoc_api();
#endif
    return true;
}

void VulkanRender::Impl::abandonDeviceOwnedResourcesAfterFault() {
    // Once the GPU has reported timeout/device-lost, Vulkan object destruction is no longer a safe
    // cleanup mechanism on the observed NVIDIA path.  This routine intentionally abandons wrapper
    // ownership so process shutdown or backend replacement does not call back into driver destroy
    // functions with a broken device.  The leaked objects are bounded to this renderer instance and
    // are preferable to a SIGSEGV while switching away from the failed scene.
    m_rendering_resources.sem_swap_wait_image.abandon();
    m_rendering_resources.sem_swap_finish.abandon();
    m_rendering_resources.fence_frame.abandon();
    m_rendering_resources.command.abandon();
    for (auto& [_, image] : m_rendering_resources.model_depth_images) {
        image.sampler.abandon();
        image.view.abandon();
        image.handle.abandon();
    }
    m_rendering_resources.model_depth_images.clear();
    if (m_rendering_resources.pipeline_cache) {
        m_rendering_resources.pipeline_cache->abandon();
    }
    if (m_device) {
        m_device->tex_cache().CancelDeferredGraphActivation();
    }
    m_rendering_resources.vertex_buf = nullptr;
    m_rendering_resources.dyn_buf = nullptr;
    m_deferred_prepare_indices.clear();
    m_deferred_waiting_indices_logged.clear();

    m_render_cmd.abandon();
    m_compiled_pass_refs.clear();
    m_passes.clear();
    (void)m_prepass.release();
    (void)m_finpass.release();
    (void)m_testpass.release();
    (void)m_vertex_buf.release();
    (void)m_dyn_buf.release();
    (void)m_ex_swapchain.release();
    (void)m_device.release();
    m_instance.Abandon();
    m_inited = false;
    m_pass_loaded = false;
}

void VulkanRender::Impl::destroy() {
    if (! m_inited && !m_device_faulted) return;
    if (m_device_faulted) {
        abandonDeviceOwnedResourcesAfterFault();
        return;
    }
    if (m_device && m_device->handle()) {
        if (!checkVkResult(m_device->handle().WaitIdle(), "device wait idle before destroy")) {
            abandonDeviceOwnedResourcesAfterFault();
            return;
        }

        // res
        for (auto& p : m_passes) {
            p->destory(*m_device, m_rendering_resources);
        }
        m_compiled_pass_refs.clear();
        m_passes.clear();
        m_deferred_prepare_indices.clear();
        m_deferred_waiting_indices_logged.clear();
        m_device->tex_cache().CancelDeferredGraphActivation();
        if (m_rendering_resources.pipeline_cache) {
            m_rendering_resources.pipeline_cache->clear();
        }
        m_vertex_buf->destroy();
        m_dyn_buf->destroy();

        m_device->Destroy();
    }
    m_instance.Destroy();
}

bool VulkanRender::Impl::CreateRenderingResource(RenderingResources& rr) {
    rr.command = m_render_cmd;
    VVK_CHECK_BOOL_RE(m_device->handle().CreateFence(
        VkFenceCreateInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        },
        rr.fence_frame));

    rr.fence_frame.Reset();

    if (m_with_surface) {
        VkSemaphoreCreateInfo ci { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                   .pNext = nullptr };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_finish));
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_wait_image));
    }

    rr.vertex_buf = m_vertex_buf.get();
    rr.dyn_buf    = m_dyn_buf.get();
    rr.pipeline_cache = std::make_shared<GraphicsPipelineStateCache>();
    return true;
}

void VulkanRender::Impl::DestroyRenderingResource(RenderingResources& rr) {}

// VulkanExSwapchain* VulkanRender::exSwapchain() const { return m_ex_swapchain.get(); }

void VulkanRender::Impl::drawFrame(Scene& scene) {
    if (m_device_faulted) return;
    if (! (m_inited && m_pass_loaded)) return;

    // The QuickJS host records getVideoTexture().play()/pause() decisions on Scene before the
    // renderer polls GStreamer. Applying them here keeps hidden authored videos from decoding
    // while prepared passes can still reuse the last uploaded frame when they are invisible.
    m_device->video_tex_cache().ApplyPlaybackStates(scene.videoTexturePaused,
                                                    scene.videoTextureStopped);
    // setCurrentTime() requests are one-shot decoder commands, so the video cache consumes and
    // removes only the requests whose concrete GStreamer pipeline already exists.
    m_device->video_tex_cache().ApplySeekRequests(scene.videoTextureSeekRequests);
    m_device->video_tex_cache().Poll();
    processDeferredGraphPreparation(scene);

        // LOG_INFO("used ram: %fm", (m_device->GetUsage()/1024.0f)/1024.0f);

#if ENABLE_RENDERDOC_API
    if (rdoc_api)
        rdoc_api->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE((VkInstance)m_instance.inst()), NULL);
#endif

    if (m_instance.offscreen()) {
        drawFrameOffscreen();
    } else {
        drawFrameSwapchain();
    }

    if (m_redraw_cb) m_redraw_cb();

#if ENABLE_RENDERDOC_API
    if (rdoc_api)
        rdoc_api->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE((VkInstance)m_instance.inst()), NULL);
#endif
}

void VulkanRender::Impl::setPaused(bool paused) {
    if (m_device == nullptr) return;
    m_device->video_tex_cache().SetGlobalPaused(paused);
}

void VulkanRender::Impl::refreshImportedTextures(Scene& scene) {
    if (!m_device) return;

    for (const auto& key : scene.dirtyImportedTextureKeys) {
        scene.DropParsedImageCache(key);
        auto image = scene.ParseImageBlockingCached(key);
        if (!image) continue;
        m_device->tex_cache().CreateTex(*image);
        scene.DropParsedImageCache(key);
    }
    scene.dirtyImportedTextureKeys.clear();
}

void VulkanRender::Impl::processDeferredGraphPreparation(Scene& scene) {
    if (m_deferred_prepare_indices.empty()) return;
    if (m_device_faulted || !m_device) return;

    const auto batch_started_at = std::chrono::steady_clock::now();
    std::size_t attempted = 0;
    std::size_t prepared = 0;

    while (attempted < kDeferredPrepareMaxPassesPerFrame && !m_deferred_prepare_indices.empty()) {
        if (attempted != 0) {
            const auto batch_elapsed_ms =
                static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - batch_started_at)
                                        .count()) /
                1000.0;
            if (batch_elapsed_ms >= kDeferredPrepareFrameBudgetMs) {
                break;
            }
        }

        const auto pass_index = m_deferred_prepare_indices.front();
        if (pass_index >= m_passes.size()) {
            m_deferred_prepare_indices.pop_front();
            m_deferred_waiting_indices_logged.erase(pass_index);
            continue;
        }

        auto* pass = m_passes[pass_index];
        if (pass == nullptr || pass->prepared()) {
            m_deferred_prepare_indices.pop_front();
            m_deferred_waiting_indices_logged.erase(pass_index);
            continue;
        }

        const auto key = pass->residencyKey();
        const auto resources_state = pass->requestDeferredPrepareResources(scene, *m_device);
        if (resources_state == DeferredPrepareResourcesState::Waiting) {
            if (m_deferred_waiting_indices_logged.insert(pass_index).second) {
                LOG_VERBOSE("RenderGraphDeferredPrepareWait: index=%zu remaining=%zu key='%s'",
                            pass_index,
                            m_deferred_prepare_indices.size(),
                            key.c_str());
            }
            break;
        }

        m_deferred_waiting_indices_logged.erase(pass_index);

        const auto pass_started_at = std::chrono::steady_clock::now();
        pass->prepareDeferred(scene, *m_device, m_rendering_resources);
        const auto pass_elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - pass_started_at)
                                         .count();
        attempted++;
        if (pass->prepared()) {
            prepared++;
            m_deferred_prepare_indices.pop_front();
        }

        LOG_VERBOSE("RenderGraphDeferredPreparePass: index=%zu prepared=%s remaining=%zu "
                    "duration=%.2fms key='%s'",
                    pass_index,
                    pass->prepared() ? "true" : "false",
                    m_deferred_prepare_indices.size(),
                    static_cast<double>(pass_elapsed_us) / 1000.0,
                    key.c_str());
        if (!pass->prepared()) {
            break;
        }
        const auto batch_elapsed_ms =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - batch_started_at)
                                    .count()) /
            1000.0;
        if (batch_elapsed_ms >= kDeferredPrepareFrameBudgetMs) {
            break;
        }
    }

    const auto batch_elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - batch_started_at)
                                      .count();
    if (attempted != 0) {
        LOG_VERBOSE("RenderGraphDeferredPrepareBatch: attempted=%zu prepared=%zu remaining=%zu "
                    "duration=%.2fms",
                    attempted,
                    prepared,
                    m_deferred_prepare_indices.size(),
                    static_cast<double>(batch_elapsed_us) / 1000.0);
    }

    if (m_deferred_prepare_indices.empty()) {
        m_deferred_waiting_indices_logged.clear();
        m_device->tex_cache().EndDeferredGraphActivation();
        LOG_VERBOSE("RenderGraphDeferredPrepareComplete");
    }
}

void VulkanRender::Impl::drawFrameSwapchain() {
    static size_t resource_index = 0;

    RenderingResources& rr = m_rendering_resources;
    resource_index         = (resource_index + 1) % 3;
    uint32_t image_index   = 0;
    {
        if (!checkVkResult(m_device->handle().AcquireNextImageKHR(*m_device->swapchain().handle(),
                                                                  vk_wait_time,
                                                                  *rr.sem_swap_wait_image,
                                                                  {},
                                                                  &image_index),
                           "acquire swapchain image"))
            return;
    }
    const auto& image = m_device->swapchain().images()[image_index];

    m_finpass->setPresent(image);

    for (auto* p : m_passes) {
        if (p->prepared()) {
            // Dynamic passes copy current CPU-side vertex/index bytes into staging before the
            // upload command is recorded. This keeps reused-source particle systems from binding a
            // freshly grown suballocation whose GPU contents have not been uploaded yet.
            p->updateBeforeUpload();
        }
    }

    if (!checkVkResult(rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    }), "begin swapchain frame command buffer"))
        return;
    // Deferred pass preparation can allocate and write static vertex/index subranges between
    // frames. Recording the static upload here keeps those newly resident passes drawable without
    // a compile-time WaitIdle, matching the frame-budgeted residency model used by streaming
    // renderers.
    m_vertex_buf->recordUpload(rr.command);
    m_dyn_buf->recordUpload(rr.command);
    m_device->tex_cache().RecordUploads(rr.command);
    m_device->video_tex_cache().RecordUploads(rr.command);
    for (auto* p : m_passes) {
        if (p->prepared()) {
            p->execute(*m_device, rr);
        }
    }
    if (!checkVkResult(rr.command.End(), "end swapchain frame command buffer"))
        return;

    VkPipelineStageFlags wait_dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo         sub_info {
                .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext                = nullptr,
                .waitSemaphoreCount   = 1,
                .pWaitSemaphores      = rr.sem_swap_wait_image.address(),
                .pWaitDstStageMask    = &wait_dst_stage,
                .commandBufferCount   = 1,
                .pCommandBuffers      = rr.command.address(),
                .signalSemaphoreCount = 1,
                .pSignalSemaphores    = rr.sem_swap_finish.address(),
    };

    if (!checkVkResult(m_device->present_queue().handle.Submit(sub_info, *rr.fence_frame),
                       "submit swapchain frame"))
        return;
    VkPresentInfoKHR present_info {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext              = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = rr.sem_swap_finish.address(),
        .swapchainCount     = 1,
        .pSwapchains        = m_device->swapchain().handle().address(),
        .pImageIndices      = &image_index,
    };
    if (!checkVkResult(m_device->present_queue().handle.Present(present_info),
                       "present swapchain frame"))
        return;

    if (!checkVkResult(rr.fence_frame.Wait(vk_wait_time), "wait swapchain frame fence"))
        return;
    m_device->tex_cache().RetireCompletedUploads();
    if (!checkVkResult(rr.fence_frame.Reset(), "reset swapchain frame fence"))
        return;
}
void VulkanRender::Impl::drawFrameOffscreen() {
    ++m_offscreen_frame_index;
    RenderingResources& rr    = m_rendering_resources;
    ImageParameters     image = m_ex_swapchain->GetInprogressImage();

    m_finpass->setPresent(image);

    for (auto* p : m_passes) {
        if (p->prepared()) {
            // Offscreen rendering exports the result to GTK, making stale particle bytes visible as
            // source-switch flicker. Pre-updating dynamic mesh data aligns the following m_dyn_buf
            // upload with the frame that will be exported.
            p->updateBeforeUpload();
        }
    }

    if (!checkVkResult(rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    }), "begin offscreen frame command buffer"))
        return;
    m_vertex_buf->recordUpload(rr.command);
    m_dyn_buf->recordUpload(rr.command);
    m_device->tex_cache().RecordUploads(rr.command);
    m_device->video_tex_cache().RecordUploads(rr.command);

    for (auto* p : m_passes) {
        if (p->prepared()) {
            p->execute(*m_device, rr);
        }
    }

    if (!checkVkResult(rr.command.End(), "end offscreen frame command buffer"))
        return;

    VkSubmitInfo sub_info {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext              = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers    = rr.command.address(),
    };
    if (!checkVkResult(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame),
                       "submit offscreen frame"))
        return;

    if (!checkVkResult(rr.fence_frame.Wait(vk_wait_time), "wait offscreen frame fence"))
        return;
    m_device->tex_cache().RetireCompletedUploads();
    if (!checkVkResult(rr.fence_frame.Reset(), "reset offscreen frame fence"))
        return;
    if (m_pending_frame_capture && m_pending_frame_capture->requested &&
        m_pending_frame_capture->target_frame == m_offscreen_frame_index &&
        !m_pending_frame_capture->completed) {
        m_pending_frame_capture->succeeded =
            captureOffscreenFrameToPng(image,
                                       m_pending_frame_capture->path,
                                       &m_pending_frame_capture->last_error);
        m_pending_frame_capture->completed = true;
    }
    m_ex_swapchain->renderFrame();
}

bool VulkanRender::Impl::requestOffscreenFrameDump(std::string output_path,
                                                   int32_t frame_number,
                                                   std::string* error_message) {
    if (output_path.empty()) {
        if (error_message) *error_message = "capture path is empty";
        return false;
    }
    if (frame_number < 1) {
        if (error_message) *error_message = "capture frame number must be >= 1";
        return false;
    }
    m_pending_frame_capture = PendingFrameCapture {
        .path = std::move(output_path),
        .last_error = {},
        .target_frame = frame_number,
        .requested = true,
        .completed = false,
        .succeeded = false,
    };
    return true;
}

bool VulkanRender::Impl::captureOffscreenFrameToPng(const ImageParameters& image,
                                                    std::string_view       output_path,
                                                    std::string*           error_message) {
    if (output_path.empty()) {
        if (error_message) *error_message = "capture path is empty";
        return false;
    }

    if (!m_device) {
        if (error_message) *error_message = "renderer device is not initialized";
        return false;
    }

    const uint32_t width = image.extent.width;
    const uint32_t height = image.extent.height;
    const std::size_t pixel_bytes = static_cast<std::size_t>(width) * height * 4u;
    if (pixel_bytes == 0) {
        if (error_message) *error_message = "offscreen frame has zero size";
        return false;
    }

    VmaBufferParameters readback_buffer;
    if (!CreateReadbackBuffer(m_device->vma_allocator(), pixel_bytes, readback_buffer)) {
        if (error_message) *error_message = "failed to allocate readback buffer";
        return false;
    }

    vvk::CommandBuffers command_buffers;
    VVK_CHECK_BOOL_RE(m_device->cmd_pool().Allocate(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY, command_buffers));
    vvk::CommandBuffer command(command_buffers[0], m_device->handle().Dispatch());

    if (!checkVkResult(command.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        }),
        "begin frame capture command buffer")) {
        return false;
    }

    VkImageMemoryBarrier image_to_transfer {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.handle,
        .subresourceRange = VkImageSubresourceRange {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    command.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0,
                            image_to_transfer);

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
        .imageExtent = image.extent,
    };
    command.CopyImageToBuffer(image.handle,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              *readback_buffer.handle,
                              spanone { copy_region });

    VkBufferMemoryBarrier buffer_ready {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = *readback_buffer.handle,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    command.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT,
                            0,
                            buffer_ready);

    VkImageMemoryBarrier image_restore {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.handle,
        .subresourceRange = image_to_transfer.subresourceRange,
    };
    command.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            0,
                            image_restore);

    if (!checkVkResult(command.End(), "end frame capture command buffer")) {
        return false;
    }

    vvk::Fence fence;
    VVK_CHECK_BOOL_RE(m_device->handle().CreateFence(VkFenceCreateInfo {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    }, fence));
    VkSubmitInfo submit {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = command.address(),
    };
    if (!checkVkResult(m_device->graphics_queue().handle.Submit(submit, *fence),
                       "submit frame capture")) {
        return false;
    }
    if (!checkVkResult(fence.Wait(vk_wait_time), "wait frame capture")) {
        return false;
    }

    void* mapped = nullptr;
    if (readback_buffer.handle.MapMemory(&mapped) != VK_SUCCESS) {
        if (error_message) *error_message = "failed to map readback buffer";
        return false;
    }
    std::vector<uint8_t> rgba(pixel_bytes);
    std::memcpy(rgba.data(), mapped, pixel_bytes);
    readback_buffer.handle.UnMapMemory();

    // Vulkan images are bottom-up relative to the fullscreen quad we present from; flip to match
    // what the viewer window shows.
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    std::vector<uint8_t> flipped(pixel_bytes);
    for (uint32_t row = 0; row < height; ++row) {
        const std::size_t src_offset = static_cast<std::size_t>(height - 1 - row) * row_bytes;
        const std::size_t dst_offset = static_cast<std::size_t>(row) * row_bytes;
        std::memcpy(flipped.data() + dst_offset, rgba.data() + src_offset, row_bytes);
    }

    // The offscreen export is a debugging surface dump. Some scenes leave the swap image alpha at
    // zero even when RGB contains valid content, which makes the PNG appear blank in normal image
    // viewers. Force opaque alpha so the exported frame is directly inspectable.
    for (std::size_t pixel = 0; pixel < pixel_bytes; pixel += 4u) {
        flipped[pixel + 3u] = 0xffu;
    }

    return WriteRgbaPng(std::string(output_path), width, height, flipped, error_message);
}

void VulkanRender::Impl::setRenderTargetSize(Scene& scene, rg::RenderGraph& rg) {
    auto& ext = m_device->out_extent();
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.enable && rt.bind.screen) {
            rt.width  = (i32)(rt.bind.scale * ext.width);
            rt.height = (i32)(rt.bind.scale * ext.height);
            // Screen-sized render targets expose the full framebuffer as both their physical and
            // logical extent. Only text-owned runtime targets intentionally diverge these values.
            rt.mapWidth = rt.width;
            rt.mapHeight = rt.height;
        }
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.screen || ! rt.bind.enable) continue;
        auto bind_rt = scene.renderTargets.find(rt.bind.name);
        if (rt.bind.name.empty() || bind_rt == scene.renderTargets.end()) {
            LOG_ERROR("unknonw render target bind: %s", rt.bind.name.c_str());
            continue;
        }
        rt.width  = (i32)(rt.bind.scale * bind_rt->second.width);
        rt.height = (i32)(rt.bind.scale * bind_rt->second.height);
        // Bound render targets inherit the sampled content rectangle from their source target so
        // shader uniforms continue to describe the authored image area rather than the raw backing
        // allocation. This keeps generic effect chains consistent whenever the source target uses
        // a logical content rectangle that differs from its physical allocation.
        rt.mapWidth = (i32)(rt.bind.scale * bind_rt->second.ContentWidth());
        rt.mapHeight = (i32)(rt.bind.scale * bind_rt->second.ContentHeight());
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (! item.first.empty() && (rt.width * rt.height <= 4)) {
            LOG_ERROR("wrong size for render target: %s", item.first.c_str());
        } else if (rt.has_mipmap) {
            rt.mipmap_level =
                std::max(3u,
                         static_cast<uint>(std::floor(std::log2(std::min(rt.width, rt.height))))) -
                2u;
        }
    }
    scene.shaderValueUpdater->SetScreenSize((i32)ext.width, (i32)ext.height);
}

void VulkanRender::Impl::UpdateCameraFillMode(wallpaper::Scene&   scene,
                                              wallpaper::FillMode fillmode) {
    using namespace wallpaper;
    auto width  = m_device->out_extent().width;
    auto height = m_device->out_extent().height;

    if (width == 0) return;
    double sw = scene.ortho[0], sh = scene.ortho[1];
    double fboAspect = width / (double)height, sAspect = sw / sh;
    auto&  gCam    = *scene.cameras.at("global");
    auto&  gPerCam = *scene.cameras.at("global_perspective");

    // Camera-layer animation mutates the same shared "global" camera object that fill mode uses to
    // adapt a 16:9 project to the monitor aspect. Preserve the live zoom value here so the render
    // side remains the single source of truth for framebuffer-relative width/height while authored
    // zoom still narrows that already aspect-correct view.
    double active_global_zoom = scene.defaultGlobalCameraZoom;
    if (!std::isfinite(active_global_zoom) || active_global_zoom <= 0.0001) {
        active_global_zoom = 1.0;
    }

    // Perspective camera layers can animate FOV directly. Keep that authored value when the active
    // layer explicitly targets the shared perspective camera; otherwise derive FOV from the
    // orthographic framing so perspective particles continue to match the visible crop/fit window.
    bool  use_active_global_perspective_fov = false;
    float active_global_perspective_fov = 50.0f;
    if (scene.activeCameraLayerId != 0) {
        auto active_layer_it = scene.cameraLayers.find(scene.activeCameraLayerId);
        if (active_layer_it != scene.cameraLayers.end()) {
            const auto& active_layer = active_layer_it->second;
            if (active_layer.camera_name.empty() || active_layer.camera_name == "global") {
                if (std::isfinite(active_layer.zoom) && active_layer.zoom > 0.0001) {
                    active_global_zoom = active_layer.zoom;
                } else {
                    active_global_zoom = 1.0;
                }
            } else if (active_layer.camera_name == "global_perspective" &&
                       std::isfinite(active_layer.fov) && active_layer.fov > 0.0001f) {
                use_active_global_perspective_fov = true;
                active_global_perspective_fov = active_layer.fov;
            }
        }
    }

    double framed_width = sw;
    double framed_height = sh;
    double perspective_aspect = sAspect;

    switch (fillmode) {
    case FillMode::STRETCH:
        framed_width = sw;
        framed_height = sh;
        perspective_aspect = sAspect;
        break;
    case FillMode::ASPECTFIT:
        if (fboAspect < sAspect) {
            // Preserve the full scene width and add vertical space when the output is taller.
            framed_width = sw;
            framed_height = sw / fboAspect;
        } else {
            framed_width = sh * fboAspect;
            framed_height = sh;
        }
        perspective_aspect = fboAspect;
        break;
    case FillMode::ASPECTCROP:
    default:
        if (fboAspect > sAspect) {
            // Preserve the full scene width and crop vertically when the output is wider.
            framed_width = sw;
            framed_height = sw / fboAspect;
        } else {
            framed_width = sh * fboAspect;
            framed_height = sh;
        }
        perspective_aspect = fboAspect;
        break;
    }

    gCam.SetWidth(std::max(1.0, framed_width / active_global_zoom));
    gCam.SetHeight(std::max(1.0, framed_height / active_global_zoom));
    gPerCam.SetAspect(perspective_aspect);
    gPerCam.SetFov(use_active_global_perspective_fov
                       ? active_global_perspective_fov
                       : algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
    gCam.Update();
    gPerCam.Update();
    scene.UpdateLinkedCamera("global");

    if (!scene.modelPerspectiveCameraName.empty()) {
        auto model_camera_it = scene.cameras.find(scene.modelPerspectiveCameraName);
        if (model_camera_it != scene.cameras.end() && model_camera_it->second) {
            // 3D model chunks render through a camera that is intentionally isolated from the
            // legacy `global_perspective` camera, but its projection still has to follow the same
            // fill-mode-adjusted framebuffer aspect. Without this, a 16:9-authored model scene keeps
            // its native projection while Vulkan draws into a 16:10 or other non-native viewport,
            // which changes the apparent object proportions even though the model transform itself
            // is uniform. Only the aspect is synchronized here: the authored 3D FOV and the
            // camera-path eye/center/up basis remain owned by the scene data and path playback.
            model_camera_it->second->SetAspect(perspective_aspect);
            model_camera_it->second->Update();
            scene.UpdateLinkedCamera(scene.modelPerspectiveCameraName);
        } else {
            // A named model camera should exist whenever model nodes were materialized. Log this
            // explicitly instead of silently falling back to another camera, because choosing a
            // substitute would hide the real render-graph/parser state mismatch and make 3D aspect
            // regressions harder to diagnose from run.log.
            LOG_ERROR("Scene3DModelCameraAspect: missing model camera '%s' while applying "
                      "fill-mode perspective aspect %.6f",
                      scene.modelPerspectiveCameraName.c_str(),
                      perspective_aspect);
        }
    }

    // Text layers with Wallpaper Engine's screen-anchor property are authored against the project
    // canvas edge, but the active orthographic camera edge moves when aspect crop/fit changes the
    // visible frame. Re-apply those anchor transforms after camera framing so HUD-style text
    // remains inside the actual output instead of the uncropped project bounds.
    ApplyTextLayerScreenAnchorTransforms(scene);
}

void VulkanRender::Impl::clearLastRenderGraph(bool clear_scene_caches) {
    if (m_device_faulted) {
        // After device loss, pass destruction can call vkDestroyPipeline and friends on a driver
        // context that already timed out.  Leave the bounded stale graph abandoned with the renderer
        // instead of turning a recoverable backend replacement into a process crash.
        return;
    }

    // A topology rebuild invalidates the compiled pass list and the backing mesh buffers that were
    // uploaded for the previous graph. Reallocating those buffers keeps the full rebuild path
    // conservative and mirrors the historical behavior used when nodes were added or removed.
    for (auto& p : m_passes) {
        p->destory(*m_device, m_rendering_resources);
    }
    m_passes.clear();
    m_compiled_pass_refs.clear();
    m_deferred_prepare_indices.clear();
    m_deferred_waiting_indices_logged.clear();
    m_device->tex_cache().CancelDeferredGraphActivation();
    if (clear_scene_caches) {
        // Scene switches and renderer shutdown still own a full cache teardown. Ordinary topology
        // rebuilds no longer do this: visibility-driven residency now releases only the keys that
        // became unreachable, so showing one deferred layer cannot evict every unrelated texture
        // and video decoder in the wallpaper.
        m_device->tex_cache().Clear();
        m_device->video_tex_cache().Clear();
        if (m_rendering_resources.pipeline_cache) {
            m_rendering_resources.pipeline_cache->clear();
        }
    }
    // Shared model depth images are tied to the compiled graph's output targets. Dropping them on
    // full graph rebuilds keeps 3D model depth opt-in and avoids stale depth attachments surviving
    // after scene topology or render-target ownership changes.
    m_rendering_resources.model_depth_images.clear();

    m_vertex_buf->destroy();
    m_dyn_buf->destroy();

    m_vertex_buf->allocate();
    m_dyn_buf->allocate();
}

void VulkanRender::Impl::releasePendingSceneResources(Scene& scene) {
    if (m_device_faulted || !m_device) return;
    if (scene.pendingStaticTextureReleaseKeys.empty() &&
        scene.pendingVideoTextureReleaseKeys.empty() &&
        scene.pendingRenderTargetReleaseKeys.empty()) {
        return;
    }

    const auto before_texture_bytes = m_device->tex_cache().GetTrackedBytes();
    const auto before_texture_count = m_device->tex_cache().GetTrackedImageCount();
    const auto before_video_bytes   = m_device->video_tex_cache().GetTrackedBytes();
    const auto before_video_count   = m_device->video_tex_cache().GetTrackedEntryCount();

    std::size_t released_static = 0;
    std::size_t released_render_targets = 0;
    std::size_t released_videos = 0;

    for (const auto& key : scene.pendingStaticTextureReleaseKeys) {
        if (m_device->tex_cache().ReleaseTexture(key)) released_static++;
        scene.DropParsedImageCache(key);
    }
    for (const auto& key : scene.pendingRenderTargetReleaseKeys) {
        if (m_device->tex_cache().ReleaseRenderTarget(key)) released_render_targets++;
    }
    for (const auto& key : scene.pendingVideoTextureReleaseKeys) {
        if (m_device->video_tex_cache().Release(key)) released_videos++;
    }

    LOG_VERBOSE("SceneResidencyRelease: static=%zu/%zu render-target=%zu/%zu video=%zu/%zu "
                "texture-bytes-before=%zu texture-bytes-after=%zu texture-images-before=%zu "
                "texture-images-after=%zu video-bytes-before=%zu video-bytes-after=%zu "
                "video-entries-before=%zu video-entries-after=%zu",
                released_static,
                scene.pendingStaticTextureReleaseKeys.size(),
                released_render_targets,
                scene.pendingRenderTargetReleaseKeys.size(),
                released_videos,
                scene.pendingVideoTextureReleaseKeys.size(),
                before_texture_bytes,
                m_device->tex_cache().GetTrackedBytes(),
                before_texture_count,
                m_device->tex_cache().GetTrackedImageCount(),
                before_video_bytes,
                m_device->video_tex_cache().GetTrackedBytes(),
                before_video_count,
                m_device->video_tex_cache().GetTrackedEntryCount());

    scene.pendingStaticTextureReleaseKeys.clear();
    scene.pendingVideoTextureReleaseKeys.clear();
    scene.pendingRenderTargetReleaseKeys.clear();
}

void VulkanRender::Impl::clearRenderGraphResources() {
    // Resource-only rebuilds are hot resource refreshes, not a miniature full rebuild. Particle
    // effects already update every frame without clearing global caches; effect-backed text must
    // follow the same rule. TextureCache::Query now detects per-key TextureKey changes and
    // reallocates only the resized render target, so clearing the entire cache here would recreate
    // unrelated offscreen images and reintroduce the minute-rollover hitch.
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg,
                                            bool refresh_resources_only) {
    if (m_device_faulted) return;
    if (! m_inited) return;
    m_pass_loaded = false;
    const bool had_resident_graph = !m_compiled_pass_refs.empty();

    if (refresh_resources_only && !m_passes.empty()) {
        setRenderTargetSize(scene, rg);

        const auto dirty_render_targets = scene.dirtyRenderTargetKeys;
        const auto dirty_text_layers = scene.dirtyTextLayerIds;
        // A resource refresh can now be targeted either by render-target key or by text layer id.
        // Treating an empty render-target set as "refresh everything" was correct before direct
        // text had its own dirty set, but it would turn every Clock tick back into a full pass walk.
        const bool has_targeted_dirty_resources =
            !dirty_render_targets.empty() || !dirty_text_layers.empty();
        const bool refresh_all =
            scene.renderGraphAllResourcesDirty || !has_targeted_dirty_resources;
        std::size_t refreshed_passes = 0;
        std::size_t prepared_passes = 0;

        for (size_t pass_index = 0; pass_index < m_passes.size(); ++pass_index) {
            auto* p = m_passes[pass_index];
            if (p == nullptr) continue;

            const bool affected =
                refresh_all || p->referencesAnyRenderTarget(dirty_render_targets) ||
                p->referencesAnyTextLayer(dirty_text_layers);
            if (!affected) continue;

            if (p->prepared()) {
                // Text bridge updates are now target-scoped like particle resources: refresh only
                // passes that touch the resized render targets so a one-pixel glyph-width change
                // cannot force every static shader pass in the wallpaper to rebind resources.
                p->refreshResources(scene, *m_device, m_rendering_resources);
                refreshed_passes++;
            }
            if (!p->prepared()) {
                p->prepare(scene, *m_device, m_rendering_resources);
                prepared_passes++;
            }
        }

        // Resource-only refreshes are intentionally silent in production; the counters stay local
        // so the branch preserves targeted text-bridge behavior without making minute rollovers
        // spend time formatting render-graph diagnostics.
        (void)refresh_all;
        (void)refreshed_passes;
        (void)prepared_passes;
        // Mature renderers do not submit a separate upload command and idle the whole device while
        // rebuilding resource bindings. The next draw command records all dirty vertex, dynamic,
        // and texture uploads before executing passes, preserving ordering without a render-thread
        // queue drain.
        m_pass_loaded = true;
        return;
    }

    auto nodes             = rg.topologicalOrder();
    auto node_release_texs = rg.getLastReadTexs(nodes);

    m_passes.clear();
    m_deferred_prepare_indices.clear();
    m_deferred_waiting_indices_logged.clear();
    m_device->tex_cache().CancelDeferredGraphActivation();
    m_passes.resize(nodes.size());

    std::unordered_map<std::string, std::shared_ptr<rg::Pass>> reusable_passes;
    std::unordered_map<std::string, std::size_t> old_key_counts;
    for (const auto& old_pass_ref : m_compiled_pass_refs) {
        auto old_pass = std::dynamic_pointer_cast<VulkanPass>(old_pass_ref);
        if (!old_pass) continue;
        const auto key = MakeResidencyInstanceKey(*old_pass, old_key_counts);
        if (!key.empty()) reusable_passes.emplace(key, old_pass_ref);
    }

    std::unordered_map<std::string, std::size_t> new_key_counts;
    std::unordered_set<VulkanPass*> reused_passes;
    std::vector<std::shared_ptr<rg::Pass>> next_compiled_pass_refs;
    next_compiled_pass_refs.reserve(nodes.size());
    std::size_t reused_count = 0;
    std::size_t new_count = 0;

    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto node_id = nodes[index];
        auto       pass_ref = rg.getPassShared(node_id);
        assert(pass_ref != nullptr);
        auto*      vpass = dynamic_cast<VulkanPass*>(pass_ref.get());
        assert(vpass != nullptr);

        const auto key = MakeResidencyInstanceKey(*vpass, new_key_counts);
        if (!key.empty()) {
            if (auto reusable_it = reusable_passes.find(key);
                reusable_it != reusable_passes.end()) {
                auto reusable_vpass = std::dynamic_pointer_cast<VulkanPass>(reusable_it->second);
                if (reusable_vpass && reusable_vpass->canReuseForResidency(*vpass)) {
                    // Mature renderers do not destroy every pipeline just because one layer toggled
                    // visibility. The new render graph describes the fresh topology, while this
                    // handoff keeps matching prepared pass objects alive and updates only their
                    // graph-local gates/texture declarations. Removed passes are retired below,
                    // which preserves hidden-layer resource release without a whole-scene PSO
                    // rebuild.
                    reusable_vpass->absorbResidencyGraphState(*vpass);
                    pass_ref = reusable_it->second;
                    vpass = reusable_vpass.get();
                    rg.replacePass(node_id, pass_ref);
                    reused_passes.insert(vpass);
                    reused_count++;
                    reusable_passes.erase(reusable_it);
                }
            }
        }
        if (reused_passes.count(vpass) == 0) {
            new_count++;
        }

        // Release ownership is compiled from the current render graph topology, not from pass
        // construction. Clear stale metadata before assigning this graph's final-reader keys so
        // reused pass objects keep an exact lifecycle contract.
        vpass->clearReleaseTexs();
        for (auto& tex : node_release_texs[index]) {
            vpass->addReleaseTexs(spanone<const std::string_view> { tex->key() });
        }
        m_passes[index] = vpass;
        next_compiled_pass_refs.push_back(std::move(pass_ref));
    }

    std::unordered_set<VulkanPass*> destroyed_passes;
    std::size_t retired_count = 0;
    for (const auto& [_, stale_pass_ref] : reusable_passes) {
        auto stale_pass = std::dynamic_pointer_cast<VulkanPass>(stale_pass_ref);
        if (!stale_pass) continue;
        const auto before_destroy_count = destroyed_passes.size();
        DestroyPassOnce(stale_pass.get(), *m_device, m_rendering_resources, destroyed_passes);
        if (destroyed_passes.size() != before_destroy_count) retired_count++;
    }
    m_compiled_pass_refs = std::move(next_compiled_pass_refs);
    releasePendingSceneResources(scene);

    LOG_VERBOSE("RenderGraphResidencyDiff: reused=%zu new=%zu retired=%zu graph-passes=%zu",
                reused_count,
                new_count,
                retired_count,
                nodes.size());

    m_passes.insert(m_passes.begin(), m_prepass.get());
    m_passes.push_back(m_finpass.get());

    setRenderTargetSize(scene, rg);

    std::size_t reused_refreshed_count = 0;
    std::size_t refreshed_count = 0;
    std::size_t prepared_count = 0;
    std::size_t dependency_prepared_count = 0;
    std::size_t deferred_count = 0;
    std::size_t deferred_waiting_count = 0;
    std::size_t already_prepared_count = 0;
    // CopyPass is a lightweight graph-residency pass, not a heavy shader pass: it registers
    // dynamic copy render targets such as `_rt_default_*_copy` in Scene::renderTargets and binds
    // their TextureCache images. Reused shader passes can legitimately sample those copy targets
    // during the same topology compile, so deferring CopyPass creation lets refreshed passes see a
    // missing input and black out the frame. Prepare copy dependencies up front, then keep the
    // expensive shader/image passes on the deferred residency queue.
    for (size_t pass_index = 0; pass_index < m_passes.size(); ++pass_index) {
        auto* p = m_passes[pass_index];
        if (p == nullptr || p->prepared()) continue;
        if (dynamic_cast<CopyPass*>(p) == nullptr) continue;
        p->prepare(scene, *m_device, m_rendering_resources);
        dependency_prepared_count++;
    }
    for (size_t pass_index = 0; pass_index < m_passes.size(); ++pass_index) {
        auto* p = m_passes[pass_index];
        if (p != nullptr && reused_passes.count(p) != 0 && p->prepared()) {
            p->refreshResources(scene, *m_device, m_rendering_resources);
            reused_refreshed_count++;
        }
        if (refresh_resources_only && p != nullptr && p->prepared()) {
            p->refreshResources(scene, *m_device, m_rendering_resources);
            refreshed_count++;
        }
        if (p != nullptr && !p->prepared()) {
            const bool is_copy_dependency = dynamic_cast<CopyPass*>(p) != nullptr;
            const bool can_defer_runtime_prepare =
                had_resident_graph && !refresh_resources_only && p != m_prepass.get() &&
                p != m_finpass.get() && !is_copy_dependency;
            if (can_defer_runtime_prepare) {
                // Runtime visibility changes should not monopolize the render thread by making
                // every newly-visible layer allocate textures, framebuffers, and staging uploads in
                // one compile call. Queue cold passes and let drawFrame() advance residency a pass
                // at a time while already-prepared scene content keeps rendering.
                if (p->requestDeferredPrepareResources(scene, *m_device) ==
                    DeferredPrepareResourcesState::Waiting) {
                    deferred_waiting_count++;
                }
                m_deferred_prepare_indices.push_back(pass_index);
                deferred_count++;
            } else {
                p->prepare(scene, *m_device, m_rendering_resources);
                prepared_count++;
            }
        } else if (p != nullptr) {
            already_prepared_count++;
        }
    }

    LOG_VERBOSE("RenderGraphCompileSummary: total=%zu reused-refreshed=%zu refreshed=%zu "
                "prepared=%zu dependency-prepared=%zu deferred=%zu already-prepared=%zu mode=%s",
                m_passes.size(),
                reused_refreshed_count,
                refreshed_count,
                prepared_count,
                dependency_prepared_count,
                deferred_count,
                already_prepared_count,
                refresh_resources_only ? "resources" : "topology");
    if (deferred_count > 0) {
        m_device->tex_cache().BeginDeferredGraphActivation();
        LOG_VERBOSE("RenderGraphDeferredPrepareQueued: count=%zu max-passes-per-frame=%zu "
                    "frame-budget=%.2fms resource-waiting=%zu",
                    deferred_count,
                    kDeferredPrepareMaxPassesPerFrame,
                    kDeferredPrepareFrameBudgetMs,
                    deferred_waiting_count);
    }

    // Upload work queued by prepare() is recorded at the start of the next frame command buffer.
    // Avoiding a compile-time queue submit + DeviceWaitIdle is what keeps visibility-driven graph
    // changes from behaving like a scene load.
    m_pass_loaded = true;
};

void VulkanRender::Impl::warmupRenderGraphPipelines(Scene& scene, rg::RenderGraph& rg) {
    if (m_device_faulted) return;
    if (!m_inited || !m_device || !m_rendering_resources.pipeline_cache) return;

    const auto started_at = std::chrono::steady_clock::now();
    auto       nodes      = rg.topologicalOrder();

    setRenderTargetSize(scene, rg);

    std::size_t pipeline_passes = 0;
    std::size_t warmed_passes   = 0;

    for (const auto node_id : nodes) {
        auto pass_ref = rg.getPassShared(node_id);
        auto vpass = std::dynamic_pointer_cast<VulkanPass>(pass_ref);
        if (!vpass) continue;
        pipeline_passes++;
        if (vpass->warmupPipeline(scene, *m_device, m_rendering_resources)) {
            warmed_passes++;
        }
    }

    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
    LOG_VERBOSE("RenderGraphPipelineWarmup: graph-passes=%zu pipeline-passes=%zu warmed=%zu "
                "cached-states=%zu duration=%.2fms",
                nodes.size(),
                pipeline_passes,
                warmed_passes,
                m_rendering_resources.pipeline_cache->size(),
                static_cast<double>(elapsed_us) / 1000.0);
}
