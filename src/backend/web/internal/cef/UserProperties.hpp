#pragma once

#include <string>

#include "include/cef_browser.h"

namespace wallpaper
{
// Install the bridge that queues property payloads until the page wires
// window.wallpaperPropertyListener.applyUserProperties(...). Injected from
// AppHandler::OnContextCreated so it exists before the page's own scripts.
std::string BuildPropertyListenerBootstrapSnippet();

// Inject the page-side user-properties payload into the main frame. The
// JSON object is forwarded verbatim from WebManifestData::user_props_json —
// it is the raw general.properties object from project.json, preserved with
// its {type, value} shape so the page's own property listener sees the same
// payload WE's own runtime delivers.
//
// When `has_user_props` is false, we deliberately do NOT push an empty `{}`
// patch: some wallpapers detect a no-op injection and skip their
// property-driven setup, which is hostile to a wallpaper that has no
// configured properties at all.
void InjectUserProperties(CefRefPtr<CefBrowser> browser, const std::string& user_props_json,
                          bool has_user_props);

// Build the same payload the live-update path uses. Public so the
// ApplyUserProperty entry in BrowserHost can build single-key patches
// without re-deriving the IIFE wrapper.
std::string BuildPropertyListenerApplySnippet(const std::string& user_props_json);

// Build a single-key applyUserProperties({key: {value: V}}) snippet. The
// BrowserHost uses this for live property updates; the bridge installed by
// BuildPropertyListenerBootstrapSnippet caches the payload if the page has
// not registered its listener yet.
std::string BuildApplyUserPropertySnippet(const std::string& key, const std::string& value_json);
} // namespace wallpaper
