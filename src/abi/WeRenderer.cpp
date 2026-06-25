#include "wallpaper/abi/WeRenderer.h"

#include "wallpaper/WallpaperSession.hpp"
#include "wallpaper/InputEvent.hpp"
#include "backend/BuiltinSessionFactory.hpp"
#include "wallpaper/WallpaperRuntime.hpp"
#include "wallpaper/scene/WEScene.hpp"
#include "wallpaper/scene/WESceneContract.hpp"
#include "wallpaper/web/Web.hpp"

#include <algorithm>
#include <memory>
#include <new>
#include <unistd.h>
#include <string>
#include <utility>
#include <cstring>

namespace
{
struct WeSessionState {
    wallpaper::WallpaperRuntime runtime;
    std::unique_ptr<wallpaper::WallpaperSession> session;
    wallpaper::RenderInitInfo renderInitInfo;
    std::shared_ptr<wallpaper::WESceneOutputBinding> binding;
    uint64_t frameSerial { 0 };
};

we_session_t* as_handle(WeSessionState* state) {
    return reinterpret_cast<we_session_t*>(state);
}

WeSessionState* as_state(we_session_t* session) {
    return reinterpret_cast<WeSessionState*>(session);
}

int32_t to_error(const wallpaper::Result<void>& result) {
    return result ? 0 : static_cast<int32_t>(result.error().code) + 1;
}

wallpaper::WallpaperSource make_source(const we_source_v1* source) {
    wallpaper::WallpaperSource out;
    if (!source) return out;
    switch (source->kind) {
    case WE_SOURCE_KIND_SCENE: out.type = wallpaper::BackendType::WEScene; break;
    case WE_SOURCE_KIND_WEB: out.type = wallpaper::BackendType::Web; break;
    case WE_SOURCE_KIND_VIDEO: out.type = wallpaper::BackendType::Video; break;
    default: out.type = wallpaper::BackendType::WEScene; break;
    }
    if (source->uri) out.uri = source->uri;
    return out;
}

bool copy_dmabuf_frame(const wallpaper::ExHandle& handle, we_frame_v1* out_frame) {
    if (!out_frame) return false;
    std::memset(out_frame, 0, sizeof(*out_frame));
    out_frame->size      = sizeof(*out_frame);
    out_frame->version   = 1;
    out_frame->kind      = WE_FRAME_KIND_DMABUF;
    out_frame->width     = static_cast<uint32_t>(std::max(handle.width, 0));
    out_frame->height    = static_cast<uint32_t>(std::max(handle.height, 0));
    out_frame->drm_fourcc = handle.drm_fourcc;
    out_frame->drm_modifier = handle.drm_modifier;
    out_frame->n_planes  = handle.n_planes;
    out_frame->flags     = handle.premultiplied ? 1u : 0u;
    for (uint32_t i = 0; i < handle.n_planes && i < 4; ++i) {
        out_frame->planes[i].fd     = handle.planes[i].fd;
        out_frame->planes[i].offset = handle.planes[i].offset;
        out_frame->planes[i].stride = handle.planes[i].stride;
    }
    return true;
}
} // namespace

