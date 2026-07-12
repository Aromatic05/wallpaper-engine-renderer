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

_Static_assert(offsetof(we_source_v1, options_json) >= sizeof(struct legacy_we_source_v1),
               "we_source_v1 optional fields must remain append-only");
_Static_assert(offsetof(we_render_config_v1, allow_shm_fallback)
                   > offsetof(we_render_config_v1, prefer_dmabuf),
               "we_render_config_v1 field order changed");
_Static_assert(offsetof(we_render_config_v1, msaa_samples)
                   >= sizeof(struct legacy_we_render_config_v1),
               "we_render_config_v1 optional fields must remain append-only");

int main(void) {
    we_session_t* session = we_session_create();
    if (session == NULL) return 1;

    uint32_t diagnostics_size = 0;
    if (we_session_get_diagnostics_json(session, NULL, &diagnostics_size) != 0) {
        we_session_destroy(session);
        return 2;
    }
    if (diagnostics_size <= 1) {
        we_session_destroy(session);
        return 3;
    }
    const int frame_ready_fd = we_session_get_frame_ready_fd(session);
    if (frame_ready_fd < 0 || (fcntl(frame_ready_fd, F_GETFL) & O_NONBLOCK) == 0) {
        we_session_destroy(session);
        return 4;
    }

    we_session_destroy(session);
    return 0;
}
