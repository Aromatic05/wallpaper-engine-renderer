#include "WPMdlParser.hpp"
#include "mdl/Section.hpp"
#include "mdl/PuppetSemantics.hpp"
#include "fs/VFS.h"
#include "fs/IBinaryStream.h"
#include "fs/MemBinaryStream.h"
#include "WPCommon.hpp"
#include "utils/Logging.h"
#include "scene/SceneMesh.h"
#include "SpecTexs.hpp"
#include "wpscene/WPMaterial.h"
#include "WPShaderParser.hpp"
#include <algorithm>
#include <limits>

using namespace wallpaper;

namespace
{
constexpr uint32_t kStaticPositionTexcoordFlag = 9;
constexpr uint32_t kStaticNormalFlag           = 11;
constexpr uint32_t kStaticTangentSpaceFlag     = 15;
constexpr uint32_t kStaticTangentSpaceSecondUvFlag = 39;
constexpr uint32_t kStaticPositionTexcoordFloats = 5;
constexpr uint32_t kStaticNormalFloats           = 8;
constexpr uint32_t kStaticTangentSpaceFloats     = 12;
constexpr uint32_t kStaticTangentSpaceSecondUvFloats = 14;
constexpr uint32_t kStaticTriangleIndexBytes     = 2 * 3;

enum class StaticHeaderFieldRole
{
    Reserved,
    MaterialPathCount,
    GeometryChunkCount,
    GeometryAndMaterialPathCount,
};

enum class StaticMaterialPathLayout
{
    InterleavedPerChunk,
    PrefixedSkinVariantTable,
};

struct StaticMdlFormat {
    int32_t                  version { 0 };
    StaticHeaderFieldRole    second_field { StaticHeaderFieldRole::Reserved };
    StaticHeaderFieldRole    third_field { StaticHeaderFieldRole::GeometryChunkCount };
    StaticMaterialPathLayout material_layout { StaticMaterialPathLayout::InterleavedPerChunk };
};

struct StaticMdlHeader {
    uint32_t mdl_flag { 0 };
    uint32_t reserved { 0 };
    uint32_t material_path_count { 0 };
    uint32_t geometry_chunk_count { 0 };
    StaticMaterialPathLayout material_layout { StaticMaterialPathLayout::InterleavedPerChunk };

    bool HasPrefixedMaterialTable() const {
        return material_layout == StaticMaterialPathLayout::PrefixedSkinVariantTable;
    }

