#include "arg.hpp"
#include "wallpaper/abi/WeRenderer.h"

#include <drm/drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace
{

bool readTextFile(const std::string& path, std::string& content, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (! input) {
        error = "failed to open " + path;
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (! input.eof() && input.fail()) {
        error = "failed to read " + path;
        return false;
    }
    if (content.empty()) {
        error = path + " is empty";
        return false;
    }
    return true;
}

void printSessionDiagnostics(we_session_t* session, const char* context) {
    if (session == nullptr) return;
    std::uint32_t required { 0 };
    if (we_session_get_diagnostics_json(session, nullptr, &required) != 0 || required <= 1) return;
    std::vector<char> diagnostics(required, '\0');
    std::uint32_t     actual = required;
    if (we_session_get_diagnostics_json(session, diagnostics.data(), &actual) != 0) return;
    std::fprintf(stderr,
                 "sceneviewer diagnostics%s%s: %s\n",
                 context != nullptr ? " after " : "",
                 context != nullptr ? context : "",
                 diagnostics.data());
}

void reportAbiFailure(we_session_t* session, const char* operation, std::int32_t result) {
    std::fprintf(stderr, "sceneviewer: %s failed: %d\n", operation, result);
    printSessionDiagnostics(session, operation);
}

bool envVarEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool envVarEquals(const char* name, const char* expected) {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, expected) == 0;
}

bool shouldForceShmForPrimeRunNvidia() {
    return envVarEnabled("__NV_PRIME_RENDER_OFFLOAD") ||
           envVarEquals("__VK_LAYER_NV_optimus", "NVIDIA_only");
}

struct WaylandBuffer {
    wl_buffer*       buffer { nullptr };
    bool             released { false };
    std::vector<int> pending_send_fds;
};

struct DmabufFormatModifierEntry {
    std::uint32_t format { 0 };
    std::uint64_t modifier { 0 };
};

struct DmabufFeedbackState {
    zwp_linux_dmabuf_feedback_v1*          feedback { nullptr };
    we_session_t*                          session { nullptr };
    void*                                  mapped_table { nullptr };
    std::size_t                            mapped_table_size { 0 };
    std::vector<DmabufFormatModifierEntry> format_table;
    std::vector<DmabufFormatModifierEntry> preferred_formats;
};

struct WaylandState {
    wl_display*                                 display { nullptr };
    wl_registry*                                registry { nullptr };
    wl_compositor*                              compositor { nullptr };
    wl_surface*                                 surface { nullptr };
    wl_seat*                                    seat { nullptr };
    wl_pointer*                                 pointer { nullptr };
    xdg_wm_base*                                wm_base { nullptr };
    xdg_surface*                                xdg_surface_obj { nullptr };
    xdg_toplevel*                               xdg_toplevel_obj { nullptr };
    zwp_linux_dmabuf_v1*                        dmabuf { nullptr };
    wl_shm*                                     shm { nullptr };
    std::uint32_t                               dmabuf_version { 0 };
    bool                                        running { true };
    bool                                        configured { false };
    std::uint32_t                               compositor_version { 0 };
    std::uint32_t                               surface_width { 0 };
    std::uint32_t                               surface_height { 0 };
    double                                      pointer_x { 0.0 };
    double                                      pointer_y { 0.0 };
    bool                                        fixed_pointer_position { false };
    float                                       fixed_pointer_x { 0.0f };
    float                                       fixed_pointer_y { 0.0f };
    we_session_t*                               session { nullptr };
    std::vector<std::unique_ptr<WaylandBuffer>> in_flight_buffers;
    DmabufFeedbackState                         surface_feedback;
    std::vector<DmabufFormatModifierEntry>      legacy_dmabuf_formats;
};

void destroyWayland(WaylandState& state);

void onLegacyDmabufFormat(void*, zwp_linux_dmabuf_v1*, std::uint32_t) {}

void onLegacyDmabufModifier(void* data, zwp_linux_dmabuf_v1*, std::uint32_t format,
                            std::uint32_t modifier_hi, std::uint32_t modifier_lo) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;
    const std::uint64_t modifier = (static_cast<std::uint64_t>(modifier_hi) << 32u) | modifier_lo;
    const DmabufFormatModifierEntry entry { format, modifier };
    const auto duplicate = std::find_if(state->legacy_dmabuf_formats.begin(),
                                        state->legacy_dmabuf_formats.end(),
                                        [&entry](const DmabufFormatModifierEntry& current) {
                                            return current.format == entry.format &&
                                                   current.modifier == entry.modifier;
                                        });
    if (duplicate == state->legacy_dmabuf_formats.end()) {
        state->legacy_dmabuf_formats.push_back(entry);
    }
}

