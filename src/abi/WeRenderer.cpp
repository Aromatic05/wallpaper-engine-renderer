#include "wallpaper/abi/WeRenderer.h"
#include "WeRendererOptions.hpp"
#include "WeRendererConfig.hpp"
#include "WeProjectSource.hpp"
#include "WeRendererFrameStatus.hpp"

#include "wallpaper/WallpaperSession.hpp"
#include "wallpaper/Diagnostics.hpp"
#include "wallpaper/InputEvent.hpp"
#include "backend/BuiltinSessionFactory.hpp"
#include "wallpaper/WallpaperRuntime.hpp"
#include "wallpaper/scene/WEScene.hpp"
#include "wallpaper/scene/WESceneContract.hpp"
#include "wallpaper/web/WebOutputBinding.hpp"
#include "backend/video/internal/VideoOutputBinding.hpp"
#include "wallpaper/OutputTargetBinding.hpp"
#include "wallpaper/OutputTarget.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <unistd.h>
#include <string>
#include <utility>
#include <cstring>

#include <nlohmann/json.hpp>

namespace
{
// Owns the saved binding polymorphically; the concrete type
// (WESceneOutputBinding, WebOutputBinding, or VideoOutputBinding) is chosen in
// we_session_set_render_config from state->sourceKind.
struct WeSessionState {
    wallpaper::WallpaperRuntime runtime;
    std::unique_ptr<wallpaper::WallpaperSession> session;
    wallpaper::RenderInitInfo renderInitInfo;
    wallpaper::BackendType     sourceType { wallpaper::BackendType::WEScene };
    bool                       sourceSet { false };
    std::shared_ptr<wallpaper::OutputTargetBinding> binding;
    std::uint64_t frameSerial { 0 };
    wallpaper::DiagnosticsSnapshot abiDiagnostics;
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

template<typename T>
int32_t to_error(const wallpaper::Result<T>& result) {
    return result ? 0 : static_cast<int32_t>(result.error().code) + 1;
}

const char* diagnostic_severity_name(wallpaper::DiagnosticSeverity severity) {
    switch (severity) {
    case wallpaper::DiagnosticSeverity::Info: return "info";
    case wallpaper::DiagnosticSeverity::Warning: return "warning";
    case wallpaper::DiagnosticSeverity::Error: return "error";
    }
    return "unknown";
}

void append_abi_error(WeSessionState* state, std::string source, const wallpaper::Error& error) {
    if (! state) return;
    state->abiDiagnostics.append(wallpaper::DiagnosticSeverity::Error,
                                 std::move(source),
                                 error.message);
}

template<typename T>
int32_t to_error_with_diagnostic(WeSessionState* state,
                                 std::string     source,
                                 const wallpaper::Result<T>& result) {
    if (! result) append_abi_error(state, std::move(source), result.error());
    return to_error(result);
}

bool source_has_field(const we_source_v1* source, std::size_t field_offset, std::size_t field_size) {
    return source && source->size >= field_offset + field_size;
}

wallpaper::WallpaperSource make_source(const we_source_v1* source) {
    wallpaper::WallpaperSource out;
    if (!source) return out;
    if (source_has_field(source, offsetof(we_source_v1, uri), sizeof(source->uri)) && source->uri) {
        out.uri = source->uri;
    }
    if (source_has_field(source, offsetof(we_source_v1, assets_uri), sizeof(source->assets_uri)) &&
        source->assets_uri && *source->assets_uri) {
        out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_ASSETS)] = source->assets_uri;
    }
    if (source_has_field(source, offsetof(we_source_v1, fps), sizeof(source->fps)) &&
        source->fps > 0) {
        out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_FPS)] = source->fps;
    }
    if (source_has_field(source, offsetof(we_source_v1, speed), sizeof(source->speed))) {
        out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_SPEED)] = source->speed;
    }
    if (source_has_field(source, offsetof(we_source_v1, volume), sizeof(source->volume))) {
        out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_VOLUME)] = source->volume;
    }
    if (source_has_field(source, offsetof(we_source_v1, muted), sizeof(source->muted))) {
        out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_MUTED)] = source->muted;
    }
    return out;
}

