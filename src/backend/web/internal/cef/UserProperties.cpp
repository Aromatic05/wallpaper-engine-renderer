#include "backend/web/internal/cef/UserProperties.hpp"

#include "include/cef_frame.h"

namespace wallpaper
{
namespace
{
std::string buildBridgeApplyCall(std::string payload_expr) {
    std::string snippet =
        "(function(){"
        "  if (typeof window.__weweb_applyUserProperties === 'function') {"
        "    window.__weweb_applyUserProperties(";
    snippet += payload_expr;
    snippet += ");"
               "    return;"
               "  }"
               "  if (typeof window.wallpaperPropertyListener !== 'object') return;"
               "  if (typeof window.wallpaperPropertyListener.applyUserProperties !== 'function') return;"
               "  try {"
               "    window.wallpaperPropertyListener.applyUserProperties(";
    snippet += payload_expr;
    snippet += ");"
               "  } catch (e) {"
               "    console.error('web: applyUserProperties threw:', e);"
               "  }"
               "})();";
    return snippet;
}
} // namespace

std::string BuildPropertyListenerBootstrapSnippet() {
    return
        "(function(){"
        "  if (window.__weweb_property_bridge_installed) return;"
        "  window.__weweb_property_bridge_installed = true;"
        "  var pending = [];"
        "  var pollId = 0;"
        "  var flush = function(){"
        "    var listener = window.wallpaperPropertyListener;"
        "    if (typeof listener !== 'object' || listener === null) return false;"
        "    if (typeof listener.applyUserProperties !== 'function') return false;"
        "    while (pending.length > 0) {"
        "      var payload = pending.shift();"
        "      try {"
        "        listener.applyUserProperties(payload);"
        "      } catch (e) {"
        "        console.error('web: applyUserProperties threw:', e);"
        "      }"
        "    }"
        "    if (pollId) { window.clearInterval(pollId); pollId = 0; }"
        "    return true;"
        "  };"
        "  var startPolling = function(){"
        "    if (pollId) return;"
        "    pollId = window.setInterval(function(){ flush(); }, 250);"
        "  };"
        "  window.__weweb_applyUserProperties = function(payload){"
        "    if (typeof payload === 'undefined') return false;"
        "    pending.push(payload);"
        "    if (!flush()) startPolling();"
        "    return true;"
        "  };"
        "  try {"
        "    var currentListener = window.wallpaperPropertyListener;"
        "    Object.defineProperty(window, 'wallpaperPropertyListener', {"
        "      configurable: true,"
        "      enumerable: true,"
        "      get: function(){ return currentListener; },"
        "      set: function(value){ currentListener = value; if (!flush()) startPolling(); }"
        "    });"
        "    if (typeof currentListener !== 'undefined') {"
        "      window.wallpaperPropertyListener = currentListener;"
        "    }"
        "  } catch (e) {"
        "    console.error('web: install property bridge failed:', e);"
        "    startPolling();"
        "  }"
        "  flush();"
        "})();";
}

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
    return buildBridgeApplyCall(user_props_json);
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
    std::string payload = "{\"";
    payload += key;
    payload += "\": ";
    payload += value_json;
    payload += "}";
    return buildBridgeApplyCall(std::move(payload));
}
} // namespace wallpaper
