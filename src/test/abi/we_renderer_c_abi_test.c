#include "wallpaper/abi/WeRenderer.h"

#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>

struct legacy_we_source_v1 {
    uint32_t size;
    uint32_t version;
    const char* uri;
    const char* assets_uri;
    int32_t fps;
    float speed;
    float volume;
    bool muted;
};

struct legacy_we_render_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    bool enable_valid_layer;
    bool prefer_dmabuf;
    bool allow_shm_fallback;
};

struct legacy_we_frame_v1 {
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
};

_Static_assert(offsetof(we_source_v1, options_json) >= sizeof(struct legacy_we_source_v1),
               "we_source_v1 optional fields must remain append-only");
_Static_assert(offsetof(we_render_config_v1, allow_shm_fallback)
                   > offsetof(we_render_config_v1, prefer_dmabuf),
               "we_render_config_v1 field order changed");
_Static_assert(offsetof(we_render_config_v1, msaa_samples)
                   >= sizeof(struct legacy_we_render_config_v1),
               "we_render_config_v1 optional fields must remain append-only");
_Static_assert(sizeof(struct legacy_we_frame_v1) == 112,
               "legacy we_frame_v1 layout changed");
_Static_assert(offsetof(we_frame_v1, buffer_id) == sizeof(struct legacy_we_frame_v1),
               "we_frame_v1 reuse fields must remain an append-only tail");

int main(void) {
    we_session_t* session = we_session_create();
    if (session == NULL) return 1;

    if (we_session_set_dmabuf_formats(NULL, NULL, NULL, 0) != -1) {
        we_session_destroy(session);
        return 2;
    }
    if (we_session_set_dmabuf_formats(session, NULL, NULL, 1) != -1) {
        we_session_destroy(session);
        return 3;
    }
    if (we_session_set_dmabuf_formats(session, NULL, NULL, 0) != 0) {
        we_session_destroy(session);
        return 4;
    }

    const uint32_t fourccs[]   = { 0x34324241u, 0x34324241u };
    const uint64_t modifiers[] = { 0, 0 };
    if (we_session_set_dmabuf_formats(session, fourccs, modifiers, 2) != 0) {
        we_session_destroy(session);
        return 5;
    }
    const uint32_t invalid_fourcc = 0;
    if (we_session_set_dmabuf_formats(session, &invalid_fourcc, modifiers, 1) != -1) {
        we_session_destroy(session);
        return 6;
    }
    if (we_session_set_dmabuf_formats(session, fourccs, modifiers, 65537) != -1) {
        we_session_destroy(session);
        return 7;
    }

    uint32_t diagnostics_size = 0;
    if (we_session_get_diagnostics_json(session, NULL, &diagnostics_size) != 0) {
        we_session_destroy(session);
        return 8;
    }
    if (diagnostics_size <= 1) {
        we_session_destroy(session);
        return 9;
    }
    const int frame_ready_fd = we_session_get_frame_ready_fd(session);
    if (frame_ready_fd < 0 || (fcntl(frame_ready_fd, F_GETFL) & O_NONBLOCK) == 0) {
        we_session_destroy(session);
        return 10;
    }

    we_session_destroy(session);
    return 0;
}
