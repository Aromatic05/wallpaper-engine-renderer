#pragma once

#include "wallpaper/web/WebTypes.hpp"

#include <filesystem>
#include <optional>

namespace wallpaper::web
{
// Read `<workshop_dir>/project.json` and parse it as a type=web manifest.
// Returns std::nullopt and prints a one-line diagnostic to stderr on
// missing/unreadable file, JSON parse failure, or `type != "web"`. Cannot
// throw — the implementation is hosted under the same exception-disabled
// environment that CEF expects downstream.
std::optional<WebManifestData> LoadWebManifest(const std::filesystem::path& workshop_dir);
} // namespace wallpaper::web