    bool UsesSkinVariantMaterials() const {
        return material_layout == StaticMaterialPathLayout::PrefixedSkinVariantTable;
    }
};

constexpr std::array<StaticMdlFormat, 2> kStaticMdlFormats {{
    { 4,
      StaticHeaderFieldRole::MaterialPathCount,
      StaticHeaderFieldRole::GeometryChunkCount,
      StaticMaterialPathLayout::PrefixedSkinVariantTable },
    { 14,
      StaticHeaderFieldRole::Reserved,
      StaticHeaderFieldRole::GeometryAndMaterialPathCount,
      StaticMaterialPathLayout::InterleavedPerChunk },
}};

std::size_t RemainingBytes(const fs::IBinaryStream& file) {
    const auto position = file.Tell();
    const auto size     = file.Size();
    if (position < 0 || size < position) return 0;
    return static_cast<std::size_t>(size - position);
}

bool CanReadBytes(const fs::IBinaryStream& file, std::uint64_t bytes) {
    return bytes <= static_cast<std::uint64_t>(RemainingBytes(file));
}

bool CanReadSectionBytes(const fs::IBinaryStream& file,
                         const WPMdlSectionHeader& section,
                         std::uint64_t bytes) {
    const auto position = file.Tell();
    return position >= section.payload_offset && position <= section.end_offset
           && bytes <= static_cast<std::uint64_t>(section.end_offset - position);
}

bool ReadSectionCString(fs::IBinaryStream& file,
                        const WPMdlSectionHeader& section,
                        std::string& value,
                        usize maxBytes = 16 * 1024 * 1024) {
    value.clear();
    while (value.size() < maxBytes && CanReadSectionBytes(file, section, 1)) {
        char ch = '\0';
        if (file.Read(&ch, 1) != 1) return false;
        if (ch == '\0') return true;
        value.push_back(ch);
    }
    return false;
}


WPPuppet::PlayMode ToPlayMode(std::string_view m) {
    if (m == "loop" || m.empty()) return WPPuppet::PlayMode::Loop;
    if (m == "mirror") return WPPuppet::PlayMode::Mirror;
    if (m == "single") return WPPuppet::PlayMode::Single;

    LOG_ERROR("unknown puppet animation play mode \"%s\"", m.data());
    return WPPuppet::PlayMode::Loop;
}


uint32_t StaticVertexFloatCount(uint32_t mdl_flag) {
    if (mdl_flag == kStaticPositionTexcoordFlag) return kStaticPositionTexcoordFloats;
    if (mdl_flag == kStaticNormalFlag) return kStaticNormalFloats;
    if (mdl_flag == kStaticTangentSpaceFlag) return kStaticTangentSpaceFloats;
    if (mdl_flag == kStaticTangentSpaceSecondUvFlag) return kStaticTangentSpaceSecondUvFloats;
    return 0;
}

bool StaticVertexHasNormal(uint32_t mdl_flag) {
    return mdl_flag == kStaticNormalFlag || mdl_flag == kStaticTangentSpaceFlag ||
           mdl_flag == kStaticTangentSpaceSecondUvFlag;
}

bool StaticVertexHasTangentSpace(uint32_t mdl_flag) {
    return mdl_flag == kStaticTangentSpaceFlag || mdl_flag == kStaticTangentSpaceSecondUvFlag;
}

bool StaticVertexHasSecondUv(uint32_t mdl_flag) {
    return mdl_flag == kStaticTangentSpaceSecondUvFlag;
}

const StaticMdlFormat* FindStaticMdlFormat(int32_t mdl_version) {
    const auto it = std::find_if(kStaticMdlFormats.begin(),
                                 kStaticMdlFormats.end(),
                                 [mdl_version](const StaticMdlFormat& format) {
                                     return format.version == mdl_version;
                                 });
    return it != kStaticMdlFormats.end() ? &*it : nullptr;
}

void ApplyStaticHeaderField(StaticMdlHeader& header,
                            StaticHeaderFieldRole role,
                            uint32_t value) {
    switch (role) {
    case StaticHeaderFieldRole::Reserved:
        header.reserved = value;
        break;
    case StaticHeaderFieldRole::MaterialPathCount:
        header.material_path_count = value;
        break;
    case StaticHeaderFieldRole::GeometryChunkCount:
        header.geometry_chunk_count = value;
        break;
    case StaticHeaderFieldRole::GeometryAndMaterialPathCount:
        header.geometry_chunk_count = value;
        header.material_path_count  = value;
        break;
    }
}

std::string_view StaticMaterialLayoutName(StaticMaterialPathLayout layout) {
    switch (layout) {
    case StaticMaterialPathLayout::InterleavedPerChunk:
        return "interleaved-per-chunk";
    case StaticMaterialPathLayout::PrefixedSkinVariantTable:
        return "prefixed-skin-variant-table";
    }
    return "unknown";
}

bool ReadStaticMdlHeader(fs::IBinaryStream& f,
                         int32_t            mdl_version,
                         std::string_view   path,
                         StaticMdlHeader&   header) {
    header.mdl_flag = f.ReadUint32();
    const uint32_t second_header_field = f.ReadUint32();
    const uint32_t third_header_field  = f.ReadUint32();

    const auto* format = FindStaticMdlFormat(mdl_version);
    if (format == nullptr) {
        LOG_ERROR("static mdl unsupported header version path='%.*s' version=%d flag=%u raw-field-1=%u "
                  "raw-field-2=%u",
                  static_cast<int>(path.size()),
                  path.data(),
                  mdl_version,
                  header.mdl_flag,
                  second_header_field,
                  third_header_field);
        return false;
    }

    // The static MDL header has only two numeric slots after the vertex flag, but those slots have
    // different meanings across format versions. A small format descriptor keeps the parse policy
    // data-driven and prevents version-specific branches from leaking into the chunk reader.
    header.material_layout = format->material_layout;
    ApplyStaticHeaderField(header, format->second_field, second_header_field);
    ApplyStaticHeaderField(header, format->third_field, third_header_field);

    LOG_INFO("StaticMdlHeader: path='%.*s' version=%d flag=%u reserved=%u material-count=%u "
             "geometry-chunks=%u material-layout=%s",
             static_cast<int>(path.size()),
             path.data(),
             mdl_version,
             header.mdl_flag,
             header.reserved,
             header.material_path_count,
             header.geometry_chunk_count,
             StaticMaterialLayoutName(header.material_layout).data());
    return true;
}

std::vector<std::string> ReadPrefixedStaticMaterialPaths(fs::IBinaryStream& f,
                                                         const StaticMdlHeader& header) {
    std::vector<std::string> material_paths;
    if (! header.HasPrefixedMaterialTable()) return material_paths;

    material_paths.reserve(header.material_path_count);
    for (uint32_t material_index = 0; material_index < header.material_path_count;
         material_index++) {
        // Prefixed material tables belong to the model header, not to individual chunk byte blocks.
        // Reading the whole table up front lets the chunk reader stay focused on geometry bytes and
        // lets the scene material resolver apply the skin index later.
        material_paths.push_back(f.ReadStr());
    }
    return material_paths;
}

std::string ReadStaticChunkMaterialPath(fs::IBinaryStream& f,
                                        const StaticMdlHeader& header,
                                        const std::vector<std::string>& prefixed_material_paths,
                                        uint32_t chunk_index) {
    if (! header.HasPrefixedMaterialTable()) return f.ReadStr();

    // For prefixed material tables, the first entries are valid fallback materials for geometry
    // chunks, while the full table is retained as skin variants on the chunk. The invariant below
    // is validated before parsing begins so this index is stable and version-agnostic.
    return prefixed_material_paths[chunk_index];
}

void UpdateStaticBounds(WPMdl::StaticChunk& chunk) {
    if (chunk.vertexs.empty()) return;

    chunk.bounds_min = chunk.vertexs.front().position;
    chunk.bounds_max = chunk.vertexs.front().position;
    for (const auto& vertex : chunk.vertexs) {
        for (uint i = 0; i < 3; i++) {
            chunk.bounds_min[i] = std::min(chunk.bounds_min[i], vertex.position[i]);
            chunk.bounds_max[i] = std::max(chunk.bounds_max[i], vertex.position[i]);
        }
    }
}

bool ReadStaticChunk(fs::IBinaryStream& f,
                     uint32_t          mdl_flag,
                     std::string       material_json_file,
                     WPMdl::StaticChunk& chunk) {
    const uint32_t vertex_float_count = StaticVertexFloatCount(mdl_flag);
    if (vertex_float_count == 0) {
        LOG_ERROR("static mdl has unknown vertex flag %u before material '%s'",
                  mdl_flag,
                  material_json_file.c_str());
        return false;
    }

    f.ReadInt32(); // Static MDLV0014 chunks carry a reserved zero before the vertex byte block.

    const uint32_t vertex_size = f.ReadUint32();
    const uint32_t vertex_stride = vertex_float_count * sizeof(float);
    if (vertex_size == 0 || vertex_size % vertex_stride != 0) {
        LOG_ERROR("static mdl material '%s' has unsupported vertex byte size %u for stride %u",
                  material_json_file.c_str(),
                  vertex_size,
                  vertex_stride);
        return false;
    }

    chunk.material_json_file = std::move(material_json_file);
    chunk.vertexs.resize(vertex_size / vertex_stride);
    for (auto& vertex : chunk.vertexs) {
        for (auto& v : vertex.position) v = f.ReadFloat();

        if (StaticVertexHasNormal(mdl_flag)) {
            // Formats 11, 15, and 39 store authored normals. The canonical runtime vertex still
            // exposes a deterministic normal fallback for older position/UV-only files, keeping
            // model shader attributes valid without weakening the stricter static MDL parser.
            for (auto& v : vertex.normal) v = f.ReadFloat();
        }

        if (StaticVertexHasTangentSpace(mdl_flag)) {
            for (auto& v : vertex.tangent4) v = f.ReadFloat();
        }

        for (auto& v : vertex.texcoord) v = f.ReadFloat();

        if (StaticVertexHasSecondUv(mdl_flag)) {
            // Arsenal's official static MDL uses flag 39, whose 14-float stride appends a second
            // UV channel after the base texture UV. Its materials enable lightmap sampling, so this
            // channel must be preserved as a real vertex attribute instead of being skipped.
            for (auto& v : vertex.texcoord2) v = f.ReadFloat();
        }
    }

    const uint32_t indices_size = f.ReadUint32();
    if (indices_size == 0 || indices_size % kStaticTriangleIndexBytes != 0) {
        LOG_ERROR("static mdl material '%s' has unsupported index byte size %u",
                  chunk.material_json_file.c_str(),
                  indices_size);
        return false;
    }

    chunk.indices.resize(indices_size / kStaticTriangleIndexBytes);
    for (auto& index : chunk.indices) {
        for (auto& v : index) v = f.ReadUint16();
    }
    UpdateStaticBounds(chunk);
    return true;
}
} // namespace

