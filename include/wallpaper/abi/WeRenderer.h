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

typedef enum we_frame_kind_v1 {
    WE_FRAME_KIND_DMABUF = 1,
    WE_FRAME_KIND_SHM    = 2,
} we_frame_kind_v1;

typedef enum we_fill_mode_v1 {
    WE_FILL_MODE_STRETCH = 0,
    WE_FILL_MODE_ASPECT_FIT = 1,
    WE_FILL_MODE_ASPECT_CROP = 2,
} we_fill_mode_v1;

typedef enum we_input_event_type_v2 {
    WE_INPUT_POINTER_MOVE = 0,
    WE_INPUT_POINTER_DOWN = 1,
    WE_INPUT_POINTER_UP   = 2,
    WE_INPUT_POINTER_WHEEL = 3,
    WE_INPUT_KEY_DOWN     = 4,
    WE_INPUT_KEY_UP       = 5,
    WE_INPUT_FOCUS        = 6,
} we_input_event_type_v2;

typedef struct we_input_event_v2 {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    float pointer_x;
    float pointer_y;
    int32_t button;
    int32_t wheel_delta_x;
    int32_t wheel_delta_y;
    int32_t key_code;
    int32_t native_key_code;
    int32_t modifiers;
    uint32_t unicode_char;
    bool focused;
} we_input_event_v2;

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
    const char* uri;
    // Optional tail fields are append-only. Consumers must set `size`
    // so the library can safely detect which fields are present. If you
    // leave newer tail fields zero-initialized on purpose, keep `size`
    // capped before those fields so legacy defaults are preserved.
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
    // Scene-backend final-output sample count. Zero preserves the legacy 1x path; unsupported
    // counts or devices without sample-rate shading fall back to 1x. Web/video return NotSupported
    // for values above 1.
    uint32_t msaa_samples;
    // Optional scene framing mode. Omitted fields preserve the legacy Aspect Crop default.
    we_fill_mode_v1 fill_mode;
    // Optional clockwise display transform for the exported frame. The producer leaves the
    // pixels in their render orientation; consumers apply this transform when presenting them.
    // Valid values are 0, 90, 180, and 270. Omitted fields preserve 0 degrees.
    uint32_t rotation_degrees;
} we_render_config_v1;

WE_RENDERER_API we_session_t* we_session_create(void);
WE_RENDERER_API we_session_t* we_session_create_with_cache_path(const char* cache_path);
WE_RENDERER_API void          we_session_destroy(we_session_t* session);

WE_RENDERER_API int32_t we_session_set_source(we_session_t* session, const we_source_v1* source);
WE_RENDERER_API int32_t we_session_set_render_config(we_session_t* session,
                                                     const we_render_config_v1* config);
WE_RENDERER_API int32_t we_session_set_user_properties_json(we_session_t* session,
                                                            const char* properties_json);
// Two-call JSON retrieval: pass buffer=NULL to query the required size, including the trailing NUL.
// Returns -2 when the provided buffer is too small and updates *inout_size to the required size.
WE_RENDERER_API int32_t we_session_get_diagnostics_json(we_session_t* session,
                                                        char* buffer,
                                                        uint32_t* inout_size);
WE_RENDERER_API int32_t we_session_play(we_session_t* session);
WE_RENDERER_API int32_t we_session_pause(we_session_t* session);
WE_RENDERER_API int32_t we_session_stop(we_session_t* session);
WE_RENDERER_API int32_t we_session_tick(we_session_t* session);

WE_RENDERER_API int32_t we_session_acquire_frame(we_session_t* session, we_frame_v1* out_frame);
WE_RENDERER_API void    we_frame_release(we_frame_v1* frame);

WE_RENDERER_API int32_t we_session_send_input_event(we_session_t* session,
                                                    const we_input_event_v2* event);

#ifdef __cplusplus
}
#endif
