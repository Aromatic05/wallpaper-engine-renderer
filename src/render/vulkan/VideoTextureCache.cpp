#include "VideoTextureCache.hpp"

#include "Device.hpp"
#include "scene/Image.hpp"
#include "scene/SceneTexture.h"
#include "TextureCache.hpp"
#include "vulkan/Util.hpp"
#include "utils/Logging.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <drm_fourcc.h>
#include <gst/cuda/gstcudaloader.h>
#if HANABI_HAS_CUDA_INTEROP
#include <gst/cuda/cuda-gst.h>
#endif
#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <gst/video/video-info-dma.h>
#include <gst/video/video.h>
#include <va/va_drmcommon.h>
#include <unistd.h>

using namespace wallpaper;
using namespace wallpaper::vulkan;

extern "C" {
typedef struct _GstCudaMemory GstCudaMemory;
typedef struct _GstCudaContext GstCudaContext;
typedef struct _GstCudaStream GstCudaStream;
}

namespace
{

#if HANABI_HAS_CUDA_INTEROP
constexpr GstMapFlags kGstMapReadCuda =
    static_cast<GstMapFlags>(GST_MAP_READ | (GST_MAP_FLAG_LAST << 1));
#endif

#if HANABI_HAS_CUDA_INTEROP
// Keep the NVIDIA path independent from libnvrtc: several distro nvcodec builds can
// decode CUDA memory without shipping the runtime compiler.  The driver can JIT PTX
// directly, so this embedded kernel avoids silently falling back to the CPU pipeline.
constexpr char kNv12ToRgbaCudaPtx[] = R"ptx(
.version 6.0
.target sm_52
.address_size 64

.visible .entry HanabiNv12ToRgba(
    .param .u64 y_plane_param,
    .param .u64 uv_plane_param,
    .param .u64 rgba_param,
    .param .u32 width_param,
    .param .u32 height_param,
    .param .u32 y_pitch_param,
    .param .u32 uv_pitch_param,
    .param .u32 rgba_pitch_param
)
{
    .reg .pred %p<4>;
    .reg .b32 %r<64>;
    .reg .b64 %rd<16>;

    ld.param.u64 %rd1, [y_plane_param];
    ld.param.u64 %rd2, [uv_plane_param];
    ld.param.u64 %rd3, [rgba_param];
    ld.param.u32 %r1, [width_param];
    ld.param.u32 %r2, [height_param];
    ld.param.u32 %r3, [y_pitch_param];
    ld.param.u32 %r4, [uv_pitch_param];
    ld.param.u32 %r5, [rgba_pitch_param];

    mov.u32 %r10, %ctaid.x;
    mov.u32 %r11, %ntid.x;
    mov.u32 %r12, %tid.x;
    mad.lo.u32 %r20, %r10, %r11, %r12;

    mov.u32 %r13, %ctaid.y;
    mov.u32 %r14, %ntid.y;
    mov.u32 %r15, %tid.y;
    mad.lo.u32 %r21, %r13, %r14, %r15;

    setp.ge.u32 %p1, %r20, %r1;
    @%p1 bra DONE;
    setp.ge.u32 %p2, %r21, %r2;
    @%p2 bra DONE;

    mad.lo.u32 %r30, %r21, %r3, %r20;
    cvt.u64.u32 %rd10, %r30;
    add.u64 %rd10, %rd1, %rd10;
    ld.global.u8 %r31, [%rd10];

    shr.u32 %r32, %r21, 1;
    and.b32 %r33, %r20, -2;
    mad.lo.u32 %r34, %r32, %r4, %r33;
    cvt.u64.u32 %rd11, %r34;
    add.u64 %rd11, %rd2, %rd11;
    ld.global.u8 %r35, [%rd11];
    add.u64 %rd12, %rd11, 1;
    ld.global.u8 %r36, [%rd12];

    sub.s32 %r37, %r31, 16;
    max.s32 %r37, %r37, 0;
    sub.s32 %r38, %r35, 128;
    sub.s32 %r39, %r36, 128;

    mul.lo.s32 %r40, %r37, 298;

    mul.lo.s32 %r41, %r39, 459;
    add.s32 %r41, %r41, %r40;
    add.s32 %r41, %r41, 128;
    shr.s32 %r41, %r41, 8;
    max.s32 %r41, %r41, 0;
    min.s32 %r41, %r41, 255;

    mul.lo.s32 %r42, %r38, 55;
    sub.s32 %r42, %r40, %r42;
    mul.lo.s32 %r43, %r39, 136;
    sub.s32 %r42, %r42, %r43;
    add.s32 %r42, %r42, 128;
    shr.s32 %r42, %r42, 8;
    max.s32 %r42, %r42, 0;
    min.s32 %r42, %r42, 255;

    mul.lo.s32 %r44, %r38, 541;
    add.s32 %r44, %r44, %r40;
    add.s32 %r44, %r44, 128;
    shr.s32 %r44, %r44, 8;
    max.s32 %r44, %r44, 0;
    min.s32 %r44, %r44, 255;

    shl.b32 %r50, %r20, 2;
    mad.lo.u32 %r51, %r21, %r5, %r50;
    cvt.u64.u32 %rd13, %r51;
    add.u64 %rd13, %rd3, %rd13;
    st.global.u8 [%rd13], %r41;
    add.u64 %rd14, %rd13, 1;
    st.global.u8 [%rd14], %r42;
    add.u64 %rd14, %rd13, 2;
    st.global.u8 [%rd14], %r44;
    add.u64 %rd14, %rd13, 3;
    mov.u32 %r52, 255;
    st.global.u8 [%rd14], %r52;

DONE:
    ret;
}
)ptx";
#endif

#if HANABI_HAS_CUDA_INTEROP
const char* CudaErrorName(CUresult result) {
    const char* name = nullptr;
    if (CuGetErrorName(result, &name) == CUDA_SUCCESS && name != nullptr) return name;
    return "unknown";
}
#endif


struct DecoderSettings {
    VideoTextureGpuPipeline gpu_pipeline { VideoTextureGpuPipeline::Nvidia };
};

const char* PipelinePolicyName(VideoTextureGpuPipeline policy) {
    switch (policy) {
    case VideoTextureGpuPipeline::Va: return "va";
    case VideoTextureGpuPipeline::NvidiaStateless: return "nvidia-stateless";
    case VideoTextureGpuPipeline::Nvidia: return "nvidia";
    }
    return "nvidia";
}

DecoderSettings MakeDecoderSettings(VideoTexturePipelineSettings runtime_settings) {
    DecoderSettings settings {
        .gpu_pipeline = runtime_settings.gpu_pipeline,
    };
    LOG_VERBOSE("VideoTextureSettings: resolved-pipeline=%s",
                PipelinePolicyName(settings.gpu_pipeline));
    return settings;
}


bool HasElementFactory(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (factory == nullptr) return false;
    gst_object_unref(factory);
    return true;
}


enum class VideoPipelineMode
{
    CpuRgba,
    CpuBgra,
    VaMemoryBgra,
    NvidiaCudaNv12,
    NvidiaStatelessCudaNv12,
};

VideoTexturePipelineMode ToPublicPipelineMode(VideoPipelineMode mode) {
    switch (mode) {
    case VideoPipelineMode::CpuRgba:
    case VideoPipelineMode::CpuBgra: return VideoTexturePipelineMode::CpuRgba;
    case VideoPipelineMode::VaMemoryBgra: return VideoTexturePipelineMode::VaMemoryBgra;
    case VideoPipelineMode::NvidiaCudaNv12: return VideoTexturePipelineMode::NvidiaCudaNv12;
    case VideoPipelineMode::NvidiaStatelessCudaNv12:
        return VideoTexturePipelineMode::NvidiaStatelessCudaNv12;
    }
    return VideoTexturePipelineMode::CpuRgba;
}

#if HANABI_HAS_CUDA_INTEROP
bool IsNvidiaCudaMode(VideoPipelineMode mode) {
    return mode == VideoPipelineMode::NvidiaCudaNv12 ||
           mode == VideoPipelineMode::NvidiaStatelessCudaNv12;
}
#endif

struct VideoPipelineConfig {
    VideoPipelineMode mode { VideoPipelineMode::CpuRgba };
    const char* name { "cpu-rgba" };
    std::string sink_caps { "video/x-raw,format=(string)RGBA" };
    std::string description;
};

std::string AddVideoSizeToCaps(std::string caps, uint32_t width, uint32_t height) {
    if (width > 0 && height > 0) {
        caps += ",width=(int)";
        caps += std::to_string(width);
        caps += ",height=(int)";
        caps += std::to_string(height);
    }
    return caps;
}

