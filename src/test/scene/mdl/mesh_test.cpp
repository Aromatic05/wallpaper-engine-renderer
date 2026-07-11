#include "backend/scene/internal/parser/mdl/Mesh.hpp"
#include "fs/MemBinaryStream.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "mdl mesh test failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

template<typename T>
void AppendPod(Bytes& bytes, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

void AppendCString(Bytes& bytes, std::string_view value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0);
}

void AppendVertex(Bytes& bytes, std::uint32_t flags, std::uint32_t index) {
    AppendPod(bytes, std::array<float, 3> {
        static_cast<float>(index), static_cast<float>(index + 1), 0.0f
    });
    if ((flags & wallpaper::WPMDL_FLAG_NORMAL) != 0) {
        AppendPod(bytes, std::array<float, 3> { 0.0f, 0.0f, 1.0f });
    }
    if ((flags & wallpaper::WPMDL_FLAG_TANGENT) != 0) {
        AppendPod(bytes, std::array<float, 4> { 1.0f, 0.0f, 0.0f, 1.0f });
    }
    if ((flags & wallpaper::WPMDL_FLAG_EXTRA4) != 0) {
        AppendPod(bytes, std::array<std::uint8_t, 4> { 1, 2, 3, 4 });
    }
    if ((flags & wallpaper::WPMDL_FLAG_SKIN_BLEND) != 0) {
        AppendPod(bytes, std::array<std::uint32_t, 4> { 0, 1, 2, 3 });
    }
    if ((flags & wallpaper::WPMDL_FLAG_SKIN_WEIGHT) != 0) {
        AppendPod(bytes, std::array<float, 4> { 1.0f, 0.0f, 0.0f, 0.0f });
    }
    if ((flags & (wallpaper::WPMDL_FLAG_UV | wallpaper::WPMDL_FLAG_UV2)) != 0) {
        AppendPod(bytes, std::array<float, 2> {
            static_cast<float>(index) / 4.0f, 0.5f
        });
    }
    if ((flags & wallpaper::WPMDL_FLAG_UV2) != 0) {
        AppendPod(bytes, std::array<float, 2> { 0.25f, 0.75f });
    }
}

struct MeshOptions {
    bool includePartUv2 { false };
    bool includePart { false };
    bool includeMask { false };
    std::uint32_t partSize { 3 };
    bool invalidIndex { false };
};

Bytes BuildMesh(const wallpaper::WPMdlHeader& header,
                std::uint32_t flags,
                std::uint32_t vertexCount,
                MeshOptions options = {}) {
    Bytes bytes;
    AppendCString(bytes, "materials/test.json");
    AppendPod<std::uint32_t>(bytes, 0);
    if (header.mdlv >= 17) {
        AppendPod(bytes, std::array<float, 3> { -1.0f, -2.0f, -3.0f });
        AppendPod(bytes, std::array<float, 3> { 4.0f, 5.0f, 6.0f });
    }
    if (header.mdlv > 14) AppendPod(bytes, flags);

    const std::uint32_t vertexBytes = wallpaper::WPMdlVertexStride(flags) * vertexCount;
    AppendPod(bytes, vertexBytes);
    for (std::uint32_t index = 0; index < vertexCount; ++index) {
        AppendVertex(bytes, flags, index);
    }

    const bool uint32Indices = wallpaper::WPMdlUsesUint32Indices(header, vertexCount);
    AppendPod<std::uint32_t>(bytes, uint32Indices ? 12 : 6);
    const std::array<std::uint32_t, 3> triangle {
        0,
        vertexCount > 1 ? 1u : 0u,
        options.invalidIndex ? vertexCount : (vertexCount > 2 ? vertexCount - 1 : 0u),
    };
    for (const auto index : triangle) {
        if (uint32Indices) {
            AppendPod(bytes, index);
        } else {
            AppendPod(bytes, static_cast<std::uint16_t>(index));
        }
    }

    if (header.mdlv >= 21) {
        AppendPod<std::uint8_t>(bytes, options.includePartUv2 ? 1 : 0);
        if (options.includePartUv2) {
            AppendPod<std::uint8_t>(bytes, 1);
            AppendPod<std::uint16_t>(bytes, 0);
            AppendPod<std::uint8_t>(bytes, 1);
            AppendPod<std::uint32_t>(bytes, vertexCount * 12);
            for (std::uint32_t index = 0; index < vertexCount; ++index) {
                AppendPod(bytes, std::array<float, 2> { 0.1f, 0.2f });
                AppendPod(bytes, index);
            }
        }

        AppendPod<std::uint8_t>(bytes, options.includePart ? 1 : 0);
        if (options.includePart) {
            AppendPod<std::uint32_t>(bytes, 16);
            AppendPod<std::uint32_t>(bytes, 9);
            AppendPod<std::uint32_t>(bytes, 0);
            AppendPod<std::uint32_t>(bytes, 0);
            AppendPod(bytes, options.partSize);
        }
    }

    if (header.mdlv > 21) {
        AppendPod<std::uint32_t>(bytes, options.includeMask ? 1 : 0);
        if (options.includeMask) {
            AppendPod<std::uint32_t>(bytes, 7);
            AppendPod<std::uint32_t>(bytes, 0);
            AppendCString(bytes, "materials/mask.json");
            AppendPod<std::uint32_t>(bytes, 0);
            AppendPod<std::uint32_t>(bytes, 1);
            AppendPod<std::uint32_t>(bytes, 0);
            AppendPod<std::uint32_t>(bytes, 1);
            AppendPod<std::uint32_t>(bytes, 0);
        }
    }
    return bytes;
}

