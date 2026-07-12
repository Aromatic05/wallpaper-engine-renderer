#include "arg.hpp"
#include "wallpaper/abi/WeRenderer.h"

#include <drm/drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fractional-scale-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

namespace {

bool envVarEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool envVarEquals(const char* name, const char* expected) {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, expected) == 0;
}

std::uint32_t toOpaqueDrmFourcc(std::uint32_t drm_fourcc) {
    switch (drm_fourcc) {
    case DRM_FORMAT_ABGR8888: return DRM_FORMAT_XBGR8888;
    case DRM_FORMAT_ARGB8888: return DRM_FORMAT_XRGB8888;
    default: return drm_fourcc;
    }
}

constexpr std::uint32_t kLayerSurfaceAnchors =
    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
constexpr std::uint32_t kFractionalScaleDenominator = 120;

struct WaylandBuffer {
    wl_buffer* buffer { nullptr };
    bool       released { false };
    std::vector<int> pending_send_fds;
};

struct WaylandState {
    wl_display*             display { nullptr };
    wl_registry*            registry { nullptr };
    wl_compositor*          compositor { nullptr };
    wl_surface*             surface { nullptr };
    wl_seat*                seat { nullptr };
    wl_pointer*             pointer { nullptr };
    wl_output*              output { nullptr };
    wp_viewporter*          viewporter { nullptr };
    wp_viewport*            viewport { nullptr };
    zwlr_layer_shell_v1*    layer_shell { nullptr };
    zwlr_layer_surface_v1*  layer_surface { nullptr };
    zwp_linux_dmabuf_v1*    dmabuf { nullptr };
    wl_shm*                 shm { nullptr };
    wp_fractional_scale_manager_v1* fractional_scale_manager { nullptr };
    wp_fractional_scale_v1* fractional_scale { nullptr };
    std::uint32_t           dmabuf_version { 0 };
    std::uint32_t           compositor_version { 0 };
    std::uint32_t           output_count { 0 };
    std::uint32_t           output_scale { 1 };
    std::uint32_t           preferred_fractional_scale { 0 };
    std::uint32_t           output_mode_width { 0 };
    std::uint32_t           output_mode_height { 0 };
    std::int32_t            output_transform { WL_OUTPUT_TRANSFORM_NORMAL };
    std::uint32_t           rotation_degrees { 0 };
    std::uint32_t           logical_width { 0 };
    std::uint32_t           logical_height { 0 };
    std::uint32_t           render_width { 0 };
    std::uint32_t           render_height { 0 };
    std::uint32_t           bound_render_width { 0 };
    std::uint32_t           bound_render_height { 0 };
    std::uint32_t           fallback_width { 0 };
    std::uint32_t           fallback_height { 0 };
    double                  pointer_x { 0.0 };
    double                  pointer_y { 0.0 };
    bool                    running { true };
    bool                    configured { false };
    bool                    extent_mismatch_reported { false };
    we_session_t*           session { nullptr };
    std::vector<std::unique_ptr<WaylandBuffer>> in_flight_buffers;
};

void destroyWayland(WaylandState& state);

void onWlBufferRelease(void* data, wl_buffer* /*buffer*/) {
    auto* entry = static_cast<WaylandBuffer*>(data);
    if (! entry) return;
    entry->released = true;
}

constexpr wl_buffer_listener kBufferListener {
    .release = onWlBufferRelease,
};

void destroyBufferEntry(WaylandBuffer& entry) {
    for (const int fd : entry.pending_send_fds) {
        if (fd >= 0) ::close(fd);
    }
    entry.pending_send_fds.clear();
    if (entry.buffer) {
        wl_buffer_destroy(entry.buffer);
        entry.buffer = nullptr;
    }
    entry.released = true;
}

void releasePendingSendFds(WaylandState& state) {
    for (auto& entry : state.in_flight_buffers) {
        if (! entry) continue;
        for (const int fd : entry->pending_send_fds) {
            if (fd >= 0) ::close(fd);
        }
        entry->pending_send_fds.clear();
    }
}

void collectReleasedBuffers(WaylandState& state) {
    auto it = state.in_flight_buffers.begin();
    while (it != state.in_flight_buffers.end()) {
        if (! *it) {
            it = state.in_flight_buffers.erase(it);
            continue;
        }
        if (! (*it)->released || ! (*it)->pending_send_fds.empty()) {
            ++it;
            continue;
        }
        destroyBufferEntry(*(*it));
        it = state.in_flight_buffers.erase(it);
    }
}

