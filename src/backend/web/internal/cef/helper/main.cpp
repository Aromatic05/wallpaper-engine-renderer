#include "backend/web/internal/cef/AppHandler.hpp"

#include "include/cef_app.h"

int main(int argc, char** argv) {
    CefMainArgs args(argc, argv);
    CefRefPtr<wallpaper::AppHandler> app = new wallpaper::AppHandler();
    if (const char* value = std::getenv("WE_CEF_PROFILE")) {
        const std::string profile { value };
        if (profile == "compat" || profile == "compatibility") {
            app->SetRuntimeProfile(wallpaper::WebCefRuntimeProfile::Compatibility);
        } else if (profile == "debug") {
            app->SetRuntimeProfile(wallpaper::WebCefRuntimeProfile::Debug);
        }
    }
    if (const char* value = std::getenv("WE_CEF_WINDOW_SYSTEM")) {
        const std::string system { value };
        if (system == "x11") {
            app->SetPreferredWindowSystem(wallpaper::WebCefWindowSystem::X11);
        } else if (system == "wayland") {
            app->SetPreferredWindowSystem(wallpaper::WebCefWindowSystem::Wayland);
        }
    }
    return CefExecuteProcess(args, app.get(), nullptr);
}