// bytes * size
constexpr uint32_t singile_vertex  = 4 * (3 + 4 + 4 + 2);
constexpr uint32_t singile_indices = 2 * 3;
constexpr uint32_t std_format_vertex_size_herald_value = 0x01800009;

// number of bytes in an MDAT attachment after the attachment name
constexpr uint32_t mdat_attachment_data_byte_length = 64;

// alternative consts for alternative mdl format
constexpr uint32_t alt_singile_vertex = 4 * (3 + 4 + 4 + 2 + 7);
constexpr uint32_t alt_format_vertex_size_herald_value = 0x0180000F;
constexpr uint32_t static_image_vertex_size_marker      = 0x0000000F;
constexpr uint32_t static_image_singile_vertex          = 4 * (3 + 3 + 4 + 2);

constexpr uint32_t singile_bone_frame = 4 * 9;

Result<WPMdlHeader> WPMdlParser::ParseHeader(std::string_view path, fs::VFS& vfs) {
    const std::string assetPath = "/assets/" + std::string(path);
    auto file = vfs.Open(assetPath);
    if (! file) {
        return Result<WPMdlHeader>::failure(ResultCode::NotFound,
                                            "model file was not found: " + assetPath);
    }
    auto stream = fs::MemBinaryStream(*file);
    return ParseWPMdlHeader(stream);
}

namespace
{
bool AppendGenericMeshToLegacyView(WPMdl& mdl, const WPMdlMesh& mesh) {
    if (mesh.positions.empty()) return false;
    if (mdl.vertexs.size() > std::numeric_limits<uint32_t>::max() - mesh.positions.size()) {
        return false;
    }

    const auto vertexBase = static_cast<uint32_t>(mdl.vertexs.size());
    mdl.vertexs.reserve(mdl.vertexs.size() + mesh.positions.size());
    for (usize index = 0; index < mesh.positions.size(); ++index) {
        WPMdl::Vertex vertex {};
        vertex.position = mesh.positions[index];
        vertex.blend_indices = index < mesh.blend_indices.size()
            ? mesh.blend_indices[index]
            : std::array<uint32_t, 4> { 0, 0, 0, 0 };
        vertex.weight = index < mesh.blend_weights.size()
            ? mesh.blend_weights[index]
            : (! mesh.blend_indices.empty()
                   ? std::array<float, 4> { 1.0f, 0.0f, 0.0f, 0.0f }
                   : std::array<float, 4> { 0.0f, 0.0f, 0.0f, 1.0f });
        vertex.texcoord = index < mesh.texcoords.size()
            ? mesh.texcoords[index]
            : std::array<float, 2> { 0.0f, 0.0f };
        mdl.vertexs.push_back(vertex);
    }

    const auto appendTriangle = [&](const std::array<uint32_t, 3>& triangle) {
        std::array<uint32_t, 3> rebased {};
        for (usize component = 0; component < rebased.size(); ++component) {
            if (triangle[component] > std::numeric_limits<uint32_t>::max() - vertexBase) {
                return false;
            }
            rebased[component] = triangle[component] + vertexBase;
        }
        mdl.indices.push_back(rebased);
        return true;
    };

    if (mesh.parts.empty()) {
        for (const auto& triangle : mesh.indices) {
            if (! appendTriangle(triangle)) return false;
        }
        return true;
    }

    for (const auto& part : mesh.parts) {
        const usize firstTriangle = static_cast<usize>(part.start / 3u);
        const usize triangleCount = static_cast<usize>(part.size / 3u);
        for (usize offset = 0; offset < triangleCount; ++offset) {
            if (! appendTriangle(mesh.indices[firstTriangle + offset])) return false;
        }
    }
    return true;
}

Result<void> ParseGenericMdlGeometry(fs::IBinaryStream& stream, WPMdl& mdl) {
    mdl.meshes.clear();
    mdl.meshes.reserve(mdl.header.mesh_count);
    mdl.vertexs.clear();
    mdl.indices.clear();

    bool hasSkinning = false;
    for (u32 meshIndex = 0; meshIndex < mdl.header.mesh_count; ++meshIndex) {
        auto meshResult = ParseWPMdlMesh(stream, mdl.header);
        if (! meshResult) return Result<void>(meshResult.error());
        hasSkinning = hasSkinning || ! meshResult.value().blend_indices.empty();
        mdl.meshes.push_back(std::move(meshResult.value()));
    }
    for (const auto& mesh : mdl.meshes) {
        if (! AppendGenericMeshToLegacyView(mdl, mesh)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "MDL mesh merge exceeds runtime index range");
        }
    }
    if (mdl.meshes.empty()) {
        return Result<void>::failure(ResultCode::InvalidArgument, "MDL contains no meshes");
    }