double renderScaleFactor(const WaylandState& state) {
    if (state.preferred_fractional_scale >= kFractionalScaleDenominator) {
        return static_cast<double>(state.preferred_fractional_scale) /
               static_cast<double>(kFractionalScaleDenominator);
    }
    return static_cast<double>(std::max(state.output_scale, 1u));
}

std::uint32_t scaledExtent(std::uint32_t logical_extent, double scale_factor) {
    return static_cast<std::uint32_t>(
        std::max(1.0, std::round(static_cast<double>(logical_extent) * scale_factor)));
}

bool swapsDimensions(std::int32_t transform) {
    return transform == WL_OUTPUT_TRANSFORM_90 || transform == WL_OUTPUT_TRANSFORM_270 ||
           transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
           transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
}

std::int32_t bufferTransformForClockwiseRotation(std::uint32_t rotation_degrees) {
    switch (rotation_degrees) {
    case 90: return WL_OUTPUT_TRANSFORM_270;
    case 180: return WL_OUTPUT_TRANSFORM_180;
    case 270: return WL_OUTPUT_TRANSFORM_90;
    default: return WL_OUTPUT_TRANSFORM_NORMAL;
    }
}

void logRenderGeometry(const WaylandState& state, const char* reason);

void updateRenderExtent(WaylandState& state) {
    if (state.output_mode_width > 0 && state.output_mode_height > 0) {
        state.render_width = state.output_mode_width;
        state.render_height = state.output_mode_height;
        if (swapsDimensions(state.output_transform) != (state.rotation_degrees == 90 ||
                                                        state.rotation_degrees == 270)) {
            std::swap(state.render_width, state.render_height);
        }
        return;
    }

    const std::uint32_t logical_width =
        state.logical_width > 0 ? state.logical_width : state.fallback_width;
    const std::uint32_t logical_height =
        state.logical_height > 0 ? state.logical_height : state.fallback_height;
    const double scale_factor = renderScaleFactor(state);

    state.render_width = scaledExtent(logical_width, scale_factor);
    state.render_height = scaledExtent(logical_height, scale_factor);
}

void resizeBoundOutputIfNeeded(WaylandState& state, const char* reason) {
    if (! state.session || state.render_width == 0 || state.render_height == 0) return;
    if (state.render_width == state.bound_render_width
        && state.render_height == state.bound_render_height) {
        return;
    }
    const std::int32_t result =
        we_session_resize_output(state.session, state.render_width, state.render_height);
    if (result != 0) {
        std::fprintf(stderr,
                     "sceneviewer-layer: output resize failed reason=%s extent=%ux%u status=%d\n",
                     reason,
                     state.render_width,
                     state.render_height,
                     result);
        return;
    }
    state.bound_render_width = state.render_width;
    state.bound_render_height = state.render_height;
    state.extent_mismatch_reported = false;
    logRenderGeometry(state, reason);
}

void updateViewportDestination(WaylandState& state) {
    if (! state.viewport || state.logical_width == 0 || state.logical_height == 0) return;
    wp_viewport_set_destination(
        state.viewport, static_cast<std::int32_t>(state.logical_width), static_cast<std::int32_t>(state.logical_height));
}

void updateSurfaceRegions(WaylandState& state) {
    if (! state.compositor || ! state.surface) return;

    wl_region* input_region = wl_compositor_create_region(state.compositor);
    if (input_region) {
        if (state.logical_width > 0 && state.logical_height > 0) {
            wl_region_add(input_region,
                          0,
                          0,
                          static_cast<std::int32_t>(state.logical_width),
                          static_cast<std::int32_t>(state.logical_height));
        }
        wl_surface_set_input_region(state.surface, input_region);
        wl_region_destroy(input_region);
    }

    if (state.logical_width == 0 || state.logical_height == 0) return;

}

void logRenderGeometry(const WaylandState& state, const char* reason) {
    std::fprintf(stderr,
                 "sceneviewer-layer: %s logical=%ux%u render=%ux%u scale=%.3f output_mode=%ux%u\n",
                 reason,
                 state.logical_width,
                 state.logical_height,
                 state.render_width,
                 state.render_height,
                 renderScaleFactor(state),
                 state.output_mode_width,
                 state.output_mode_height);
}

void onPointerEnter(void* data,
                    wl_pointer* /*pointer*/,
                    std::uint32_t /*serial*/,
                    wl_surface* /*surface*/,
                    wl_fixed_t sx,
                    wl_fixed_t sy) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;
    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);
}

