#pragma once

#include "Format.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wallpaper
{
struct WPMdlMeshPart {
    u32 id { 0 };
    u32 start { 0 };
    u32 size { 0 };
};

struct WPMdlMeshMask {
    u32 leading_a { 0 };
    std::string material_json;
    std::vector<u32> part_ids_a;
    std::vector<u32> part_ids_b;
};

struct WPMdlMesh {
    std::string material_json_file;
    u32 flag_a { 0 };
    bool has_flag_a2_one { false };
    u32 flag { 0 };
    std::array<float, 3> aabb_min {};
    std::array<float, 3> aabb_max {};
    bool has_aabb { false };

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 4>> tangents;
    std::vector<std::array<std::uint8_t, 4>> extra4;
    std::vector<std::array<u32, 4>> blend_indices;
    std::vector<std::array<float, 4>> blend_weights;
    std::vector<std::array<float, 2>> texcoords;
    std::vector<std::array<float, 2>> texcoord2;
    std::vector<std::array<u32, 3>> indices;

    std::vector<std::array<float, 2>> part_uv2;
    std::vector<u32> part_uv2_pad;
    std::vector<WPMdlMeshPart> parts;
    std::vector<WPMdlMeshMask> masks;
};

bool WPMdlUsesUint32Indices(const WPMdlHeader& header, u32 vertexCount);
Result<WPMdlMesh> ParseWPMdlMesh(fs::IBinaryStream& stream,
                                 const WPMdlHeader& header);
} // namespace wallpaper
