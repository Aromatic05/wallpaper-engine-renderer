#include "WPMdlParser.hpp"
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

bool ValidateElementCount(const fs::IBinaryStream& file, std::string_view label,
                          std::uint64_t count, std::uint64_t minimum_bytes_per_element,
                          std::uint64_t hard_limit) {
    if (count > hard_limit) {
        LOG_ERROR("mdl %.*s count is implausible: %llu (limit=%llu)",
                  static_cast<int>(label.size()),
                  label.data(),
                  static_cast<unsigned long long>(count),
                  static_cast<unsigned long long>(hard_limit));
        return false;
    }
    if (minimum_bytes_per_element != 0 &&
        (count > std::numeric_limits<std::uint64_t>::max() / minimum_bytes_per_element ||
         ! CanReadBytes(file, count * minimum_bytes_per_element))) {
        LOG_ERROR("mdl %.*s count exceeds remaining payload: count=%llu remaining=%zu",
                  static_cast<int>(label.size()),
                  label.data(),
                  static_cast<unsigned long long>(count),
                  RemainingBytes(file));
        return false;
    }
    return true;
}

WPPuppet::PlayMode ToPlayMode(std::string_view m) {
    if (m == "loop" || m.empty()) return WPPuppet::PlayMode::Loop;
    if (m == "mirror") return WPPuppet::PlayMode::Mirror;
    if (m == "single") return WPPuppet::PlayMode::Single;

    LOG_ERROR("unknown puppet animation play mode \"%s\"", m.data());
    assert(m == "loop");
    return WPPuppet::PlayMode::Loop;
}

int32_t SeekNextMDLVersion(fs::IBinaryStream& f, std::string_view prefix) {
    const auto start = f.Tell();
    const auto end = f.Size();
    for (auto pos = start; pos + 9 <= end; ++pos) {
        f.SeekSet(pos);
        auto ver = ReadVersion(prefix, f);
        if (ver > 0)
            return ver;
    }
    f.SeekSet(start);
    return 0;
}