constexpr zwp_linux_dmabuf_v1_listener kLegacyDmabufListener {
    .format   = onLegacyDmabufFormat,
    .modifier = onLegacyDmabufModifier,
};

void destroyDmabufFeedback(DmabufFeedbackState& feedback) {
    if (feedback.mapped_table != nullptr && feedback.mapped_table_size != 0) {
        ::munmap(feedback.mapped_table, feedback.mapped_table_size);
        feedback.mapped_table      = nullptr;
        feedback.mapped_table_size = 0;
    }
    if (feedback.feedback != nullptr) {
        zwp_linux_dmabuf_feedback_v1_destroy(feedback.feedback);
        feedback.feedback = nullptr;
    }
    feedback.format_table.clear();
    feedback.preferred_formats.clear();
    feedback.session = nullptr;
}

std::int32_t applyDmabufFormats(we_session_t*                                 session,
                                const std::vector<DmabufFormatModifierEntry>& formats) {
    if (session == nullptr) return 0;
    std::vector<std::uint32_t> fourccs;
    std::vector<std::uint64_t> modifiers;
    fourccs.reserve(formats.size());
    modifiers.reserve(formats.size());
    for (const auto& format : formats) {
        fourccs.push_back(format.format);
        modifiers.push_back(format.modifier);
    }
    return we_session_set_dmabuf_formats(session,
                                         fourccs.empty() ? nullptr : fourccs.data(),
                                         modifiers.empty() ? nullptr : modifiers.data(),
                                         static_cast<std::uint32_t>(fourccs.size()));
}

std::int32_t applyDmabufFeedback(DmabufFeedbackState& feedback) {
    return applyDmabufFormats(feedback.session, feedback.preferred_formats);
}

void onDmabufFeedbackDone(void* data, zwp_linux_dmabuf_feedback_v1*) {
    auto* feedback = static_cast<DmabufFeedbackState*>(data);
    if (! feedback || feedback->session == nullptr) return;
    const std::int32_t result = applyDmabufFeedback(*feedback);
    if (result != 0) {
        reportAbiFailure(feedback->session, "updated DMA-BUF feedback", result);
    }
}

void onDmabufFeedbackFormatTable(void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd,
                                 uint32_t size) {
    auto* feedback = static_cast<DmabufFeedbackState*>(data);
    if (! feedback) {
        if (fd >= 0) ::close(fd);
        return;
    }
    if (feedback->mapped_table != nullptr && feedback->mapped_table_size != 0) {
        ::munmap(feedback->mapped_table, feedback->mapped_table_size);
        feedback->mapped_table      = nullptr;
        feedback->mapped_table_size = 0;
    }
    feedback->format_table.clear();
    if (fd < 0 || size == 0) {
        if (fd >= 0) ::close(fd);
        return;
    }

    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) return;

    feedback->mapped_table      = mapped;
    feedback->mapped_table_size = size;

    const auto* bytes = static_cast<const std::uint8_t*>(mapped);
    for (std::size_t offset = 0; offset + 16 <= size; offset += 16) {
        std::uint32_t format { 0 };
        std::uint64_t modifier { 0 };
        std::memcpy(&format, bytes + offset, sizeof(format));
        std::memcpy(&modifier, bytes + offset + 8, sizeof(modifier));
        feedback->format_table.push_back({ format, modifier });
    }
}

void onDmabufFeedbackMainDevice(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
void onDmabufFeedbackTrancheDone(void*, zwp_linux_dmabuf_feedback_v1*) {}
void onDmabufFeedbackTrancheTargetDevice(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}

void onDmabufFeedbackTrancheFormats(void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* indices) {
    auto* feedback = static_cast<DmabufFeedbackState*>(data);
    if (! feedback || ! indices) return;
    const auto count      = indices->size / sizeof(std::uint16_t);
    auto*      index_data = static_cast<const std::uint16_t*>(indices->data);
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(index_data[i]);
        if (index < feedback->format_table.size()) {
            feedback->preferred_formats.push_back(feedback->format_table[index]);
        }
    }
}

