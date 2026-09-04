#include "wallpaper/abi/WeRenderer.h"
#include "WeRendererOptions.hpp"
#include "WeRendererConfig.hpp"
#include "WeProjectSource.hpp"
#include "WeRendererFrameStatus.hpp"
#include "WeRendererFrameReady.hpp"
#include "WeRendererRuntime.hpp"

#include "wallpaper/WallpaperSession.hpp"
#include "wallpaper/MediaState.hpp"
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
#include <vector>
#include <cstring>

#include <nlohmann/json.hpp>

namespace
{
// Owns the saved binding polymorphically; the concrete type
// (WESceneOutputBinding, WebOutputBinding, or VideoOutputBinding) is chosen in
// we_session_set_render_config from state->sourceKind.
struct WeSessionState {
    wallpaper::WallpaperRuntime runtime;
    std::shared_ptr<wallpaper::RendererFrameReadySignal> frameReadySignal;
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

std::int32_t scene_fill_mode(we_fill_mode_v1 fill_mode) {
    switch (fill_mode) {
    case WE_FILL_MODE_STRETCH: return 0;
    case WE_FILL_MODE_ASPECT_FIT: return 1;
    case WE_FILL_MODE_CENTER: return 3;
    case WE_FILL_MODE_ASPECT_CROP: return 2;
    }
    return 2;
}

wallpaper::Result<void> unsupported_runtime_setting(std::string name,
                                                    wallpaper::BackendType backend) {
    return wallpaper::Result<void>::failure(
        wallpaper::ResultCode::NotSupported,
        std::move(name) + " is not supported by backend "
            + std::to_string(static_cast<std::int32_t>(backend)));
}

wallpaper::WallpaperSource make_source(const we_source_v1* source,
                                         wallpaper::BackendType backend) {
    wallpaper::WallpaperSource out;
    if (! source) return out;
    if (source_has_field(source, offsetof(we_source_v1, uri), sizeof(source->uri)) && source->uri) {
        out.uri = source->uri;
    }

    const auto add_fps = [&]() {
        if (source_has_field(source, offsetof(we_source_v1, fps), sizeof(source->fps))
            && source->fps > 0) {
            out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_FPS)] = source->fps;
        }
    };
    const auto add_volume = [&]() {
        if (source_has_field(source, offsetof(we_source_v1, volume), sizeof(source->volume))) {
            out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_VOLUME)] = source->volume;
        }
    };
    const auto add_muted = [&]() {
        if (source_has_field(source, offsetof(we_source_v1, muted), sizeof(source->muted))) {
            out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_MUTED)] = source->muted;
        }
    };

    switch (backend) {
    case wallpaper::BackendType::WEScene:
        if (source_has_field(source, offsetof(we_source_v1, assets_uri), sizeof(source->assets_uri))
            && source->assets_uri && *source->assets_uri) {
            out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_ASSETS)] =
                source->assets_uri;
        }
        add_fps();
        if (source_has_field(source, offsetof(we_source_v1, speed), sizeof(source->speed))) {
            out.initialProperties[std::string(wallpaper::WE_SCENE_PROPERTY_SPEED)] = source->speed;
        }
        add_volume();
        add_muted();
        break;
    case wallpaper::BackendType::Web:
        add_fps();
        add_volume();
        add_muted();
        break;
    case wallpaper::BackendType::Video:
        add_volume();
        add_muted();
        break;
    case wallpaper::BackendType::Image:
        break;
    }
    return out;
}


void install_frame_ready_callback(
    const std::shared_ptr<wallpaper::RendererFrameReadySignal>& signal,
    const std::shared_ptr<wallpaper::OutputTargetBinding>& binding) {
    if (! binding) return;
    std::weak_ptr<wallpaper::RendererFrameReadySignal> weak_signal = signal;
    binding->setFrameReadyCallback([weak_signal]() {
        if (auto locked = weak_signal.lock()) locked->notify();
    });
}

wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>> bind_current_output(
    WeSessionState* state) {
    if (! state || ! state->session) {
        return wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>>::failure(
            wallpaper::ResultCode::InvalidState,
            "renderer session is not initialized");
    }

    switch (state->sourceType) {
    case wallpaper::BackendType::WEScene: {
        auto result = wallpaper::BindWESceneOutput(*state->session, state->renderInitInfo);
        if (! result) {
            return wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>>(
                result.error());
        }
        auto binding = std::static_pointer_cast<wallpaper::OutputTargetBinding>(result.value());
        install_frame_ready_callback(state->frameReadySignal, binding);
        return wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>>::success(
            std::move(binding));
    }
    case wallpaper::BackendType::Web: {
        auto binding = wallpaper::MakeWebOutputBinding(state->renderInitInfo);
        auto result = state->session->bindOutput(wallpaper::MakeWebOutputTarget(binding));
        if (! result) {
            return wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>>(
                result.error());
        }
        install_frame_ready_callback(state->frameReadySignal, binding);
        return wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>>::success(
            std::move(binding));
    }
    case wallpaper::BackendType::Video: {
        auto binding = wallpaper::MakeVideoOutputBinding(state->renderInitInfo);
        auto result = state->session->bindOutput(wallpaper::MakeVideoOutputTarget(binding));
        if (! result) {
            return wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>>(
                result.error());
        }
        install_frame_ready_callback(state->frameReadySignal, binding);
        return wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>>::success(
            std::move(binding));
    }
    case wallpaper::BackendType::Image:
        break;
    }
    return wallpaper::Result<std::shared_ptr<wallpaper::OutputTargetBinding>>::failure(
        wallpaper::ResultCode::NotSupported,
        "source backend does not expose a texture output");
}

bool move_texture_frame_to_abi(wallpaper::TextureFrame frame,
                               we_frame_v1* out_frame,
                               std::size_t output_size) {
    if (! out_frame || ! frame.valid()) return false;
    if (frame.shmSize > std::numeric_limits<std::uint32_t>::max()) return false;

    const auto writable_size = std::min(output_size, sizeof(*out_frame));
    std::memset(out_frame, 0, writable_size);
    out_frame->size = static_cast<std::uint32_t>(writable_size);
    out_frame->version = 1;
    out_frame->width = frame.extent.width;
    out_frame->height = frame.extent.height;
    out_frame->flags = frame.premultiplied ? WE_FRAME_FLAG_PREMULTIPLIED : 0u;
    if (frame.descriptorsOmitted) out_frame->flags |= WE_FRAME_FLAG_FDS_OMITTED;

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
    if (output_size >= offsetof(we_frame_v1, buffer_id) + sizeof(out_frame->buffer_id)) {
        out_frame->buffer_id = frame.bufferId;
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
    if (! state) return nullptr;
    state->frameReadySignal = std::make_shared<wallpaper::RendererFrameReadySignal>();
    if (! state->frameReadySignal || ! state->frameReadySignal->valid()) {
        delete state;
        return nullptr;
    }
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
    wallpaper::WallpaperSource normalized = make_source(source, parsed.value().type);
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

    if (parsed_config->prefer_dmabuf && state->renderInitInfo.consumer_dmabuf_formats_known &&
        state->renderInitInfo.consumer_dmabuf_formats.empty() &&
        ! parsed_config->allow_shm_fallback) {
        const auto unsupported = wallpaper::Result<void>::failure(
            wallpaper::ResultCode::NotSupported,
            "DMA-BUF was requested but the consumer reported no supported formats");
        return to_error_with_diagnostic(state, "abi.render-config.dmabuf", unsupported);
    }

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

    auto bindingResult = bind_current_output(state);
    if (! bindingResult) {
        return to_error_with_diagnostic(state, "abi.render-config.output", bindingResult);
    }
    state->binding = std::move(bindingResult.value());
    return 0;
}

int32_t we_session_set_dmabuf_formats(we_session_t* session, const uint32_t* fourccs,
                                      const uint64_t* modifiers, uint32_t count) {
    auto* state = as_state(session);
    if (! state || ! state->session) return -1;
    if (count > 0 && (! fourccs || ! modifiers)) return -1;
    if (count > 65536) return -1;

    std::vector<wallpaper::DmabufFormatModifier> formats;
    try {
        formats.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (fourccs[index] == 0) return -1;
            const wallpaper::DmabufFormatModifier format { fourccs[index], modifiers[index] };
            if (std::find(formats.begin(), formats.end(), format) == formats.end()) {
                formats.push_back(format);
            }
        }
    } catch (...) {
        return -1;
    }

    if (state->renderInitInfo.consumer_dmabuf_formats_known &&
        state->renderInitInfo.consumer_dmabuf_formats == formats) {
        return 0;
    }

    if (state->renderInitInfo.export_mode == wallpaper::ExternalFrameExportMode::DMA_BUF &&
        formats.empty() && ! state->renderInitInfo.allow_shm_fallback) {
        const auto unsupported = wallpaper::Result<void>::failure(
            wallpaper::ResultCode::NotSupported,
            "DMA-BUF consumer format list is empty and SHM fallback is disabled");
        return to_error_with_diagnostic(state, "abi.dmabuf-formats", unsupported);
    }

    const bool old_known   = state->renderInitInfo.consumer_dmabuf_formats_known;
    auto       old_formats = std::move(state->renderInitInfo.consumer_dmabuf_formats);
    state->renderInitInfo.consumer_dmabuf_formats_known = true;
    state->renderInitInfo.consumer_dmabuf_formats       = std::move(formats);

    if (! state->binding ||
        state->renderInitInfo.export_mode != wallpaper::ExternalFrameExportMode::DMA_BUF) {
        return 0;
    }

    auto binding_result = bind_current_output(state);
    if (! binding_result) {
        const wallpaper::Error bind_error                   = binding_result.error();
        state->renderInitInfo.consumer_dmabuf_formats_known = old_known;
        state->renderInitInfo.consumer_dmabuf_formats       = std::move(old_formats);

        auto rollback_result = bind_current_output(state);
        if (rollback_result) {
            state->binding = std::move(rollback_result.value());
        } else {
            append_abi_error(state, "abi.dmabuf-formats.rollback", rollback_result.error());
        }
        const auto failure = wallpaper::Result<void>::failure(bind_error.code, bind_error.message);
        return to_error_with_diagnostic(state, "abi.dmabuf-formats.output", failure);
    }
    state->binding = std::move(binding_result.value());
    return 0;
}

