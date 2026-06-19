#pragma once

#include <span>
#include <string_view>
#include "scene/Scene.h"
#include "scene/SceneShader.h"
#include "Type.hpp"

namespace wallpaper
{
namespace fs
{
class VFS;
}
using Combos = Map<std::string, std::string>;

// ui material name to gl uniform name
using WPAliasValueDict = Map<std::string, std::string>;

using WPDefaultTexs = std::vector<std::pair<i32, std::string>>;

struct WPShaderInfo {
    Combos           combos;
    ShaderValueMap   svs;
    ShaderValueMap   baseConstSvs;
    WPAliasValueDict alias;
    WPDefaultTexs    defTexs;
};

struct WPPreprocessorInfo {
    Map<std::string, std::string> input; // name to line
    Map<std::string, std::string> output;
    Map<std::string, std::string> uniforms; // non-sampler uniform name to WE type + optional array suffix

    Set<uint> active_tex_slots;
};

struct WPShaderTexInfo {
    bool                enabled { false };
    std::array<bool, 3> composEnabled { false, false, false };
    // Some runtime render targets are meant to be sampled with screen-space UVs reconstructed by
    // the authored shader. The shader preparer adjusts only calls that sample the affected
    // g_TextureN slot, so ordinary textures and unrelated 2D effect targets keep their old UV path.
    bool                screenSpaceSampleYFlip { false };
};

struct WPShaderUnit {
    ShaderType         stage;
    std::string        src;
    WPPreprocessorInfo preprocess_info;
};

class WPShaderParser {
public:
    static std::string PreShaderSrc(fs::VFS&, const std::string& src, WPShaderInfo* pWPShaderInfo,
                                    const std::vector<WPShaderTexInfo>& texs);

    static bool CompileToSpv(std::string_view         scene_id, std::span<WPShaderUnit>,
                             std::vector<ShaderCode>& spvs, fs::VFS&, WPShaderInfo*,
                             std::span<const WPShaderTexInfo>);
};
} // namespace wallpaper
