#pragma once

#include "core/NoCopyMove.hpp"
#include "Parameters.hpp"
#include "scene/SceneTexture.h"

#include <memory>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

typedef struct _GstSample GstSample;

namespace wallpaper
{

class Image;
struct SceneTexture;

namespace vulkan
{

class Device;

enum class VideoTextureGpuPipeline {
    Nvidia,
    NvidiaStateless,
    Va,
};

struct VideoTexturePipelineSettings {
    VideoTextureGpuPipeline gpu_pipeline { VideoTextureGpuPipeline::Nvidia };
};

enum class VideoTexturePipelineMode : uint32_t {
    CpuRgba,
    VaMemoryBgra,
    NvidiaCudaNv12,
    NvidiaStatelessCudaNv12,
};

struct VideoTextureStatus {
    VideoTexturePipelineMode pipeline_mode { VideoTexturePipelineMode::CpuRgba };
    bool pipeline_failed { false };
    bool paused { false };
    bool stopped { false };
    bool waiting_for_loop_sample { false };
    bool loop_rebuild_attempted { false };
    bool upload_pending { false };
    uint64_t loop_count { 0 };
    uint64_t uploaded_sample_count { 0 };
};

class VideoTextureCache : NoCopy, NoMove {
public:
    VideoTextureCache(const Device&, VideoTexturePipelineSettings settings = {});
    ~VideoTextureCache();

    // Takes ownership of the parsed video payload when the Image has a unique owner; shared
    // Images are copied so callers that retain a parsed image remain valid.
    ImageSlotsRef Acquire(std::string_view key,
                          const SceneTexture&,
                          std::shared_ptr<Image>,
                          VideoTexturePlaybackState initial_state =
                              VideoTexturePlaybackState::Playing);
    void          ApplyPlaybackStates(const std::unordered_map<std::string, bool>& paused_by_key,
                                      const std::unordered_set<std::string>& stopped_keys);
    void          SetGlobalPaused(bool paused);
    void          ApplySeekRequests(std::unordered_map<std::string, double>& seek_seconds_by_key);
    void          Poll();
    void          RecordUploads(vvk::CommandBuffer&);
    void          Clear();
    bool          Release(std::string_view key);
    std::size_t   GetTrackedBytes() const;
    std::size_t   GetTrackedEntryCount() const;
    std::optional<VideoTextureStatus> GetStatus(std::string_view key) const;

private:
    struct Entry;

    Entry*       find(std::string_view key);
    const Entry* find(std::string_view key) const;
    void         allocateCmd();
    bool         ensureCpuStaging(Entry&);
    bool         startPipelineOnce(Entry&);
    bool         startPipeline(Entry&);
    bool         fallbackPipeline(Entry&, std::string_view reason);
    void         stopPipeline(Entry&);
    bool         restartPipeline(Entry&);
    bool         loopPipeline(Entry&);
    bool         applyPipelinePlaybackState(Entry&);
    bool         setPaused(Entry&, bool paused);
    bool         stopPlayback(Entry&);
    bool         seekTo(Entry&, double seconds);
    bool         uploadSample(Entry&, ::GstSample*);

    const Device& m_device;
    VideoTexturePipelineSettings m_settings;
    vvk::CommandBuffers m_cmds;
    vvk::CommandBuffer  m_cmd;
    std::vector<std::unique_ptr<Entry>> m_entries;
    bool m_globally_paused { false };
};

} // namespace vulkan
} // namespace wallpaper
