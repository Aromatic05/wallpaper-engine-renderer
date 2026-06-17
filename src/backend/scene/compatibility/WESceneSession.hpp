#pragma once

#include "backend/scene/compatibility/WESceneOutputTarget.hpp"
#include "backend/scene/compatibility/WESceneSource.hpp"
#include "common/result/Result.hpp"
#include "runtime/WallpaperRuntime.hpp"

#include <memory>
#include <string>

namespace wallpaper
{
SessionConfig MakeWESceneSessionConfig(std::string cachePath = {});

std::unique_ptr<WallpaperSession> CreateWESceneSession(WallpaperRuntime& runtime,
                                                       std::string       cachePath = {});

Result<void> LoadWEScene(WallpaperSession& session, const WESceneSourceConfig& sourceConfig);

Result<std::shared_ptr<WESceneOutputBinding>> BindWESceneOutput(WallpaperSession&    session,
                                                                const RenderInitInfo& renderInitInfo);

Result<void> BindWESceneOutput(WallpaperSession&                           session,
                               const std::shared_ptr<WESceneOutputBinding>& binding);
} // namespace wallpaper
