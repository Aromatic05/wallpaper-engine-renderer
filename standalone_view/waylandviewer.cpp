#include "arg.hpp"
#include "wallpaper/abi/WeRenderer.h"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
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

#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace {

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
    xdg_wm_base*            wm_base { nullptr };
    xdg_surface*            xdg_surface_obj { nullptr };
    xdg_toplevel*           xdg_toplevel_obj { nullptr };
    zwp_linux_dmabuf_v1*    dmabuf { nullptr };
    std::uint32_t           dmabuf_version { 0 };
    bool                    running { true };
    bool                    configured { false };
    std::uint32_t           compositor_version { 0 };
    std::uint32_t           surface_width { 0 };
    std::uint32_t           surface_height { 0 };
    double                  pointer_x { 0.0 };
    double                  pointer_y { 0.0 };
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
    if (! state || ! state->session || state->surface_width == 0 || state->surface_height == 0) return;

    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);
    we_session_send_pointer_event(state->session,
                                  WE_POINTER_MOVE,
                                  static_cast<float>(state->pointer_x / state->surface_width),
                                  static_cast<float>(state->pointer_y / state->surface_height));
}

void onPointerButton(void* data,
                     wl_pointer* /*pointer*/,
                     std::uint32_t /*serial*/,
                     std::uint32_t /*time*/,
                     std::uint32_t button,
                     std::uint32_t button_state) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || ! state->session || state->surface_width == 0 || state->surface_height == 0) return;
    if (button != BTN_LEFT) return;

    const std::uint32_t event_type =
        button_state == WL_POINTER_BUTTON_STATE_PRESSED ? WE_POINTER_DOWN : WE_POINTER_UP;
    we_session_send_pointer_event(state->session,
                                  event_type,
                                  static_cast<float>(state->pointer_x / state->surface_width),
                                  static_cast<float>(state->pointer_y / state->surface_height));
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

void onWmBasePing(void* /*data*/, xdg_wm_base* wm_base, std::uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

constexpr xdg_wm_base_listener kWmBaseListener {
    .ping = onWmBasePing,
};

void onXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;

    xdg_surface_ack_configure(surface, serial);
    state->configured = true;
}

constexpr xdg_surface_listener kXdgSurfaceListener {
    .configure = onXdgSurfaceConfigure,
};

void onToplevelConfigure(void* /*data*/,
                         xdg_toplevel* /*toplevel*/,
                         std::int32_t /*width*/,
                         std::int32_t /*height*/,
                         wl_array* /*states*/) {}

void onToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;
    state->running = false;
}

void onToplevelConfigureBounds(void* /*data*/,
                               xdg_toplevel* /*toplevel*/,
                               std::int32_t /*width*/,
                               std::int32_t /*height*/) {}

void onToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/, wl_array* /*caps*/) {}

constexpr xdg_toplevel_listener kToplevelListener {
    .configure = onToplevelConfigure,
    .close = onToplevelClose,
    .configure_bounds = onToplevelConfigureBounds,
    .wm_capabilities = onToplevelWmCapabilities,
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
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->wm_base =
            static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 7u)));
        xdg_wm_base_add_listener(state->wm_base, &kWmBaseListener, state);
    } else if (std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        state->dmabuf_version = std::min(version, 4u);
        state->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, state->dmabuf_version));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat =
            static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(state->seat, &kSeatListener, state);
    }
}

void onRegistryRemove(void* /*data*/, wl_registry* /*registry*/, std::uint32_t /*name*/) {}