void onDmabufFeedbackTrancheFlags(void*, zwp_linux_dmabuf_feedback_v1*, std::uint32_t) {}

constexpr zwp_linux_dmabuf_feedback_v1_listener kDmabufFeedbackListener {
    .done                  = onDmabufFeedbackDone,
    .format_table          = onDmabufFeedbackFormatTable,
    .main_device           = onDmabufFeedbackMainDevice,
    .tranche_done          = onDmabufFeedbackTrancheDone,
    .tranche_target_device = onDmabufFeedbackTrancheTargetDevice,
    .tranche_formats       = onDmabufFeedbackTrancheFormats,
    .tranche_flags         = onDmabufFeedbackTrancheFlags,
};

void onWlBufferRelease(void* data, wl_buffer* /*buffer*/) {
    auto* entry = static_cast<WaylandBuffer*>(data);
    if (! entry) return;
    entry->released = true;
}

constexpr wl_buffer_listener kBufferListener {
    .release = onWlBufferRelease,
};

bool sendPointerMove(WaylandState& state, float x, float y) {
    if (state.session == nullptr) return false;
    we_input_event_v2 event {};
    event.size      = sizeof(event);
    event.version   = 2;
    event.type      = WE_INPUT_POINTER_MOVE;
    event.pointer_x = x;
    event.pointer_y = y;
    return we_session_send_input_event(state.session, &event) == 0;
}

void onPointerEnter(void* data, wl_pointer* /*pointer*/, std::uint32_t /*serial*/,
                    wl_surface* /*surface*/, wl_fixed_t sx, wl_fixed_t sy) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || state->fixed_pointer_position) return;
    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);
}

void onPointerLeave(void* /*data*/, wl_pointer* /*pointer*/, std::uint32_t /*serial*/,
                    wl_surface* /*surface*/) {}

void onPointerMotion(void* data, wl_pointer* /*pointer*/, std::uint32_t /*time*/, wl_fixed_t sx,
                     wl_fixed_t sy) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || ! state->session || state->surface_width == 0 || state->surface_height == 0)
        return;
    if (state->fixed_pointer_position) {
        sendPointerMove(*state, state->fixed_pointer_x, state->fixed_pointer_y);
        return;
    }

    state->pointer_x = wl_fixed_to_double(sx);
    state->pointer_y = wl_fixed_to_double(sy);
    sendPointerMove(*state,
                    static_cast<float>(state->pointer_x / state->surface_width),
                    static_cast<float>(state->pointer_y / state->surface_height));
}

void onPointerButton(void* data, wl_pointer* /*pointer*/, std::uint32_t /*serial*/,
                     std::uint32_t /*time*/, std::uint32_t button, std::uint32_t button_state) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || ! state->session || state->surface_width == 0 || state->surface_height == 0)
        return;
    if (button != BTN_LEFT) return;

    we_input_event_v2 event {};
    event.size      = sizeof(event);
    event.version   = 2;
    event.type      = button_state == WL_POINTER_BUTTON_STATE_PRESSED ? WE_INPUT_POINTER_DOWN
                                                                      : WE_INPUT_POINTER_UP;
    event.pointer_x = state->fixed_pointer_position
                          ? state->fixed_pointer_x
                          : static_cast<float>(state->pointer_x / state->surface_width);
    event.pointer_y = state->fixed_pointer_position
                          ? state->fixed_pointer_y
                          : static_cast<float>(state->pointer_y / state->surface_height);
    event.button    = 0;
    we_session_send_input_event(state->session, &event);
}

