#include "backend/web/internal/cef/helper/UserProperties.hpp"

#include "include/cef_frame.h"

namespace wallpaper
{
void InjectUserProperties(CefRefPtr<CefBrowser> browser, const std::string& user_props_json,
                          bool has_user_props) {
    if (! browser) return;
    auto frame = browser->GetMainFrame();
    if (! frame) return;
    if (! has_user_props) return;
    auto snippet = BuildPropertyListenerApplySnippet(user_props_json);
    if (snippet.empty()) return;
    frame->ExecuteJavaScript(
        snippet, "wallpaper://internal/inject_user_properties.js", 0);
}
} // namespace wallpaper