    mdl.mat_json_file = mdl.meshes.front().material_json_file;
    mdl.kind = hasSkinning ? WPMdl::MeshKind::Puppet : WPMdl::MeshKind::StaticImage;
    return Result<void>::success();
}

bool ParseLegacySingleMeshGeometry(fs::IBinaryStream& f,
                                   WPMdl& mdl,
                                   bool staticImageMesh,
                                   bool& alternativeFormat) {
    mdl.mat_json_file = f.ReadStr();
    f.ReadInt32(); // mesh flag_a / reserved

    uint32_t current = f.ReadUint32();
    const auto isAlternativeMarker = [&](uint32_t value) {
        return value == alt_format_vertex_size_herald_value
            || (staticImageMesh && value == static_image_vertex_size_marker);
    };
    if (current == 0) {
        alternativeFormat = true;
        while (! isAlternativeMarker(current) && f.Tell() < f.Size()) {
            current = f.ReadUint32();
        }
        if (! isAlternativeMarker(current)) {
            LOG_ERROR("failed to locate alternative vertex herald 0x%08x",
                      alt_format_vertex_size_herald_value);
            return false;
        }
        current = f.ReadUint32();
    } else if (current == std_format_vertex_size_herald_value
               || (staticImageMesh && current == static_image_vertex_size_marker)) {
        current = f.ReadUint32();
    }

    const uint32_t vertexSize = current;
    const uint32_t vertexStride = staticImageMesh
        ? static_image_singile_vertex
        : (alternativeFormat ? alt_singile_vertex : singile_vertex);
    if (vertexSize == 0 || vertexSize % vertexStride != 0 || ! CanReadBytes(f, vertexSize)) {
        LOG_ERROR("unsupported or truncated legacy mdl vertex payload: bytes=%u", vertexSize);
        return false;
    }

    const uint32_t vertexCount = vertexSize / vertexStride;
    mdl.vertexs.resize(vertexCount);
    for (auto& vertex : mdl.vertexs) {
        if (staticImageMesh) {
            for (auto& value : vertex.position) value = f.ReadFloat();
            for (int index = 0; index < 7; ++index) f.ReadFloat();
            vertex.blend_indices = { 0, 0, 0, 0 };
            vertex.weight = { 0.0f, 0.0f, 0.0f, 1.0f };
            for (auto& value : vertex.texcoord) value = f.ReadFloat();
        } else {
            for (auto& value : vertex.position) value = f.ReadFloat();
            if (alternativeFormat) {
                for (int index = 0; index < 7; ++index) f.ReadUint32();
            }
            for (auto& value : vertex.blend_indices) value = f.ReadUint32();
            for (auto& value : vertex.weight) value = f.ReadFloat();
            for (auto& value : vertex.texcoord) value = f.ReadFloat();
        }
    }

    const uint32_t indexBytes = f.ReadUint32();
    if (indexBytes == 0 || indexBytes % singile_indices != 0 || ! CanReadBytes(f, indexBytes)) {
        LOG_ERROR("unsupported or truncated legacy mdl index payload: bytes=%u", indexBytes);
        return false;
    }
    mdl.indices.resize(indexBytes / singile_indices);
    for (auto& triangle : mdl.indices) {
        for (auto& index : triangle) index = f.ReadUint16();
    }
    return true;
}