void onPointerAxis(void* data, wl_pointer* /*pointer*/, std::uint32_t /*time*/, std::uint32_t axis,
                   wl_fixed_t value) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || ! state->session || state->surface_width == 0 || state->surface_height == 0)
        return;

    const double raw_delta = std::round(wl_fixed_to_double(value));
    const double clamped_delta =
        std::clamp(raw_delta,
                   static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                   static_cast<double>(std::numeric_limits<std::int32_t>::max()));

    we_input_event_v2 event {};
    event.size      = sizeof(event);
    event.version   = 2;
    event.type      = WE_INPUT_POINTER_WHEEL;
    event.pointer_x = state->fixed_pointer_position
                          ? state->fixed_pointer_x
                          : static_cast<float>(state->pointer_x / state->surface_width);
    event.pointer_y = state->fixed_pointer_position
                          ? state->fixed_pointer_y
                          : static_cast<float>(state->pointer_y / state->surface_height);
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        event.wheel_delta_x = static_cast<std::int32_t>(clamped_delta);
    } else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        event.wheel_delta_y = static_cast<std::int32_t>(clamped_delta);
    } else {
        return;
    }
    we_session_send_input_event(state->session, &event);
}

void onPointerFrame(void* /*data*/, wl_pointer* /*pointer*/) {}

void onPointerAxisSource(void* /*data*/, wl_pointer* /*pointer*/, std::uint32_t /*axis_source*/) {}

void onPointerAxisStop(void* /*data*/, wl_pointer* /*pointer*/, std::uint32_t /*time*/,
                       std::uint32_t /*axis*/) {}

void onPointerAxisDiscrete(void* /*data*/, wl_pointer* /*pointer*/, std::uint32_t /*axis*/,
                           std::int32_t /*discrete*/) {}

constexpr wl_pointer_listener kPointerListener {
    .enter         = onPointerEnter,
    .leave         = onPointerLeave,
    .motion        = onPointerMotion,
    .button        = onPointerButton,
    .axis          = onPointerAxis,
    .frame         = onPointerFrame,
    .axis_source   = onPointerAxisSource,
    .axis_stop     = onPointerAxisStop,
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
    .name         = onSeatName,
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

void onToplevelConfigure(void* /*data*/, xdg_toplevel* /*toplevel*/, std::int32_t /*width*/,
                         std::int32_t /*height*/, wl_array* /*states*/) {}

void onToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state) return;
    state->running = false;
}

void onToplevelConfigureBounds(void* /*data*/, xdg_toplevel* /*toplevel*/, std::int32_t /*width*/,
                               std::int32_t /*height*/) {}

void onToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/, wl_array* /*caps*/) {}

constexpr xdg_toplevel_listener kToplevelListener {
    .configure        = onToplevelConfigure,
    .close            = onToplevelClose,
    .configure_bounds = onToplevelConfigureBounds,
    .wm_capabilities  = onToplevelWmCapabilities,
};

void onRegistryGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                      std::uint32_t version) {
    auto* state = static_cast<WaylandState*>(data);
    if (! state || ! interface) return;

    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        const std::uint32_t bind_version = std::min(version, 4u);
        state->compositor                = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, bind_version));
        state->compositor_version = bind_version;
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 7u)));
        xdg_wm_base_add_listener(state->wm_base, &kWmBaseListener, state);
    } else if (std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        state->dmabuf_version = std::min(version, 4u);
        state->dmabuf         = static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(
            registry, name, &zwp_linux_dmabuf_v1_interface, state->dmabuf_version));
        zwp_linux_dmabuf_v1_add_listener(state->dmabuf, &kLegacyDmabufListener, state);
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = static_cast<wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, std::min(version, 1u)));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(state->seat, &kSeatListener, state);
    }
}

void onRegistryRemove(void* /*data*/, wl_registry* /*registry*/, std::uint32_t /*name*/) {}