wallpaper::Result<wallpaper::WPMdlMesh> Parse(Bytes bytes,
                                               const wallpaper::WPMdlHeader& header) {
    wallpaper::fs::MemBinaryStream stream(std::move(bytes));
    return wallpaper::ParseWPMdlMesh(stream, header);
}

void TestPuppetMesh() {
    wallpaper::WPMdlHeader header {
        .mdlv = 13,
        .mdl_flag = 0x01800009u,
        .unk_a = 1,
        .mesh_count = 1,
    };
    auto result = Parse(BuildMesh(header, header.mdl_flag, 3), header);
    Require(result.ok(), "MDLV13 puppet mesh should parse");
    const auto& mesh = result.value();
    Require(mesh.positions.size() == 3, "puppet position count mismatch");
    Require(mesh.blend_indices.size() == 3, "puppet blend index count mismatch");
    Require(mesh.blend_weights.size() == 3, "puppet blend weight count mismatch");
    Require(mesh.texcoords.size() == 3, "puppet texcoord count mismatch");
    Require(mesh.indices.size() == 1 && mesh.indices[0][2] == 2,
            "puppet index data mismatch");
}

void TestExtendedMesh() {
    wallpaper::WPMdlHeader header {
        .mdlv = 17,
        .mdl_flag = wallpaper::WPMDL_FLAG_POSITION,
        .unk_a = 1,
        .mesh_count = 1,
    };
    const std::uint32_t flags = wallpaper::WPMDL_FLAG_POSITION
        | wallpaper::WPMDL_FLAG_NORMAL | wallpaper::WPMDL_FLAG_TANGENT
        | wallpaper::WPMDL_FLAG_EXTRA4 | wallpaper::WPMDL_FLAG_UV2;
    auto result = Parse(BuildMesh(header, flags, 3), header);
    Require(result.ok(), "MDLV17 extended mesh should parse");
    const auto& mesh = result.value();
    Require(mesh.has_aabb && mesh.aabb_min[0] == -1.0f && mesh.aabb_max[2] == 6.0f,
            "MDLV17 bounds mismatch");
    Require(mesh.normals.size() == 3 && mesh.tangents.size() == 3,
            "extended normal/tangent streams missing");
    Require(mesh.extra4.size() == 3 && mesh.texcoord2.size() == 3,
            "extended extra4/UV2 streams missing");
}

void TestPartsAndMasks() {
    wallpaper::WPMdlHeader header {
        .mdlv = 23,
        .mdl_flag = wallpaper::WPMDL_FLAG_POSITION | wallpaper::WPMDL_FLAG_UV,
        .unk_a = 1,
        .mesh_count = 1,
    };
    MeshOptions options;
    options.includePartUv2 = true;
    options.includePart = true;
    options.includeMask = true;
    auto result = Parse(BuildMesh(header, header.mdl_flag, 3, options), header);
    Require(result.ok(), "MDLV23 parts/masks mesh should parse");
    const auto& mesh = result.value();
    Require(mesh.part_uv2.size() == 3 && mesh.part_uv2_pad[2] == 2,
            "MDLV23 part UV2 payload mismatch");
    Require(mesh.parts.size() == 1 && mesh.parts[0].id == 9 && mesh.parts[0].size == 3,
            "MDLV23 part range mismatch");
    Require(mesh.masks.size() == 1
                && mesh.masks[0].material_json == "materials/mask.json"
                && mesh.masks[0].part_ids_a == std::vector<std::uint32_t> { 0 }
                && mesh.masks[0].part_ids_b == std::vector<std::uint32_t> { 0 },
            "MDLV23 mask payload mismatch");
}

void TestUint32Indices() {
    wallpaper::WPMdlHeader header {
        .mdlv = 23,
        .mdl_flag = wallpaper::WPMDL_FLAG_POSITION,
        .unk_a = 1,
        .mesh_count = 1,
    };
    constexpr std::uint32_t vertexCount = 65'537;
    auto result = Parse(BuildMesh(header, header.mdl_flag, vertexCount), header);
    Require(result.ok(), "large MDLV23 mesh should parse uint32 indices");
    Require(result.value().indices[0][2] == vertexCount - 1,
            "uint32 global index was truncated");
}

void TestMalformedMeshes() {
    wallpaper::WPMdlHeader header {
        .mdlv = 21,
        .mdl_flag = wallpaper::WPMDL_FLAG_POSITION,
        .unk_a = 1,
        .mesh_count = 1,
    };
    {
        MeshOptions options;
        options.invalidIndex = true;
        auto result = Parse(BuildMesh(header, header.mdl_flag, 3, options), header);
        Require(! result && result.error().message.find("index exceeds") != std::string::npos,
                "out-of-range index must fail");
    }
    {
        MeshOptions options;
        options.includePart = true;
        options.partSize = 4;
        auto result = Parse(BuildMesh(header, header.mdl_flag, 3, options), header);
        Require(! result && result.error().message.find("part range") != std::string::npos,
                "out-of-range part must fail");
    }
    {
        auto bytes = BuildMesh(header, header.mdl_flag, 3);
        bytes.pop_back();
        auto result = Parse(std::move(bytes), header);
        Require(! result, "truncated modern mesh must fail");
    }
}
} // namespace

int main() {
    TestPuppetMesh();
    TestExtendedMesh();
    TestPartsAndMasks();
    TestUint32Indices();
    TestMalformedMeshes();
    return 0;
}