Result<void> ParseSkeletonSection(fs::IBinaryStream& f,
                                  WPMdl& mdl,
                                  const WPMdlSectionHeader& section) {
    mdl.mdls = section.version;
    if (! CanReadSectionBytes(f, section, 4)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDLS bone-count header");
    }
    const uint16_t bonesNum = f.ReadUint16();
    f.ReadUint16();
    if (bonesNum > 4096) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "implausible MDLS bone count");
    }

    mdl.puppet = std::make_shared<WPPuppet>();
    auto& bones = mdl.puppet->bones;
    bones.resize(bonesNum);
    for (uint32_t index = 0; index < bonesNum; ++index) {
        auto& bone = bones[index];
        if (! ReadSectionCString(f, section, bone.name)
            || ! CanReadSectionBytes(f, section, 12 + 64)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLS bone record");
        }
        f.ReadInt32();
        bone.file_parent = f.ReadUint32();
        if (bone.file_parent >= index && ! bone.noFileParent()) {
            LOG_INFO("mdl bone %u has out-of-order parent index %u, fallback to root",
                     index, bone.file_parent);
            bone.file_parent = WPPuppet::NO_PARENT;
        }
        bone.bind_parent = bone.file_parent;
        bone.anim_parent = bone.file_parent;
        const uint32_t transformBytes = f.ReadUint32();
        if (transformBytes != 64 || ! CanReadSectionBytes(f, section, transformBytes)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "unsupported or truncated MDLS bone matrix");
        }
        for (auto column : bone.local_bind.matrix().colwise()) {
            for (auto& value : column) value = f.ReadFloat();
        }
        if (! ReadSectionCString(f, section, bone.simulation_json)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLS bone simulation JSON");
        }
    }

    if (mdl.mdls > 1) {
        if (! CanReadSectionBytes(f, section, 3)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLS extended header");
        }
        f.ReadInt16();
        const uint8_t hasTransforms = f.ReadUint8();
        const auto transformBytes = static_cast<std::uint64_t>(bonesNum) * 64u;
        if (hasTransforms != 0) {
            if (! CanReadSectionBytes(f, section, transformBytes)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "MDLS transform payload exceeds section bounds");
            }
            for (uint32_t bone = 0; bone < bonesNum; ++bone)
                for (uint32_t value = 0; value < 16; ++value) f.ReadFloat();
        }
        if (! CanReadSectionBytes(f, section, 4)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLS metadata count");
        }
        const uint32_t metadataCount = f.ReadUint32();
        const auto metadataBytes = static_cast<std::uint64_t>(metadataCount) * 12u;
        if (metadataCount > 65536 || ! CanReadSectionBytes(f, section, metadataBytes + 5u)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "MDLS metadata payload exceeds section bounds");
        }
        for (uint32_t index = 0; index < metadataCount; ++index) {
            f.ReadUint32(); f.ReadUint32(); f.ReadUint32();
        }
        f.ReadUint32();
        const uint8_t hasOffsetTransforms = f.ReadUint8();
        const auto offsetBytes = static_cast<std::uint64_t>(bonesNum) * 76u;
        if (hasOffsetTransforms != 0) {
            if (! CanReadSectionBytes(f, section, offsetBytes)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "MDLS offset-transform payload exceeds section bounds");
            }
            for (auto& bone : bones) {
                for (auto& value : bone.file_skin_pivot) value = f.ReadFloat();
                for (auto column : bone.file_skin_matrix.colwise()) {
                    for (auto& value : column) value = f.ReadFloat();
                }
                bone.has_file_skin_pivot = true;
            }
        }
        if (! CanReadSectionBytes(f, section, 1)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLS bone-index flag");
        }
        const uint8_t hasIndices = f.ReadUint8();
        const auto indexBytes = static_cast<std::uint64_t>(bonesNum) * sizeof(uint32_t);
        if (hasIndices != 0) {
            if (! CanReadSectionBytes(f, section, indexBytes)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "MDLS bone-index payload exceeds section bounds");
            }
            for (uint32_t bone = 0; bone < bonesNum; ++bone) f.ReadUint32();
        }
    }
    return SeekToWPMdlSectionEnd(f, section);
}

Result<void> ParseAttachmentSection(fs::IBinaryStream& f,
                                    WPMdl& mdl,
                                    const WPMdlSectionHeader& section) {
    if (! mdl.puppet || ! CanReadSectionBytes(f, section, 2)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDAT attachment count");
    }
    const uint16_t count = f.ReadUint16();
    if (count > 4096) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "implausible MDAT attachment count");
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (! CanReadSectionBytes(f, section, 2)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDAT attachment record");
        }
        WPPuppet::Attachment attachment;
        attachment.bone_index = f.ReadUint16();
        if (attachment.bone_index >= mdl.puppet->bones.size()
            || ! ReadSectionCString(f, section, attachment.name)
            || ! CanReadSectionBytes(f, section, 64)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "invalid or truncated MDAT attachment record");
        }
        for (auto column : attachment.transform.matrix().colwise()) {
            for (auto& value : column) value = f.ReadFloat();
        }
        mdl.puppet->attachments.push_back(std::move(attachment));
    }
    return SeekToWPMdlSectionEnd(f, section);
}

Result<void> ParseAnimationSection(fs::IBinaryStream& f,
                                   WPMdl& mdl,
                                   const WPMdlSectionHeader& section,
                                   bool alternativeFormat) {
    if (! mdl.puppet || ! CanReadSectionBytes(f, section, 4)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDLA animation count");
    }
    mdl.mdla = section.version;
    const uint32_t animationCount = f.ReadUint32();
    if (animationCount > 4096) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "implausible MDLA animation count");
    }
    auto& animations = mdl.puppet->anims;
    animations.resize(animationCount);
    for (auto& animation : animations) {
        animation.id = 0;
        while (animation.id == 0) {
            if (! CanReadSectionBytes(f, section, 4)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDLA animation id");
            }
            animation.id = f.ReadInt32();
        }
        if (animation.id < 0 || ! CanReadSectionBytes(f, section, 4)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "invalid MDLA animation id");
        }
        f.ReadInt32();
        if (! ReadSectionCString(f, section, animation.name)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLA animation name");
        }
        if (animation.name.empty() && ! ReadSectionCString(f, section, animation.name)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLA fallback animation name");
        }
        std::string mode;
        if (! ReadSectionCString(f, section, mode) || ! CanReadSectionBytes(f, section, 16)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLA animation header");
        }
        animation.mode = ToPlayMode(mode);
        animation.fps = f.ReadFloat();
        animation.length = f.ReadInt32();
        f.ReadInt32();
        const uint32_t boneTrackCount = f.ReadUint32();
        if (boneTrackCount > 4096) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "implausible MDLA bone-track count");
        }
        animation.bframes_array.resize(boneTrackCount);
        for (auto& boneFrames : animation.bframes_array) {
            if (! CanReadSectionBytes(f, section, 8)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDLA bone-track header");
            }
            f.ReadInt32();
            const uint32_t byteSize = f.ReadUint32();
            if (byteSize % singile_bone_frame != 0
                || ! CanReadSectionBytes(f, section, byteSize)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "invalid MDLA bone-frame payload size");
            }
            boneFrames.frames.resize(byteSize / singile_bone_frame);
            for (auto& frame : boneFrames.frames) {
                for (auto& value : frame.position) value = f.ReadFloat();
                for (auto& value : frame.angle) value = f.ReadFloat();
                for (auto& value : frame.scale) value = f.ReadFloat();
            }
        }
        if (alternativeFormat) {
            const uint32_t separatorBytes = mdl.mdla >= 3 ? 3u : 2u;
            if (! CanReadSectionBytes(f, section, separatorBytes)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated alternative MDLA separator");
            }
            for (uint32_t index = 0; index < separatorBytes; ++index) f.ReadUint8();
        } else if (mdl.mdla >= 3) {
            if (! CanReadSectionBytes(f, section, 1)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDLA separator");
            }
            f.ReadUint8();
        } else {
            if (! CanReadSectionBytes(f, section, 4)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDLA metadata count");
            }
            const uint32_t metadataCount = f.ReadUint32();
            if (metadataCount > 65536) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "implausible MDLA metadata count");
            }
            for (uint32_t index = 0; index < metadataCount; ++index) {
                if (! CanReadSectionBytes(f, section, 4)) {
                    return Result<void>::failure(ResultCode::InvalidArgument,
                                                 "truncated MDLA metadata record");
                }
                f.ReadFloat();
                std::string metadata;
                if (! ReadSectionCString(f, section, metadata)) {
                    return Result<void>::failure(ResultCode::InvalidArgument,
                                                 "truncated MDLA metadata string");
                }
            }
        }
    }
    return SeekToWPMdlSectionEnd(f, section);
}