bool SeekNextMDLSection(fs::IBinaryStream& f, std::span<const std::string_view> prefixes) {
    const auto start = f.Tell();
    const auto end = f.Size();
    idx best_pos = -1;
    for (const auto prefix : prefixes) {
        for (auto pos = start; pos + 9 <= end; ++pos) {
            f.SeekSet(pos);
            auto ver = ReadVersion(prefix, f);
            if (ver > 0) {
                if (best_pos < 0 || pos < best_pos)
                    best_pos = pos;
                break;
            }
        }
    }
    if (best_pos >= 0) {
        f.SeekSet(best_pos);
        return true;
    }
    f.SeekSet(start);
    return false;
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
    const int32_t mdl_flag = static_cast<int32_t>(mdl.header.mdl_flag);
    if (mdl.header.mesh_count != 1) {
        LOG_ERROR("mdl multi-mesh body is not migrated yet: path='%s' mesh-count=%u",
                  str_path.c_str(),
                  mdl.header.mesh_count);
        return false;
    }
    const bool static_image_mesh = mdl_flag == 9;
    if (static_image_mesh) {
        // Flag 9 image puppet files are authored static image meshes, not broken animated
        // puppets. They reuse the puppet slot to carry crop/shape geometry for image layers and
        // intentionally stop before MDLS/MDLA skeleton data, so routing them through the animated
        // puppet reader would either reject them or interpret their compact vertex stream with the
        // wrong stride.
        mdl.kind = WPMdl::MeshKind::StaticImage;
    } else {
        mdl.kind = WPMdl::MeshKind::Puppet;
    }
    f.ReadInt32(); // unk, 1
    f.ReadInt32(); // unk, 1

    mdl.mat_json_file = f.ReadStr();
    // 0    
    f.ReadInt32();

    bool alt_mdl_format = false;
    uint32_t curr = f.ReadUint32();

    auto is_alt_vertex_marker = [&](uint32_t value) {
        return value == alt_format_vertex_size_herald_value ||
               (static_image_mesh && value == static_image_vertex_size_marker);
    };

    // If the uint at the normal vertex size position is 0, this file uses a marker-delimited
    // vertex-size block. Static image puppets have their own marker value because their file
    // family is image-layer geometry rather than animated skeleton data, but the cursor contract is
    // the same: seek the marker, then read the byte size immediately after it.
    if(curr == 0){
        alt_mdl_format = true;
        while (! is_alt_vertex_marker(curr) && f.Tell() < f.Size()){
            curr = f.ReadUint32();
        }
        if (! is_alt_vertex_marker(curr)) {
            LOG_ERROR("failed to locate alternative vertex herald 0x%08x", alt_format_vertex_size_herald_value);
            return false;
        }
        curr = f.ReadUint32();
    }
    else if(curr == std_format_vertex_size_herald_value ||
            (static_image_mesh && curr == static_image_vertex_size_marker)){
        curr = f.ReadUint32();
    }

    uint32_t vertex_size = curr;
    const uint32_t vertex_stride =
        static_image_mesh ? static_image_singile_vertex
                          : (alt_mdl_format ? alt_singile_vertex : singile_vertex);
    if (vertex_size % vertex_stride != 0) {
        LOG_ERROR("unsupport mdl vertex size %d", vertex_size);
        return false;
    }

    uint32_t vertex_num = vertex_size / vertex_stride;
    if (! CanReadBytes(f, vertex_size)) {
        LOG_ERROR("mdl vertex payload exceeds file: bytes=%u remaining=%zu",
                  vertex_size,
                  RemainingBytes(f));
        return false;
    }
    mdl.vertexs.resize(vertex_num);
    for (auto& vert : mdl.vertexs) {
        if (static_image_mesh) {
            // Static image puppet meshes store position.xyz, normal.xyz, a vec4 payload, and
            // texcoord.xy. They have no skinning attributes, so synthesize a neutral bone binding
            // while preserving the authored shape and UV crop exactly for the final image layer.
            for (auto& v : vert.position) v = f.ReadFloat();
            for (int i = 0; i < 7; i++) f.ReadFloat();
            vert.blend_indices = { 0, 0, 0, 0 };
            vert.weight        = { 0.0f, 0.0f, 0.0f, 1.0f };
            for (auto& v : vert.texcoord) v = f.ReadFloat();
        } else {
            for (auto& v : vert.position) v = f.ReadFloat();
            // If using the alternative MDL format, vertexes contain 7 extra 32-bit chunks between
            // position and blend indices. They are opaque payload for animated puppet meshes and
            // must stay separate from the compact static-image path above.
            if(alt_mdl_format) {for (int i = 0; i < 7; i++) f.ReadUint32();}
            for (auto& v : vert.blend_indices) v = f.ReadUint32();
            for (auto& v : vert.weight) v = f.ReadFloat();
            for (auto& v : vert.texcoord) v = f.ReadFloat();
        }
    }

    uint32_t indices_size = f.ReadUint32();
    if (indices_size % singile_indices != 0) {
        LOG_ERROR("unsupport mdl indices size %d", indices_size);
        return false;
    }

    uint32_t indices_num = indices_size / singile_indices;
    if (! CanReadBytes(f, indices_size)) {
        LOG_ERROR("mdl index payload exceeds file: bytes=%u remaining=%zu",
                  indices_size,
                  RemainingBytes(f));
        return false;
    }
    mdl.indices.resize(indices_num);
    for (auto& id : mdl.indices) {
        for (auto& v : id) v = f.ReadUint16();
    }

    if (static_image_mesh) {
        // Static image puppet meshes end after the index buffer. Returning here makes the contract
        // explicit for callers: the mesh is valid render geometry, but there is no WPPuppet object,
        // no bone uniforms, and no animation state to attach to the scene node.
        LOG_INFO("read static image puppet: mdlv: %d, vertices: %u, indices: %u",
                 mdl.mdlv,
                 vertex_num,
                 indices_num);
        return true;
    }

    mdl.mdls = ReadMDLVesion(f);
    if (mdl.mdls == 0) {
        mdl.mdls = SeekNextMDLVersion(f, "MDLS");
    }
    if (mdl.mdls == 0) {
        LOG_ERROR("failed to locate MDLS section");
        return false;
    }

    size_t bones_file_end = f.ReadUint32();
    (void)bones_file_end;

    uint16_t bones_num = f.ReadUint16();
    // 1 byte
    f.ReadUint16(); // unk

    if (! ValidateElementCount(f, "bone", bones_num, 74, 4096)) return false;

    mdl.puppet  = std::make_shared<WPPuppet>();
    auto& bones = mdl.puppet->bones;
    auto& anims = mdl.puppet->anims;

    bones.resize(bones_num);
    for (uint i = 0; i < bones_num; i++) {
        auto&       bone = bones[i];
        bone.name = f.ReadStr();
        f.ReadInt32(); // unk

        bone.parent = f.ReadUint32();
        if (bone.parent >= i && !bone.noParent()) {
            LOG_INFO("mdl bone %u has out-of-order parent index %u, fallback to root", i, bone.parent);
            bone.parent = 0xFFFFFFFFu;
        }

        uint32_t size = f.ReadUint32();
        if (size != 64) {
            LOG_ERROR("mdl unsupport bones size: %d", size);
            return false;
        }
        for (auto row : bone.transform.matrix().colwise()) {
            for (auto& x : row) x = f.ReadFloat();
        }

        std::string bone_simulation_json = f.ReadStr();
        /*
        auto trans = bone.transform.translation();
        LOG_INFO("trans: %f %f %f", trans[0], trans[1], trans[2]);
        */
    }

    if (mdl.mdls > 1) {
        int16_t unk = f.ReadInt16();
        if (unk != 0) {
            LOG_INFO("puppet: one unk is not 0, may be wrong");
        }

        uint8_t has_trans = f.ReadUint8();
        if (has_trans) {
            const auto transform_bytes = static_cast<std::uint64_t>(bones_num) * 16 * sizeof(float);
            if (! CanReadBytes(f, transform_bytes)) {
                LOG_ERROR("mdl skeleton transform payload exceeds file: bytes=%llu remaining=%zu",
                          static_cast<unsigned long long>(transform_bytes),
                          RemainingBytes(f));
                return false;
            }
            for (uint i = 0; i < bones_num; i++)
                for (uint j = 0; j < 16; j++) f.ReadFloat(); // mat
        }
        uint32_t size_unk = f.ReadUint32();
        if (! ValidateElementCount(f, "skeleton metadata", size_unk, 12, 65536)) return false;
        for (uint i = 0; i < size_unk; i++)
            for (int j = 0; j < 3; j++) f.ReadUint32();

        f.ReadUint32(); // unk

        uint8_t has_offset_trans = f.ReadUint8();
        if (has_offset_trans) {
            const auto offset_transform_bytes =
                static_cast<std::uint64_t>(bones_num) * 19 * sizeof(float);
            if (! CanReadBytes(f, offset_transform_bytes)) {
                LOG_ERROR("mdl skeleton offset payload exceeds file: bytes=%llu remaining=%zu",
                          static_cast<unsigned long long>(offset_transform_bytes),
                          RemainingBytes(f));
                return false;
            }
            for (uint i = 0; i < bones_num; i++) {
                for (uint j = 0; j < 3; j++) f.ReadFloat();  // like pos
                for (uint j = 0; j < 16; j++) f.ReadFloat(); // mat
            }
        }

        uint8_t has_index = f.ReadUint8();
        if (has_index) {
            const auto index_bytes = static_cast<std::uint64_t>(bones_num) * sizeof(uint32_t);
            if (! CanReadBytes(f, index_bytes)) {
                LOG_ERROR("mdl skeleton index payload exceeds file: bytes=%llu remaining=%zu",
                          static_cast<unsigned long long>(index_bytes),
                          RemainingBytes(f));
                return false;
            }
            for (uint i = 0; i < bones_num; i++) {
                f.ReadUint32();
            }
        }
    }

    {
        const auto probe_pos = f.Tell();
        bool aligned = false;
        for (const auto prefix : { std::string_view("MDAT"), std::string_view("MDLA") }) {
            auto ver = ReadVersion(prefix, f);
            f.SeekSet(probe_pos);
            if (ver > 0) {
                aligned = true;
                break;
            }
        }
        constexpr std::array<std::string_view, 2> kAnimSections { "MDAT", "MDLA" };
        if (!aligned)
            SeekNextMDLSection(f, kAnimSections);
    }

    // sometimes there can be one or more zero bytes and/or MDAT sections containing
    // attachments before the MDLA section, so we need to skip them
    std::string mdType = "";
    std::string mdVersion;
    
    do {
        if (f.Tell() >= f.Size()) {
            LOG_ERROR("failed to locate MDLA section before EOF");
            return false;
        }
        std::string mdPrefix = f.ReadStr();

        // sometimes there can be other garbage in this gap, so we need to 
        // skip over that as well
        if(mdPrefix.length() == 8){
            mdType = mdPrefix.substr(0, 4);
            mdVersion = mdPrefix.substr(4, 4);

            if(mdType == "MDAT"){
                f.ReadUint32(); // skip 4 bytes
                uint32_t num_attachments = f.ReadUint16(); // number of attachments in the MDAT section
                if (! ValidateElementCount(f, "attachment", num_attachments, 67, 4096)) {
                    return false;
                }

                for(int i = 0; i < num_attachments; i++){
                    WPPuppet::Attachment attachment;
                    attachment.bone_index = f.ReadUint16();
                    attachment.name       = f.ReadStr();
                    for (auto col : attachment.transform.matrix().colwise()) {
                        for (auto& x : col) x = f.ReadFloat();
                    }
                    mdl.puppet->attachments.push_back(std::move(attachment));
                }
            }
        }
    } while (mdType != "MDLA");
    

    if(mdType == "MDLA" && mdVersion.length() > 0){
        mdl.mdla = std::stoi(mdVersion);
        if (mdl.mdla != 0) {
            uint end_size = f.ReadUint32();
            (void)end_size;

            uint anim_num = f.ReadUint32();
            if (! ValidateElementCount(f, "animation", anim_num, 26, 4096)) return false;
            anims.resize(anim_num);
            for (auto& anim : anims) {
                // there can be a variable number of 32-bit 0s between animations
                anim.id = 0;
                while(anim.id == 0){
                    if (f.Tell() >= f.Size()) {
                        LOG_ERROR("unexpected EOF while reading animation id");
                        return false;
                    }
                    const auto before = f.Tell();
                    anim.id = f.ReadInt32();
                    const auto after = f.Tell();
                    if (after <= before) {
                        LOG_ERROR("stream did not advance while reading animation id");
                        return false;
                    }
                }
    
                if (anim.id <= 0) {
                    LOG_ERROR("wrong anime id %d", anim.id);
                    return false;
                }
                f.ReadInt32();
                anim.name   = f.ReadStr();
                if(anim.name.empty()){
                    anim.name = f.ReadStr();
                }
                anim.mode   = ToPlayMode(f.ReadStr());
                anim.fps    = f.ReadFloat();
                anim.length = f.ReadInt32();
                f.ReadInt32();

                uint32_t b_num = f.ReadUint32();
                if (! ValidateElementCount(f, "animation bone-track", b_num, 8, 4096)) {
                    return false;
                }
                anim.bframes_array.resize(b_num);
                for (auto& bframes : anim.bframes_array) {
                    f.ReadInt32();
                    uint32_t byte_size = f.ReadUint32();
                    uint32_t num       = byte_size / singile_bone_frame;
                    if (byte_size % singile_bone_frame != 0) {
                        LOG_ERROR("wrong bone frame size %u", byte_size);
                        return false;
                    }
                    if (! CanReadBytes(f, byte_size)) {
                        LOG_ERROR("mdl bone frame payload exceeds file: bytes=%u remaining=%zu",
                                  byte_size,
                                  RemainingBytes(f));
                        return false;
                    }
                    bframes.frames.resize(num);
                    for (auto& frame : bframes.frames) {
                        for (auto& v : frame.position) v = f.ReadFloat();
                        for (auto& v : frame.angle) v = f.ReadFloat();
                        for (auto& v : frame.scale) v = f.ReadFloat();
                    }
                }
                
                // in the alternative MDL format there are 2 empty bytes followed
                // by a variable number of 32-bit 0s between animations. We'll read
                // the two bytes now so that the cursor is aligned to read through the
                // 32-bit 0s in the next iteration
                if(alt_mdl_format)
                {
                    f.ReadUint8();
                    f.ReadUint8();
                    if (mdl.mdla >= 3)
                        f.ReadUint8();
                }
                else if(mdl.mdla >= 3){
                    // Newer MDLA variants insert an extra 8-bit zero between animations.
                    // If we don't consume it here, subsequent animation ids are shifted by 8 bits.
                    f.ReadUint8();
                }
                else{
                    uint32_t unk_extra_uint = f.ReadUint32();
                    for (uint i = 0; i < unk_extra_uint; i++) {
                        f.ReadFloat();
                        // data is like: {"$$hashKey":"object:2110","frame":1,"name":"random_anim"}
                        std::string unk_extra = f.ReadStr();
                    }
                }
            }
        }
    }
    
    mdl.puppet->prepared();

    LOG_INFO("read puppet: mdlv: %d, nmdls: %d, mdla: %d, bones: %d, anims: %d",
             mdl.mdlv,
             mdl.mdls,
             mdl.mdla,
             mdl.puppet->bones.size(),
             mdl.puppet->anims.size());
    return true;
}

