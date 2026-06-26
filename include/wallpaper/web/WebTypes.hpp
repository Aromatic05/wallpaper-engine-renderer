#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wallpaper
{
// ---------------------------------------------------------------------------
// Web wallpaper types exposed to the rest of the runtime.
//
// These types are deliberately CEF-free so any consumer of the public
// wallpaper include surface can use them without pulling in CEF's libcef_dll
// headers. CEF-specific surfaces (BrowserHost, the OSR/Client/App handlers,
// the V8 audio API) live under src/backend/web/internal/cef/ and are kept
// off the public include path.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// DMA-BUF frame delivered by CEF's OnAcceleratedPaint callback.
// ---------------------------------------------------------------------------
// One plane as delivered by CEF. `fd` is borrowed: it is only valid for the
// lifetime of the synchronous AcceleratedPaintCallback invocation; CEF
// reclaims the underlying buffer when the callback returns. Receivers that
// need to keep the FD past the callback must dup() it.
struct DmaBufPlane {
    int          fd { -1 };
    std::uint32_t stride { 0 };
    std::uint64_t offset { 0 };
    std::uint64_t size { 0 };
};

enum class DmaBufFormat : int
{
    BGRA8_UNORM = 0, // CEF_COLOR_TYPE_BGRA_8888 — chunked layout B,G,R,A
    RGBA8_UNORM = 1, // CEF_COLOR_TYPE_RGBA_8888
};

// Snapshot of a CEF accelerated-paint frame.
// `modifier` is the DRM format modifier; `0x00ffffffffffffff` (= -1 in signed
// 64-bit / `DRM_FORMAT_MOD_INVALID`) means the producer did not pick a
// modifier explicitly — for the cases we observe from CEF this is
// functionally equivalent to `DRM_FORMAT_MOD_LINEAR` (stride matches
// width*bpp exactly, no tiling padding).
struct DmaBufFrame {
    DmaBufPlane  planes[4] {};
    int          plane_count { 0 };
    std::uint64_t modifier { 0 };
    DmaBufFormat format { DmaBufFormat::BGRA8_UNORM };

    // Dimensions of the underlying GPU buffer (= "coded size").
    int coded_width { 0 };
    int coded_height { 0 };
    // Visible portion (typically equals coded for our use case).
    int visible_x { 0 };
    int visible_y { 0 };
    int visible_width { 0 };
    int visible_height { 0 };
};

// Synchronous receiver for accelerated-paint frames. The callback runs on
// the main thread inside CefDoMessageLoopWork. FDs in the delivered frame
// are valid only for the duration of the call.
using AcceleratedPaintCallback = std::function<void(const DmaBufFrame&)>;
using SoftwarePaintCallback =
    std::function<void(const void* buffer, int width, int height, int stride_bytes)>;

// ---------------------------------------------------------------------------
// Parsed <workshop_dir>/project.json for a Wallpaper Engine *web* wallpaper.
// ---------------------------------------------------------------------------
// `user_props_json` holds the verbatim `general.properties` object from
// project.json as a JSON string. The web backend forwards it to the page
// via the wallpaperPropertyListener.applyUserProperties(...) hook without
// further interpretation — the page is responsible for matching the
// {type, value} shape that Wallpaper Engine's own runtime delivers.
//
// `has_user_props` is true iff the source file contained a non-null
// `general.properties` object; consumers should not synthesise an empty
// `{}` injection on their own (some wallpapers detect a no-op injection
// and skip their property-driven setup).
struct WebManifestData {
    std::string title;             // project.json:title (default "Wallpaper")
    std::string entry_html;        // project.json:file  (default "index.html")
    std::string user_props_json;   // raw JSON object (may be empty)
    bool        has_user_props { false };
};
} // namespace wallpaper