extern "C" {
we_session_t* we_session_create(void) {
    auto* state = new (std::nothrow) WeSessionState();
    if (!state) return nullptr;
    state->session = wallpaper::CreateBuiltinSession(state->runtime, {});
    return as_handle(state);
}

void we_session_destroy(we_session_t* session) {
    delete as_state(session);
}

int32_t we_session_set_source(we_session_t* session, const we_source_v1* source) {
    auto* state = as_state(session);
    if (!state || !state->session) return -1;
    auto result = state->session->load(make_source(source));
    if (! result) return to_error(result);
    if (source && source->assets_uri && *source->assets_uri) {
        auto assets_result = state->session->setProperty(
            wallpaper::WE_SCENE_PROPERTY_ASSETS, std::string(source->assets_uri));
        if (! assets_result) return to_error(assets_result);
    }
    if (source && source->fps > 0) {
        auto fps_result = state->session->setProperty(
            wallpaper::WE_SCENE_PROPERTY_FPS, source->fps);
        if (! fps_result) return to_error(fps_result);
    }
    return 0;
}

int32_t we_session_set_render_config(we_session_t* session, const we_render_config_v1* config) {
    auto* state = as_state(session);
    if (!state || !state->session || !config) return -1;
    if (config->size < sizeof(we_render_config_v1) || config->version != 1) return -1;
    state->renderInitInfo.enable_valid_layer = config->enable_valid_layer;
    state->renderInitInfo.offscreen          = true;
    state->renderInitInfo.export_mode        = config->prefer_dmabuf
                                                    ? wallpaper::ExternalFrameExportMode::DMA_BUF
                                                    : wallpaper::ExternalFrameExportMode::OPAQUE_FD;
    state->renderInitInfo.width              = static_cast<uint16_t>(config->width);
    state->renderInitInfo.height             = static_cast<uint16_t>(config->height);
    state->renderInitInfo.render_scale       = 1.0;
    auto binding_result = wallpaper::BindWESceneOutput(*state->session, state->renderInitInfo);
    if (! binding_result) return 1;
    state->binding = std::move(binding_result.value());
    return 0;
}

int32_t we_session_play(we_session_t* session) {
    auto* state = as_state(session);
    if (!state || !state->session) return -1;
    auto result = state->session->play();
    return to_error(result);
}

int32_t we_session_pause(we_session_t* session) {
    auto* state = as_state(session);
    if (!state || !state->session) return -1;
    auto result = state->session->pause();
    return to_error(result);
}

int32_t we_session_stop(we_session_t* session) {
    auto* state = as_state(session);
    if (!state || !state->session) return -1;
    auto result = state->session->stop();
    return to_error(result);
}

int32_t we_session_tick(we_session_t* session) {
    auto* state = as_state(session);
    if (!state || !state->session) return -1;
    auto result = state->session->tick();
    return result ? 0 : static_cast<int32_t>(result.error().code) + 1;
}

int32_t we_session_acquire_frame(we_session_t* session, we_frame_v1* out_frame) {
    auto* state = as_state(session);
    if (!state || !state->session || !out_frame) return -1;
    if (!state->binding || !state->binding->swapchain()) return 1;
    if (out_frame->size != 0 && out_frame->size < sizeof(we_frame_v1)) return -1;

    auto* ex_swapchain = state->binding->swapchain();
    auto* frame        = ex_swapchain->eatFrame();
    if (!frame) return 1;
    if (!frame->isDmabuf()) return -2;

    if (!copy_dmabuf_frame(*frame, out_frame)) {
        return -1;
    }
    out_frame->serial = ++state->frameSerial;
    for (uint32_t i = 0; i < out_frame->n_planes && i < 4; ++i) {
        if (out_frame->planes[i].fd >= 0) {
            const int dup_fd = ::dup(out_frame->planes[i].fd);
            if (dup_fd < 0) {
                we_frame_release(out_frame);
                return -1;
            }
            out_frame->planes[i].fd = dup_fd;
        }
    }
    return 0;
}

void we_frame_release(we_frame_v1* frame) {
    if (!frame) return;
    for (uint32_t i = 0; i < frame->n_planes && i < 4; ++i) {
        if (frame->planes[i].fd >= 0) {
            ::close(frame->planes[i].fd);
            frame->planes[i].fd = -1;
        }
    }
}

int32_t we_session_send_pointer_event(we_session_t* session, uint32_t type, float x, float y) {
    auto* state = as_state(session);
    if (!state || !state->session) return -1;
    wallpaper::InputEventType event_type;
    switch (type) {
    case WE_POINTER_DOWN: event_type = wallpaper::InputEventType::PointerDown; break;
    case WE_POINTER_UP:   event_type = wallpaper::InputEventType::PointerUp;   break;
    case WE_POINTER_MOVE: event_type = wallpaper::InputEventType::PointerMove; break;
    default: return -1;
    }
    wallpaper::InputEvent event;
    event.type     = event_type;
    event.pointerX = x;
    event.pointerY = y;
    return to_error(state->session->sendInput(event));
}
} // extern "C"