bool move_texture_frame_to_abi(wallpaper::TextureFrame frame, we_frame_v1* out_frame) {
    if (! out_frame || ! frame.valid()) return false;
    if (frame.shmSize > std::numeric_limits<std::uint32_t>::max()) return false;

    std::memset(out_frame, 0, sizeof(*out_frame));
    out_frame->size = sizeof(*out_frame);
    out_frame->version = 1;
    out_frame->width = frame.extent.width;
    out_frame->height = frame.extent.height;
    out_frame->flags = frame.premultiplied ? 1u : 0u;

    if (frame.exportKind == wallpaper::TextureExportKind::DmaBuf) {
        out_frame->kind = WE_FRAME_KIND_DMABUF;
        out_frame->drm_fourcc = frame.drmFourcc;
        out_frame->drm_modifier = frame.drmModifier;
        out_frame->n_planes = frame.planeCount;
    } else {
        out_frame->kind = WE_FRAME_KIND_SHM;
        out_frame->n_planes = 1;
        out_frame->shm_stride = frame.planes[0].stride;
        out_frame->shm_size = static_cast<std::uint32_t>(frame.shmSize);
    }

    for (std::uint32_t index = 0; index < out_frame->n_planes; ++index) {
        out_frame->planes[index].fd = frame.planes[index].descriptor.release();
        out_frame->planes[index].offset = frame.planes[index].offset;
        out_frame->planes[index].stride = frame.planes[index].stride;
    }
    return true;
}
} // namespace