bool SupportsVaDmabufImport(const Device& device) {
    return device.supportExt(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
           device.supportExt(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
           device.supportExt(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
}

std::string BuildVaDirectDmabufCaps(const Device& device) {
    if (! SupportsVaDmabufImport(device)) return {};

    // The destination video image for the VA path is BGRA, so direct DMA-BUF input must expose
    // the byte-compatible DRM ARGB8888 layout. Offer every single-plane modifier that Vulkan can
    // consume as a transfer source; GStreamer/VA then intersects this list with the modifiers the
    // decoder and post-processor can actually allocate on the selected DRM device.
    const auto modifiers = device.gpu().GetDrmFormatModifierProperties2(VK_FORMAT_B8G8R8A8_UNORM);
    std::vector<std::string> drm_formats;
    for (const auto& modifier : modifiers) {
        if (modifier.drmFormatModifierPlaneCount != 1 ||
            (modifier.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0) {
            continue;
        }
        gchar* text =
            gst_video_dma_drm_fourcc_to_string(DRM_FORMAT_ARGB8888, modifier.drmFormatModifier);
        if (text == nullptr) continue;
        drm_formats.emplace_back(text);
        g_free(text);
    }
    if (drm_formats.empty()) return {};

    std::string caps =
        "video/x-raw(memory:DMABuf),format=(string)DMA_DRM,drm-format=(string)";
    if (drm_formats.size() == 1) {
        caps += drm_formats.front();
        return caps;
    }

    caps += "{ ";
    for (std::size_t i = 0; i < drm_formats.size(); ++i) {
        if (i != 0) caps += ", ";
        caps += drm_formats[i];
    }
    caps += " }";
    return caps;
}

VideoPipelineConfig BuildVideoOnlyPipelineConfig(VideoPipelineMode mode,
                                                 uint32_t            width = 0,
                                                 uint32_t            height = 0,
                                                 std::string_view    direct_va_caps = {}) {
    if (mode == VideoPipelineMode::VaMemoryBgra) {
        const bool direct_dmabuf = ! direct_va_caps.empty();
        const auto caps = AddVideoSizeToCaps(
            direct_dmabuf ? std::string(direct_va_caps)
                          : "video/x-raw(memory:VAMemory),format=(string)BGRA",
            width,
            height);
        return VideoPipelineConfig {
            .mode = mode,
            .name = direct_dmabuf ? "va-dmabuf-bgra-vulkan-image"
                                  : "va-vamemory-bgra-vulkan-image",
            .sink_caps = caps,
            .description =
                "giostreamsrc name=src "
                "! qtdemux name=demux "
                "demux.video_0 ! queue "
                "! h264parse "
                "! vah264dec "
                // The modern GStreamer vapostproc scales when downstream caps request a size.
                // Do not use vaapipostproc-only properties such as scale-method/sharpen/skin-tone.
                "! vapostproc "
                "! " + caps +
                " ! appsink name=sink sync=true max-buffers=1 drop=true enable-last-sample=false",
        };
    }

    if (mode == VideoPipelineMode::NvidiaCudaNv12) {
        return VideoPipelineConfig {
            .mode = mode,
            .name = "nvidia-cuda-nv12-vulkan-buffer",
            .sink_caps = "video/x-raw(memory:CUDAMemory),format=(string)NV12",
            .description =
                "giostreamsrc name=src "
                "! qtdemux name=demux "
                "demux.video_0 ! queue "
                "! h264parse "
                "! nvh264dec "
                "! video/x-raw(memory:CUDAMemory),format=(string)NV12 "
                "! appsink name=sink sync=true max-buffers=1 drop=true enable-last-sample=false",
        };
    }

    if (mode == VideoPipelineMode::NvidiaStatelessCudaNv12) {
        return VideoPipelineConfig {
            .mode = mode,
            .name = "nvidia-stateless-cuda-nv12-vulkan-buffer",
            .sink_caps = "video/x-raw(memory:CUDAMemory),format=(string)NV12",
            .description =
                "giostreamsrc name=src "
                "! qtdemux name=demux "
                "demux.video_0 ! queue "
                "! h264parse "
                "! nvh264sldec "
                "! video/x-raw(memory:CUDAMemory),format=(string)NV12 "
                "! appsink name=sink sync=true max-buffers=1 drop=true enable-last-sample=false",
        };
    }

    const bool bgra = mode == VideoPipelineMode::CpuBgra;
    const auto caps = AddVideoSizeToCaps(
        bgra ? "video/x-raw,format=(string)BGRA" : "video/x-raw,format=(string)RGBA",
        width,
        height);
    return VideoPipelineConfig {
        .mode = mode,
        .name = bgra ? "cpu-bgra" : "cpu-rgba",
        .sink_caps = caps,
        .description =
            "giostreamsrc name=src "
            "! qtdemux name=demux "
            "demux.video_0 ! queue "
            "! h264parse "
            // MP4 carries AVC length-prefixed NAL units, while common software fallbacks such as
            // openh264dec only advertise byte-stream input. Normalize before decodebin so a system
            // without gst-libav can still use its installed H.264 decoder.
            "! video/x-h264,stream-format=(string)byte-stream,alignment=(string)au "
            "! decodebin "
            "! videoconvert "
            "! videoscale "
            "! " + caps +
            " ! appsink name=sink sync=true max-buffers=1 drop=true enable-last-sample=false",
    };
}

VideoPipelineMode SelectVideoPipelineMode(const Device& device, const DecoderSettings& settings) {
    const bool has_cuda_loader =
#if HANABI_HAS_CUDA_INTEROP
        gst_cuda_load_library();
#else
        false;
#endif

    // These GPU paths name their decoder explicitly. Selection must therefore be local to this
    // cache instead of mutating process-global GStreamer feature ranks, which would also change
    // decoder choice for the standalone video backend and any other GStreamer user in-process.
    if (settings.gpu_pipeline == VideoTextureGpuPipeline::Va && HasElementFactory("vah264dec") &&
        HasElementFactory("vapostproc") && SupportsVaDmabufImport(device)) {
        LOG_VERBOSE("VideoTexturePipelineSelect: selected VA VAMemory BGRA path "
                    "decoder=vah264dec resolved-pipeline=%s",
                    PipelinePolicyName(settings.gpu_pipeline));
        return VideoPipelineMode::VaMemoryBgra;
    }

    if (settings.gpu_pipeline == VideoTextureGpuPipeline::NvidiaStateless && has_cuda_loader &&
        HasElementFactory("nvh264sldec")) {
        LOG_VERBOSE("VideoTexturePipelineSelect: selected NVIDIA stateless CUDA NV12 path "
                    "decoder=nvh264sldec resolved-pipeline=%s",
                    PipelinePolicyName(settings.gpu_pipeline));
        return VideoPipelineMode::NvidiaStatelessCudaNv12;
    }

    if ((settings.gpu_pipeline == VideoTextureGpuPipeline::Nvidia ||
         settings.gpu_pipeline == VideoTextureGpuPipeline::NvidiaStateless) &&
        has_cuda_loader && HasElementFactory("nvh264dec")) {
        LOG_VERBOSE("VideoTexturePipelineSelect: selected NVIDIA CUDA NV12 path "
                    "decoder=nvh264dec resolved-pipeline=%s",
                    PipelinePolicyName(settings.gpu_pipeline));
        return VideoPipelineMode::NvidiaCudaNv12;
    }

    LOG_VERBOSE("VideoTexturePipelineSelect: selected CPU RGBA fallback resolved-pipeline=%s",
                PipelinePolicyName(settings.gpu_pipeline));
    return VideoPipelineMode::CpuRgba;
}

VkFormat TargetImageFormatForPipelineMode(VideoPipelineMode mode) {
    return mode == VideoPipelineMode::VaMemoryBgra || mode == VideoPipelineMode::CpuBgra
        ? VK_FORMAT_B8G8R8A8_UNORM
        : VK_FORMAT_R8G8B8A8_UNORM;
}

std::pair<uint32_t, uint32_t> FitVideoSizeToOutput(const Device& device,
                                                    uint32_t       width,
                                                    uint32_t       height) {
    const auto output = device.out_extent();
    if (width == 0 || height == 0 || output.width == 0 || output.height == 0 ||
        (width <= output.width && height <= output.height)) {
        return { width, height };
    }

    const auto output_width_limited = static_cast<uint64_t>(output.width) * height;
    const auto output_height_limited = static_cast<uint64_t>(output.height) * width;
    if (output_width_limited <= output_height_limited) {
        const auto fitted_height = static_cast<uint32_t>(
            std::max<uint64_t>(1, (output_width_limited + width / 2u) / width));
        return { output.width, std::min(output.height, fitted_height) };
    }

    const auto fitted_width = static_cast<uint32_t>(
        std::max<uint64_t>(1, (output_height_limited + height / 2u) / height));
    return { std::min(output.width, fitted_width), output.height };
}

long long ElapsedMillis(std::chrono::steady_clock::time_point start,
                        std::chrono::steady_clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

std::optional<VmaImageParameters>
CreateVideoImage(const Device& device, uint32_t width, uint32_t height, const TextureSample& sample,
                 VkFormat format, VkImageUsageFlags usage) {
    VmaImageParameters image;
    do {
        VkSamplerCreateInfo sampler_info {
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext                   = nullptr,
            .magFilter               = ToVkType(sample.magFilter),
            .minFilter               = ToVkType(sample.minFilter),
            .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU            = ToVkType(sample.wrapS),
            .addressModeV            = ToVkType(sample.wrapT),
            .addressModeW            = ToVkType(sample.wrapT),
            .anisotropyEnable        = false,
            .maxAnisotropy           = 1.0f,
            .compareEnable           = false,
            .compareOp               = VK_COMPARE_OP_NEVER,
            .minLod                  = 0.0f,
            .maxLod                  = 1.0f,
            .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            .unnormalizedCoordinates = false,
        };
        VkImageCreateInfo info {
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = format,
            .extent                = VkExtent3D { width, height, 1 },
            .mipLevels             = 1,
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
        vma_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        VVK_CHECK_ACT(break, vvk::CreateImage(device.vma_allocator(), info, vma_info, image.handle));

        VkImageViewCreateInfo view_info {
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
        VVK_CHECK_ACT(break, device.handle().CreateImageView(view_info, image.view));
        VVK_CHECK_ACT(break, device.handle().CreateSampler(sampler_info, image.sampler));
        return image;
    } while (false);
    return std::nullopt;
}

VkResult TransitionImageLayout(const vvk::Queue& queue, vvk::CommandBuffer& cmd,
                               const ImageParameters& image, VkImageLayout old_layout,
                               VkImageLayout new_layout) {
    VkResult result;
    do {
        result = cmd.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        });
        if (result != VK_SUCCESS) break;

        VkImageMemoryBarrier barrier {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .oldLayout        = old_layout,
            .newLayout        = new_layout,
            .image            = image.handle,
            .subresourceRange =
                VkImageSubresourceRange {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel   = 0,
                    .levelCount     = 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
        };
        barrier.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags {}
            : VkAccessFlags(VK_ACCESS_MEMORY_READ_BIT);
        barrier.dstAccessMask = new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            ? VkAccessFlags(VK_ACCESS_SHADER_READ_BIT)
            : VkAccessFlags(VK_ACCESS_TRANSFER_WRITE_BIT);
        cmd.PipelineBarrier(old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                    : VK_PIPELINE_STAGE_TRANSFER_BIT,
                            new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                : VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        result = cmd.End();
        if (result != VK_SUCCESS) break;

        VkSubmitInfo submit {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = cmd.address(),
        };
        result = queue.Submit(submit);
    } while (false);
    return result;
}

std::optional<uint32_t> FindMemoryType(const vvk::PhysicalDevice& gpu,
                                       uint32_t memory_type_bits,
                                       VkMemoryPropertyFlags preferred_flags) {
    const auto memory_properties = gpu.GetMemoryProperties().memoryProperties;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((memory_type_bits & (1u << i)) == 0) continue;
        if ((memory_properties.memoryTypes[i].propertyFlags & preferred_flags) == preferred_flags) {
            return i;
        }
    }
    return std::nullopt;
}

#if HANABI_HAS_CUDA_INTEROP
struct GstCudaMemoryPrefix {
    GstMemory mem;
    GstCudaContext* context;
    GstVideoInfo info;
};

GstCudaContext* GetCudaMemoryContext(GstMemory* memory) {
    if (memory == nullptr || ! gst_is_cuda_memory(memory)) return nullptr;
    return reinterpret_cast<GstCudaMemoryPrefix*>(memory)->context;
}

struct CudaExternalBuffer {
    VkDevice device { VK_NULL_HANDLE };
    const vvk::DeviceDispatch* dispatch { nullptr };
    VkBuffer buffer { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkDeviceSize size { 0 };
    int fd { -1 };
    CUexternalMemory cuda_memory {};
    CUdeviceptr cuda_ptr {};
    GstCudaContext* cuda_context { nullptr };

    CudaExternalBuffer() = default;
    CudaExternalBuffer(const CudaExternalBuffer&) = delete;
    CudaExternalBuffer& operator=(const CudaExternalBuffer&) = delete;

    CudaExternalBuffer(CudaExternalBuffer&& other) noexcept {
        *this = std::move(other);
    }

    CudaExternalBuffer& operator=(CudaExternalBuffer&& other) noexcept {
        if (this == &other) return *this;
        reset();
        device = std::exchange(other.device, VK_NULL_HANDLE);
        dispatch = std::exchange(other.dispatch, nullptr);
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        memory = std::exchange(other.memory, VK_NULL_HANDLE);
        size = std::exchange(other.size, 0);
        fd = std::exchange(other.fd, -1);
        cuda_memory = std::exchange(other.cuda_memory, nullptr);
        cuda_ptr = std::exchange(other.cuda_ptr, 0);
        cuda_context = std::exchange(other.cuda_context, nullptr);
        return *this;
    }

    ~CudaExternalBuffer() { reset(); }

    explicit operator bool() const { return buffer != VK_NULL_HANDLE && cuda_ptr != 0; }

    void reset() {
        if (cuda_memory != nullptr) {
            bool pushed = false;
            if (cuda_context != nullptr) pushed = gst_cuda_context_push(cuda_context);
            (void)CuDestroyExternalMemory(cuda_memory);
            if (pushed) {
                CUcontext popped {};
                (void)gst_cuda_context_pop(&popped);
            }
            cuda_memory = nullptr;
            cuda_ptr = 0;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        if (dispatch != nullptr && device != VK_NULL_HANDLE) {
            if (buffer != VK_NULL_HANDLE) {
                dispatch->vkDestroyBuffer(device, buffer, nullptr);
                buffer = VK_NULL_HANDLE;
            }
            if (memory != VK_NULL_HANDLE) {
                dispatch->vkFreeMemory(device, memory, nullptr);
                memory = VK_NULL_HANDLE;
            }
        }
        if (cuda_context != nullptr) {
            gst_object_unref(cuda_context);
            cuda_context = nullptr;
        }
        device = VK_NULL_HANDLE;
        dispatch = nullptr;
        size = 0;
    }
};

std::optional<CudaExternalBuffer> CreateCudaExternalTransferBuffer(const Device& device,
                                                                   VkDeviceSize size,
                                                                   GstCudaContext* cuda_context) {
    if (size == 0 || cuda_context == nullptr) return std::nullopt;
    if (device.handle().Dispatch().vkGetMemoryFdKHR == nullptr) {
        LOG_ERROR("video texture: Vulkan device has no vkGetMemoryFdKHR for CUDA interop");
        return std::nullopt;
    }

    CudaExternalBuffer result;
    result.device = *device.handle();
    result.dispatch = &device.handle().Dispatch();
    result.size = size;
    result.cuda_context = reinterpret_cast<GstCudaContext*>(gst_object_ref(cuda_context));

    VkExternalMemoryBufferCreateInfo external_buffer {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkBufferCreateInfo buffer_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_buffer,
        .flags = 0,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (result.dispatch->vkCreateBuffer(result.device, &buffer_info, nullptr, &result.buffer) != VK_SUCCESS) {
        LOG_ERROR("video texture: create Vulkan external CUDA buffer failed");
        return std::nullopt;
    }

    VkBufferMemoryRequirementsInfo2 req_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = nullptr,
        .buffer = result.buffer,
    };
    VkMemoryRequirements2 reqs {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = nullptr,
    };
    result.dispatch->vkGetBufferMemoryRequirements2(result.device, &req_info, &reqs);

    const auto memory_type =
        FindMemoryType(device.gpu(), reqs.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (! memory_type.has_value()) {
        LOG_ERROR("video texture: no device-local memory type for CUDA interop buffer");
        return std::nullopt;
    }

    VkExportMemoryAllocateInfo export_info {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkMemoryAllocateInfo alloc_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = reqs.memoryRequirements.size,
        .memoryTypeIndex = memory_type.value(),
    };
    if (result.dispatch->vkAllocateMemory(result.device, &alloc_info, nullptr, &result.memory) != VK_SUCCESS ||
        result.dispatch->vkBindBufferMemory(result.device, result.buffer, result.memory, 0) != VK_SUCCESS) {
        LOG_ERROR("video texture: allocate/bind Vulkan external CUDA buffer failed");
        return std::nullopt;
    }

    VkMemoryGetFdInfoKHR fd_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = result.memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    if (result.dispatch->vkGetMemoryFdKHR(result.device, &fd_info, &result.fd) != VK_SUCCESS ||
        result.fd < 0) {
        LOG_ERROR("video texture: export Vulkan buffer fd for CUDA failed");
        return std::nullopt;
    }

    bool pushed = gst_cuda_context_push(cuda_context);
    if (! pushed) {
        LOG_ERROR("video texture: push CUDA context for Vulkan external buffer failed");
        return std::nullopt;
    }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC import_desc {};
    import_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    import_desc.handle.fd = result.fd;
    import_desc.size = static_cast<unsigned long long>(reqs.memoryRequirements.size);
    CUresult cuda_result = CuImportExternalMemory(&result.cuda_memory, &import_desc);
    if (cuda_result == CUDA_SUCCESS) result.fd = -1;

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer_desc {};
    buffer_desc.offset = 0;
    buffer_desc.size = static_cast<unsigned long long>(size);
    if (cuda_result == CUDA_SUCCESS) {
        cuda_result = CuExternalMemoryGetMappedBuffer(&result.cuda_ptr,
                                                      result.cuda_memory,
                                                      &buffer_desc);
    }
    CUcontext popped {};
    (void)gst_cuda_context_pop(&popped);

    if (cuda_result != CUDA_SUCCESS || result.cuda_ptr == 0) {
        LOG_ERROR("video texture: import Vulkan buffer into CUDA failed result=%s",
                  CudaErrorName(cuda_result));
        return std::nullopt;
    }
    return result;
}
#else
struct CudaExternalBuffer {
    VkBuffer buffer { VK_NULL_HANDLE };
    VkDeviceSize size { 0 };
    explicit operator bool() const { return false; }
};
#endif

struct DmabufImportedImage {
    VkDevice device { VK_NULL_HANDLE };
    const vvk::DeviceDispatch* dispatch { nullptr };
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkFormat format { VK_FORMAT_UNDEFINED };
    uint32_t width { 0 };
    uint32_t height { 0 };

    DmabufImportedImage() = default;
    DmabufImportedImage(const DmabufImportedImage&) = delete;
    DmabufImportedImage& operator=(const DmabufImportedImage&) = delete;

    DmabufImportedImage(DmabufImportedImage&& other) noexcept {
        *this = std::move(other);
    }

    DmabufImportedImage& operator=(DmabufImportedImage&& other) noexcept {
        if (this == &other) return *this;
        reset();
        device = std::exchange(other.device, VK_NULL_HANDLE);
        dispatch = std::exchange(other.dispatch, nullptr);
        image = std::exchange(other.image, VK_NULL_HANDLE);
        memory = std::exchange(other.memory, VK_NULL_HANDLE);
        format = std::exchange(other.format, VK_FORMAT_UNDEFINED);
        width = std::exchange(other.width, 0);
        height = std::exchange(other.height, 0);
        return *this;
    }

    ~DmabufImportedImage() { reset(); }

    explicit operator bool() const { return image != VK_NULL_HANDLE; }

    void reset() {
        if (dispatch != nullptr && device != VK_NULL_HANDLE) {
            if (image != VK_NULL_HANDLE) {
                dispatch->vkDestroyImage(device, image, nullptr);
                image = VK_NULL_HANDLE;
            }
            if (memory != VK_NULL_HANDLE) {
                dispatch->vkFreeMemory(device, memory, nullptr);
                memory = VK_NULL_HANDLE;
            }
        }
        device = VK_NULL_HANDLE;
        dispatch = nullptr;
        format = VK_FORMAT_UNDEFINED;
        width = 0;
        height = 0;
    }
};

std::optional<off_t> QueryFdSize(int fd) {
    if (fd < 0) return std::nullopt;
    const off_t current = ::lseek(fd, 0, SEEK_CUR);
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (current >= 0) (void)::lseek(fd, current, SEEK_SET);
    if (end <= 0) return std::nullopt;
    return end;
}

bool ParseVaRgbaDmabufCaps(GstCaps* caps,
                           GstVideoInfoDmaDrm& drm_info,
                           VkFormat& vk_format,
                           uint64_t& drm_modifier) {
    if (caps == nullptr || ! gst_video_is_dma_drm_caps(caps)) return false;
    gst_video_info_dma_drm_init(&drm_info);
    if (! gst_video_info_dma_drm_from_caps(&drm_info, caps)) return false;

    // Accept either common little-endian 32-bit RGBA layout. The Vulkan format must describe
    // the bytes exactly because vkCmdCopyImage does not perform a channel conversion.
    if (drm_info.drm_fourcc == DRM_FORMAT_ABGR8888)
        vk_format = VK_FORMAT_R8G8B8A8_UNORM;
    else if (drm_info.drm_fourcc == DRM_FORMAT_ARGB8888)
        vk_format = VK_FORMAT_B8G8R8A8_UNORM;
    else
        return false;
    drm_modifier = drm_info.drm_modifier == DRM_FORMAT_MOD_INVALID
        ? static_cast<uint64_t>(DRM_FORMAT_MOD_LINEAR)
        : drm_info.drm_modifier;
    return true;
}

std::string DrmFourccToString(uint32_t fourcc) {
    char text[5] {
        static_cast<char>(fourcc & 0xff),
        static_cast<char>((fourcc >> 8) & 0xff),
        static_cast<char>((fourcc >> 16) & 0xff),
        static_cast<char>((fourcc >> 24) & 0xff),
        0,
    };
    return text;
}

std::optional<VkFormat> VkFormatForRgbaDrmFourcc(uint32_t drm_format) {
    if (drm_format == DRM_FORMAT_ABGR8888) return VK_FORMAT_R8G8B8A8_UNORM;
    if (drm_format == DRM_FORMAT_ARGB8888) return VK_FORMAT_B8G8R8A8_UNORM;
    return std::nullopt;
}

std::optional<DmabufImportedImage> ImportSinglePlaneDrmRgbaImage(const Device& device,
                                                                 uint32_t width,
                                                                 uint32_t height,
                                                                 VkFormat vk_format,
                                                                 uint64_t drm_modifier,
                                                                 int fd,
                                                                 uint32_t object_size,
                                                                 uint32_t plane_offset,
                                                                 uint32_t plane_stride) {
    if (width == 0 || height == 0 || fd < 0 || plane_stride == 0) return std::nullopt;
    if (drm_modifier == DRM_FORMAT_MOD_INVALID) drm_modifier = DRM_FORMAT_MOD_LINEAR;

    DmabufImportedImage result;
    result.device = *device.handle();
    result.dispatch = &device.handle().Dispatch();
    result.format = vk_format;
    result.width = width;
    result.height = height;

    VkExternalMemoryImageCreateInfo external_image {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkSubresourceLayout plane_layout {
        .offset = static_cast<VkDeviceSize>(plane_offset),
        .size = static_cast<VkDeviceSize>(plane_stride) * height,
        .rowPitch = static_cast<VkDeviceSize>(plane_stride),
        .arrayPitch = 0,
        .depthPitch = 0,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = &external_image,
        .drmFormatModifier = drm_modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout,
    };
    VkImageCreateInfo image_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format,
        .extent = VkExtent3D { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (result.dispatch->vkCreateImage(result.device, &image_info, nullptr, &result.image) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkMemoryRequirements reqs {};
    result.dispatch->vkGetImageMemoryRequirements(result.device, result.image, &reqs);
    const auto memory_type = FindMemoryType(device.gpu(), reqs.memoryTypeBits, {});
    if (! memory_type.has_value()) {
        LOG_ERROR("video texture: no Vulkan memory type accepts VA DMABuf import");
        return std::nullopt;
    }

    const int import_fd = ::dup(fd);
    if (import_fd < 0) {
        LOG_ERROR("video texture: dup VA DMABuf fd failed errno=%d message=%s",
                  errno,
                  std::strerror(errno));
        return std::nullopt;
    }

    VkMemoryDedicatedAllocateInfo dedicated_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = result.image,
        .buffer = VK_NULL_HANDLE,
    };
    VkImportMemoryFdInfoKHR import_info {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = import_fd,
    };
    VkMemoryAllocateInfo alloc_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = std::max(reqs.size, static_cast<VkDeviceSize>(object_size)),
        .memoryTypeIndex = memory_type.value(),
    };
    if (result.dispatch->vkAllocateMemory(result.device, &alloc_info, nullptr, &result.memory) !=
        VK_SUCCESS) {
        ::close(import_fd);
        LOG_ERROR("video texture: import VA DMABuf into Vulkan memory failed");
        return std::nullopt;
    }
    if (result.dispatch->vkBindImageMemory(result.device, result.image, result.memory, 0) !=
        VK_SUCCESS) {
        LOG_ERROR("video texture: bind imported VA DMABuf image memory failed");
        return std::nullopt;
    }

    return result;
}

gboolean AppsinkProposeVideoMetaAllocation(GstAppSink*, GstQuery* query, gpointer) {
    if (query == nullptr || GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) return FALSE;
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
    return TRUE;
}

std::optional<DmabufImportedImage> ImportVaDmabufRgbaImage(const Device& device,
                                                           GstCaps* caps,
                                                           GstBuffer* buffer) {
    if (caps == nullptr || buffer == nullptr || gst_buffer_n_memory(buffer) != 1) {
        return std::nullopt;
    }
    GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
    if (memory == nullptr || ! gst_is_dmabuf_memory(memory)) {
        return std::nullopt;
    }

    GstVideoInfoDmaDrm drm_info;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    uint64_t drm_modifier = DRM_FORMAT_MOD_INVALID;
    if (! ParseVaRgbaDmabufCaps(caps, drm_info, vk_format, drm_modifier)) {
        return std::nullopt;
    }

    gsize memory_offset = 0;
    gsize memory_size = 0;
    gsize max_memory_size = gst_memory_get_sizes(memory, &memory_offset, &memory_size);
    const int fd = gst_dmabuf_memory_get_fd(memory);
    if (fd < 0 || max_memory_size == 0) return std::nullopt;

    GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
    const gsize plane_offset = meta != nullptr && meta->n_planes > 0
        ? static_cast<gsize>(meta->offset[0])
        : static_cast<gsize>(GST_VIDEO_INFO_PLANE_OFFSET(&drm_info.vinfo, 0));
    const gint plane_stride = meta != nullptr && meta->n_planes > 0
        ? meta->stride[0]
        : GST_VIDEO_INFO_PLANE_STRIDE(&drm_info.vinfo, 0);
    if (plane_stride <= 0) return std::nullopt;
    const gint width = GST_VIDEO_INFO_WIDTH(&drm_info.vinfo);
    const gint height = GST_VIDEO_INFO_HEIGHT(&drm_info.vinfo);
    if (width <= 0 || height <= 0) return std::nullopt;

    return ImportSinglePlaneDrmRgbaImage(device,
                                         static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(height),
                                         vk_format,
                                         drm_modifier,
                                         fd,
                                         static_cast<uint32_t>(
                                             QueryFdSize(fd).value_or(static_cast<off_t>(max_memory_size))),
                                         static_cast<uint32_t>(memory_offset + plane_offset),
                                         static_cast<uint32_t>(plane_stride));
}

void CloseVaDescriptorFds(VADRMPRIMESurfaceDescriptor& descriptor) {
    for (uint32_t i = 0; i < descriptor.num_objects && i < 4; ++i) {
        if (descriptor.objects[i].fd >= 0) {
            ::close(descriptor.objects[i].fd);
            descriptor.objects[i].fd = -1;
        }
    }
}

std::optional<DmabufImportedImage> ExportVaMemoryRgbaImage(const Device& device,
                                                           GstCaps* caps,
                                                           GstBuffer* buffer) {
    if (caps == nullptr || buffer == nullptr || gst_buffer_n_memory(buffer) == 0) {
        return std::nullopt;
    }

    GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
    if (memory == nullptr || ! gst_memory_is_type(memory, GST_ALLOCATOR_VASURFACE)) {
        return std::nullopt;
    }

    GstVideoInfo info;
    if (! gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA) {
        return std::nullopt;
    }

    GstVaDisplay* display = gst_va_memory_peek_display(memory);
    VASurfaceID surface = gst_va_memory_get_surface(memory);
    if (display == nullptr) display = gst_va_buffer_peek_display(buffer);
    if (surface == VA_INVALID_SURFACE) surface = gst_va_buffer_get_surface(buffer);
    if (display == nullptr || surface == VA_INVALID_SURFACE) return std::nullopt;

    auto* va_display = reinterpret_cast<VADisplay>(gst_va_display_get_va_dpy(display));
    if (va_display == nullptr) return std::nullopt;

    VAStatus status = vaSyncSurface(va_display, surface);
    if (status != VA_STATUS_SUCCESS) {
        LOG_ERROR("video texture: vaSyncSurface failed status=%s", vaErrorStr(status));
        return std::nullopt;
    }

    VADRMPRIMESurfaceDescriptor descriptor {};
    for (auto& object : descriptor.objects) object.fd = -1;
    // GStreamer can decode and post-process into VAMemory on drivers where
    // directly negotiating memory:DMABuf fails. Exporting the ready VA surface
    // here keeps the frame on the GPU while giving Vulkan the dma-buf metadata
    // it needs for the existing image-copy path.
    status = vaExportSurfaceHandle(va_display,
                                   surface,
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &descriptor);
    if (status != VA_STATUS_SUCCESS) {
        LOG_ERROR("video texture: vaExportSurfaceHandle failed status=%s", vaErrorStr(status));
        return std::nullopt;
    }

    std::optional<DmabufImportedImage> imported;
    if (descriptor.num_layers == 1 && descriptor.layers[0].num_planes == 1 &&
        descriptor.num_objects > 0 && descriptor.layers[0].object_index[0] < descriptor.num_objects) {
        const auto& layer = descriptor.layers[0];
        const auto& object = descriptor.objects[layer.object_index[0]];
        if (auto vk_format = VkFormatForRgbaDrmFourcc(layer.drm_format); vk_format.has_value()) {
            imported = ImportSinglePlaneDrmRgbaImage(device,
                                                     descriptor.width,
                                                     descriptor.height,
                                                     vk_format.value(),
                                                     object.drm_format_modifier,
                                                     object.fd,
                                                     object.size,
                                                     layer.offset[0],
                                                     layer.pitch[0]);
        }
    }

    if (! imported.has_value()) {
        LOG_ERROR("video texture: unsupported VA exported surface fourcc=%s layers=%u objects=%u "
                  "layer0-format=%s layer0-planes=%u",
                  DrmFourccToString(descriptor.fourcc).c_str(),
                  descriptor.num_layers,
                  descriptor.num_objects,
                  descriptor.num_layers > 0
                      ? DrmFourccToString(descriptor.layers[0].drm_format).c_str()
                      : "none",
                  descriptor.num_layers > 0 ? descriptor.layers[0].num_planes : 0);
    }

    CloseVaDescriptorFds(descriptor);
    return imported;
}

} // namespace

struct VideoTextureCache::Entry {
    std::string key;
    ImageDataPtr encoded_storage;
    const uint8_t* encoded_data { nullptr };
    std::size_t encoded_size { 0 };
    VmaImageParameters image;
    VmaBufferParameters staging;
    CudaExternalBuffer cuda_rgba_buffer;
    DmabufImportedImage va_imported_image;
    uint8_t* staging_mapped { nullptr };
    uint32_t width { 0 };
    uint32_t height { 0 };
    bool     dirty { false };
    bool     cuda_rgba_dirty { false };
    bool     va_dmabuf_dirty { false };
    std::string va_direct_caps;
    bool     warned_size_mismatch { false };
    bool     warned_unexpected_format { false };
    bool     warned_cuda_interop_failed { false };
    bool     warned_va_dmabuf_import_failed { false };
    bool     pipeline_failed { false };
    VideoPipelineMode pipeline_mode { VideoPipelineMode::CpuRgba };
    bool     paused { false };
    bool     stopped { false };
    bool     eos_loop_waiting_for_sample { false };
    bool     eos_loop_rebuild_attempted { false };
    uint64_t eos_loop_count { 0 };
    uint64_t uploaded_sample_count { 0 };
#if HANABI_HAS_CUDA_INTEROP
    CUmodule cuda_module {};
    CUfunction cuda_kernel {};
#endif

    GstElement* pipeline { nullptr };
    GstElement* source_elem { nullptr };
    GstElement* appsink_elem { nullptr };
    GstBus*     bus { nullptr };
    GInputStream* memory_stream { nullptr };

    ~Entry() {
#if HANABI_HAS_CUDA_INTEROP
        if (cuda_module != nullptr) {
            bool pushed = false;
            if (cuda_rgba_buffer.cuda_context != nullptr) {
                pushed = gst_cuda_context_push(cuda_rgba_buffer.cuda_context);
            }
            (void)CuModuleUnload(cuda_module);
            if (pushed) {
                CUcontext popped {};
                (void)gst_cuda_context_pop(&popped);
            }
            cuda_module = nullptr;
            cuda_kernel = nullptr;
        }
#endif
        if (staging_mapped != nullptr) staging.handle.UnMapMemory();
        if (pipeline != nullptr) (void)gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus != nullptr) gst_object_unref(bus);
        if (appsink_elem != nullptr) gst_object_unref(appsink_elem);
        if (source_elem != nullptr) gst_object_unref(source_elem);
        if (pipeline != nullptr) gst_object_unref(pipeline);
        if (memory_stream != nullptr) g_object_unref(memory_stream);
    }
};

VideoTextureCache::VideoTextureCache(const Device& device, VideoTexturePipelineSettings settings)
    : m_device(device), m_settings(settings) {
    if (! gst_is_initialized()) gst_init(nullptr, nullptr);
}

VideoTextureCache::~VideoTextureCache() = default;

VideoTextureCache::Entry* VideoTextureCache::find(std::string_view key) {
    const auto it =
        std::find_if(m_entries.begin(), m_entries.end(), [key](const auto& entry) {
            return entry != nullptr && entry->key == key;
        });
    return it == m_entries.end() ? nullptr : it->get();
}

const VideoTextureCache::Entry* VideoTextureCache::find(std::string_view key) const {
    const auto it =
        std::find_if(m_entries.begin(), m_entries.end(), [key](const auto& entry) {
            return entry != nullptr && entry->key == key;
        });
    return it == m_entries.end() ? nullptr : it->get();
}

void VideoTextureCache::allocateCmd() {
    const auto& pool = m_device.cmd_pool();
    VVK_CHECK(pool.Allocate(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_cmds));
    m_cmd = vvk::CommandBuffer(m_cmds[0], m_device.handle().Dispatch());
}

void VideoTextureCache::stopPipeline(Entry& entry) {
    if (entry.pipeline != nullptr) (void)gst_element_set_state(entry.pipeline, GST_STATE_NULL);
    if (entry.bus != nullptr) gst_object_unref(entry.bus);
    if (entry.appsink_elem != nullptr) gst_object_unref(entry.appsink_elem);
    if (entry.source_elem != nullptr) gst_object_unref(entry.source_elem);
    if (entry.pipeline != nullptr) gst_object_unref(entry.pipeline);
    if (entry.memory_stream != nullptr) g_object_unref(entry.memory_stream);
    entry.bus = nullptr;
    entry.appsink_elem = nullptr;
    entry.source_elem = nullptr;
    entry.pipeline = nullptr;
    entry.memory_stream = nullptr;
}

bool VideoTextureCache::ensureCpuStaging(Entry& entry) {
    if (! entry.staging.handle) {
        if (! CreateStagingBuffer(m_device.vma_allocator(),
                                  static_cast<size_t>(entry.width) * entry.height * 4u,
                                  entry.staging)) {
            LOG_ERROR("create video staging buffer for '%s' failed", entry.key.c_str());
            return false;
        }
    }
    if (entry.staging_mapped == nullptr) {
        void* mapped = nullptr;
        if (entry.staging.handle.MapMemory(&mapped) != VK_SUCCESS || mapped == nullptr) {
            LOG_ERROR("map video staging buffer for '%s' failed", entry.key.c_str());
            return false;
        }
        entry.staging_mapped = static_cast<uint8_t*>(mapped);
        std::memset(entry.staging_mapped, 0, entry.staging.req_size);
        (void)vmaFlushAllocation(m_device.vma_allocator(),
                                 entry.staging.handle.Allocation(),
                                 0,
                                 entry.staging.req_size);
    }
    return true;
}

bool VideoTextureCache::fallbackPipeline(Entry& entry, std::string_view reason) {
    if (entry.pipeline_mode != VideoPipelineMode::VaMemoryBgra) return false;

    entry.va_imported_image.reset();
    entry.va_dmabuf_dirty = false;
    entry.warned_va_dmabuf_import_failed = false;
    stopPipeline(entry);

    if (! entry.va_direct_caps.empty()) {
        LOG_INFO("video texture '%s': direct VA DMA-BUF failed (%.*s); falling back to VAMemory",
                 entry.key.c_str(),
                 static_cast<int>(reason.size()),
                 reason.data());
        entry.va_direct_caps.clear();
        return true;
    }

    LOG_INFO("video texture '%s': VA path failed (%.*s); falling back to CPU BGRA",
             entry.key.c_str(),
             static_cast<int>(reason.size()),
             reason.data());
    entry.pipeline_mode = VideoPipelineMode::CpuBgra;
    return ensureCpuStaging(entry);
}

bool VideoTextureCache::startPipelineOnce(Entry& entry) {
    stopPipeline(entry);
    entry.eos_loop_waiting_for_sample = false;
    entry.eos_loop_rebuild_attempted = false;
    entry.pipeline_failed = false;

    GError* error = nullptr;
    const auto pipeline_config = BuildVideoOnlyPipelineConfig(entry.pipeline_mode,
                                                              entry.width,
                                                              entry.height,
                                                              entry.va_direct_caps);
    entry.pipeline = gst_parse_launch(pipeline_config.description.c_str(), &error);
    if (entry.pipeline == nullptr) {
        LOG_ERROR("create video pipeline for '%s' failed: %s",
                  entry.key.c_str(),
                  error != nullptr ? error->message : "unknown error");
        if (error != nullptr) g_error_free(error);
        return false;
    }
    if (error != nullptr) {
        LOG_ERROR("create video pipeline for '%s' failed: %s", entry.key.c_str(), error->message);
        g_error_free(error);
        stopPipeline(entry);
        return false;
    }

    entry.memory_stream =
        G_INPUT_STREAM(g_memory_input_stream_new_from_data(entry.encoded_data,
                                                           static_cast<gssize>(entry.encoded_size),
                                                           nullptr));
    if (entry.memory_stream == nullptr) {
        LOG_ERROR("create video memory stream for '%s' failed", entry.key.c_str());
        stopPipeline(entry);
        return false;
    }

    entry.source_elem = gst_bin_get_by_name(GST_BIN(entry.pipeline), "src");
    entry.appsink_elem = gst_bin_get_by_name(GST_BIN(entry.pipeline), "sink");
    entry.bus = gst_element_get_bus(entry.pipeline);
    if (entry.source_elem == nullptr || entry.appsink_elem == nullptr || entry.bus == nullptr) {
        LOG_ERROR("video texture '%s' pipeline is missing giostreamsrc/appsink", entry.key.c_str());
        stopPipeline(entry);
        return false;
    }

    g_object_set(entry.source_elem, "stream", entry.memory_stream, nullptr);
    if (entry.pipeline_mode == VideoPipelineMode::VaMemoryBgra && ! entry.va_direct_caps.empty()) {
        // GstVaPostProc requires downstream to advertise GstVideoMeta before it will allocate
        // DMA-BUF output. Use the callback API so appsink does not emit a signal for every frame.
        GstAppSinkCallbacks callbacks {};
        callbacks.propose_allocation = AppsinkProposeVideoMetaAllocation;
        gst_app_sink_set_callbacks(GST_APP_SINK(entry.appsink_elem), &callbacks, nullptr, nullptr);
    }
    LOG_VERBOSE("VideoTexturePipeline key='%s' backend='%s' source=giostreamsrc bytes=%zu desc='%s'",
                entry.key.c_str(),
                pipeline_config.name,
                entry.encoded_size,
                pipeline_config.description.c_str());

    GstCaps* sink_caps = gst_caps_from_string(pipeline_config.sink_caps.c_str());
    if (sink_caps == nullptr) {
        LOG_ERROR("video texture '%s': invalid appsink caps '%s'",
                  entry.key.c_str(),
                  pipeline_config.sink_caps.c_str());
        stopPipeline(entry);
        return false;
    }
    g_object_set(entry.appsink_elem, "caps", sink_caps, nullptr);
    gst_caps_unref(sink_caps);

    if (gst_element_set_state(entry.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("start video pipeline preroll for '%s' failed", entry.key.c_str());
        stopPipeline(entry);
        return false;
    }
    (void)gst_element_get_state(entry.pipeline, nullptr, nullptr, GST_SECOND);
    if (entry.appsink_elem != nullptr) {
        GstSample* preroll = gst_app_sink_try_pull_preroll(GST_APP_SINK(entry.appsink_elem), GST_SECOND);
        if (preroll != nullptr) {
            const bool uploaded = uploadSample(entry, preroll);
            gst_sample_unref(preroll);
            if (! uploaded) {
                LOG_ERROR("video texture '%s': preroll upload failed", entry.key.c_str());
                stopPipeline(entry);
                return false;
            }
        }
    }

    const auto target_state = m_globally_paused ? GST_STATE_PAUSED : GST_STATE_PLAYING;
    if (gst_element_set_state(entry.pipeline, target_state) == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("start video pipeline playback for '%s' failed", entry.key.c_str());
        stopPipeline(entry);
        return false;
    }
    entry.paused = false;
    entry.stopped = false;
    return true;
}

bool VideoTextureCache::startPipeline(Entry& entry) {
    while (! startPipelineOnce(entry)) {
        if (! fallbackPipeline(entry, "startup or preroll failure")) return false;
    }
    return true;
}

bool VideoTextureCache::restartPipeline(Entry& entry) {
    const bool was_paused = entry.paused;
    if (!startPipeline(entry)) {
        LOG_ERROR("video texture '%s': failed to rebuild pipeline for loop", entry.key.c_str());
        entry.pipeline_failed = true;
        return false;
    }
    if (was_paused) setPaused(entry, true);
    return true;
}

bool VideoTextureCache::loopPipeline(Entry& entry) {
    if (entry.pipeline == nullptr || entry.stopped) return false;

    if (entry.eos_loop_waiting_for_sample) {
        if (entry.eos_loop_rebuild_attempted) {
            LOG_ERROR("video texture '%s': EOS loop seek still produced no sample after rebuild",
                      entry.key.c_str());
            entry.pipeline_failed = true;
            return false;
        }

        // If EOS arrives again before a decoded frame proves that the flush seek worked, keep the
        // render resource stable but rebuild the decoder graph once. Repeating this indefinitely
        // would hide the real failure behind an EOS storm and could stall future texture updates.
        LOG_ERROR("video texture '%s': EOS loop seek produced no sample; restarting pipeline once",
                  entry.key.c_str());
        const uint64_t uploaded_before_restart = entry.uploaded_sample_count;
        if (!restartPipeline(entry)) return false;
        if (entry.uploaded_sample_count == uploaded_before_restart) {
            entry.eos_loop_waiting_for_sample = true;
            entry.eos_loop_rebuild_attempted = true;
        }
        return true;
    }

    entry.eos_loop_count++;
    const auto start = std::chrono::steady_clock::now();
    if (gst_element_set_state(entry.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("video texture '%s': failed to pause pipeline for EOS loop seek",
                  entry.key.c_str());
        entry.pipeline_failed = true;
        return false;
    }
    const auto paused = std::chrono::steady_clock::now();

    // Looping by seek keeps decoder state, negotiated caps, appsink ownership, and Vulkan texture
    // bindings stable. Rebuilding here can force shader/resource refresh at the exact frame where
    // a wallpaper loop is most noticeable.
    const gboolean seek_ok = gst_element_seek(
        entry.pipeline,
        1.0,
        GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        GST_SEEK_TYPE_SET,
        0,
        GST_SEEK_TYPE_NONE,
        -1);
    const auto seeked = std::chrono::steady_clock::now();
    if (! seek_ok) {
        LOG_ERROR("VideoTexturePerf eos-loop-seek-failed key='%s' loop=%llu paused_ms=%lld seek_ms=%lld",
                  entry.key.c_str(),
                  static_cast<unsigned long long>(entry.eos_loop_count),
                  ElapsedMillis(start, paused),
                  ElapsedMillis(paused, seeked));
        return restartPipeline(entry);
    }

    entry.eos_loop_waiting_for_sample = ! entry.paused && ! m_globally_paused;
    const auto target_state =
        (entry.paused || m_globally_paused) ? GST_STATE_PAUSED : GST_STATE_PLAYING;
    if (gst_element_set_state(entry.pipeline, target_state) == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("video texture '%s': failed to resume pipeline after EOS loop seek",
                  entry.key.c_str());
        entry.pipeline_failed = true;
        return false;
    }
    const auto playing = std::chrono::steady_clock::now();
    LOG_INFO("VideoTexturePerf eos-loop-seek key='%s' loop=%llu paused_ms=%lld seek_ms=%lld play_ms=%lld",
             entry.key.c_str(),
             static_cast<unsigned long long>(entry.eos_loop_count),
             ElapsedMillis(start, paused),
             ElapsedMillis(paused, seeked),
             ElapsedMillis(seeked, playing));
    return true;
}

bool VideoTextureCache::applyPipelinePlaybackState(Entry& entry) {
    if (entry.pipeline == nullptr || entry.stopped) return true;

    const auto target_state =
        (entry.paused || m_globally_paused) ? GST_STATE_PAUSED : GST_STATE_PLAYING;
    if (gst_element_set_state(entry.pipeline, target_state) == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("video texture '%s': failed to switch playback state paused=%s global_paused=%s",
                  entry.key.c_str(),
                  entry.paused ? "true" : "false",
                  m_globally_paused ? "true" : "false");
        entry.pipeline_failed = true;
        return false;
    }
    return true;
}

bool VideoTextureCache::setPaused(Entry& entry, bool paused) {
    if (! paused && entry.pipeline == nullptr) {
        return startPipeline(entry);
    }
    if (entry.paused == paused) return true;
    if (entry.pipeline == nullptr) {
        entry.paused = paused;
        entry.stopped = false;
        return true;
    }

    entry.paused = paused;
    entry.stopped = false;
    return applyPipelinePlaybackState(entry);
}

bool VideoTextureCache::stopPlayback(Entry& entry) {
    if (entry.stopped && entry.pipeline == nullptr) return true;

    // stop() is intentionally stronger than pause(): authored intro videos call it after their
    // fade-out and expect decoder, colorspace conversion, appsink polling, and Vulkan upload work
    // to end. Keep the already-created Vulkan image alive for descriptor stability, but release the
    // GStreamer graph so future frames are not decoded until play() rebuilds the pipeline.
    stopPipeline(entry);
    entry.paused = true;
    entry.stopped = true;
    entry.pipeline_failed = false;
    entry.eos_loop_waiting_for_sample = false;
    entry.eos_loop_rebuild_attempted = false;
    return true;
}

bool VideoTextureCache::seekTo(Entry& entry, double seconds) {
    if (entry.pipeline == nullptr) return false;

    const double clamped_seconds = std::max(0.0, seconds);
    const auto   target_time =
        static_cast<gint64>(clamped_seconds * static_cast<double>(GST_SECOND));

    // SceneScript setCurrentTime() is a visible decoder command: use a flushing accurate seek so
    // stale frames already queued in appsink are discarded before the next render poll observes the
    // texture. This keeps one-shot intro videos deterministic when scripts stop, rewind, then wait
    // for a later play() call.
    if (! gst_element_seek_simple(
            entry.pipeline,
            GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            target_time)) {
        LOG_ERROR("video texture '%s': failed to seek to %.3fs",
                  entry.key.c_str(),
                  clamped_seconds);
        return false;
    }

    if (entry.appsink_elem != nullptr) {
        while (GstSample* stale =
                   gst_app_sink_try_pull_sample(GST_APP_SINK(entry.appsink_elem), 0)) {
            gst_sample_unref(stale);
        }
    }

    entry.eos_loop_waiting_for_sample = false;
    entry.eos_loop_rebuild_attempted = false;
    LOG_VERBOSE("video texture '%s': seek accepted seconds=%.3f", entry.key.c_str(), clamped_seconds);
    return true;
}

bool VideoTextureCache::uploadSample(VideoTextureCache::Entry& entry, ::GstSample* sample) {
    if (sample == nullptr) return false;

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    if (caps == nullptr || buffer == nullptr || ! gst_video_info_from_caps(&info, caps)) {
        return false;
    }

    if (entry.pipeline_mode == VideoPipelineMode::VaMemoryBgra) {
        auto imported = ImportVaDmabufRgbaImage(m_device, caps, buffer);
        if (! imported.has_value()) imported = ExportVaMemoryRgbaImage(m_device, caps, buffer);
        if (imported.has_value()) {
            const auto copy_width = std::min<uint32_t>(entry.width, imported->width);
            const auto copy_height = std::min<uint32_t>(entry.height, imported->height);
            if ((copy_width != entry.width || copy_height != entry.height) &&
                ! entry.warned_size_mismatch) {
                LOG_INFO("video texture '%s' VA decoded size %ux%u differs from texture size %ux%u",
                         entry.key.c_str(),
                         imported->width,
                         imported->height,
                         entry.width,
                         entry.height);
                entry.warned_size_mismatch = true;
            }
            entry.va_imported_image = std::move(imported.value());
            entry.va_dmabuf_dirty = true;
            entry.uploaded_sample_count++;
            entry.eos_loop_waiting_for_sample = false;
            entry.eos_loop_rebuild_attempted = false;
            return true;
        }

        if (! entry.warned_va_dmabuf_import_failed) {
            gchar* caps_text = gst_caps_to_string(caps);
            LOG_ERROR("video texture '%s': VA surface export/import failed caps='%s' memories=%u",
                      entry.key.c_str(),
                      caps_text != nullptr ? caps_text : "unknown",
                      gst_buffer_n_memory(buffer));
            if (caps_text != nullptr) g_free(caps_text);
            entry.warned_va_dmabuf_import_failed = true;
        }
        return false;
    }

#if HANABI_HAS_CUDA_INTEROP
    if (IsNvidiaCudaMode(entry.pipeline_mode) &&
        GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_NV12 &&
        gst_buffer_n_memory(buffer) > 0 &&
        gst_is_cuda_memory(gst_buffer_peek_memory(buffer, 0))) {
        GstVideoFrame frame;
        if (! gst_video_frame_map(&frame, &info, buffer, kGstMapReadCuda)) {
            if (! entry.warned_cuda_interop_failed) {
                LOG_ERROR("video texture '%s': CUDA frame map failed", entry.key.c_str());
                entry.warned_cuda_interop_failed = true;
            }
            return false;
        }

        GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
        auto* cuda_context = GetCudaMemoryContext(memory);
        if (cuda_context == nullptr) {
            gst_video_frame_unmap(&frame);
            LOG_ERROR("video texture '%s': CUDA memory has no context", entry.key.c_str());
            return false;
        }

        const auto copy_width =
            std::min<uint32_t>(entry.width, static_cast<uint32_t>(GST_VIDEO_INFO_WIDTH(&info)));
        const auto copy_height =
            std::min<uint32_t>(entry.height, static_cast<uint32_t>(GST_VIDEO_INFO_HEIGHT(&info)));
        const VkDeviceSize rgba_size = static_cast<VkDeviceSize>(entry.width) * entry.height * 4u;
        if (! entry.cuda_rgba_buffer) {
            if (auto cuda_buffer =
                    CreateCudaExternalTransferBuffer(m_device, rgba_size, cuda_context);
                cuda_buffer.has_value()) {
                entry.cuda_rgba_buffer = std::move(cuda_buffer.value());
                LOG_INFO("video texture '%s': created NVIDIA CUDA/Vulkan shared RGBA buffer bytes=%llu",
                         entry.key.c_str(),
                         static_cast<unsigned long long>(rgba_size));
            } else {
                gst_video_frame_unmap(&frame);
                entry.warned_cuda_interop_failed = true;
                return false;
            }
        }

        if (entry.cuda_kernel == nullptr) {
            bool pushed = gst_cuda_context_push(cuda_context);
            CUresult cuda_result = pushed
                ? CuModuleLoadData(&entry.cuda_module, kNv12ToRgbaCudaPtx)
                : CUDA_ERROR_UNKNOWN;
            if (cuda_result == CUDA_SUCCESS) {
                cuda_result = CuModuleGetFunction(&entry.cuda_kernel,
                                                  entry.cuda_module,
                                                  "HanabiNv12ToRgba");
            }
            if (pushed) {
                CUcontext popped {};
                (void)gst_cuda_context_pop(&popped);
            }

            if (cuda_result != CUDA_SUCCESS || entry.cuda_kernel == nullptr) {
                gst_video_frame_unmap(&frame);
                LOG_ERROR("video texture '%s': load embedded CUDA NV12 converter failed result=%s",
                          entry.key.c_str(),
                          CudaErrorName(cuda_result));
                return false;
            }
        }

        auto* cuda_memory = reinterpret_cast<GstCudaMemory*>(memory);
        GstCudaStream* gst_stream = gst_cuda_memory_get_stream(cuda_memory);
        CUstream stream = gst_stream != nullptr ? gst_cuda_stream_get_handle(gst_stream) : nullptr;
        CUdeviceptr y_ptr =
            reinterpret_cast<CUdeviceptr>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        CUdeviceptr uv_ptr =
            reinterpret_cast<CUdeviceptr>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
        CUdeviceptr rgba_ptr = entry.cuda_rgba_buffer.cuda_ptr;
        int width = static_cast<int>(copy_width);
        int height = static_cast<int>(copy_height);
        int y_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        int uv_pitch = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
        int rgba_pitch = static_cast<int>(entry.width) * 4;
        void* params[] {
            &y_ptr,
            &uv_ptr,
            &rgba_ptr,
            &width,
            &height,
            &y_pitch,
            &uv_pitch,
            &rgba_pitch,
        };

        bool pushed = gst_cuda_context_push(cuda_context);
        CUresult cuda_result = pushed
            ? CuLaunchKernel(entry.cuda_kernel,
                             (copy_width + 15u) / 16u,
                             (copy_height + 15u) / 16u,
                             1,
                             16,
                             16,
                             1,
                             0,
                             stream,
                             params,
                             nullptr)
            : CUDA_ERROR_UNKNOWN;
        if (cuda_result == CUDA_SUCCESS) {
            cuda_result = stream != nullptr ? CuStreamSynchronize(stream) : CuCtxSynchronize();
        }
        if (pushed) {
            CUcontext popped {};
            (void)gst_cuda_context_pop(&popped);
        }
        gst_video_frame_unmap(&frame);

        if (cuda_result != CUDA_SUCCESS) {
            LOG_ERROR("video texture '%s': CUDA NV12->RGBA launch failed result=%s",
                      entry.key.c_str(),
                      CudaErrorName(cuda_result));
            return false;
        }

        entry.cuda_rgba_dirty = true;
        entry.uploaded_sample_count++;
        entry.eos_loop_waiting_for_sample = false;
        entry.eos_loop_rebuild_attempted = false;
        return true;
    }
#endif

    GstVideoFrame frame;
    if (! gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        return false;
    }

    const auto copy_width = std::min<uint32_t>(entry.width, static_cast<uint32_t>(GST_VIDEO_INFO_WIDTH(&info)));
    const auto copy_height =
        std::min<uint32_t>(entry.height, static_cast<uint32_t>(GST_VIDEO_INFO_HEIGHT(&info)));
    if ((copy_width != entry.width || copy_height != entry.height) && ! entry.warned_size_mismatch) {
        LOG_INFO("video texture '%s' decoded size %ux%u differs from texture size %ux%u",
                 entry.key.c_str(),
                 GST_VIDEO_INFO_WIDTH(&info),
                 GST_VIDEO_INFO_HEIGHT(&info),
                 entry.width,
                 entry.height);
        entry.warned_size_mismatch = true;
    }

    const auto expected_cpu_format = entry.pipeline_mode == VideoPipelineMode::CpuBgra
        ? GST_VIDEO_FORMAT_BGRA
        : GST_VIDEO_FORMAT_RGBA;
    if (GST_VIDEO_INFO_FORMAT(&info) != expected_cpu_format) {
        if (! entry.warned_unexpected_format) {
            LOG_ERROR("video texture '%s' negotiated unexpected format %d; expected %d",
                      entry.key.c_str(),
                      GST_VIDEO_INFO_FORMAT(&info),
                      expected_cpu_format);
            entry.warned_unexpected_format = true;
        }
        gst_video_frame_unmap(&frame);
        return false;
    }

    uint8_t* mapped = entry.staging_mapped;
    if (mapped == nullptr) {
        gst_video_frame_unmap(&frame);
        return false;
    }

    {
        auto* dst = mapped;
        const auto* src_base = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        const auto src_stride = static_cast<size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
        const auto dst_stride = static_cast<size_t>(entry.width) * 4u;
        const auto row_bytes = static_cast<size_t>(copy_width) * 4u;
        for (uint32_t y = 0; y < copy_height; y++) {
            std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                        src_base + static_cast<size_t>(y) * src_stride,
                        row_bytes);
            if (copy_width < entry.width) {
                std::memset(dst + static_cast<size_t>(y) * dst_stride + row_bytes,
                            0,
                            dst_stride - row_bytes);
            }
        }
        if (copy_height < entry.height) {
            std::memset(dst + static_cast<size_t>(copy_height) * dst_stride,
                        0,
                        (static_cast<size_t>(entry.height) - copy_height) * dst_stride);
        }
        (void)vmaFlushAllocation(m_device.vma_allocator(),
                                 entry.staging.handle.Allocation(),
                                 0,
                                 entry.staging.req_size);
        entry.dirty = true;
    }

    gst_video_frame_unmap(&frame);
    entry.uploaded_sample_count++;
    if (entry.eos_loop_waiting_for_sample) {
        LOG_INFO("VideoTexturePerf eos-loop-sample key='%s' loop=%llu uploaded=%llu",
                 entry.key.c_str(),
                 static_cast<unsigned long long>(entry.eos_loop_count),
                 static_cast<unsigned long long>(entry.uploaded_sample_count));
    }
    entry.eos_loop_waiting_for_sample = false;
    entry.eos_loop_rebuild_attempted = false;
    return true;
}

ImageSlotsRef VideoTextureCache::Acquire(std::string_view key,
                                         const SceneTexture& texture,
                                         std::shared_ptr<Image> image,
                                         VideoTexturePlaybackState initial_state) {
    if (auto* entry = find(key)) {
        if (initial_state == VideoTexturePlaybackState::Stopped)
            stopPlayback(*entry);
        else
            setPaused(*entry, initial_state == VideoTexturePlaybackState::Paused);
        ImageSlotsRef ref;
        ref.slots.push_back(ImageParameters(entry->image));
        return ref;
    }

    if (image == nullptr || image->slots.empty() || image->slots[0].mipmaps.empty() ||
        image->slots[0].mipmaps[0].data == nullptr) {
        LOG_ERROR("video texture '%.*s' is missing embedded payload",
                  static_cast<int>(key.size()),
                  key.data());
        return {};
    }

    auto entry = std::make_unique<Entry>();
    entry->key.assign(key);
    const auto& encoded = image->slots[0].mipmaps[0];
    entry->encoded_size = static_cast<std::size_t>(encoded.size);
    // CustomShaderPass transfers the parser result when it is the sole owner. If another owner
    // exists, preserve its Image contents and use a private copy instead.
    if (image.use_count() == 1) {
        entry->encoded_storage = std::move(image->slots[0].mipmaps[0].data);
    } else {
        entry->encoded_storage = ImageDataPtr(new uint8_t[entry->encoded_size], [](uint8_t* data) {
            delete[] data;
        });
        std::copy(encoded.data.get(),
                  encoded.data.get() + entry->encoded_size,
                  entry->encoded_storage.get());
    }
    entry->encoded_data = entry->encoded_storage.get();

    entry->width = static_cast<uint32_t>(std::max(
        1, texture.mapWidth > 0 ? texture.mapWidth
                                : (texture.width > 0 ? texture.width : image->slots[0].width)));
    entry->height = static_cast<uint32_t>(std::max(
        1, texture.mapHeight > 0 ? texture.mapHeight
                                 : (texture.height > 0 ? texture.height : image->slots[0].height)));
    entry->pipeline_mode = SelectVideoPipelineMode(m_device, MakeDecoderSettings(m_settings));
    // Keep direct DMA-BUF opt-in on Intel for now, but negotiate the modifier instead of assuming
    // one Intel tiling layout. Other VA devices retain the portable VAMemory export path.
    constexpr uint32_t kIntelVendorId = 0x8086;
    if (entry->pipeline_mode == VideoPipelineMode::VaMemoryBgra &&
        m_device.gpu().GetProperties().vendorID == kIntelVendorId) {
        entry->va_direct_caps = BuildVaDirectDmabufCaps(m_device);
        if (entry->va_direct_caps.empty()) {
            LOG_INFO("video texture '%s': no Vulkan-compatible direct VA DMA-BUF modifier; using VAMemory",
                     entry->key.c_str());
        }
    }

    if (entry->pipeline_mode == VideoPipelineMode::VaMemoryBgra) {
        const auto authored_width = entry->width;
        const auto authored_height = entry->height;
        const auto fitted_size = FitVideoSizeToOutput(m_device, authored_width, authored_height);
        entry->width = fitted_size.first;
        entry->height = fitted_size.second;
        if (entry->width != authored_width || entry->height != authored_height) {
            LOG_INFO("VideoTextureScale key='%s' source=%ux%u output=%ux%u",
                     entry->key.c_str(),
                     authored_width,
                     authored_height,
                     entry->width,
                     entry->height);
        }
    }

    if (auto opt = CreateVideoImage(m_device,
                                    entry->width,
                                    entry->height,
                                    texture.sample,
                                    TargetImageFormatForPipelineMode(entry->pipeline_mode),
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        opt.has_value()) {
        entry->image = std::move(opt.value());
    } else {
        LOG_ERROR("create video texture image for '%s' failed", entry->key.c_str());
        return {};
    }

    // GPU-native VA/CUDA paths upload from imported external memory and do not need a host
    // staging allocation. CPU modes allocate it lazily, including the VA -> CPU fallback.
    if ((entry->pipeline_mode == VideoPipelineMode::CpuRgba ||
         entry->pipeline_mode == VideoPipelineMode::CpuBgra) &&
        ! ensureCpuStaging(*entry)) {
        return {};
    }

    if (! m_cmd) allocateCmd();
    if (TransitionImageLayout(m_device.graphics_queue().handle,
                              m_cmd,
                              ImageParameters(entry->image),
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) != VK_SUCCESS) {
        LOG_ERROR("transition video texture '%s' image layout failed", entry->key.c_str());
        return {};
    }
    VVK_CHECK(m_device.handle().WaitIdle());

    if (initial_state == VideoTexturePlaybackState::Stopped) {
        entry->paused = true;
        entry->stopped = true;
    } else {
        if (!startPipeline(*entry)) {
            return {};
        }
        if (initial_state == VideoTexturePlaybackState::Paused) setPaused(*entry, true);
    }

    m_entries.emplace_back(std::move(entry));
    ImageSlotsRef ref;
    ref.slots.push_back(ImageParameters(m_entries.back()->image));
    return ref;
}

void VideoTextureCache::ApplyPlaybackStates(
    const std::unordered_map<std::string, bool>& paused_by_key,
    const std::unordered_set<std::string>& stopped_keys) {
    for (auto& entry_ptr : m_entries) {
        if (entry_ptr == nullptr) continue;
        if (stopped_keys.count(entry_ptr->key) != 0) {
            stopPlayback(*entry_ptr);
            continue;
        }
        auto state_it = paused_by_key.find(entry_ptr->key);
        if (state_it == paused_by_key.end()) continue;
        setPaused(*entry_ptr, state_it->second);
    }
}

void VideoTextureCache::SetGlobalPaused(bool paused) {
    if (m_globally_paused == paused) return;
    m_globally_paused = paused;

    // Shell-level pause stops the render timer, but GStreamer keeps decoding on its own threads
    // unless the pipeline is told to pause. Apply this immediately so video-texture wallpapers do
    // not burn CPU while the visible scene is frozen by GJS playback controls.
    for (auto& entry_ptr : m_entries) {
        if (entry_ptr == nullptr) continue;
        (void)applyPipelinePlaybackState(*entry_ptr);
    }
}

void VideoTextureCache::ApplySeekRequests(std::unordered_map<std::string, double>& seek_seconds_by_key) {
    for (auto iter = seek_seconds_by_key.begin(); iter != seek_seconds_by_key.end();) {
        auto* entry = find(iter->first);
        if (entry == nullptr) {
            ++iter;
            continue;
        }

        // A request is erased only after the concrete decoder accepts it. Init scripts can therefore
        // queue setCurrentTime() before CustomShaderPass has materialized the texture, without
        // losing the rewind on the frame where the video cache entry finally appears.
        if (seekTo(*entry, iter->second))
            iter = seek_seconds_by_key.erase(iter);
        else
            ++iter;
    }
}

void VideoTextureCache::Poll() {
    for (auto& entry_ptr : m_entries) {
        auto& entry = *entry_ptr;
        if (entry.pipeline_failed) continue;
        if (entry.stopped || entry.pipeline == nullptr) continue;
        if (m_globally_paused) continue;

        bool should_loop = false;
        bool pipeline_error = false;
        while (entry.bus != nullptr) {
            GstMessage* message = gst_bus_pop(entry.bus);
            if (message == nullptr) break;

            switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ERROR: {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                LOG_ERROR("video texture '%s' pipeline error source=%s domain=%s code=%d "
                          "message=%s debug=%s",
                          entry.key.c_str(),
                          GST_MESSAGE_SRC_NAME(message),
                          error != nullptr ? g_quark_to_string(error->domain) : "unknown",
                          error != nullptr ? error->code : 0,
                          error != nullptr ? error->message : "unknown error",
                          debug != nullptr ? debug : "none");
                if (error != nullptr) g_error_free(error);
                if (debug != nullptr) g_free(debug);
                pipeline_error = true;
                break;
            }
            case GST_MESSAGE_EOS: should_loop = true; break;
            default: break;
            }

            gst_message_unref(message);
        }

        if (pipeline_error) {
            if (entry.pipeline_mode == VideoPipelineMode::VaMemoryBgra &&
                fallbackPipeline(entry, "GStreamer pipeline error") && startPipeline(entry)) {
                continue;
            }
            entry.pipeline_failed = true;
            continue;
        }

        if (should_loop) {
            if (!loopPipeline(entry)) continue;
        }

        if (entry.paused) continue;

        GstSample* latest_sample = nullptr;
        while (entry.appsink_elem != nullptr) {
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(entry.appsink_elem), 0);
            if (sample == nullptr) break;
            if (latest_sample != nullptr) gst_sample_unref(latest_sample);
            latest_sample = sample;
        }
        if (latest_sample == nullptr) continue;

        const bool uploaded = uploadSample(entry, latest_sample);
        gst_sample_unref(latest_sample);
        if (! uploaded && entry.pipeline_mode == VideoPipelineMode::VaMemoryBgra) {
            if (! fallbackPipeline(entry, "frame import/upload failure") || ! startPipeline(entry)) {
                entry.pipeline_failed = true;
            }
        }
    }
}

void VideoTextureCache::RecordUploads(vvk::CommandBuffer& cmd) {
    for (auto& entry_ptr : m_entries) {
        auto& entry = *entry_ptr;
        if (entry.pipeline_failed) continue;

        if (! entry.dirty && ! entry.cuda_rgba_dirty && ! entry.va_dmabuf_dirty) continue;
        const bool use_cuda_rgba = entry.cuda_rgba_dirty && entry.cuda_rgba_buffer.buffer != VK_NULL_HANDLE;
        const bool use_va_dmabuf = entry.va_dmabuf_dirty && entry.va_imported_image.image != VK_NULL_HANDLE;

        if (use_cuda_rgba) {
            VkBufferMemoryBarrier cuda_to_transfer {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = entry.cuda_rgba_buffer.buffer,
                .offset = 0,
                .size = entry.cuda_rgba_buffer.size,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
	                                VK_DEPENDENCY_BY_REGION_BIT,
	                                cuda_to_transfer);
        }
        if (use_va_dmabuf) {
            VkImageMemoryBarrier va_to_transfer {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = entry.va_imported_image.image,
                .subresourceRange =
                    VkImageSubresourceRange {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                va_to_transfer);
        }

        VkImageMemoryBarrier to_transfer {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image            = *entry.image.handle,
            .subresourceRange =
                VkImageSubresourceRange {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel   = 0,
                    .levelCount     = 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            to_transfer);

        if (use_va_dmabuf) {
            VkImageCopy copy {
                .srcSubresource =
                    VkImageSubresourceLayers {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .srcOffset = VkOffset3D { 0, 0, 0 },
                .dstSubresource =
                    VkImageSubresourceLayers {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .dstOffset = VkOffset3D { 0, 0, 0 },
                .extent =
                    VkExtent3D {
                        std::min(entry.width, entry.va_imported_image.width),
                        std::min(entry.height, entry.va_imported_image.height),
                        1,
                    },
            };
            VkImageCopy copy_regions[] { copy };
            cmd.CopyImage(entry.va_imported_image.image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          *entry.image.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          copy_regions);
        } else {
            VkBufferImageCopy copy {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    VkImageSubresourceLayers {
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel       = 0,
                        .baseArrayLayer = 0,
                        .layerCount     = 1,
                    },
                .imageOffset = VkOffset3D { 0, 0, 0 },
                .imageExtent = VkExtent3D { entry.width, entry.height, 1 },
            };
            VkBufferImageCopy copy_regions[] { copy };
            cmd.CopyBufferToImage(use_cuda_rgba ? entry.cuda_rgba_buffer.buffer : *entry.staging.handle,
                                  *entry.image.handle,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  copy_regions);
        }

        VkImageMemoryBarrier to_shader {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = *entry.image.handle,
            .subresourceRange = to_transfer.subresourceRange,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            to_shader);
        entry.dirty = false;
        entry.cuda_rgba_dirty = false;
        entry.va_dmabuf_dirty = false;
    }
}

void VideoTextureCache::Clear() {
    m_entries.clear();
}

bool VideoTextureCache::Release(std::string_view key) {
    const auto iter =
        std::find_if(m_entries.begin(), m_entries.end(), [key](const auto& entry) {
            return entry != nullptr && entry->key == key;
        });
    if (iter == m_entries.end()) return false;

    const std::string key_string(key);
    const auto        before_bytes = GetTrackedBytes();
    const auto        before_count = GetTrackedEntryCount();
    m_entries.erase(iter);
    LOG_VERBOSE("VideoTextureCacheRelease: key='%s' bytes-before=%zu bytes-after=%zu "
                "entries-before=%zu entries-after=%zu",
                key_string.c_str(),
                before_bytes,
                GetTrackedBytes(),
                before_count,
                GetTrackedEntryCount());
    return true;
}

std::size_t VideoTextureCache::GetTrackedBytes() const {
    std::size_t total = 0;
    for (const auto& entry : m_entries) {
        if (! entry) continue;
        if (entry->image.handle) total += static_cast<std::size_t>(entry->image.handle.AllocationSize());
        total += entry->encoded_size;
        if (entry->staging.handle) total += static_cast<std::size_t>(entry->staging.handle.AllocationSize());
        if (entry->cuda_rgba_buffer.buffer != VK_NULL_HANDLE) {
            total += static_cast<std::size_t>(entry->cuda_rgba_buffer.size);
        }
    }
    return total;
}

std::optional<VideoTextureStatus> VideoTextureCache::GetStatus(std::string_view key) const {
    const auto* entry = find(key);
    if (entry == nullptr) return std::nullopt;

    return VideoTextureStatus {
        .pipeline_mode = ToPublicPipelineMode(entry->pipeline_mode),
        .pipeline_failed = entry->pipeline_failed,
        .paused = entry->paused,
        .stopped = entry->stopped,
        .waiting_for_loop_sample = entry->eos_loop_waiting_for_sample,
        .loop_rebuild_attempted = entry->eos_loop_rebuild_attempted,
        .upload_pending = entry->dirty || entry->cuda_rgba_dirty || entry->va_dmabuf_dirty,
        .loop_count = entry->eos_loop_count,
        .uploaded_sample_count = entry->uploaded_sample_count,
    };
}

std::size_t VideoTextureCache::GetTrackedEntryCount() const {
    return m_entries.size();
}
