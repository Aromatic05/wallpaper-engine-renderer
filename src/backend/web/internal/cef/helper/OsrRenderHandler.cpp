#include "backend/web/internal/cef/helper/OsrRenderHandler.hpp"

namespace wallpaper
{
void OsrRenderHandler::SetViewSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    std::lock_guard lk(mu_);
    view_w_ = width;
    view_h_ = height;
}

void OsrRenderHandler::GetViewRect(CefRefPtr<CefBrowser> /*browser*/, CefRect& rect) {
    std::lock_guard lk(mu_);
    rect.x      = 0;
    rect.y      = 0;
    rect.width  = view_w_;
    rect.height = view_h_;
}

bool OsrRenderHandler::GetScreenInfo(CefRefPtr<CefBrowser> /*browser*/, CefScreenInfo& info) {
    std::lock_guard lk(mu_);
    info.device_scale_factor = 1.0f;
    info.depth               = 32;
    info.depth_per_component = 8;
    info.is_monochrome       = false;
    info.rect.x              = 0;
    info.rect.y              = 0;
    info.rect.width          = view_w_;
    info.rect.height         = view_h_;
    info.available_rect      = info.rect;
    return true;
}

void OsrRenderHandler::OnPaint(CefRefPtr<CefBrowser> /*browser*/, PaintElementType /*type*/,
                               const RectList& /*dirtyRects*/, const void* buffer,
                               int width, int height) {
    if (software_cb_) {
        software_cb_(buffer, width, height, width * 4);
    }
}

void OsrRenderHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> /*browser*/, PaintElementType type,
                                          const RectList& /*dirtyRects*/,
                                          const CefAcceleratedPaintInfo& info) {
    if (type != PET_VIEW) return;
    if (! accel_cb_) return;

    DmaBufFrame frame;
    frame.plane_count = info.plane_count;
    if (frame.plane_count > 4) frame.plane_count = 4;
    for (int i = 0; i < frame.plane_count; ++i) {
        frame.planes[i].fd     = info.planes[i].fd;
        frame.planes[i].stride = info.planes[i].stride;
        frame.planes[i].offset = info.planes[i].offset;
        frame.planes[i].size   = info.planes[i].size;
    }
    frame.modifier       = info.modifier;
    frame.format         = info.format == CEF_COLOR_TYPE_BGRA_8888 ? DmaBufFormat::BGRA8_UNORM
                                                                   : DmaBufFormat::RGBA8_UNORM;
    frame.coded_width    = info.extra.coded_size.width;
    frame.coded_height   = info.extra.coded_size.height;
    frame.visible_x      = info.extra.visible_rect.x;
    frame.visible_y      = info.extra.visible_rect.y;
    frame.visible_width  = info.extra.visible_rect.width;
    frame.visible_height = info.extra.visible_rect.height;

    // Synchronous: callback must finish before this returns; CEF
    // reclaims the DMA-BUF the moment we exit.
    accel_cb_(frame);
}
} // namespace wallpaper
