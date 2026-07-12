#include "abi/WeRendererConfig.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{
struct LegacyRenderConfigV1 {
    uint32_t size;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    bool enable_valid_layer;
    bool prefer_dmabuf;
    bool allow_shm_fallback;
};

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "we-renderer-config-test: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}
} // namespace

int main() {
    LegacyRenderConfigV1 legacy {
        .size = sizeof(LegacyRenderConfigV1),
        .version = 1,
        .width = 1280,
        .height = 720,
        .enable_valid_layer = true,
        .prefer_dmabuf = true,
        .allow_shm_fallback = true,
    };
    const auto legacy_result = wallpaper::ParseRendererRenderConfig(
        reinterpret_cast<const we_render_config_v1*>(&legacy));
    Require(legacy_result.has_value(), "legacy render config was rejected");
    Require(legacy_result->width == 1280 && legacy_result->height == 720,
            "legacy dimensions changed");
    Require(legacy_result->enable_valid_layer && legacy_result->prefer_dmabuf &&
                legacy_result->allow_shm_fallback,
            "legacy flags changed");
    Require(legacy_result->msaa_samples == 1,
            "legacy render config must default to one sample");
    Require(legacy_result->fill_mode == WE_FILL_MODE_ASPECT_CROP,
            "legacy render config must default to aspect crop");
    Require(legacy_result->rotation_degrees == 0,
            "legacy render config must default to no rotation");

    we_render_config_v1 current {};
    current.size = sizeof(current);
    current.version = 1;
    current.width = 1920;
    current.height = 1080;
    current.msaa_samples = 8;
    current.fill_mode = WE_FILL_MODE_ASPECT_FIT;
    current.rotation_degrees = 90;
    const auto current_result = wallpaper::ParseRendererRenderConfig(&current);
    Require(current_result.has_value(), "current render config was rejected");
    Require(current_result->msaa_samples == 8, "MSAA tail field was not preserved");
    Require(current_result->fill_mode == WE_FILL_MODE_ASPECT_FIT,
            "fill mode tail field was not preserved");
    Require(current_result->rotation_degrees == 90,
            "rotation tail field was not preserved");

    current.msaa_samples = 0;
    Require(wallpaper::ParseRendererRenderConfig(&current)->msaa_samples == 1,
            "zero MSAA request must preserve legacy behavior");

    current.rotation_degrees = 45;
    Require(! wallpaper::ParseRendererRenderConfig(&current).has_value(),
            "unsupported output rotation must be rejected");
    current.rotation_degrees = 0;

    current.fill_mode = static_cast<we_fill_mode_v1>(3);
    Require(! wallpaper::ParseRendererRenderConfig(&current).has_value(),
            "unsupported fill mode must be rejected");
    current.fill_mode = WE_FILL_MODE_ASPECT_FIT;

    current.size = offsetof(we_render_config_v1, height);
    Require(! wallpaper::ParseRendererRenderConfig(&current).has_value(),
            "render config truncated before height must be rejected");

    current.size = sizeof(current);
    current.version = 1;
    current.width = 0;
    current.height = 1080;
    Require(! wallpaper::ParseRendererRenderConfig(&current).has_value(),
            "zero-width render config must be rejected");

    current.width = 70000;
    Require(! wallpaper::ParseRendererRenderConfig(&current).has_value(),
            "render config wider than the public renderer limit must be rejected");

    current.width = 1920;
    current.height = 70000;
    Require(! wallpaper::ParseRendererRenderConfig(&current).has_value(),
            "render config taller than the public renderer limit must be rejected");

    current.width = 1920;
    current.height = 1080;
    current.size = sizeof(current);
    current.version = 2;
    Require(! wallpaper::ParseRendererRenderConfig(&current).has_value(),
            "unsupported render config version must be rejected");
    return 0;
}