int32_t we_session_resize_output(we_session_t* session,
                                 uint32_t width,
                                 uint32_t height) {
    auto* state = as_state(session);
    if (! state || ! state->session || ! state->sourceSet || ! state->binding) return -1;
    if (width == 0 || height == 0 || width > std::numeric_limits<std::uint16_t>::max()
        || height > std::numeric_limits<std::uint16_t>::max()) {
        return -1;
    }
    if (state->renderInitInfo.width == width && state->renderInitInfo.height == height) return 0;

    state->renderInitInfo.width = static_cast<std::uint16_t>(width);
    state->renderInitInfo.height = static_cast<std::uint16_t>(height);
    auto bindingResult = bind_current_output(state);
    if (! bindingResult) {
        return to_error_with_diagnostic(state, "abi.output.resize", bindingResult);
    }
    state->binding = std::move(bindingResult.value());
    return 0;
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


int32_t we_session_apply_runtime_settings(we_session_t* session,
                                          const we_runtime_settings_v1* settings) {
    auto* state = as_state(session);
    if (! state || ! state->session || ! state->sourceSet) return -1;
    const auto parsed = wallpaper::ParseRendererRuntimeSettings(settings);
    if (! parsed) return -1;

    if ((parsed->fields & WE_RUNTIME_SETTINGS_FILL_MODE) != 0
        && state->sourceType != wallpaper::BackendType::WEScene) {
        const auto unsupported = unsupported_runtime_setting("fill mode", state->sourceType);
        return to_error_with_diagnostic(state, "abi.runtime.fill-mode", unsupported);
    }
    if ((parsed->fields & WE_RUNTIME_SETTINGS_SPEED) != 0
        && state->sourceType == wallpaper::BackendType::Web) {
        const auto unsupported = unsupported_runtime_setting("speed", state->sourceType);
        return to_error_with_diagnostic(state, "abi.runtime.speed", unsupported);
    }

    const auto apply = [&](std::string_view name,
                           wallpaper::PropertyValue value,
                           const char* diagnostic_source) -> int32_t {
        auto result = state->session->setProperty(name, std::move(value));
        return to_error_with_diagnostic(state, diagnostic_source, result);
    };

    if ((parsed->fields & WE_RUNTIME_SETTINGS_FPS) != 0) {
        const auto status = apply(wallpaper::WE_SCENE_PROPERTY_FPS,
                                  parsed->fps,
                                  "abi.runtime.fps");
        if (status != 0) return status;
    }
    if ((parsed->fields & WE_RUNTIME_SETTINGS_SPEED) != 0) {
        const auto status = apply(wallpaper::WE_SCENE_PROPERTY_SPEED,
                                  parsed->speed,
                                  "abi.runtime.speed");
        if (status != 0) return status;
    }
    if ((parsed->fields & WE_RUNTIME_SETTINGS_VOLUME) != 0) {
        const auto status = apply(wallpaper::WE_SCENE_PROPERTY_VOLUME,
                                  parsed->volume,
                                  "abi.runtime.volume");
        if (status != 0) return status;
    }
    if ((parsed->fields & WE_RUNTIME_SETTINGS_MUTED) != 0) {
        const auto status = apply(wallpaper::WE_SCENE_PROPERTY_MUTED,
                                  parsed->muted,
                                  "abi.runtime.muted");
        if (status != 0) return status;
    }
    if ((parsed->fields & WE_RUNTIME_SETTINGS_FILL_MODE) != 0) {
        const auto status = apply(wallpaper::WE_SCENE_PROPERTY_FILLMODE,
                                  scene_fill_mode(parsed->fillMode),
                                  "abi.runtime.fill-mode");
        if (status != 0) return status;
    }
    return 0;
}

int32_t we_session_set_media_state(we_session_t* session,
                                   const we_media_state_v1* media_state) {
    auto* state = as_state(session);
    if (! state || ! state->session || ! state->sourceSet) return -1;
    if (state->sourceType != wallpaper::BackendType::WEScene) {
        const auto unsupported = unsupported_runtime_setting("media state", state->sourceType);
        return to_error_with_diagnostic(state, "abi.media", unsupported);
    }
    auto parsed = wallpaper::ParseRendererMediaState(media_state);
    if (! parsed) return -1;
    auto result = state->session->setProperty(
        wallpaper::WE_SCENE_PROPERTY_MEDIA_STATE,
        std::static_pointer_cast<void>(
            std::make_shared<wallpaper::MediaState>(std::move(*parsed))));
    return to_error_with_diagnostic(state, "abi.media", result);
}

int32_t we_session_push_audio_samples(we_session_t* session,
                                      const float* samples,
                                      uint32_t count) {
    auto* state = as_state(session);
    if (! state || ! state->session || ! state->sourceSet) return -1;
    if (state->sourceType == wallpaper::BackendType::Video) {
        const auto unsupported = unsupported_runtime_setting("audio samples", state->sourceType);
        return to_error_with_diagnostic(state, "abi.audio", unsupported);
    }
    auto copied = wallpaper::CopyRendererAudioSamples(samples, count);
    if (! copied) return -1;
    auto result = state->session->setProperty(
        wallpaper::WE_SCENE_PROPERTY_AUDIO_SAMPLES,
        std::static_pointer_cast<void>(
            std::make_shared<std::vector<float>>(std::move(*copied))));
    return to_error_with_diagnostic(state, "abi.audio", result);
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

int32_t we_session_get_frame_ready_fd(we_session_t* session) {
    auto* state = as_state(session);
    if (! state || ! state->frameReadySignal || ! state->frameReadySignal->valid()) return -1;
    return state->frameReadySignal->descriptor();
}

int32_t we_session_acquire_frame(we_session_t* session, we_frame_v1* out_frame) {
    auto* state = as_state(session);
    if (!state || !state->session || !out_frame) return -1;
    constexpr std::size_t base_frame_size = offsetof(we_frame_v1, buffer_id);
    const std::size_t output_size = out_frame->size == 0 ? base_frame_size : out_frame->size;
    if (output_size < base_frame_size) return -1;
    const auto reusable_buffer_mask =
        output_size >= offsetof(we_frame_v1, reusable_buffer_mask)
                + sizeof(out_frame->reusable_buffer_mask)
        ? out_frame->reusable_buffer_mask
        : 0u;
    if (!state->binding) return 1;

    if (state->frameReadySignal) state->frameReadySignal->consume();
    auto acquired = state->binding->acquireTexture(reusable_buffer_mask);
    if (! acquired) {
        const auto status =
            wallpaper::MapTextureAcquireErrorToAbiStatus(acquired.error().code);
        if (status.publishDiagnostic) {
            append_abi_error(state, "abi.frame.acquire", acquired.error());
        }
        return status.abiStatus;
    }
    if (! move_texture_frame_to_abi(std::move(acquired.value()), out_frame, output_size)) return -1;
    out_frame->serial = ++state->frameSerial;
    return 0;
}

void we_frame_release(we_frame_v1* frame) {
    if (!frame) return;
    if ((frame->flags & WE_FRAME_FLAG_FDS_OMITTED) != 0) return;
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