constexpr wl_registry_listener kRegistryListener {
    .global = onRegistryGlobal,
    .global_remove = onRegistryRemove,
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

bool initWayland(WaylandState& state, std::uint32_t width, std::uint32_t height) {
    state.display = wl_display_connect(nullptr);
    if (! state.display) {
        std::fprintf(stderr, "sceneviewer: wl_display_connect failed\n");
        return false;
    }

    state.registry = wl_display_get_registry(state.display);
    if (! state.registry) {
        std::fprintf(stderr, "sceneviewer: wl_display_get_registry failed\n");
        return false;
    }
    wl_registry_add_listener(state.registry, &kRegistryListener, &state);

    if (wl_display_roundtrip(state.display) < 0 || wl_display_roundtrip(state.display) < 0) {
        std::fprintf(stderr, "sceneviewer: initial Wayland roundtrip failed\n");
        return false;
    }
    if (! state.compositor || ! state.wm_base || ! state.dmabuf) {
        std::fprintf(stderr,
                     "sceneviewer: missing required Wayland globals compositor=%p wm_base=%p dmabuf=%p\n",
                     static_cast<void*>(state.compositor),
                     static_cast<void*>(state.wm_base),
                     static_cast<void*>(state.dmabuf));
        return false;
    }
    if (state.dmabuf_version < 2) {
        std::fprintf(stderr,
                     "sceneviewer: zwp_linux_dmabuf_v1 version %u does not support create_immed\n",
                     state.dmabuf_version);
        return false;
    }

    state.surface_width  = width;
    state.surface_height = height;
    state.surface        = wl_compositor_create_surface(state.compositor);
    if (! state.surface) {
        std::fprintf(stderr, "sceneviewer: wl_compositor_create_surface failed\n");
        return false;
    }

    state.xdg_surface_obj = xdg_wm_base_get_xdg_surface(state.wm_base, state.surface);
    state.xdg_toplevel_obj = xdg_surface_get_toplevel(state.xdg_surface_obj);
    if (! state.xdg_surface_obj || ! state.xdg_toplevel_obj) {
        std::fprintf(stderr, "sceneviewer: xdg surface creation failed\n");
        return false;
    }

    xdg_surface_add_listener(state.xdg_surface_obj, &kXdgSurfaceListener, &state);
    xdg_toplevel_add_listener(state.xdg_toplevel_obj, &kToplevelListener, &state);
    xdg_toplevel_set_title(state.xdg_toplevel_obj, "sceneviewer");
    xdg_toplevel_set_app_id(state.xdg_toplevel_obj, "wallpaper-engine-renderer.sceneviewer");
    xdg_toplevel_set_min_size(
        state.xdg_toplevel_obj, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
    xdg_toplevel_set_max_size(
        state.xdg_toplevel_obj, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));

    wl_region* opaque_region = wl_compositor_create_region(state.compositor);
    if (opaque_region) {
        wl_region_add(opaque_region, 0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
        wl_surface_set_opaque_region(state.surface, opaque_region);
        wl_region_destroy(opaque_region);
    }

    wl_surface_commit(state.surface);
    if (wl_display_roundtrip(state.display) < 0 || ! state.configured) {
        std::fprintf(stderr, "sceneviewer: initial xdg configure failed\n");
        return false;
    }
    return true;
}

std::unique_ptr<WaylandBuffer> createBufferForFrame(WaylandState& state, const we_frame_v1& frame) {
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
            std::fprintf(stderr, "sceneviewer: dup(fd=%d) failed: %s\n", frame.planes[i].fd, std::strerror(errno));
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
        frame.drm_fourcc,
        0);
    zwp_linux_buffer_params_v1_destroy(params);

    if (! buffer) {
        std::fprintf(stderr, "sceneviewer: create_immed returned null wl_buffer\n");
        for (const int fd : send_fds) {
            if (fd >= 0) ::close(fd);
        }
        return nullptr;
    }

    auto entry = std::make_unique<WaylandBuffer>();
    entry->buffer           = buffer;
    entry->pending_send_fds = std::move(send_fds);
    wl_buffer_add_listener(entry->buffer, &kBufferListener, entry.get());
    return entry;
}

bool presentFrame(WaylandState& state, const we_frame_v1& frame) {
    auto entry = createBufferForFrame(state, frame);
    if (! entry) return false;

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

    if (state.pointer) {
        wl_pointer_destroy(state.pointer);
        state.pointer = nullptr;
    }
    if (state.seat) {
        wl_seat_destroy(state.seat);
        state.seat = nullptr;
    }
    if (state.xdg_toplevel_obj) {
        xdg_toplevel_destroy(state.xdg_toplevel_obj);
        state.xdg_toplevel_obj = nullptr;
    }
    if (state.xdg_surface_obj) {
        xdg_surface_destroy(state.xdg_surface_obj);
        state.xdg_surface_obj = nullptr;
    }
    if (state.surface) {
        wl_surface_destroy(state.surface);
        state.surface = nullptr;
    }
    if (state.dmabuf) {
        zwp_linux_dmabuf_v1_destroy(state.dmabuf);
        state.dmabuf = nullptr;
    }
    if (state.wm_base) {
        xdg_wm_base_destroy(state.wm_base);
        state.wm_base = nullptr;
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
    if (! initWayland(wayland, static_cast<std::uint32_t>(args.width), static_cast<std::uint32_t>(args.height))) {
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
    // Only advertise fields up through fps. Passing sizeof(we_source_v1) while leaving the
    // append-only tail zero-initialized would explicitly force speed=0 and volume=0, which
    // freezes scene animation and silences audio compared to the historical standalone viewer.
    source.size       = static_cast<std::uint32_t>(offsetof(we_source_v1, speed));
    source.version    = 1;
    source.uri        = args.uri.c_str();
    source.assets_uri = args.assets_uri.c_str();
    source.fps        = args.fps;
    if (const std::int32_t r = we_session_set_source(session, &source); r != 0) {
        std::cerr << "we_session_set_source failed: " << r << "\n";
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    we_render_config_v1 config {};
    config.size          = sizeof(config);
    config.version       = 1;
    config.width         = static_cast<std::uint32_t>(args.width);
    config.height        = static_cast<std::uint32_t>(args.height);
    config.prefer_dmabuf = true;
    if (const std::int32_t r = we_session_set_render_config(session, &config); r != 0) {
        std::cerr << "we_session_set_render_config failed: " << r << "\n";
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    if (const std::int32_t r = we_session_play(session); r != 0) {
        std::cerr << "we_session_play failed: " << r << "\n";
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    std::uint64_t acquired  = 0;
    std::uint64_t presented = 0;
    std::uint64_t no_frame_polls = 0;
    std::int32_t  last_acquire_status = 1;
    auto          last_log  = std::chrono::steady_clock::now();
    const int     display_fd = wl_display_get_fd(wayland.display);
    constexpr auto kPollInterval = std::chrono::milliseconds(5);

    while (wayland.running) {
        if (we_session_tick(session) != 0) {
            std::fprintf(stderr, "sceneviewer: we_session_tick failed\n");
            break;
        }

        we_frame_v1 frame {};
        frame.size    = sizeof(frame);
        frame.version = 1;
        const std::int32_t acquire_result = we_session_acquire_frame(session, &frame);
        last_acquire_status = acquire_result;
        if (acquire_result == 0) {
            ++acquired;
            if (presentFrame(wayland, frame)) ++presented;
            we_frame_release(&frame);
        } else if (acquire_result == 1) {
            ++no_frame_polls;
        } else {
            std::fprintf(stderr, "sceneviewer: we_session_acquire_frame failed: %d\n", acquire_result);
        }

        bool flush_blocked = false;
        if (wl_display_flush(wayland.display) < 0) {
            if (errno == EAGAIN) {
                flush_blocked = true;
            } else {
                std::fprintf(stderr, "sceneviewer: wl_display_flush failed: %s\n", std::strerror(errno));
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
                         "sceneviewer: acquired=%lu presented=%lu no_frame_polls=%lu last_acquire_status=%s(%d)\n",
                         static_cast<unsigned long>(acquired),
                         static_cast<unsigned long>(presented),
                         static_cast<unsigned long>(no_frame_polls),
                         acquire_status_text,
                         last_acquire_status);
        }

        pollfd pfd {};
        pfd.fd     = display_fd;
        pfd.events = POLLIN | (flush_blocked ? POLLOUT : 0);
        const int poll_result = ::poll(&pfd, 1, static_cast<int>(kPollInterval.count()));
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "sceneviewer: poll failed: %s\n", std::strerror(errno));
            break;
        }
        if (poll_result == 0) {
            while (wl_display_dispatch_pending(wayland.display) > 0) {}
            collectReleasedBuffers(wayland);
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP)) != 0) {
            std::fprintf(stderr, "sceneviewer: Wayland connection closed\n");
            break;
        }
        if ((pfd.revents & POLLIN) != 0) {
            if (wl_display_dispatch(wayland.display) < 0) {
                std::fprintf(stderr, "sceneviewer: wl_display_dispatch failed\n");
                break;
            }
            collectReleasedBuffers(wayland);
        }
        if ((pfd.revents & POLLOUT) != 0 && wl_display_flush(wayland.display) >= 0) {
            releasePendingSendFds(wayland);
            collectReleasedBuffers(wayland);
        }
    }

    we_session_stop(session);
    we_session_destroy(session);
    destroyWayland(wayland);
    std::cout << "sceneviewer: presented " << presented << " frame(s) of " << acquired << " acquired\n";
    return 0;
}
