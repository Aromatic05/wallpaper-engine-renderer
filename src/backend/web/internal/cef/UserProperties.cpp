#include "backend/web/internal/cef/UserProperties.hpp"

#include "include/cef_frame.h"

namespace wallpaper
{
namespace
{
// We need to inject a JS literal that may itself contain an arbitrary
// JSON object. The page-side listener convention is:
//
//   window.wallpaperPropertyListener = {
//     applyUserProperties: function(props) { ... }
//   };
//
// We hand it the project.json `general.properties` object verbatim
// (already serialised by LoadWebManifest); each entry preserves its
// `type` and `value` fields, matching what WE's own runtime delivers.
// The wrapper is defensive: if the page hasn't installed the listener
// yet (or installed one with the wrong type), the call is dropped with
// a console-side diagnostic instead of throwing.
const char* kListenerGuard =
    "  if (typeof window.wallpaperPropertyListener !== 'object') return false;"
    "  if (typeof window.wallpaperPropertyListener.applyUserProperties !== 'function') return false;"
    "  try {";

const char* kListenerGuardEnd =
    "  } catch (e) {"
    "    console.error('web: applyUserProperties threw:', e);"
    "    return false;"
    "  }"
    "  return true;";
} // namespace

std::string BuildPropertyListenerApplySnippet(const std::string& user_props_json) {
    // The page side typically registers a listener like:
    //
    //   window.wallpaperPropertyListener = {
    //     applyUserProperties: function(props) { ... }
    //   };
    //
    // When the wallpaper declares no user properties at all, the
    // runtime injects nothing — the listener wrapper itself isn't
    // useful without a payload.
    if (user_props_json.empty()) {
        return {};
    }
    std::string snippet =
        "(function(){"
        "  if (typeof window.wallpaperPropertyListener !== 'object') return;"
        "  if (typeof window.wallpaperPropertyListener.applyUserProperties !== 'function') return;"
        "  try {"
        "    window.wallpaperPropertyListener.applyUserProperties(";
    snippet += user_props_json;
    snippet += "    );"
               "  } catch (e) {"
               "    console.error('web: applyUserProperties threw:', e);"
               "  }"
               "})();";
    return snippet;
}

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

std::string BuildApplyUserPropertySnippet(const std::string& key, const std::string& value_json) {
    // The BrowserHost::ApplyUserProperty entry builds this snippet and
    // feeds it to frame->ExecuteJavaScript on every live update.
    // The value_json is expected to be a JSON-encoded object (e.g.
    // `{"value": 0.8, "type": "slider"}`); we splice it directly so
    // the page sees the same {type, value} envelope it would on
    // project.json load.
    std::string snippet =
        "(function(){"
        "  if (typeof window.wallpaperPropertyListener !== 'object') return;"
        "  if (typeof window.wallpaperPropertyListener.applyUserProperties !== 'function') return;"
        "  try {"
        "    window.wallpaperPropertyListener.applyUserProperties({\"";
    snippet += key;
    snippet += "\": ";
    snippet += value_json;
    snippet += "});"
               "  } catch (e) {"
               "    console.error('web: applyUserProperties patch threw:', e);"
               "  }"
               "})();";
    return snippet;
}
} // namespace wallpaper