void onPointerLeave(void* /*data*/,
                    wl_pointer* /*pointer*/,
                    std::uint32_t /*serial*/,
                    wl_surface* /*surface*/) {}

void onPointerMotion(void* data,
                     wl_pointer* /*pointer*/,
                     std::uint32_t /*time*/,
                     wl_fixed_t sx,
                     wl_fixed_t sy) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || ! state->session || state->logical_width == 0 || state->logical_height == 0) {
        return;
    }

    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);

    we_input_event_v2 event {};
    event.size = sizeof(event);
    event.version = 2;
    event.type = WE_INPUT_POINTER_MOVE;
    event.pointer_x = static_cast<float>(state->pointer_x / static_cast<double>(state->logical_width));
    event.pointer_y = static_cast<float>(state->pointer_y / static_cast<double>(state->logical_height));
    we_session_send_input_event(state->session, &event);
}

void onPointerButton(void* data,
                     wl_pointer* /*pointer*/,
                     std::uint32_t /*serial*/,
                     std::uint32_t /*time*/,
                     std::uint32_t button,
                     std::uint32_t button_state) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || ! state->session || state->logical_width == 0 || state->logical_height == 0) {
        return;
    }
    if (button != BTN_LEFT) return;

    we_input_event_v2 event {};
    event.size = sizeof(event);
    event.version = 2;
    event.type = button_state == WL_POINTER_BUTTON_STATE_PRESSED
        ? WE_INPUT_POINTER_DOWN
        : WE_INPUT_POINTER_UP;
    event.pointer_x = static_cast<float>(state->pointer_x / static_cast<double>(state->logical_width));
    event.pointer_y = static_cast<float>(state->pointer_y / static_cast<double>(state->logical_height));
    event.button = 0;
    we_session_send_input_event(state->session, &event);
}

void onPointerAxis(void* /*data*/,
                   wl_pointer* /*pointer*/,
                   std::uint32_t /*time*/,
                   std::uint32_t /*axis*/,
                   wl_fixed_t /*value*/) {}

void onPointerFrame(void* /*data*/, wl_pointer* /*pointer*/) {}

void onPointerAxisSource(void* /*data*/,
                         wl_pointer* /*pointer*/,
                         std::uint32_t /*axis_source*/) {}

void onPointerAxisStop(void* /*data*/,
                       wl_pointer* /*pointer*/,
                       std::uint32_t /*time*/,
                       std::uint32_t /*axis*/) {}

void onPointerAxisDiscrete(void* /*data*/,
                           wl_pointer* /*pointer*/,
                           std::uint32_t /*axis*/,
                           std::int32_t /*discrete*/) {}

constexpr wl_pointer_listener kPointerListener {
    .enter = onPointerEnter,
    .leave = onPointerLeave,
    .motion = onPointerMotion,
    .button = onPointerButton,
    .axis = onPointerAxis,
    .frame = onPointerFrame,
    .axis_source = onPointerAxisSource,
    .axis_stop = onPointerAxisStop,
    .axis_discrete = onPointerAxisDiscrete,
};

void onSeatCapabilities(void* data, wl_seat* seat, std::uint32_t capabilities) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;

    const bool has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_pointer && ! state->pointer) {
        state->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->pointer, &kPointerListener, state);
    } else if (! has_pointer && state->pointer) {
        wl_pointer_destroy(state->pointer);
        state->pointer = nullptr;
    }
}

void onSeatName(void* /*data*/, wl_seat* /*seat*/, const char* /*name*/) {}

constexpr wl_seat_listener kSeatListener {
    .capabilities = onSeatCapabilities,
    .name = onSeatName,
};

void onLayerSurfaceConfigure(void* data,
                             zwlr_layer_surface_v1* layer_surface,
                             std::uint32_t serial,
                             std::uint32_t width,
                             std::uint32_t height) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;

    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    state->configured = true;
    state->logical_width = width > 0 ? width : state->fallback_width;
    state->logical_height = height > 0 ? height : state->fallback_height;
    updateRenderExtent(*state);
    updateViewportDestination(*state);
    updateSurfaceRegions(*state);
    resizeBoundOutputIfNeeded(*state, "resized configured wallpaper surface");
    logRenderGeometry(*state, "configured wallpaper surface");
}

void onLayerSurfaceClosed(void* data, zwlr_layer_surface_v1* /*layer_surface*/) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;
    state->running = false;
}

constexpr zwlr_layer_surface_v1_listener kLayerSurfaceListener {
    .configure = onLayerSurfaceConfigure,
    .closed = onLayerSurfaceClosed,
};