constexpr wl_registry_listener kRegistryListener {
    .global        = onRegistryGlobal,
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
    if (! state.compositor || ! state.wm_base || (! state.dmabuf && ! state.shm)) {
        std::fprintf(stderr,
                     "sceneviewer: missing required Wayland globals compositor=%p wm_base=%p "
                     "dmabuf=%p shm=%p\n",
                     static_cast<void*>(state.compositor),
                     static_cast<void*>(state.wm_base),
                     static_cast<void*>(state.dmabuf),
                     static_cast<void*>(state.shm));
        return false;
    }
    if (state.dmabuf && state.dmabuf_version < 2) {
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

    state.xdg_surface_obj  = xdg_wm_base_get_xdg_surface(state.wm_base, state.surface);
    state.xdg_toplevel_obj = xdg_surface_get_toplevel(state.xdg_surface_obj);
    if (! state.xdg_surface_obj || ! state.xdg_toplevel_obj) {
        std::fprintf(stderr, "sceneviewer: xdg surface creation failed\n");
        return false;
    }

    xdg_surface_add_listener(state.xdg_surface_obj, &kXdgSurfaceListener, &state);
    xdg_toplevel_add_listener(state.xdg_toplevel_obj, &kToplevelListener, &state);
    xdg_toplevel_set_title(state.xdg_toplevel_obj, "sceneviewer");
    xdg_toplevel_set_app_id(state.xdg_toplevel_obj, "wallpaper-engine-renderer.sceneviewer");
    xdg_toplevel_set_min_size(state.xdg_toplevel_obj,
                              static_cast<std::int32_t>(width),
                              static_cast<std::int32_t>(height));
    xdg_toplevel_set_max_size(state.xdg_toplevel_obj,
                              static_cast<std::int32_t>(width),
                              static_cast<std::int32_t>(height));
    if (state.dmabuf && state.dmabuf_version >= 4) {
        state.surface_feedback.feedback =
            zwp_linux_dmabuf_v1_get_surface_feedback(state.dmabuf, state.surface);
        if (state.surface_feedback.feedback) {
            zwp_linux_dmabuf_feedback_v1_add_listener(
                state.surface_feedback.feedback, &kDmabufFeedbackListener, &state.surface_feedback);
        }
    }

    wl_surface_commit(state.surface);
    if (wl_display_roundtrip(state.display) < 0 || ! state.configured) {
        std::fprintf(stderr, "sceneviewer: initial xdg configure failed\n");
        return false;
    }
    return true;
}

std::unique_ptr<WaylandBuffer> createBufferForFrame(WaylandState& state, const we_frame_v1& frame) {
    if (frame.kind == WE_FRAME_KIND_SHM) {
        if (! state.shm || frame.planes[0].fd < 0 || frame.shm_stride == 0 || frame.shm_size == 0) {
            return nullptr;
        }
        wl_shm_pool* pool =
            wl_shm_create_pool(state.shm, frame.planes[0].fd, static_cast<int>(frame.shm_size));
        if (! pool) return nullptr;
        wl_buffer* buffer = wl_shm_pool_create_buffer(pool,
                                                      0,
                                                      static_cast<int>(frame.width),
                                                      static_cast<int>(frame.height),
                                                      static_cast<int>(frame.shm_stride),
                                                      WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        if (! buffer) return nullptr;
        auto entry    = std::make_unique<WaylandBuffer>();
        entry->buffer = buffer;
        wl_buffer_add_listener(entry->buffer, &kBufferListener, entry.get());
        return entry;
    }

    if (frame.kind != WE_FRAME_KIND_DMABUF || frame.n_planes == 0 || frame.n_planes > 4)
        return nullptr;
    auto params = zwp_linux_dmabuf_v1_create_params(state.dmabuf);
    if (! params) return nullptr;

    std::vector<int> send_fds;
    send_fds.reserve(frame.n_planes);
    const std::uint32_t modifier_hi = static_cast<std::uint32_t>(frame.drm_modifier >> 32U);
    const std::uint32_t modifier_lo =
        static_cast<std::uint32_t>(frame.drm_modifier & 0xffffffffULL);
    for (std::uint32_t i = 0; i < frame.n_planes; ++i) {
        const int dup_fd = ::dup(frame.planes[i].fd);
        if (dup_fd < 0) {
            std::fprintf(stderr,
                         "sceneviewer: dup(fd=%d) failed: %s\n",
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

    wl_buffer* buffer =
        zwp_linux_buffer_params_v1_create_immed(params,
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

    auto entry              = std::make_unique<WaylandBuffer>();
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
    destroyDmabufFeedback(state.surface_feedback);
    if (state.shm) {
        wl_shm_destroy(state.shm);
        state.shm = nullptr;
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
    Args        args;
    std::string err;
    if (! parseArgs(argc, argv, args, err)) {
        printHelp(argv[0]);
        if (! err.empty()) std::cerr << "error: " << err << "\n";
        return err.empty() ? 0 : 1;
    }

    std::string user_properties_json;
    if (! args.user_properties_path.empty() &&
        ! readTextFile(args.user_properties_path, user_properties_json, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    const std::string source_options = buildSourceOptionsJson(args, user_properties_json);

    WaylandState wayland;
    wayland.fixed_pointer_position = args.fixed_mouse_position;
    wayland.fixed_pointer_x        = args.mouse_x;
    wayland.fixed_pointer_y        = args.mouse_y;
    if (! initWayland(wayland,
                      static_cast<std::uint32_t>(args.width),
                      static_cast<std::uint32_t>(args.height))) {
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
    source.size = source_options.empty() ? static_cast<std::uint32_t>(offsetof(we_source_v1, speed))
                                         : static_cast<std::uint32_t>(sizeof(source));
    source.version    = 1;
    source.uri        = args.uri.c_str();
    source.assets_uri = args.assets_uri.c_str();
    source.fps        = args.fps;
    if (! source_options.empty()) {
        source.speed        = 1.0f;
        source.volume       = 1.0f;
        source.muted        = false;
        source.options_json = source_options.c_str();
    }
    if (const std::int32_t r = we_session_set_source(session, &source); r != 0) {
        reportAbiFailure(session, "we_session_set_source", r);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    if (wayland.dmabuf && wayland.dmabuf_version >= 4) {
        wayland.surface_feedback.session = session;
        const std::int32_t result        = applyDmabufFeedback(wayland.surface_feedback);
        if (result != 0) {
            reportAbiFailure(session, "we_session_set_dmabuf_formats", result);
            we_session_destroy(session);
            wayland.surface_feedback.session = nullptr;
            destroyWayland(wayland);
            return 1;
        }
    } else if (wayland.dmabuf && wayland.dmabuf_version >= 3) {
        const std::int32_t result = applyDmabufFormats(session, wayland.legacy_dmabuf_formats);
        if (result != 0) {
            reportAbiFailure(session, "we_session_set_dmabuf_formats", result);
            we_session_destroy(session);
            destroyWayland(wayland);
            return 1;
        }
    }

    we_render_config_v1 config {};
    config.size               = sizeof(config);
    config.version            = 1;
    config.width              = static_cast<std::uint32_t>(args.width);
    config.height             = static_cast<std::uint32_t>(args.height);
    config.enable_valid_layer = args.enable_valid_layer;
    config.prefer_dmabuf      = ! args.force_shm;
    config.allow_shm_fallback = true;
    config.msaa_samples       = args.msaa_samples;
    if (config.prefer_dmabuf) {
        if (shouldForceShmForPrimeRunNvidia()) {
            std::fprintf(stderr,
                         "sceneviewer: detected prime-run/NVIDIA offload environment, forcing SHM "
                         "fallback\n");
            config.prefer_dmabuf = false;
        }
    }
    if (const std::int32_t r = we_session_set_render_config(session, &config); r != 0) {
        reportAbiFailure(session, "we_session_set_render_config", r);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    if (const std::int32_t r = we_session_play(session); r != 0) {
        reportAbiFailure(session, "we_session_play", r);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }
    if (wayland.fixed_pointer_position &&
        ! sendPointerMove(wayland, wayland.fixed_pointer_x, wayland.fixed_pointer_y)) {
        reportAbiFailure(session, "we_session_send_input_event", -1);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    std::uint64_t acquired             = 0;
    std::uint64_t presented            = 0;
    std::uint64_t spurious_frame_wakes = 0;
    std::int32_t  last_acquire_status  = 1;
    auto          last_log             = std::chrono::steady_clock::now();
    const int     display_fd           = wl_display_get_fd(wayland.display);
    const int     frame_ready_fd       = we_session_get_frame_ready_fd(session);
    if (frame_ready_fd < 0) {
        std::fprintf(stderr, "sceneviewer: failed to get frame-ready fd\n");
        we_session_stop(session);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }
    const int tick_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tick_fd < 0) {
        std::fprintf(stderr, "sceneviewer: timerfd_create failed: %s\n", std::strerror(errno));
        we_session_stop(session);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }
    const std::int64_t tick_interval_ns =
        1000000000LL / static_cast<std::int64_t>(std::max(args.fps, 1));
    itimerspec tick_timer {};
    tick_timer.it_value.tv_sec  = tick_interval_ns / 1000000000LL;
    tick_timer.it_value.tv_nsec = tick_interval_ns % 1000000000LL;
    tick_timer.it_interval      = tick_timer.it_value;
    if (::timerfd_settime(tick_fd, 0, &tick_timer, nullptr) != 0) {
        std::fprintf(stderr, "sceneviewer: timerfd_settime failed: %s\n", std::strerror(errno));
        ::close(tick_fd);
        we_session_stop(session);
        we_session_destroy(session);
        destroyWayland(wayland);
        return 1;
    }

    if (we_session_tick(session) != 0) {
        std::fprintf(stderr, "sceneviewer: initial we_session_tick failed\n");
        wayland.running = false;
    }

    while (wayland.running) {
        while (wl_display_dispatch_pending(wayland.display) > 0) {
        }
        collectReleasedBuffers(wayland);

        bool flush_blocked = false;
        if (wl_display_flush(wayland.display) < 0) {
            if (errno == EAGAIN) {
                flush_blocked = true;
            } else {
                std::fprintf(
                    stderr, "sceneviewer: wl_display_flush failed: %s\n", std::strerror(errno));
                break;
            }
        } else {
            releasePendingSendFds(wayland);
        }
        collectReleasedBuffers(wayland);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(5)) {
            last_log                        = now;
            const char* acquire_status_text = "ok";
            if (last_acquire_status == 1)
                acquire_status_text = "no-frame";
            else if (last_acquire_status != 0)
                acquire_status_text = "error";
            std::fprintf(stderr,
                         "sceneviewer: acquired=%lu presented=%lu spurious_frame_wakes=%lu "
                         "last_acquire_status=%s(%d)\n",
                         static_cast<unsigned long>(acquired),
                         static_cast<unsigned long>(presented),
                         static_cast<unsigned long>(spurious_frame_wakes),
                         acquire_status_text,
                         last_acquire_status);
        }

        pollfd pfds[3] {};
        pfds[0].fd            = display_fd;
        pfds[0].events        = POLLIN | (flush_blocked ? POLLOUT : 0);
        pfds[1].fd            = frame_ready_fd;
        pfds[1].events        = POLLIN;
        pfds[2].fd            = tick_fd;
        pfds[2].events        = POLLIN;
        const int poll_result = ::poll(pfds, 3, -1);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "sceneviewer: poll failed: %s\n", std::strerror(errno));
            break;
        }
        if ((pfds[0].revents & (POLLERR | POLLHUP)) != 0) {
            std::fprintf(stderr, "sceneviewer: Wayland connection closed\n");
            break;
        }
        if ((pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::fprintf(stderr, "sceneviewer: frame-ready fd closed\n");
            break;
        }
        if ((pfds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::fprintf(stderr, "sceneviewer: tick timer fd closed\n");
            break;
        }
        if ((pfds[0].revents & POLLIN) != 0) {
            if (wl_display_dispatch(wayland.display) < 0) {
                std::fprintf(stderr, "sceneviewer: wl_display_dispatch failed\n");
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
            while (::read(tick_fd, &expirations, sizeof(expirations)) < 0 && errno == EINTR) {
            }
            if (const std::int32_t tick_result = we_session_tick(session); tick_result != 0) {
                reportAbiFailure(session, "we_session_tick", tick_result);
                wayland.running = false;
                continue;
            }
            if (wayland.fixed_pointer_position &&
                ! sendPointerMove(wayland, wayland.fixed_pointer_x, wayland.fixed_pointer_y)) {
                reportAbiFailure(session, "we_session_send_input_event", -1);
                wayland.running = false;
                continue;
            }
        }
        if ((pfds[1].revents & POLLIN) != 0) {
            we_frame_v1 frame {};
            frame.size                        = sizeof(frame);
            frame.version                     = 1;
            const std::int32_t acquire_result = we_session_acquire_frame(session, &frame);
            last_acquire_status               = acquire_result;
            if (acquire_result == 0) {
                ++acquired;
                if (presentFrame(wayland, frame)) ++presented;
                we_frame_release(&frame);
            } else if (acquire_result == 1) {
                ++spurious_frame_wakes;
            } else {
                reportAbiFailure(session, "we_session_acquire_frame", acquire_result);
            }
        }
    }

    ::close(tick_fd);

    we_session_stop(session);
    if (args.print_diagnostics) printSessionDiagnostics(session, "viewer shutdown");
    we_session_destroy(session);
    destroyWayland(wayland);
    std::cout << "sceneviewer: presented " << presented << " frame(s) of " << acquired
              << " acquired\n";
    return 0;
}
