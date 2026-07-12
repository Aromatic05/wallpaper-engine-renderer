#pragma once

#include "wallpaper/Result.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>

namespace wallpaper::test
{
Result<nlohmann::json> ProbeWorkshopProject(const std::filesystem::path& projectPath);
} // namespace wallpaper::test