Result<void> ParseMorphSection(fs::IBinaryStream& f,
                               WPMdl& mdl,
                               const WPMdlSectionHeader& section) {
    mdl.mdmp = section.version;
    mdl.morph_sections.clear();
    while (f.Tell() < section.end_offset) {
        if (! CanReadSectionBytes(f, section, 10)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDMP event header");
        }
        WPMdl::MorphSection event;
        const uint16_t sectionCount = f.ReadUint16();
        event.event_time = f.ReadFloat();
        event.event_id = f.ReadUint16();
        f.ReadUint16();
        event.sections.resize(sectionCount);
        for (auto& data : event.sections) {
            if (! CanReadSectionBytes(f, section, 8)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDMP section record");
            }
            data.shape_id = f.ReadUint32();
            f.ReadUint32();
            if (! ReadSectionCString(f, section, data.tag)
                || ! CanReadSectionBytes(f, section, 8)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDMP section metadata");
            }
            const uint32_t length = f.ReadUint32();
            data.hash = f.ReadUint32();
            if (length % 6u != 0) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "MDMP vertex payload is not divisible by six");
            }
            const uint32_t vertexCount = length / 6u;
            if (! CanReadSectionBytes(f, section, length)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "MDMP vertex payload exceeds section bounds");
            }
            data.vertices.resize(vertexCount);
            for (auto& vertex : data.vertices)
                for (auto& value : vertex) value = f.ReadUint16();
            if (data.shape_id == 0) {
                if (! CanReadSectionBytes(f, section, length)) {
                    return Result<void>::failure(ResultCode::InvalidArgument,
                                                 "MDMP trailer exceeds section bounds");
                }
                data.trailer.resize(length);
                for (auto& value : data.trailer) value = f.ReadUint8();
            } else {
                const auto trailerBytes = static_cast<std::uint64_t>(vertexCount) * 2u;
                if (! CanReadSectionBytes(f, section, trailerBytes)) {
                    return Result<void>::failure(ResultCode::InvalidArgument,
                                                 "MDMP vertex trailer exceeds section bounds");
                }
                data.vertex_trailers.resize(vertexCount);
                for (auto& value : data.vertex_trailers) value = f.ReadUint16();
            }
        }
        mdl.morph_sections.push_back(std::move(event));
    }
    return SeekToWPMdlSectionEnd(f, section);
}

Result<void> ParseWorldBindSection(fs::IBinaryStream& f,
                                   WPMdl& mdl,
                                   const WPMdlSectionHeader& section) {
    if (! mdl.puppet || ! CanReadSectionBytes(f, section, 4)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDLE payload size");
    }
    mdl.mdle = section.version;
    const uint32_t payloadBytes = f.ReadUint32();
    const auto expected = static_cast<std::uint64_t>(mdl.puppet->bones.size()) * 64u;
    if (payloadBytes != expected || ! CanReadSectionBytes(f, section, payloadBytes)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "MDLE payload size does not match bone count");
    }
    for (auto& bone : mdl.puppet->bones) {
        for (auto column : bone.file_world_bind.matrix().colwise()) {
            for (auto& value : column) value = f.ReadFloat();
        }
        bone.has_file_world_bind = true;
    }
    return SeekToWPMdlSectionEnd(f, section);
}
} // namespace

bool WPMdlParser::ParseStaticModel(std::string_view path, fs::VFS& vfs, WPMdl& mdl) {
    auto str_path = std::string(path);
    auto pfile    = vfs.Open("/assets/" + str_path);
    if (! pfile) {
        LOG_ERROR("static mdl open failed: %s", str_path.c_str());
        return false;
    }

    auto memfile = fs::MemBinaryStream(*pfile);
    auto& f      = memfile;

    mdl = WPMdl {};
    mdl.mdlv = ReadMDLVesion(f);
    if (mdl.mdlv <= 0) {
        LOG_ERROR("static mdl version read failed: %s", str_path.c_str());
        return false;
    }

    StaticMdlHeader header;
    if (! ReadStaticMdlHeader(f, mdl.mdlv, str_path, header)) return false;
    mdl.header.mdlv = mdl.mdlv;
    mdl.header.mdl_flag = header.mdl_flag;
    mdl.header.unk_a = header.reserved;
    mdl.header.mesh_count = header.geometry_chunk_count;

    const uint32_t mdl_flag = header.mdl_flag;
    const uint32_t chunk_count = header.geometry_chunk_count;
    if (chunk_count == 0) {
        LOG_ERROR("static mdl has no chunks: %s", str_path.c_str());
        return false;
    }
    if (header.material_path_count == 0) {
        LOG_ERROR("static mdl has no material paths: %s", str_path.c_str());
        return false;
    }
    if (header.HasPrefixedMaterialTable() && header.material_path_count < chunk_count) {
        LOG_ERROR("static mdl has fewer prefixed material paths than chunks: %s materials=%u chunks=%u",
                  str_path.c_str(),
                  header.material_path_count,
                  chunk_count);
        return false;
    }

    mdl.kind = WPMdl::MeshKind::Static;
    mdl.static_chunks.clear();
    mdl.static_chunks.reserve(chunk_count);

    const auto prefixed_material_paths = ReadPrefixedStaticMaterialPaths(f, header);

    for (uint32_t chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
        std::string material_json_file =
            ReadStaticChunkMaterialPath(f, header, prefixed_material_paths, chunk_index);
        if (material_json_file.empty()) {
            LOG_ERROR("static mdl chunk %u has empty material path: %s",
                      chunk_index,
                      str_path.c_str());
            return false;
        }

        WPMdl::StaticChunk chunk;
        if (! ReadStaticChunk(f, mdl_flag, material_json_file, chunk)) return false;
        if (header.UsesSkinVariantMaterials()) {
            // Keep the variant table on the parsed chunk so WPSceneParser can apply the model
            // object's skin index after sidecar material remapping. This keeps binary parsing
            // independent from scene-object policy while still preserving the authored skin choice.
            chunk.material_json_variants = prefixed_material_paths;
        }
        mdl.static_chunks.push_back(std::move(chunk));
    }

    return true;
}

