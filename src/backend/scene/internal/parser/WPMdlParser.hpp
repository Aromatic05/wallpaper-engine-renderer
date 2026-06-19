#pragma once
#include <string>
#include <cstdint>
#include <array>
#include <vector>
#include <memory>
#include <Eigen/Dense>

#include "WPPuppet.hpp"

namespace wallpaper
{

class WPShaderInfo;

namespace wpscene
{
class WPMaterial;
};
namespace fs
{
class VFS;
};

struct WPMdl {
    enum class MeshKind
    {
        Unknown,
        Static,
        StaticImage,
        Puppet,
    };

    i32 mdlv { 13 };
    i32 mdls { 1 };
    i32 mdla { 1 };
    MeshKind kind { MeshKind::Unknown };

    std::string mat_json_file;
    struct StaticVertex {
        // Static Wallpaper Engine model chunks use one canonical runtime layout here:
        // position.xyz, normal.xyz, tangent.xyzw, texcoord.xy, and optional texcoord2.xy. Chunks
        // that omit authored normals, tangents, or the secondary UV channel still keep the same
        // struct with safe defaults so Vulkan attributes and shaders never disagree about offsets
        // inside a model-only mesh.
        std::array<float, 3> position { 0.0f, 0.0f, 0.0f };
        std::array<float, 3> normal { 0.0f, 1.0f, 0.0f };
        std::array<float, 4> tangent4 { 1.0f, 0.0f, 0.0f, 1.0f };
        std::array<float, 2> texcoord { 0.0f, 0.0f };
        std::array<float, 2> texcoord2 { 0.0f, 0.0f };
    };
    struct StaticChunk {
        std::string                         material_json_file;
        // Older static model formats can store a prefixed material table before the geometry bytes.
        // Scene model objects select one of those entries through their `skin` index, so the parser
        // keeps the full table here while material_json_file remains the default/fallback material.
        std::vector<std::string>            material_json_variants;
        std::vector<StaticVertex>           vertexs;
        std::vector<std::array<uint16_t, 3>> indices;
        std::array<float, 3>                bounds_min { 0.0f, 0.0f, 0.0f };
        std::array<float, 3>                bounds_max { 0.0f, 0.0f, 0.0f };
    };
    std::vector<StaticChunk> static_chunks;

    struct Vertex {
        std::array<float, 3>    position;
        std::array<uint32_t, 4> blend_indices;
        std::array<float, 4>    weight;
        std::array<float, 2>    texcoord;
    };
    std::vector<Vertex>                  vertexs;
    std::vector<std::array<uint16_t, 3>> indices;

    // std::vector<Eigen::Matrix<float, 3, 4>> bones;
    std::shared_ptr<WPPuppet> puppet;
    // combo
    // SKINNING = 1
    // BONECOUNT

    // input
    // uvec4 a_BlendIndices
    // vec4 a_BlendWeights
    // uniform mat4x3 g_Bones[BONECOUNT]
};

class SceneMesh;

class WPMdlParser {
public:
    static bool Parse(std::string_view path, fs::VFS&, WPMdl&);
    static bool ParseStaticModel(std::string_view path, fs::VFS&, WPMdl&);

    static void AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl);
    static void AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl);

    static void GenPuppetMesh(SceneMesh& mesh, const WPMdl& mdl);
    static void GenStaticMesh(SceneMesh& mesh, const WPMdl::StaticChunk& chunk);
};

} // namespace wallpaper
