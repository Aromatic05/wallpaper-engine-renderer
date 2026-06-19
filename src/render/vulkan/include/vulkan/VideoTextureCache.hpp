#pragma once

#include "Parameters.hpp"
#include "core/NoCopyMove.hpp"
#include "scene/SceneTexture.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wallpaper
{

class Image;

enum class VideoTexturePlaybackState {
    Playing,
    Paused,
    Stopped,
};

namespace vulkan
{

class Device;

enum class VideoTextureGpuPipeline {
    Cpu,
};

struct VideoTexturePipelineSettings {
    VideoTextureGpuPipeline gpu_pipeline { VideoTextureGpuPipeline::Cpu };
};

class VideoTextureCache : NoCopy, NoMove {
public:
    explicit VideoTextureCache(const Device&, VideoTexturePipelineSettings settings = {});
    ~VideoTextureCache();

    ImageSlotsRef Acquire(std::string_view key,
                          const SceneTexture&,
                          const Image&,
                          VideoTexturePlaybackState initial_state =
                              VideoTexturePlaybackState::Playing);

    void ApplyPlaybackStates(const std::unordered_map<std::string, bool>& paused_by_key,
                             const std::unordered_set<std::string>& stopped_keys);
    void SetGlobalPaused(bool paused);
    void ApplySeekRequests(std::unordered_map<std::string, double>& seek_seconds_by_key);
    void Poll();
    void RecordUploads(vvk::CommandBuffer&);
    void Clear();
    bool Release(std::string_view key);
    std::size_t GetTrackedBytes() const;
    std::size_t GetTrackedEntryCount() const;
    std::size_t GetPendingUploadCount() const;
    std::size_t GetRecordedUploadCount() const;
    std::size_t GetAppliedSeekCount() const;
    double GetPlaybackSeconds(std::string_view key) const;
    bool HasPipelineDiagnostic(std::string_view key) const;

private:
    struct Entry {
        std::string                 key;
        ImageSlotsRef               image;
        VideoTexturePlaybackState   playback_state { VideoTexturePlaybackState::Playing };
        double                      seek_seconds { 0.0 };
        double                      playback_seconds { 0.0 };
        std::size_t                 tracked_bytes { 0 };
        std::size_t                 pending_uploads { 0 };
        std::size_t                 recorded_uploads { 0 };
        std::size_t                 applied_seeks { 0 };
        bool                        seek_pending { false };
        bool                        pipeline_diagnostic_emitted { false };
    };

    Entry* find(std::string_view key);
    const Entry* find(std::string_view key) const;

    const Device& m_device;
    VideoTexturePipelineSettings m_settings;
    std::vector<std::unique_ptr<Entry>> m_entries;
    bool m_globally_paused { false };
};

} // namespace vulkan
} // namespace wallpaper
