#include "VideoTextureCache.hpp"

#include "Device.hpp"
#include "TextureCache.hpp"
#include "scene/Image.hpp"
#include "utils/Logging.h"

#include <algorithm>

using namespace wallpaper;
using namespace wallpaper::vulkan;

VideoTextureCache::VideoTextureCache(const Device& device, VideoTexturePipelineSettings settings)
    : m_device(device), m_settings(settings) {}

VideoTextureCache::~VideoTextureCache() = default;

VideoTextureCache::Entry* VideoTextureCache::find(std::string_view key) {
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [key](const auto& entry) {
        return entry != nullptr && entry->key == key;
    });
    return it == m_entries.end() ? nullptr : it->get();
}

const VideoTextureCache::Entry* VideoTextureCache::find(std::string_view key) const {
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [key](const auto& entry) {
        return entry != nullptr && entry->key == key;
    });
    return it == m_entries.end() ? nullptr : it->get();
}

ImageSlotsRef VideoTextureCache::Acquire(std::string_view key,
                                         const SceneTexture&,
                                         const Image& image,
                                         VideoTexturePlaybackState initial_state) {
    if (auto* entry = find(key); entry != nullptr) {
        return entry->image;
    }

    auto entry = std::make_unique<Entry>();
    entry->key = std::string(key);
    entry->playback_state = m_globally_paused && initial_state == VideoTexturePlaybackState::Playing
                                ? VideoTexturePlaybackState::Paused
                                : initial_state;
    if (m_device.handle()) {
        entry->image = m_device.tex_cache().CreateTex(const_cast<Image&>(image));
    } else {
        LOG_INFO("VideoTextureCache: no Vulkan device available for '%s'; "
                 "tracking playback state without a GPU image",
                 entry->key.c_str());
    }
    for (const auto& slot : image.slots) {
        for (const auto& mip : slot.mipmaps) {
            entry->tracked_bytes += static_cast<std::size_t>(std::max<isize>(mip.size, 0));
        }
    }

    auto ref = entry->image;
    m_entries.emplace_back(std::move(entry));
    return ref;
}

void VideoTextureCache::ApplyPlaybackStates(
    const std::unordered_map<std::string, bool>& paused_by_key,
    const std::unordered_set<std::string>& stopped_keys) {
    for (auto& entry : m_entries) {
        if (entry == nullptr) continue;
        if (stopped_keys.count(entry->key) != 0) {
            entry->playback_state = VideoTexturePlaybackState::Stopped;
            continue;
        }
        if (auto it = paused_by_key.find(entry->key); it != paused_by_key.end()) {
            entry->playback_state =
                it->second || m_globally_paused ? VideoTexturePlaybackState::Paused
                                                : VideoTexturePlaybackState::Playing;
        } else if (m_globally_paused &&
                   entry->playback_state == VideoTexturePlaybackState::Playing) {
            entry->playback_state = VideoTexturePlaybackState::Paused;
        }
    }
}

void VideoTextureCache::SetGlobalPaused(bool paused) {
    m_globally_paused = paused;
    for (auto& entry : m_entries) {
        if (entry == nullptr || entry->playback_state == VideoTexturePlaybackState::Stopped) {
            continue;
        }
        entry->playback_state =
            paused ? VideoTexturePlaybackState::Paused : VideoTexturePlaybackState::Playing;
    }
}

void VideoTextureCache::ApplySeekRequests(
    std::unordered_map<std::string, double>& seek_seconds_by_key) {
    for (auto& [key, seconds] : seek_seconds_by_key) {
        if (auto* entry = find(key); entry != nullptr) {
            entry->seek_seconds = std::max(0.0, seconds);
            entry->seek_pending = true;
        }
    }
    seek_seconds_by_key.clear();
}

void VideoTextureCache::Poll() {
    constexpr double frame_step_seconds = 1.0 / 30.0;

    for (auto& entry : m_entries) {
        if (entry == nullptr) continue;

        if (!entry->pipeline_diagnostic_emitted) {
            LOG_INFO("VideoTextureCache: CPU video pipeline has no decoder backend yet; "
                     "tracking playback state for '%s'",
                     entry->key.c_str());
            entry->pipeline_diagnostic_emitted = true;
        }

        if (entry->seek_pending) {
            entry->playback_seconds = std::max(0.0, entry->seek_seconds);
            entry->seek_pending = false;
            ++entry->applied_seeks;
            if (entry->playback_state != VideoTexturePlaybackState::Stopped) {
                ++entry->pending_uploads;
            }
        }

        if (entry->playback_state != VideoTexturePlaybackState::Playing) {
            continue;
        }

        entry->playback_seconds += frame_step_seconds;
        ++entry->pending_uploads;
    }
}

void VideoTextureCache::RecordUploads(vvk::CommandBuffer&) {
    for (auto& entry : m_entries) {
        if (entry == nullptr || entry->pending_uploads == 0) continue;
        entry->recorded_uploads += entry->pending_uploads;
        entry->pending_uploads = 0;
    }
}

void VideoTextureCache::Clear() {
    m_entries.clear();
}

bool VideoTextureCache::Release(std::string_view key) {
    const auto old_size = m_entries.size();
    std::erase_if(m_entries, [key](const auto& entry) {
        return entry != nullptr && entry->key == key;
    });
    return m_entries.size() != old_size;
}

std::size_t VideoTextureCache::GetTrackedBytes() const {
    std::size_t total = 0;
    for (const auto& entry : m_entries) {
        if (entry != nullptr) total += entry->tracked_bytes;
    }
    return total;
}

std::size_t VideoTextureCache::GetTrackedEntryCount() const {
    return m_entries.size();
}

std::size_t VideoTextureCache::GetPendingUploadCount() const {
    std::size_t total = 0;
    for (const auto& entry : m_entries) {
        if (entry != nullptr) total += entry->pending_uploads;
    }
    return total;
}

std::size_t VideoTextureCache::GetRecordedUploadCount() const {
    std::size_t total = 0;
    for (const auto& entry : m_entries) {
        if (entry != nullptr) total += entry->recorded_uploads;
    }
    return total;
}

std::size_t VideoTextureCache::GetAppliedSeekCount() const {
    std::size_t total = 0;
    for (const auto& entry : m_entries) {
        if (entry != nullptr) total += entry->applied_seeks;
    }
    return total;
}

double VideoTextureCache::GetPlaybackSeconds(std::string_view key) const {
    const auto* entry = find(key);
    return entry == nullptr ? 0.0 : entry->playback_seconds;
}

bool VideoTextureCache::HasPipelineDiagnostic(std::string_view key) const {
    const auto* entry = find(key);
    return entry != nullptr && entry->pipeline_diagnostic_emitted;
}
