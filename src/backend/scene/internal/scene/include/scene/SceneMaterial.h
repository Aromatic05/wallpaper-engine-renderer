#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

#include "SceneShader.h"
#include "Type.hpp"

namespace wallpaper
{

struct SceneMaterialCustomShader {
    std::shared_ptr<SceneShader> shader;
    ShaderValues                 constValues;
};

enum class SceneCullMode
{
    None,
    Back,
    Front,
};

enum class SceneModelColorLoadMode
{
    DontCare,
    Load,
    Clear,
};

struct SceneModelRenderState {
    // This optional state is attached only by scene-level 3D model materialization. Keeping it out
    // of ordinary image/effect/text materials prevents the model render-policy defaults from
    // changing the historical 2D scene path.
    // Model passes can target either the main scene buffer, which already has the ordinary pre-pass
    // clear contract, or private offscreen buffers that are sampled later by another material. The
    // color load mode makes that ownership explicit: the first offscreen producer clears to
    // transparent, later producers load and composite, and legacy main-target first passes can keep
    // the historical custom-shader load behavior.
    SceneModelColorLoadMode colorLoadMode { SceneModelColorLoadMode::DontCare };
    bool          depthTest { true };
    bool          depthWrite { true };
    SceneCullMode cullMode { SceneCullMode::Back };
    // Reflection model chunks may be drawn with a negative scale on the floor normal, which changes
    // the transform handedness and reverses triangle winding. Carry that fact as explicit model-only
    // material state so the Vulkan pass can correct culling without changing legacy 2D materials.
    bool          mirroredHandedness { false };
    std::string   outputOverride;
};

struct SceneMaterial {
public:
    SceneMaterial()                     = default;
    SceneMaterial(const SceneMaterial&) = default;
    SceneMaterial(SceneMaterial&& o)
        : name(std::move(o.name)),
          textures(std::move(o.textures)),
          defines(std::move(o.defines)),
          uniformAliases(std::move(o.uniformAliases)),
          hasSprite(o.hasSprite),
          customShader(std::move(o.customShader)),
          blenmode(o.blenmode),
          modelRenderState(o.modelRenderState) {};

    std::string              name;
    std::vector<std::string> textures;
    std::vector<std::string> defines;

    // Wallpaper Engine scripts address shader controls through authored material names such as
    // `raythreshold`, while the compiled shader consumes GLSL uniforms such as `g_Threshold`.
    // Keeping the parser alias table on the runtime material lets script proxies resolve those
    // authored names without depending on project-specific shader source at assignment time.
    Map<std::string, std::string> uniformAliases;

    bool hasSprite { false };

    SceneMaterialCustomShader customShader;
    BlendMode                 blenmode { BlendMode::Disable };
    std::optional<SceneModelRenderState> modelRenderState;
};
} // namespace wallpaper