void onOutputGeometry(void* data,
                      wl_output* /*output*/,
                      std::int32_t /*x*/,
                      std::int32_t /*y*/,
                      std::int32_t /*physical_width*/,
                      std::int32_t /*physical_height*/,
                      std::int32_t /*subpixel*/,
                      const char* /*make*/,
                      const char* /*model*/,
                      std::int32_t transform) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;
    state->output_transform = transform;
    if (state->logical_width == 0 || state->logical_height == 0) return;
    updateRenderExtent(*state);
    resizeBoundOutputIfNeeded(*state, "resized after output transform");
    logRenderGeometry(*state, "updated output transform");
}

void onOutputMode(void* data,
                  wl_output* /*output*/,
                  std::uint32_t flags,
                  std::int32_t width,
                  std::int32_t height,
                  std::int32_t /*refresh*/) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || (flags & WL_OUTPUT_MODE_CURRENT) == 0) return;
    state->output_mode_width = static_cast<std::uint32_t>(std::max(width, 0));
    state->output_mode_height = static_cast<std::uint32_t>(std::max(height, 0));
    if (state->logical_width > 0 && state->logical_height > 0) {
        updateRenderExtent(*state);
        resizeBoundOutputIfNeeded(*state, "resized after output mode");
    }
}

void onOutputDone(void* /*data*/, wl_output* /*output*/) {}

void onOutputScale(void* data, wl_output* /*output*/, std::int32_t factor) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;
    state->output_scale = static_cast<std::uint32_t>(std::max(factor, 1));
    if (state->logical_width == 0 || state->logical_height == 0) return;
    updateRenderExtent(*state);
    resizeBoundOutputIfNeeded(*state, "resized after output scale");
    logRenderGeometry(*state, "updated output scale");
}

constexpr wl_output_listener kOutputListener {
    .geometry = onOutputGeometry,
    .mode = onOutputMode,
    .done = onOutputDone,
    .scale = onOutputScale,
};

void onFractionalScalePreferredScale(void* data,
                                     wp_fractional_scale_v1* /*fractional_scale*/,
                                     std::uint32_t scale) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;
    state->preferred_fractional_scale = std::max(scale, kFractionalScaleDenominator);
    if (state->logical_width == 0 || state->logical_height == 0) return;
    updateRenderExtent(*state);
    resizeBoundOutputIfNeeded(*state, "resized after fractional scale");
    logRenderGeometry(*state, "updated fractional scale");
}

constexpr wp_fractional_scale_v1_listener kFractionalScaleListener {
    .preferred_scale = onFractionalScalePreferredScale,
};

void onRegistryGlobal(void* data,
                      wl_registry* registry,
                      std::uint32_t name,
                      const char* interface,
                      std::uint32_t version) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || ! interface) return;

    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        const std::uint32_t bind_version = std::min(version, 4u);
        state->compositor =
            static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, bind_version));
        state->compositor_version = bind_version;
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        ++state->output_count;
        if (! state->output) {
            state->output =
                static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 3u)));
            wl_output_add_listener(state->output, &kOutputListener, state);
        }
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat =
            static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(state->seat, &kSeatListener, state);
    } else if (std::strcmp(interface, wp_viewporter_interface.name) == 0) {
        state->viewporter =
            static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1));
    } else if (std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        state->dmabuf_version = std::min(version, 4u);
        state->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, state->dmabuf_version));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm =
            static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, std::min(version, 1u)));
    } else if (std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        state->fractional_scale_manager = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
    }
}

void onRegistryRemove(void* /*data*/, wl_registry* /*registry*/, std::uint32_t /*name*/) {}

constexpr wl_registry_listener kRegistryListener {
    .global = onRegistryGlobal,
    .global_remove = onRegistryRemove,
};