bool WPMdlParser::Parse(std::string_view path, fs::VFS& vfs, WPMdl& mdl) {
    const auto str_path = std::string(path);
    auto pfile = vfs.Open("/assets/" + str_path);
    if (! pfile) return false;
    auto memfile = fs::MemBinaryStream(*pfile);
    auto& f = memfile;

    auto headerResult = ParseWPMdlHeader(f);
    if (! headerResult) {
        LOG_ERROR("mdl header parse failed for '%s': %s",
                  str_path.c_str(),
                  headerResult.error().message.c_str());
        return false;
    }
    mdl = WPMdl {};
    mdl.header = headerResult.value();
    mdl.mdlv = mdl.header.mdlv;
    const int32_t mdlFlag = static_cast<int32_t>(mdl.header.mdl_flag);
    const idx geometryStart = f.Tell();
    bool alternativeMdlFormat = false;

    auto geometryResult = ParseGenericMdlGeometry(f, mdl);
    if (! geometryResult) {
        if (mdl.header.mesh_count != 1 || ! f.SeekSet(geometryStart)) {
            LOG_ERROR("mdl geometry parse failed for '%s': %s",
                      str_path.c_str(),
                      geometryResult.error().message.c_str());
            return false;
        }

        LOG_INFO("generic mdl geometry parse failed for '%s', trying legacy single-mesh path: %s",
                 str_path.c_str(),
                 geometryResult.error().message.c_str());
        mdl.meshes.clear();
        mdl.vertexs.clear();
        mdl.indices.clear();
        mdl.mat_json_file.clear();
        const bool legacyStaticImage = mdlFlag == static_cast<int32_t>(kStaticPositionTexcoordFlag);
        mdl.kind = legacyStaticImage ? WPMdl::MeshKind::StaticImage
                                     : WPMdl::MeshKind::Puppet;
        if (! ParseLegacySingleMeshGeometry(
                f, mdl, legacyStaticImage, alternativeMdlFormat)) {
            return false;
        }
    }

    const bool staticImageMesh = mdl.kind == WPMdl::MeshKind::StaticImage;
    if (staticImageMesh) {
        // Geometry-only image meshes intentionally have no MDLS/MDLA skeleton sections. Generic
        // multi-mesh files are merged into the runtime draw view while retaining their parsed
        // per-mesh records for future material/mask consumers.
        LOG_INFO("read static image mesh: mdlv=%d meshes=%zu vertices=%zu triangles=%zu",
                 mdl.mdlv,
                 mdl.meshes.empty() ? usize { 1 } : mdl.meshes.size(),
                 mdl.vertexs.size(),
                 mdl.indices.size());
        return true;
    }

    constexpr std::array<std::string_view, 1> skeletonTypes { "MDLS" };
    auto skeletonSection = FindNextWPMdlSection(
        f, skeletonTypes, std::max<idx>(0, f.Size() - f.Tell()));
    if (! skeletonSection) {
        LOG_ERROR("failed to locate a bounded MDLS section for '%s': %s",
                  str_path.c_str(),
                  skeletonSection.error().message.c_str());
        return false;
    }
    auto skeletonResult = ParseSkeletonSection(f, mdl, skeletonSection.value());
    if (! skeletonResult) {
        LOG_ERROR("failed to parse MDLS for '%s': %s",
                  str_path.c_str(),
                  skeletonResult.error().message.c_str());
        return false;
    }

    while (f.Tell() < f.Size()) {
        const idx scanBytes = std::max<idx>(0, f.Size() - f.Tell());
        auto sectionResult = FindNextWPMdlSection(f, {}, scanBytes);
        if (! sectionResult) break;

        const auto section = sectionResult.value();
        Result<void> parseResult = Result<void>::success();
        if (section.Is("MDAT")) {
            parseResult = ParseAttachmentSection(f, mdl, section);
        } else if (section.Is("MDLA")) {
            parseResult = ParseAnimationSection(f, mdl, section, alternativeMdlFormat);
            if (! parseResult && mdl.puppet) {
                // A malformed animation block must not invalidate geometry and bind pose. The
                // bounded section header still lets us recover exactly at the declared end.
                LOG_ERROR("failed to parse MDLA for '%s'; keeping bind pose: %s",
                          str_path.c_str(),
                          parseResult.error().message.c_str());
                mdl.puppet->anims.clear();
                if (! f.SeekSet(section.end_offset)) return false;
                parseResult = Result<void>::success();
            }
        } else if (section.Is("MDMP")) {
            parseResult = ParseMorphSection(f, mdl, section);
        } else if (section.Is("MDLE")) {
            parseResult = ParseWorldBindSection(f, mdl, section);
        } else {
            LOG_INFO("skip unsupported mdl section %s v%d at 0x%llx",
                     section.Type().c_str(),
                     section.version,
                     static_cast<unsigned long long>(section.header_offset));
            parseResult = SeekToWPMdlSectionEnd(f, section);
        }

        if (! parseResult) {
            LOG_ERROR("failed to parse mdl section %s v%d for '%s': %s",
                      section.Type().c_str(),
                      section.version,
                      str_path.c_str(),
                      parseResult.error().message.c_str());
            return false;
        }
    }

    ApplyWPMdlPuppetSemantics(mdl);
    mdl.puppet->prepared();

    LOG_INFO("read puppet: mdlv=%d mdls=%d mdla=%d mdmp=%d mdle=%d bones=%zu anims=%zu",
             mdl.mdlv,
             mdl.mdls,
             mdl.mdla,
             mdl.mdmp,
             mdl.mdle,
             mdl.puppet->bones.size(),
             mdl.puppet->anims.size());
    return true;
}

