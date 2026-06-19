#pragma once

#include <memory>

namespace wallpaper
{

class Scene;
namespace rg
{
class RenderGraph;
}

std::unique_ptr<rg::RenderGraph> BuildWESceneRenderPlan(Scene&);
std::unique_ptr<rg::RenderGraph> BuildWEScenePipelineWarmupRenderPlan(Scene&);
} // namespace wallpaper
