#include "wallpaper/abi/WeRenderer.h"

#include "wallpaper/WallpaperSession.hpp"
#include "wallpaper/InputEvent.hpp"
#include "backend/BuiltinSessionFactory.hpp"
#include "wallpaper/WallpaperRuntime.hpp"
#include "wallpaper/scene/WEScene.hpp"
#include "wallpaper/scene/WESceneContract.hpp"
#include "wallpaper/web/WebOutputBinding.hpp"
#include "wallpaper/OutputTargetBinding.hpp"
#include "wallpaper/OutputTarget.hpp"

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
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
// (WESceneOutputBinding or WebOutputBinding) is chosen in
// we_session_set_render_config from state->sourceKind.
struct WeSessionState {
    wallpaper::WallpaperRuntime runtime;
    std::unique_ptr<wallpaper::WallpaperSession> session;
    wallpaper::RenderInitInfo renderInitInfo;
    wallpaper::BackendType     sourceType { wallpaper::BackendType::WEScene };
    bool                       sourceSet { false };
    std::shared_ptr<wallpaper::OutputTargetBinding> binding;
    uint64_t                  frameSerial { 0 };
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

bool source_has_field(const we_source_v1* source, std::size_t field_offset, std::size_t field_size) {
    return source && source->size >= field_offset + field_size;
}

std::string lower_ascii(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char ch) { return ! std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

struct ProjectSourceInfo {
    wallpaper::BackendType type { wallpaper::BackendType::WEScene };
    std::filesystem::path  projectJson;
    std::filesystem::path  sourcePath;
    std::string            backendUri;
};

wallpaper::Result<ProjectSourceInfo> parse_project_source(const char* uri) {
    if (! uri || ! *uri) {
        return wallpaper::Result<ProjectSourceInfo>::failure(
            wallpaper::ResultCode::InvalidArgument, "source uri is empty");
    }

    ProjectSourceInfo info;
    info.sourcePath = uri;

    std::error_code ec;
    if (std::filesystem::is_directory(info.sourcePath, ec) && ! ec) {
        info.projectJson = info.sourcePath / "project.json";
    } else {
        info.projectJson = info.sourcePath;
    }

    std::ifstream is(info.projectJson);
    if (! is) {
        return wallpaper::Result<ProjectSourceInfo>::failure(
            wallpaper::ResultCode::NotFound,
            "cannot open project.json: " + info.projectJson.string());
    }

    auto j = nlohmann::json::parse(is, nullptr, false, true);
    if (j.is_discarded()) {
        return wallpaper::Result<ProjectSourceInfo>::failure(
            wallpaper::ResultCode::InvalidArgument,
            "invalid JSON: " + info.projectJson.string());
    }

    auto type_it = j.find("type");
    if (type_it == j.end() || ! type_it->is_string()) {
        return wallpaper::Result<ProjectSourceInfo>::failure(
            wallpaper::ResultCode::InvalidArgument,
            "project.json is missing a string type field: " + info.projectJson.string());
    }

    const std::string type = lower_ascii(type_it->get<std::string>());
    if (type == "web") {
        info.type = wallpaper::BackendType::Web;
        info.backendUri = info.projectJson.parent_path().string();
    } else if (type == "scene") {
        info.type = wallpaper::BackendType::WEScene;
        info.backendUri = info.projectJson.parent_path().string();
        std::ifstream pj(info.projectJson);
        if (! pj) {
            return wallpaper::Result<ProjectSourceInfo>::failure(
                wallpaper::ResultCode::NotFound,
                "cannot open project.json: " + info.projectJson.string());
        }
        auto j = nlohmann::json::parse(pj, nullptr, false, true);
        if (j.is_discarded()) {
            return wallpaper::Result<ProjectSourceInfo>::failure(
                wallpaper::ResultCode::InvalidArgument,
                "invalid JSON: " + info.projectJson.string());
        }
        auto file_it = j.find("file");
        if (file_it != j.end() && file_it->is_string()) {
            const auto file_value = trim_copy(file_it->get<std::string>());
            if (! file_value.empty()) {
                const std::filesystem::path file_path { file_value };
                if (file_path.has_extension() && file_path.extension() == ".pkg") {
                    info.backendUri = (info.projectJson.parent_path() / file_path).string();
                } else {
                    info.backendUri =
                        (info.projectJson.parent_path() / file_path).replace_extension("pkg").string();
                }
            }
        }
    } else if (type == "video") {
        info.type = wallpaper::BackendType::Video;
    } else {
        return wallpaper::Result<ProjectSourceInfo>::failure(
            wallpaper::ResultCode::NotSupported,
            "unsupported project type: " + type);
    }

    return wallpaper::Result<ProjectSourceInfo>::success(std::move(info));
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

bool copy_shm_frame(const wallpaper::ExHandle& handle, we_frame_v1* out_frame) {
    if (! out_frame) return false;
    if (handle.fd < 0 || handle.width <= 0 || handle.height <= 0 || handle.size == 0
        || handle.shm_stride == 0) {
        return false;
    }

    std::memset(out_frame, 0, sizeof(*out_frame));
    out_frame->size = sizeof(*out_frame);
    out_frame->version = 1;
    out_frame->kind = WE_FRAME_KIND_SHM;
    out_frame->width = static_cast<uint32_t>(handle.width);
    out_frame->height = static_cast<uint32_t>(handle.height);
    out_frame->shm_stride = handle.shm_stride;
    out_frame->shm_size = static_cast<uint32_t>(handle.size);
    out_frame->flags = handle.premultiplied ? 1u : 0u;
    out_frame->planes[0].fd = handle.fd;
    return true;
}

// Centralised dynamic_cast helpers. The base class does not expose
// swapchain() because scene and web bindings have different
// lifecycle semantics around it; the ABI is the one place that
// needs to read frames, so it pays the cast cost.
wallpaper::WESceneOutputBinding* asSceneBinding(const std::shared_ptr<wallpaper::OutputTargetBinding>& b) {
    return std::dynamic_pointer_cast<wallpaper::WESceneOutputBinding>(b).get();
}

wallpaper::WebOutputBinding* asWebBinding(const std::shared_ptr<wallpaper::OutputTargetBinding>& b) {
    return std::dynamic_pointer_cast<wallpaper::WebOutputBinding>(b).get();
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
    auto parsed = parse_project_source(source->uri);
    if (! parsed) return to_error(parsed);

    state->sourceType = parsed.value().type;
    state->sourceSet  = true;
    wallpaper::WallpaperSource normalized = make_source(source);
    normalized.type = parsed.value().type;
    normalized.uri  = parsed.value().backendUri;
    auto result = state->session->load(normalized);
    return to_error(result);
}

int32_t we_session_set_render_config(we_session_t* session, const we_render_config_v1* config) {
    auto* state = as_state(session);
    if (!state || !state->session || !config) return -1;
    if (config->size < sizeof(we_render_config_v1) || config->version != 1) return -1;
    if (! state->sourceSet) return -1;

    if (state->sourceType == wallpaper::BackendType::Web) {
        // Web can prefer dma-buf while still falling back to SHM on CPU paint.
    }

    state->renderInitInfo.enable_valid_layer = config->enable_valid_layer;
    state->renderInitInfo.offscreen          = true;
    state->renderInitInfo.allow_shm_fallback = config->allow_shm_fallback;
    state->renderInitInfo.export_mode = config->prefer_dmabuf
        ? wallpaper::ExternalFrameExportMode::DMA_BUF
        : wallpaper::ExternalFrameExportMode::SHM;
    // DMA_BUF export requires the offscreen image to use LINEAR tiling
    // (TextureCache.cpp:307); pick it automatically when the consumer
    // asked for dmabuf so we don't leak that internal constraint.
    state->renderInitInfo.offscreen_tiling = config->prefer_dmabuf
        ? wallpaper::TexTiling::LINEAR
        : wallpaper::TexTiling::OPTIMAL;
    state->renderInitInfo.width              = static_cast<std::uint16_t>(config->width);
    state->renderInitInfo.height             = static_cast<std::uint16_t>(config->height);
    state->renderInitInfo.render_scale       = 1.0;

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
    case wallpaper::BackendType::Video:
    default:
        return 1;
    }
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

    // Dispatch to the right swapchain based on state->sourceKind.
    // swapchain() lives on the derived binding, not the base, so a
    // dynamic_cast is the cheapest way to read it.
    wallpaper::ExSwapchain* ex_swapchain = nullptr;
    if (state->sourceType == wallpaper::BackendType::WEScene) {
        auto* sceneBinding = asSceneBinding(state->binding);
        if (! sceneBinding) return -1;
        ex_swapchain = sceneBinding->swapchain();
    } else if (state->sourceType == wallpaper::BackendType::Web) {
        auto* webBinding = asWebBinding(state->binding);
        if (! webBinding) return -1;
        ex_swapchain = webBinding->swapchain();
    } else {
        return -1;
    }
    if (! ex_swapchain) return 1;

    auto* frame = ex_swapchain->eatFrame();
    if (!frame) return 1;
    if (frame->isDmabuf()) {
        if (! copy_dmabuf_frame(*frame, out_frame)) {
            return -1;
        }
    } else if (frame->isShm()) {
        if (! copy_shm_frame(*frame, out_frame)) {
            return -1;
        }
    } else {
        return -2;
    }
    out_frame->serial = ++state->frameSerial;
    if (out_frame->kind == WE_FRAME_KIND_DMABUF) {
        std::array<int, 4> duplicated_fds { -1, -1, -1, -1 };
        for (uint32_t i = 0; i < out_frame->n_planes && i < 4; ++i) {
            if (out_frame->planes[i].fd < 0) continue;
            duplicated_fds[i] = ::dup(out_frame->planes[i].fd);
            if (duplicated_fds[i] < 0) {
                for (int duplicated_fd : duplicated_fds) {
                    if (duplicated_fd >= 0) ::close(duplicated_fd);
                }
                return -1;
            }
        }
        for (uint32_t i = 0; i < out_frame->n_planes && i < 4; ++i) {
            if (duplicated_fds[i] >= 0) out_frame->planes[i].fd = duplicated_fds[i];
        }
    } else if (out_frame->kind == WE_FRAME_KIND_SHM) {
        const int dup_fd = ::dup(out_frame->planes[0].fd);
        if (dup_fd < 0) return -1;
        out_frame->planes[0].fd = dup_fd;
    }
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
