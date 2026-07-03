#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_CEF_HOST_API_VERSION 1u

#if defined(__GNUC__) || defined(__clang__)
#define WE_CEF_EXPORT __attribute__((visibility("default")))
#else
#define WE_CEF_EXPORT
#endif

typedef struct WeCefHostHandle WeCefHostHandle;

typedef struct WeCefStringView {
    const char* data;
    size_t      len;
} WeCefStringView;

typedef struct WeCefStringList {
    const char* const* items;
    size_t             count;
} WeCefStringList;

typedef struct WeCefInitOptions {
    uint32_t struct_size;

    const char* resources_dir;
    const char* locales_dir;
    const char* cache_dir;
    const char* browser_subprocess_path;

    int enable_remote_debugging;
    int remote_debugging_port;
    int enable_audio;

    int runtime_profile;
    int preferred_window_system;
    int prefer_accelerated_paint;

    WeCefStringList extra_command_line_switches;
} WeCefInitOptions;

typedef struct WeCefManifestData {
    uint32_t struct_size;

    const char* title;
    const char* entry_html;
    const char* user_props_json;
    int         has_user_props;
} WeCefManifestData;

typedef struct WeCefDmaBufPlane {
    int      fd;
    uint32_t stride;
    uint64_t offset;
    uint64_t size;
} WeCefDmaBufPlane;

typedef struct WeCefDmaBufFrame {
    uint32_t struct_size;

    WeCefDmaBufPlane planes[4];
    int              plane_count;

    uint64_t modifier;
    int      format;

    int coded_width;
    int coded_height;
    int visible_x;
    int visible_y;
    int visible_width;
    int visible_height;
} WeCefDmaBufFrame;

typedef struct WeCefSoftwareFrame {
    uint32_t    struct_size;
    const void* buffer;
    int         width;
    int         height;
    int         stride_bytes;
} WeCefSoftwareFrame;

typedef struct WeCefCallbacks {
    uint32_t struct_size;
    void*    userdata;

    void (*on_accelerated_paint)(void* userdata, const WeCefDmaBufFrame* frame);
    void (*on_software_paint)(void* userdata, const WeCefSoftwareFrame* frame);
} WeCefCallbacks;

typedef struct WeCefHostApi {
    uint32_t version;
    uint32_t struct_size;

    WeCefHostHandle* (*create_host)(void);
    void (*destroy_host)(WeCefHostHandle* host);

    void (*set_callbacks)(WeCefHostHandle* host, const WeCefCallbacks* callbacks);

    int (*init)(WeCefHostHandle* host, const WeCefInitOptions* opts);

    int (*open_wallpaper)(WeCefHostHandle* host,
                          const WeCefManifestData* manifest,
                          const char* workshop_dir,
                          int width,
                          int height);

    void (*on_resize)(WeCefHostHandle* host, int width, int height);
    void (*invalidate)(WeCefHostHandle* host);

    void (*on_mouse_move)(WeCefHostHandle* host, int x, int y, int left_down);
    void (*on_mouse_button)(WeCefHostHandle* host,
                            int x,
                            int y,
                            int cef_button,
                            int down,
                            int click_count);
    void (*on_mouse_wheel)(WeCefHostHandle* host, int x, int y, int delta_x, int delta_y);
    void (*on_key)(WeCefHostHandle* host,
                   int cef_key_event_type,
                   int native_key_code,
                   int windows_key_code,
                   int modifiers,
                   unsigned int unicode_char);
    void (*on_focus)(WeCefHostHandle* host, int gained);

    void (*pump)(WeCefHostHandle* host);

    void (*apply_volume)(WeCefHostHandle* host, float volume);
    void (*set_frame_rate)(WeCefHostHandle* host, int fps);
    void (*set_paused)(WeCefHostHandle* host, int paused);
    void (*apply_user_property)(WeCefHostHandle* host,
                                WeCefStringView key,
                                WeCefStringView value_json);
    void (*push_audio_data)(WeCefHostHandle* host, const float* data, size_t count);

    int  (*should_exit)(WeCefHostHandle* host);
    void (*request_close)(WeCefHostHandle* host);
    void (*shutdown)(WeCefHostHandle* host);

    int (*helper_main)(int argc, char** argv);

    const char* (*last_error)(void);
} WeCefHostApi;

typedef const WeCefHostApi* (*WeCefGetHostApiFn)(uint32_t version);

WE_CEF_EXPORT const WeCefHostApi* we_cef_get_host_api(uint32_t version);

#ifdef __cplusplus
}
#endif