void WPMdlParser::GenPuppetMesh(SceneMesh& mesh, const WPMdl& mdl) {
    SceneVertexArray vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3 },
                              { WE_IN_BLENDINDICES.data(), VertexType::UINT4 },
                              { WE_IN_BLENDWEIGHTS.data(), VertexType::FLOAT4 },
                              { WE_IN_TEXCOORD.data(), VertexType::FLOAT2 } },
                            mdl.vertexs.size());

    std::array<float, 16> one_vert;
    auto                  to_one = [](const WPMdl::Vertex& in, decltype(one_vert)& out) {
        uint offset = 0;
        memcpy(out.data() + 4 * (offset++), in.position.data(), sizeof(in.position));
        memcpy(out.data() + 4 * (offset++), in.blend_indices.data(), sizeof(in.blend_indices));
        memcpy(out.data() + 4 * (offset++), in.weight.data(), sizeof(in.weight));
        memcpy(out.data() + 4 * (offset++), in.texcoord.data(), sizeof(in.texcoord));
    };
    for (uint i = 0; i < mdl.vertexs.size(); i++) {
        auto& v = mdl.vertexs[i];
        to_one(v, one_vert);
        vertex.SetVertexs(i, one_vert);
    }
    std::vector<uint32_t> indices;
    size_t                u16_count = mdl.indices.size() * 3;
    indices.resize(u16_count / 2 + 1);
    memcpy(indices.data(), mdl.indices.data(), u16_count * sizeof(uint16_t));

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
    info.combos["SKINNING"]  = "1";
    info.combos["BONECOUNT"] = std::to_string(mdl.puppet->bones.size());
}

void WPMdlParser::AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl) {
    mat.combos["SKINNING"]  = 1;
    mat.combos["BONECOUNT"] = (i32)mdl.puppet->bones.size();
    mat.use_puppet          = true;
}
