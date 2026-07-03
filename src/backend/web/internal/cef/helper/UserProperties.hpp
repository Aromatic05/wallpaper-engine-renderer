#pragma once

#include "backend/web/internal/cef/PropertyBridge.hpp"

#include <string>

#include "include/cef_browser.h"

namespace wallpaper
{
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
} // namespace wallpaper
