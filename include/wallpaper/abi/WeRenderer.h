#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define WE_RENDERER_API __declspec(dllexport)
#else
#define WE_RENDERER_API __attribute__((visibility("default")))
#endif

typedef struct we_session we_session_t;

typedef enum we_source_kind_v1 {
    WE_SOURCE_KIND_SCENE = 1,
    WE_SOURCE_KIND_WEB   = 2,
    WE_SOURCE_KIND_VIDEO = 3,
} we_source_kind_v1;

typedef enum we_frame_kind_v1 {
    WE_FRAME_KIND_DMABUF = 1,
    WE_FRAME_KIND_SHM    = 2,
} we_frame_kind_v1;

typedef enum we_pointer_event_v1 {
    WE_POINTER_DOWN = 0,
    WE_POINTER_UP   = 1,
    WE_POINTER_MOVE = 2,
} we_pointer_event_v1;

typedef struct we_dmabuf_plane_v1 {
    int32_t  fd;
    uint32_t offset;
    uint32_t stride;
} we_dmabuf_plane_v1;

typedef struct we_frame_v1 {
    uint32_t size;
    uint32_t version;
    we_frame_kind_v1 kind;
    uint32_t width;
    uint32_t height;
    uint32_t drm_fourcc;
    uint64_t drm_modifier;
    uint32_t n_planes;
    uint32_t flags;
    uint64_t serial;
    uint64_t pts_ns;
    uint32_t shm_stride;
    uint32_t shm_size;
    we_dmabuf_plane_v1 planes[4];
} we_frame_v1;

typedef struct we_source_v1 {
    uint32_t size;
    uint32_t version;
    we_source_kind_v1 kind;
    const char* uri;
    // Optional tail fields are append-only. Consumers must set `size`
    // so the library can safely detect which fields are present.
    const char* assets_uri;
    int32_t fps;
    float speed;
    float volume;
    bool muted;
    const char* options_json;
} we_source_v1;

typedef struct we_render_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    bool enable_valid_layer;
    bool prefer_dmabuf;
    bool allow_shm_fallback;
} we_render_config_v1;

// CEF helper-process short-circuit. Must be called from the host's
// main() before any other we_session_* entry point, with the real
// main(int, char**) argv. CEF fork-execs the host binary for
// renderer / zygote / utility processes; those helpers detect
// themselves by reading --type=… from argv and exit early via
// CefExecuteProcess. Passing a stub-built argv disables CEF's
// multi-process model and degrades to single-process. argc <= 0
// or argv == nullptr is a no-op; subsequent CEF Init will fail
// for any non-trivial host.
WE_RENDERER_API int32_t we_runtime_init(int argc, char** argv);

WE_RENDERER_API we_session_t* we_session_create(void);
WE_RENDERER_API void          we_session_destroy(we_session_t* session);

WE_RENDERER_API int32_t we_session_set_source(we_session_t* session, const we_source_v1* source);
WE_RENDERER_API int32_t we_session_set_render_config(we_session_t* session,
                                                     const we_render_config_v1* config);
WE_RENDERER_API int32_t we_session_play(we_session_t* session);
WE_RENDERER_API int32_t we_session_pause(we_session_t* session);
WE_RENDERER_API int32_t we_session_stop(we_session_t* session);
WE_RENDERER_API int32_t we_session_tick(we_session_t* session);

WE_RENDERER_API int32_t we_session_acquire_frame(we_session_t* session, we_frame_v1* out_frame);
WE_RENDERER_API void    we_frame_release(we_frame_v1* frame);

WE_RENDERER_API int32_t we_session_send_pointer_event(we_session_t* session,
                                                      uint32_t type,
                                                      float    x,
                                                      float    y);

#ifdef __cplusplus
}
#endif
