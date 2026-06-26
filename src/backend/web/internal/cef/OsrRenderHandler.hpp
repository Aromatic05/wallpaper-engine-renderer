#pragma once

#include <mutex>

#include "include/cef_render_handler.h"

#include "wallpaper/web/WebTypes.hpp"

namespace wallpaper
{
// CefRenderHandler routing CEF's accelerated-paint output to the consumer
// as platform-neutral DmaBufFrame. Runs on CEF's UI thread which, in
// single-threaded message-loop mode, is the main thread that pumps
// CefDoMessageLoopWork. The accelerated-paint callback is synchronous
// for the lifetime of the CefAcceleratedPaintInfo — receivers that
// need to keep the FDs past the call must dup() them.
class OsrRenderHandler : public CefRenderHandler {
public:
    OsrRenderHandler() = default;

    void SetViewSize(int width, int height);

    // Install the accelerated-paint sink. Must NOT be reset while a
    // frame callback is in flight.
    void SetAcceleratedPaintCallback(AcceleratedPaintCallback cb) { accel_cb_ = std::move(cb); }

    // CefRenderHandler.
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects,
                 const void* buffer, int width, int height) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList&                dirtyRects,
                            const CefAcceleratedPaintInfo& info) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& info) override;

private:
    std::mutex               mu_;
    int                      view_w_ { 1280 };
    int                      view_h_ { 720 };
    AcceleratedPaintCallback accel_cb_;

    IMPLEMENT_REFCOUNTING(OsrRenderHandler);
    DISALLOW_COPY_AND_ASSIGN(OsrRenderHandler);
};
} // namespace wallpaper