bool initWayland(WaylandState& state,
                 std::uint32_t fallback_width,
                 std::uint32_t fallback_height,
                 std::uint32_t rotation_degrees) {
    state.fallback_width = fallback_width;
    state.fallback_height = fallback_height;
    state.rotation_degrees = rotation_degrees;

    state.display = wl_display_connect(nullptr);
    if (! state.display) {
        std::fprintf(stderr, "sceneviewer-layer: wl_display_connect failed\n");
        return false;
    }

    state.registry = wl_display_get_registry(state.display);
    if (! state.registry) {
        std::fprintf(stderr, "sceneviewer-layer: wl_display_get_registry failed\n");
        return false;
    }
    wl_registry_add_listener(state.registry, &kRegistryListener, &state);

    if (wl_display_roundtrip(state.display) < 0 || wl_display_roundtrip(state.display) < 0) {
        std::fprintf(stderr, "sceneviewer-layer: initial Wayland roundtrip failed\n");
        return false;
    }
    if (! state.compositor || ! state.layer_shell || (! state.dmabuf && ! state.shm)) {
        std::fprintf(stderr,
                     "sceneviewer-layer: missing required Wayland globals compositor=%p layer_shell=%p dmabuf=%p shm=%p\n",
                     static_cast<void*>(state.compositor),
                     static_cast<void*>(state.layer_shell),
                     static_cast<void*>(state.dmabuf),
                     static_cast<void*>(state.shm));
        return false;
    }
    if (state.dmabuf && state.dmabuf_version < 2) {
        std::fprintf(stderr,
                     "sceneviewer-layer: zwp_linux_dmabuf_v1 version %u does not support create_immed\n",
                     state.dmabuf_version);
        return false;
    }

    state.surface = wl_compositor_create_surface(state.compositor);
    if (! state.surface) {
        std::fprintf(stderr, "sceneviewer-layer: wl_compositor_create_surface failed\n");
        return false;
    }
    wl_surface_set_buffer_scale(state.surface, 1);
    wl_surface_set_buffer_transform(
        state.surface, bufferTransformForClockwiseRotation(state.rotation_degrees));

    if (state.viewporter) {
        state.viewport = wp_viewporter_get_viewport(state.viewporter, state.surface);
    }
    if (state.fractional_scale_manager) {
        state.fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(state.fractional_scale_manager, state.surface);
        wp_fractional_scale_v1_add_listener(state.fractional_scale, &kFractionalScaleListener, &state);
    }

    state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell,
        state.surface,
        state.output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        "wallpaper-engine-renderer");
    if (! state.layer_surface) {
        std::fprintf(stderr, "sceneviewer-layer: zwlr_layer_shell_v1_get_layer_surface failed\n");
        return false;
    }
    zwlr_layer_surface_v1_add_listener(state.layer_surface, &kLayerSurfaceListener, &state);
    zwlr_layer_surface_v1_set_anchor(state.layer_surface, kLayerSurfaceAnchors);
    zwlr_layer_surface_v1_set_size(state.layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
    zwlr_layer_surface_v1_set_margin(state.layer_surface, 0, 0, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layer_surface, 0);
    updateSurfaceRegions(state);
    wl_surface_commit(state.surface);

    while (! state.configured) {
        if (wl_display_roundtrip(state.display) < 0) {
            std::fprintf(stderr, "sceneviewer-layer: waiting for layer configure failed\n");
            return false;
        }
    }

    if (state.logical_width == 0) state.logical_width = state.fallback_width;
    if (state.logical_height == 0) state.logical_height = state.fallback_height;
    updateRenderExtent(state);
    updateViewportDestination(state);
    updateSurfaceRegions(state);

    if (state.output_count > 1) {
        std::fprintf(stderr,
                     "sceneviewer-layer: compositor exposed %u outputs, using the first one for this test client\n",
                     state.output_count);
    }
    if (! state.viewporter) {
        std::fprintf(stderr,
                     "sceneviewer-layer: wp_viewporter unavailable, fractional high-DPI buffers will not map correctly\n");
    }
    if (! state.fractional_scale_manager) {
        std::fprintf(stderr,
                     "sceneviewer-layer: fractional-scale-v1 unavailable, falling back to wl_output integer scale\n");
    }
    return true;
}

