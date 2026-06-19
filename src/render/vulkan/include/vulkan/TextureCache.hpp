#pragma once

#include "Parameters.hpp"
#include "Type.hpp"
#include "core/NoCopyMove.hpp"
#include "core/MapSet.hpp"
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wallpaper
{

class Image;

namespace vulkan
{

VkFormat             ToVkType(TextureFormat);
VkSamplerAddressMode ToVkType(TextureWrap);
VkFilter             ToVkType(TextureFilter);

enum class TexUsage
{
    COLOR,
    DEPTH
};

using TexHash = std::size_t;

struct TextureCachePendingImageUpload {
    std::string                      key;
    ImageParameters                  image;
    VkImageLayout                    old_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    std::vector<VmaBufferParameters> stage_bufs;
    std::vector<VkExtent3D>          extents;
};

struct TextureCachePendingRenderTargetClear {
    std::string     key;
    ImageParameters image;
};

enum class TextureCacheStreamingState
{
    Ready,
    Waiting,
    Failed,
};

struct TextureKey {
    i32           width;
    i32           height;
    TexUsage      usage;
    TextureFormat format;
    TextureSample sample;
    uint          mipmap_level { 1 };

    static TexHash HashValue(const TextureKey&);
};

class TextureCache : NoCopy, NoMove {
public:
    TextureCache(const Device&);
    ~TextureCache();

    void Clear();
    bool ReleaseTexture(std::string_view key);
    bool ReleaseRenderTarget(std::string_view key);

    std::optional<ExImageParameters> CreateExTex(uint32_t witdh, uint32_t height, VkFormat,
                                                 VkImageTiling, ExternalFrameExportMode,
                                                 uint32_t export_drm_fourcc = 0,
                                                 std::span<const uint64_t> export_drm_modifiers = {});
    ImageSlotsRef                    CreateTex(Image&);
    std::optional<ImageSlotsRef>     FindTex(std::string_view key) const;
    // Continue an already-started streaming upload using the TextureCache-owned parsed image. This
    // keeps Scene free to drop its CPU-side image cache as soon as ownership has crossed into
    // Vulkan residency, avoiding a second long-lived copy of large decoded texture payloads.
    TextureCacheStreamingState       StagePendingTexUploads(std::string_view key,
                                                            std::size_t byte_budget);
    TextureCacheStreamingState       StageTexUploads(std::shared_ptr<Image> image,
                                                     std::optional<usize> priority_slot,
                                                     std::size_t byte_budget);

    std::optional<ImageParameters> Query(std::string_view key, TextureKey content_hash,
                                         bool persist = false);

    void RecordUploads(vvk::CommandBuffer&);
    void RetireCompletedUploads();
    void MarkShareReady(std::string_view key);
    void BeginDeferredGraphActivation();
    void EndDeferredGraphActivation();
    void CancelDeferredGraphActivation();
    std::size_t GetTrackedBytes() const;
    std::size_t GetTrackedImageCount() const;

    void RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const;

private:
    std::optional<VmaImageParameters> CreateTex(TextureKey);
    void                              allocateCmd();
    void                              purgeQueuedWorkForKey(std::string_view key);
    vvk::CommandBuffers               m_tex_cmds;
    vvk::CommandBuffer                m_tex_cmd;

    const Device&                m_device;
    Map<std::string, ImageSlots> m_tex_map;
    Map<std::string, uint64_t>   m_tex_revision_map;

    struct StreamingTexUpload {
        std::shared_ptr<Image> image;
        std::deque<usize>      remaining_slots;
        VkImageLayout          old_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    };
    Map<std::string, StreamingTexUpload> m_streaming_tex_uploads;

    struct QueryTex {
        idx                index { 0 };
        bool               share_ready { false };
        bool               persist { false };
        TexHash            content_hash;
        VmaImageParameters image;
        Set<std::string>   query_keys;
    };
    std::vector<std::unique_ptr<QueryTex>> m_query_texs;
    Map<std::string, QueryTex*>            m_query_map;
    std::vector<TextureCachePendingImageUpload>       m_pending_image_uploads;
    std::vector<TextureCachePendingImageUpload>       m_inflight_image_uploads;
    std::vector<TextureCachePendingRenderTargetClear> m_pending_render_target_clears;
    std::size_t                                      m_deferred_graph_activation_depth { 0 };
    Set<std::string>                                 m_deferred_share_ready_keys;
};

} // namespace vulkan
} // namespace wallpaper
