#include "backend/web/internal/cef/AppHandler.hpp"

#include "include/cef_app.h"

int main(int argc, char** argv) {
    CefMainArgs args(argc, argv);
    CefRefPtr<wallpaper::AppHandler> app = new wallpaper::AppHandler();
    return CefExecuteProcess(args, app.get(), nullptr);
}