void WPMdlParser::GenPuppetMesh(SceneMesh& mesh, const WPMdl& mdl,
                                Eigen::Vector3f position_offset) {
    SceneVertexArray vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3 },
                              { WE_IN_BLENDINDICES.data(), VertexType::UINT4 },
                              { WE_IN_BLENDWEIGHTS.data(), VertexType::FLOAT4 },
                              { WE_IN_TEXCOORD.data(), VertexType::FLOAT2 } },
                            mdl.vertexs.size());

    std::array<float, 16> one_vert {};
    auto to_one = [position_offset](const WPMdl::Vertex& in, decltype(one_vert)& out) {
        uint offset = 0;
        const std::array<float, 3> position {
            in.position[0] + position_offset.x(),
            in.position[1] + position_offset.y(),
            in.position[2] + position_offset.z(),
        };
        memcpy(out.data() + 4 * (offset++), position.data(), sizeof(position));
        memcpy(out.data() + 4 * (offset++), in.blend_indices.data(), sizeof(in.blend_indices));
        memcpy(out.data() + 4 * (offset++), in.weight.data(), sizeof(in.weight));
        memcpy(out.data() + 4 * (offset++), in.texcoord.data(), sizeof(in.texcoord));
    };
    for (uint i = 0; i < mdl.vertexs.size(); i++) {
        auto& v = mdl.vertexs[i];
        one_vert.fill(0.0f);
        to_one(v, one_vert);
        vertex.SetVertexs(i, one_vert);
    }
    std::vector<uint32_t> indices;
    indices.reserve(mdl.indices.size() * 3);
    for (const auto& triangle : mdl.indices) {
        indices.insert(indices.end(), triangle.begin(), triangle.end());
    }

    mesh.AddVertexArray(std::move(vertex));
    mesh.AddIndexArray(SceneIndexArray(indices));
}

void WPMdlParser::GenStaticMesh(SceneMesh& mesh, const WPMdl::StaticChunk& chunk) {
    SceneVertexArray vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3 },
                              { WE_IN_NORMAL.data(), VertexType::FLOAT3 },
                              { WE_IN_TANGENT4.data(), VertexType::FLOAT4 },
                              { WE_IN_TEXCOORD.data(), VertexType::FLOAT2 },
                              { WE_IN_TEXCOORDC2.data(), VertexType::FLOAT2 },
                              { WE_IN_TEXCOORDVEC4.data(), VertexType::FLOAT4 } },
                            chunk.vertexs.size());

    // The static model vertex upload intentionally uses the padded SceneVertexArray contract:
    // FLOAT3 attributes reserve four floats, FLOAT4 reserves four, and FLOAT2 reserves four. The
    // filler values stay zero, giving Vulkan a stable 24-float stride while preserving the actual
    // shader-facing attribute sizes reflected from the model shader. Static model shaders are not
    // consistent about UV naming: older variants read a_TexCoord, some tools expose the secondary
    // channel as a_TexCoordC2, and Arsenal's lightmapped generic shader reads both channels packed
    // as a_TexCoordVec4.xyzw. Duplicating the two UV channels keeps all three contracts valid and
    // prevents missing attributes from being silently rebound to offset zero.
    std::array<float, 24> one_vert {};
    for (uint i = 0; i < chunk.vertexs.size(); i++) {
        const auto& v = chunk.vertexs[i];
        one_vert.fill(0.0f);
        memcpy(one_vert.data(), v.position.data(), sizeof(v.position));
        memcpy(one_vert.data() + 4, v.normal.data(), sizeof(v.normal));
        memcpy(one_vert.data() + 8, v.tangent4.data(), sizeof(v.tangent4));
        memcpy(one_vert.data() + 12, v.texcoord.data(), sizeof(v.texcoord));
        memcpy(one_vert.data() + 16, v.texcoord2.data(), sizeof(v.texcoord2));
        memcpy(one_vert.data() + 20, v.texcoord.data(), sizeof(v.texcoord));
        memcpy(one_vert.data() + 22, v.texcoord2.data(), sizeof(v.texcoord2));
        vertex.SetVertexs(i, one_vert);
    }

    std::vector<uint32_t> indices;
    size_t                u16_count = chunk.indices.size() * 3;
    indices.resize(u16_count / 2 + 1);
    memcpy(indices.data(), chunk.indices.data(), u16_count * sizeof(uint16_t));

    mesh.AddVertexArray(std::move(vertex));
    mesh.AddIndexArray(SceneIndexArray(indices));
}

void WPMdlParser::AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl) {
    info.combos[std::string(WE_CB_SKINNING)]  = "1";
    info.combos[std::string(WE_CB_BONECOUNT)] = std::to_string(mdl.puppet->bones.size());
}

void WPMdlParser::AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl) {
    mat.combos[std::string(WE_CB_SKINNING)]  = 1;
    mat.combos[std::string(WE_CB_BONECOUNT)] = (i32)mdl.puppet->bones.size();
    mat.use_puppet          = true;
}