std::unique_ptr<WaylandBuffer> createBufferForFrame(WaylandState& state, const we_frame_v1& frame) {
    if (frame.kind == WE_FRAME_KIND_SHM) {
        if (! state.shm || frame.planes[0].fd < 0 || frame.shm_stride == 0 || frame.shm_size == 0) {
            return nullptr;
        }
        wl_shm_pool* pool = wl_shm_create_pool(state.shm, frame.planes[0].fd, static_cast<int>(frame.shm_size));
        if (! pool) return nullptr;
        wl_buffer* buffer = wl_shm_pool_create_buffer(pool,
                                                      0,
                                                      static_cast<int>(frame.width),
                                                      static_cast<int>(frame.height),
                                                      static_cast<int>(frame.shm_stride),
                                                      WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        if (! buffer) return nullptr;
        auto entry = std::make_unique<WaylandBuffer>();
        entry->buffer = buffer;
        wl_buffer_add_listener(entry->buffer, &kBufferListener, entry.get());
        return entry;
    }

    if (frame.kind != WE_FRAME_KIND_DMABUF || frame.n_planes == 0 || frame.n_planes > 4) return nullptr;
    auto params = zwp_linux_dmabuf_v1_create_params(state.dmabuf);
    if (! params) return nullptr;

    std::vector<int> send_fds;
    send_fds.reserve(frame.n_planes);
    const std::uint32_t modifier_hi = static_cast<std::uint32_t>(frame.drm_modifier >> 32U);
    const std::uint32_t modifier_lo = static_cast<std::uint32_t>(frame.drm_modifier & 0xffffffffULL);
    for (std::uint32_t i = 0; i < frame.n_planes; ++i) {
        const int dup_fd = ::dup(frame.planes[i].fd);
        if (dup_fd < 0) {
            std::fprintf(stderr,
                         "sceneviewer-layer: dup(fd=%d) failed: %s\n",
                         frame.planes[i].fd,
                         std::strerror(errno));
            for (const int fd : send_fds) {
                if (fd >= 0) ::close(fd);
            }
            zwp_linux_buffer_params_v1_destroy(params);
            return nullptr;
        }
        send_fds.push_back(dup_fd);
        zwp_linux_buffer_params_v1_add(params,
                                       dup_fd,
                                       i,
                                       frame.planes[i].offset,
                                       frame.planes[i].stride,
                                       modifier_hi,
                                       modifier_lo);
    }

    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
        params,
        static_cast<std::int32_t>(frame.width),
        static_cast<std::int32_t>(frame.height),
        toOpaqueDrmFourcc(frame.drm_fourcc),
        0);
    zwp_linux_buffer_params_v1_destroy(params);

    if (! buffer) {
        std::fprintf(stderr, "sceneviewer-layer: create_immed returned null wl_buffer\n");
        for (const int fd : send_fds) {
            if (fd >= 0) ::close(fd);
        }
        return nullptr;
    }

    auto entry = std::make_unique<WaylandBuffer>();
    entry->buffer = buffer;
    entry->pending_send_fds = std::move(send_fds);
    wl_buffer_add_listener(entry->buffer, &kBufferListener, entry.get());
    return entry;
}

bool presentFrame(WaylandState& state, const we_frame_v1& frame) {
    auto entry = createBufferForFrame(state, frame);
    if (! entry) return false;

    if (! state.extent_mismatch_reported
        && (frame.width != state.render_width || frame.height != state.render_height)) {
        std::fprintf(stderr,
                     "sceneviewer-layer: frame extent %ux%u differs from configured render extent %ux%u\n",
                     frame.width,
                     frame.height,
                     state.render_width,
                     state.render_height);
        state.extent_mismatch_reported = true;
    }

    updateViewportDestination(state);
    wl_surface_attach(state.surface, entry->buffer, 0, 0);
    if (state.compositor_version >= 4) {
        wl_surface_damage_buffer(state.surface, 0, 0, INT32_MAX, INT32_MAX);
    } else {
        wl_surface_damage(state.surface, 0, 0, INT32_MAX, INT32_MAX);
    }
    wl_surface_commit(state.surface);
    state.in_flight_buffers.push_back(std::move(entry));
    return true;
}

