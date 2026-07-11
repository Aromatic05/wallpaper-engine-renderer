#include "wallpaper/abi/WeRenderer.h"

#include <stddef.h>
#include <stdint.h>

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

_Static_assert(offsetof(we_source_v1, options_json) >= sizeof(struct legacy_we_source_v1),
               "we_source_v1 optional fields must remain append-only");
_Static_assert(offsetof(we_render_config_v1, allow_shm_fallback)
                   > offsetof(we_render_config_v1, prefer_dmabuf),
               "we_render_config_v1 field order changed");

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

    we_session_destroy(session);
    return 0;
}
