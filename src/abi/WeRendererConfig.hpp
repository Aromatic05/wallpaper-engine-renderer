#pragma once

#include "wallpaper/abi/WeRenderer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace wallpaper
{

struct RendererRenderConfig {
    std::uint32_t width { 0 };
    std::uint32_t height { 0 };
    bool          enable_valid_layer { false };
    bool          prefer_dmabuf { false };
    bool          allow_shm_fallback { false };
    std::uint32_t msaa_samples { 1 };
};

inline bool RendererRenderConfigHasField(const we_render_config_v1* config,
                                         std::size_t                field_offset,
                                         std::size_t                field_size) noexcept {
    return config != nullptr && config->size >= field_offset + field_size;
}

inline std::optional<RendererRenderConfig> ParseRendererRenderConfig(
    const we_render_config_v1* config) noexcept {
    if (config == nullptr || config->version != 1 ||
        ! RendererRenderConfigHasField(config,
                                       offsetof(we_render_config_v1, height),
                                       sizeof(config->height))) {
        return std::nullopt;
    }
    if (config->width == 0 || config->height == 0 ||
        config->width > std::numeric_limits<std::uint16_t>::max() ||
        config->height > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }

    RendererRenderConfig result {
        .width = config->width,
        .height = config->height,
    };
    if (RendererRenderConfigHasField(config,
                                     offsetof(we_render_config_v1, enable_valid_layer),
                                     sizeof(config->enable_valid_layer))) {
        result.enable_valid_layer = config->enable_valid_layer;
    }
    if (RendererRenderConfigHasField(config,
                                     offsetof(we_render_config_v1, prefer_dmabuf),
                                     sizeof(config->prefer_dmabuf))) {
        result.prefer_dmabuf = config->prefer_dmabuf;
    }
    if (RendererRenderConfigHasField(config,
                                     offsetof(we_render_config_v1, allow_shm_fallback),
                                     sizeof(config->allow_shm_fallback))) {
        result.allow_shm_fallback = config->allow_shm_fallback;
    }
    if (RendererRenderConfigHasField(config,
                                     offsetof(we_render_config_v1, msaa_samples),
                                     sizeof(config->msaa_samples)) &&
        config->msaa_samples > 1) {
        result.msaa_samples = config->msaa_samples;
    }
    return result;
}

} // namespace wallpaper