void destroyWayland(WaylandState& state) {
    for (auto& entry : state.in_flight_buffers) {
        if (entry) destroyBufferEntry(*entry);
    }
    state.in_flight_buffers.clear();

    if (state.fractional_scale) {
        wp_fractional_scale_v1_destroy(state.fractional_scale);
        state.fractional_scale = nullptr;
    }
    if (state.viewport) {
        wp_viewport_destroy(state.viewport);
        state.viewport = nullptr;
    }
    if (state.layer_surface) {
        zwlr_layer_surface_v1_destroy(state.layer_surface);
        state.layer_surface = nullptr;
    }
    if (state.output) {
        wl_output_destroy(state.output);
        state.output = nullptr;
    }
    if (state.pointer) {
        wl_pointer_destroy(state.pointer);
        state.pointer = nullptr;
    }
    if (state.seat) {
        wl_seat_destroy(state.seat);
        state.seat = nullptr;
    }
    if (state.surface) {
        wl_surface_destroy(state.surface);
        state.surface = nullptr;
    }
    if (state.dmabuf) {
        zwp_linux_dmabuf_v1_destroy(state.dmabuf);
        state.dmabuf = nullptr;
    }
    if (state.shm) {
        wl_shm_destroy(state.shm);
        state.shm = nullptr;
    }
    if (state.fractional_scale_manager) {
        wp_fractional_scale_manager_v1_destroy(state.fractional_scale_manager);
        state.fractional_scale_manager = nullptr;
    }
    if (state.viewporter) {
        wp_viewporter_destroy(state.viewporter);
        state.viewporter = nullptr;
    }
    if (state.layer_shell) {
        zwlr_layer_shell_v1_destroy(state.layer_shell);
        state.layer_shell = nullptr;
    }
    if (state.registry) {
        wl_registry_destroy(state.registry);
        state.registry = nullptr;
    }
    if (state.display) {
        wl_display_disconnect(state.display);
        state.display = nullptr;
    }
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    std::string err;
    if (! parseArgs(argc, argv, args, err)) {
        printHelp(argv[0]);
        if (! err.empty()) std::cerr << "error: " << err << "\n";
        return err.empty() ? 0 : 1;
    }

    WaylandState wayland;
    if (! initWayland(wayland,
                      static_cast<std::uint32_t>(args.width),
                      static_cast<std::uint32_t>(args.height),
                      args.rotation_degrees)) {
        destroyWayland(wayland);
        return 1;
    }

    we_session_t* session = args.cache_path.empty()
        ? we_session_create()
        : we_session_create_with_cache_path(args.cache_path.c_str());
    if (! session) {
        std::cerr << "we_session_create failed\n";
        destroyWayland(wayland);
        return 1;
    }
    wayland.session = session;

    we_source_v1 source {};
    source.size = static_cast<std::uint32_t>(offsetof(we_source_v1, speed));
    source.version = 1;
    source.uri = args.uri.c_str();
    source.assets_uri = args.assets_uri.c_str();
    source.fps = args.fps;
    if (const std::int32_t r = we_session_set_source(session, &source); r != 0) {
        std::cerr << "we_session_set_source failed: " << r << "\n";
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    we_render_config_v1 config {};
    config.size = sizeof(config);
    config.version = 1;
    config.width = wayland.render_width;
    config.height = wayland.render_height;
    config.fill_mode = static_cast<we_fill_mode_v1>(args.fill_mode);
    config.rotation_degrees = args.rotation_degrees;
    // NVIDIA GPUs render using a hardware-specific pixel layout that AMD/Intel
    // GPUs do not understand.  vkGetMemoryFdKHR on NVIDIA produces dmabufs that
    // are tied to the nvidia-drm device; the compositor (running on the iGPU)
    // cannot import them via PRIME because NVIDIA's Vulkan driver does not
    // implement the linear-layout conversion path for Wayland applications.
    // This is acknowledged by NVIDIA as a known limitation:
    //   https://github.com/NVIDIA/egl-wayland/issues/72
    // When prime-render-offload is detected, always use SHM instead of dmabuf.
    config.prefer_dmabuf = true;
    config.allow_shm_fallback = true;
    if (envVarEnabled("__NV_PRIME_RENDER_OFFLOAD") ||
        envVarEquals("__VK_LAYER_NV_optimus", "NVIDIA_only")) {
        config.prefer_dmabuf = false;
    }
    if (const std::int32_t r = we_session_set_render_config(session, &config); r != 0) {
        std::cerr << "we_session_set_render_config failed: " << r << "\n";
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }
    wayland.bound_render_width = wayland.render_width;
    wayland.bound_render_height = wayland.render_height;

    if (const std::int32_t r = we_session_play(session); r != 0) {
        std::cerr << "we_session_play failed: " << r << "\n";
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    std::uint64_t acquired = 0;
    std::uint64_t presented = 0;
    std::uint64_t spurious_frame_wakes = 0;
    std::int32_t last_acquire_status = 1;
    auto last_log = std::chrono::steady_clock::now();
    const int display_fd = wl_display_get_fd(wayland.display);
    const int frame_ready_fd = we_session_get_frame_ready_fd(session);
    if (frame_ready_fd < 0) {
        std::fprintf(stderr, "sceneviewer-layer: failed to get frame-ready fd\n");
        we_session_stop(session);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }
    const int tick_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tick_fd < 0) {
        std::fprintf(stderr, "sceneviewer-layer: timerfd_create failed: %s\n", std::strerror(errno));
        we_session_stop(session);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }
    const std::int64_t tick_interval_ns =
        1000000000LL / static_cast<std::int64_t>(std::max(args.fps, 1));
    itimerspec tick_timer {};
    tick_timer.it_value.tv_sec = tick_interval_ns / 1000000000LL;
    tick_timer.it_value.tv_nsec = tick_interval_ns % 1000000000LL;
    tick_timer.it_interval = tick_timer.it_value;
    if (::timerfd_settime(tick_fd, 0, &tick_timer, nullptr) != 0) {
        std::fprintf(stderr, "sceneviewer-layer: timerfd_settime failed: %s\n", std::strerror(errno));
        ::close(tick_fd);
        we_session_stop(session);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    if (we_session_tick(session) != 0) {
        std::fprintf(stderr, "sceneviewer-layer: initial we_session_tick failed\n");
        wayland.running = false;
    }

    while (wayland.running) {
        while (wl_display_dispatch_pending(wayland.display) > 0) {}
        collectReleasedBuffers(wayland);

        bool flush_blocked = false;
        if (wl_display_flush(wayland.display) < 0) {
            if (errno == EAGAIN) {
                flush_blocked = true;
            } else {
                std::fprintf(stderr, "sceneviewer-layer: wl_display_flush failed: %s\n", std::strerror(errno));
                break;
            }
        } else {
            releasePendingSendFds(wayland);
        }
        collectReleasedBuffers(wayland);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(5)) {
            last_log = now;
            const char* acquire_status_text = "ok";
            if (last_acquire_status == 1) acquire_status_text = "no-frame";
            else if (last_acquire_status != 0) acquire_status_text = "error";
            std::fprintf(stderr,
                         "sceneviewer-layer: acquired=%lu presented=%lu spurious_frame_wakes=%lu last_acquire_status=%s(%d)\n",
                         static_cast<unsigned long>(acquired),
                         static_cast<unsigned long>(presented),
                         static_cast<unsigned long>(spurious_frame_wakes),
                         acquire_status_text,
                         last_acquire_status);
        }

        pollfd pfds[3] {};
        pfds[0].fd = display_fd;
        pfds[0].events = POLLIN | (flush_blocked ? POLLOUT : 0);
        pfds[1].fd = frame_ready_fd;
        pfds[1].events = POLLIN;
        pfds[2].fd = tick_fd;
        pfds[2].events = POLLIN;
        const int poll_result = ::poll(pfds, 3, -1);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "sceneviewer-layer: poll failed: %s\n", std::strerror(errno));
            break;
        }
        if ((pfds[0].revents & (POLLERR | POLLHUP)) != 0) {
            std::fprintf(stderr, "sceneviewer-layer: Wayland connection closed\n");
            break;
        }
        if ((pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::fprintf(stderr, "sceneviewer-layer: frame-ready fd closed\n");
            break;
        }
        if ((pfds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::fprintf(stderr, "sceneviewer-layer: tick timer fd closed\n");
            break;
        }
        if ((pfds[0].revents & POLLIN) != 0) {
            if (wl_display_dispatch(wayland.display) < 0) {
                std::fprintf(stderr, "sceneviewer-layer: wl_display_dispatch failed\n");
                break;
            }
            collectReleasedBuffers(wayland);
        }
        if ((pfds[0].revents & POLLOUT) != 0 && wl_display_flush(wayland.display) >= 0) {
            releasePendingSendFds(wayland);
            collectReleasedBuffers(wayland);
        }
        if ((pfds[2].revents & POLLIN) != 0) {
            std::uint64_t expirations = 0;
            while (::read(tick_fd, &expirations, sizeof(expirations)) < 0 && errno == EINTR) {}
            if (we_session_tick(session) != 0) {
                std::fprintf(stderr, "sceneviewer-layer: we_session_tick failed\n");
                wayland.running = false;
                continue;
            }
        }
        if ((pfds[1].revents & POLLIN) != 0) {
            we_frame_v1 frame {};
            frame.size = sizeof(frame);
            frame.version = 1;
            const std::int32_t acquire_result = we_session_acquire_frame(session, &frame);
            last_acquire_status = acquire_result;
            if (acquire_result == 0) {
                ++acquired;
                if (presentFrame(wayland, frame)) ++presented;
                we_frame_release(&frame);
            } else if (acquire_result == 1) {
                ++spurious_frame_wakes;
            } else {
                std::fprintf(stderr,
                             "sceneviewer-layer: we_session_acquire_frame failed: %d\n",
                             acquire_result);
            }
        }
    }

    ::close(tick_fd);

    we_session_stop(session);
    we_session_destroy(session);
    destroyWayland(wayland);
    std::cout << "sceneviewer-layer: presented " << presented << " frame(s) of " << acquired << " acquired\n";
    return 0;
}
