#pragma once
#include <memory>

namespace wallpaper
{

class Scene;
namespace rg
{
class RenderGraph;
}

std::unique_ptr<rg::RenderGraph> sceneToRenderGraph(Scene&);
std::unique_ptr<rg::RenderGraph> sceneToPipelineWarmupRenderGraph(Scene&);
} // namespace wallpaper