extern "C" {
we_session_t* we_session_create(void) {
    return we_session_create_with_cache_path(nullptr);
}

we_session_t* we_session_create_with_cache_path(const char* cache_path) {
    auto* state = new (std::nothrow) WeSessionState();
    if (!state) return nullptr;
    state->session = wallpaper::CreateBuiltinSession(
        state->runtime, cache_path ? std::string(cache_path) : std::string {});
    return as_handle(state);
}

void we_session_destroy(we_session_t* session) {
    delete as_state(session);
}

int32_t we_session_set_source(we_session_t* session, const we_source_v1* source) {
    auto* state = as_state(session);
    if (!state || !state->session) return -1;
    if (!source || source->version != 1) return -1;
    if (! source_has_field(source, offsetof(we_source_v1, uri), sizeof(source->uri)) || ! source->uri) {
        return -1;
    }
    auto parsed = wallpaper::ResolveProjectSource(source->uri);
    if (! parsed) return to_error_with_diagnostic(state, "abi.source", parsed);

    state->sourceType = parsed.value().type;
    wallpaper::WallpaperSource normalized = make_source(source);
    normalized.type = parsed.value().type;
    normalized.uri  = parsed.value().backendUri;
    if (source_has_field(source,
                         offsetof(we_source_v1, options_json),
                         sizeof(source->options_json))
        && source->options_json && *source->options_json) {
        auto optionsResult =
            wallpaper::ApplyRendererSourceOptionsJson(source->options_json, normalized);
        if (! optionsResult) {
            return to_error_with_diagnostic(state, "abi.source.options", optionsResult);
        }
    }
    auto result = state->session->load(normalized);
    state->sourceSet = result.ok();
    return to_error_with_diagnostic(state, "abi.source.load", result);
}

int32_t we_session_set_render_config(we_session_t* session, const we_render_config_v1* config) {
    auto* state = as_state(session);
    if (!state || !state->session || !config) return -1;
    const auto parsed_config = wallpaper::ParseRendererRenderConfig(config);
    if (! parsed_config.has_value()) return -1;
    if (! state->sourceSet) return -1;

    if (parsed_config->msaa_samples > 1 &&
        state->sourceType != wallpaper::BackendType::WEScene) {
        const auto unsupported = wallpaper::Result<void>::failure(
            wallpaper::ResultCode::NotSupported,
            "final-output MSAA is supported only by the scene backend");
        return to_error_with_diagnostic(state, "abi.render-config.msaa", unsupported);
    }

    if (state->sourceType == wallpaper::BackendType::Web) {
        // Web can prefer dma-buf while still falling back to SHM on CPU paint.
    }

    state->renderInitInfo.enable_valid_layer = parsed_config->enable_valid_layer;
    state->renderInitInfo.offscreen          = true;
    state->renderInitInfo.allow_shm_fallback = parsed_config->allow_shm_fallback;
    state->renderInitInfo.export_mode = parsed_config->prefer_dmabuf
        ? wallpaper::ExternalFrameExportMode::DMA_BUF
        : wallpaper::ExternalFrameExportMode::SHM;
    // DMA_BUF export requires the offscreen image to use LINEAR tiling
    // (TextureCache.cpp:307); pick it automatically when the consumer
    // asked for dmabuf so we don't leak that internal constraint.
    state->renderInitInfo.offscreen_tiling = parsed_config->prefer_dmabuf
        ? wallpaper::TexTiling::LINEAR
        : wallpaper::TexTiling::OPTIMAL;
    state->renderInitInfo.width = static_cast<std::uint16_t>(parsed_config->width);
    state->renderInitInfo.height = static_cast<std::uint16_t>(parsed_config->height);
    state->renderInitInfo.render_scale = 1.0;
    state->renderInitInfo.msaa_samples = parsed_config->msaa_samples;

    switch (state->sourceType) {
    case wallpaper::BackendType::WEScene: {
        auto binding_result = wallpaper::BindWESceneOutput(*state->session, state->renderInitInfo);
        if (! binding_result) return 1;
        state->binding = binding_result.value();
        return 0;
    }
    case wallpaper::BackendType::Web: {
        auto binding = wallpaper::MakeWebOutputBinding(state->renderInitInfo);
        wallpaper::OutputTarget target {};
        target.type    = wallpaper::OutputTargetType::Offscreen;
        target.binding = binding;
        target.width   = state->renderInitInfo.width;
        target.height  = state->renderInitInfo.height;
        auto bindResult = state->session->bindOutput(target);
        if (! bindResult) return 1;
        state->binding = std::move(binding);
        return 0;
    }
    case wallpaper::BackendType::Video: {
        auto binding = wallpaper::MakeVideoOutputBinding(state->renderInitInfo);
        wallpaper::OutputTarget target {};
        target.type = wallpaper::OutputTargetType::Offscreen;
        target.binding = binding;
        target.width = state->renderInitInfo.width;
        target.height = state->renderInitInfo.height;
        auto bindResult = state->session->bindOutput(target);
        if (! bindResult) return 1;
        state->binding = std::move(binding);
        return 0;
    }
    default:
        return 1;
    }
}
int32_t we_session_set_user_properties_json(we_session_t* session,
                                            const char* properties_json) {
    auto* state = as_state(session);
    if (! state || ! state->session || ! properties_json) return -1;
    if (! state->sourceSet) return -1;
    if (state->sourceType != wallpaper::BackendType::WEScene) {
        return static_cast<int32_t>(wallpaper::ResultCode::NotSupported) + 1;
    }

    auto normalized = wallpaper::NormalizeUserPropertiesJson(properties_json);
    if (! normalized) {
        return to_error_with_diagnostic(state, "abi.user-properties", normalized);
    }
    auto result = state->session->setProperty(
        wallpaper::WE_SCENE_PROPERTY_USER_PROPERTIES_JSON, normalized.value());
    return to_error_with_diagnostic(state, "abi.user-properties", result);
}

int32_t we_session_get_diagnostics_json(we_session_t* session,
                                        char* buffer,
                                        uint32_t* inout_size) {
    auto* state = as_state(session);
    if (! state || ! state->session || ! inout_size) return -1;

    nlohmann::json entries = nlohmann::json::array();
    const auto appendSnapshot = [&entries](const wallpaper::DiagnosticsSnapshot& snapshot) {
        for (const auto& entry : snapshot.entries) {
            entries.push_back({ { "severity", diagnostic_severity_name(entry.severity) },
                                { "source", entry.source },
                                { "message", entry.message } });
        }
    };
    appendSnapshot(state->abiDiagnostics);
    appendSnapshot(state->session->diagnostics());

    const std::string payload =
        nlohmann::json { { "version", 1 }, { "entries", std::move(entries) } }.dump();
    if (payload.size() >= std::numeric_limits<uint32_t>::max()) return -1;

    const uint32_t required = static_cast<uint32_t>(payload.size() + 1);
    const uint32_t provided = *inout_size;
    *inout_size = required;
    if (! buffer) return 0;
    if (provided < required) return -2;

    std::memcpy(buffer, payload.c_str(), required);
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
    if (out_frame->size != 0 && out_frame->size < sizeof(we_frame_v1)) return -1;
    if (!state->binding) return 1;

    auto acquired = state->binding->acquireTexture();
    if (! acquired) {
        const auto status =
            wallpaper::MapTextureAcquireErrorToAbiStatus(acquired.error().code);
        if (status.publishDiagnostic) {
            append_abi_error(state, "abi.frame.acquire", acquired.error());
        }
        return status.abiStatus;
    }
    if (! move_texture_frame_to_abi(std::move(acquired.value()), out_frame)) return -1;
    out_frame->serial = ++state->frameSerial;
    return 0;
}

void we_frame_release(we_frame_v1* frame) {
    if (!frame) return;
    if (frame->kind == WE_FRAME_KIND_SHM) {
        if (frame->planes[0].fd >= 0) {
            ::close(frame->planes[0].fd);
            frame->planes[0].fd = -1;
        }
        return;
    }
    for (uint32_t i = 0; i < frame->n_planes && i < 4; ++i) {
        if (frame->planes[i].fd >= 0) {
            ::close(frame->planes[i].fd);
            frame->planes[i].fd = -1;
        }
    }
}

int32_t we_session_send_input_event(we_session_t* session, const we_input_event_v2* event) {
    auto* state = as_state(session);
    if (! state || ! state->session || ! event) return -1;
    if (event->version != 2) return -1;
    if (event->size < sizeof(we_input_event_v2)) return -1;

    wallpaper::InputEvent input;
    switch (event->type) {
    case WE_INPUT_POINTER_MOVE:
        input.type = wallpaper::InputEventType::PointerMove;
        break;
    case WE_INPUT_POINTER_DOWN:
        input.type = wallpaper::InputEventType::PointerDown;
        break;
    case WE_INPUT_POINTER_UP:
        input.type = wallpaper::InputEventType::PointerUp;
        break;
    case WE_INPUT_POINTER_WHEEL:
        input.type = wallpaper::InputEventType::PointerWheel;
        break;
    case WE_INPUT_KEY_DOWN:
        input.type = wallpaper::InputEventType::KeyDown;
        break;
    case WE_INPUT_KEY_UP:
        input.type = wallpaper::InputEventType::KeyUp;
        break;
    case WE_INPUT_FOCUS:
        input.type = event->focused ? wallpaper::InputEventType::FocusGained
                                    : wallpaper::InputEventType::FocusLost;
        break;
    default:
        return -1;
    }

    input.pointerX = event->pointer_x;
    input.pointerY = event->pointer_y;
    input.button = event->button;
    input.wheelDeltaX = event->wheel_delta_x;
    input.wheelDeltaY = event->wheel_delta_y;
    input.keyCode = event->key_code;
    input.nativeKeyCode = event->native_key_code;
    input.modifiers = event->modifiers;
    input.unicodeChar = event->unicode_char;
    return to_error(state->session->sendInput(input));
}
} // extern "C"
