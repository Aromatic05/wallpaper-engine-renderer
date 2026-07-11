#include "Mesh.hpp"

#include "fs/IBinaryStream.h"

#include <limits>
#include <string>

namespace wallpaper
{
namespace
{
constexpr u32 MAX_VERTEX_COUNT = 2'000'000;
constexpr u32 MAX_TRIANGLE_COUNT = 4'000'000;
constexpr u32 MAX_PART_COUNT = 1'000'000;
constexpr u32 MAX_MASK_COUNT = 65'536;
constexpr u32 MAX_MASK_PART_COUNT = 1'000'000;
constexpr usize MAX_STRING_BYTES = 16 * 1024 * 1024;

class CheckedReader {
public:
    explicit CheckedReader(fs::IBinaryStream& stream)
        : stream_(stream) {}

    template<typename T>
    bool read(T& value) {
        return stream_.Read(&value, sizeof(value)) == sizeof(value);
    }

    bool canRead(std::uint64_t bytes) const {
        const auto position = stream_.Tell();
        const auto size = stream_.Size();
        return position >= 0 && size >= position
               && bytes <= static_cast<std::uint64_t>(size - position);
    }

    bool readCString(std::string& value) {
        value.clear();
        while (value.size() < MAX_STRING_BYTES) {
            char ch = '\0';
            if (! read(ch)) return false;
            if (ch == '\0') return true;
            value.push_back(ch);
        }
        return false;
    }

private:
    fs::IBinaryStream& stream_;
};

Result<WPMdlMesh> Invalid(std::string message) {
    return Result<WPMdlMesh>::failure(ResultCode::InvalidArgument, std::move(message));
}

Result<void> ReadVertex(CheckedReader& reader, u32 flags, WPMdlMesh& mesh, usize index) {
    if (! reader.read(mesh.positions[index])) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDL vertex position");
    }
    if ((flags & WPMDL_FLAG_NORMAL) != 0 && ! reader.read(mesh.normals[index])) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDL vertex normal");
    }
    if ((flags & WPMDL_FLAG_TANGENT) != 0 && ! reader.read(mesh.tangents[index])) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDL vertex tangent");
    }
    if ((flags & WPMDL_FLAG_EXTRA4) != 0 && ! reader.read(mesh.extra4[index])) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDL vertex extra4");
    }
    if ((flags & WPMDL_FLAG_SKIN_BLEND) != 0 && ! reader.read(mesh.blend_indices[index])) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDL vertex blend indices");
    }
    if ((flags & WPMDL_FLAG_SKIN_WEIGHT) != 0 && ! reader.read(mesh.blend_weights[index])) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDL vertex blend weights");
    }
    if ((flags & (WPMDL_FLAG_UV | WPMDL_FLAG_UV2)) != 0
        && ! reader.read(mesh.texcoords[index])) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDL vertex texcoord");
    }
    if ((flags & WPMDL_FLAG_UV2) != 0 && ! reader.read(mesh.texcoord2[index])) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDL vertex second texcoord");
    }
    return Result<void>::success();
}

Result<void> ParseParts(CheckedReader& reader,
                        const WPMdlHeader& header,
                        WPMdlMesh& mesh) {
    if (header.mdlv < 21) return Result<void>::success();

    std::uint8_t extraBlock = 0;
    if (! reader.read(extraBlock)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDLV21 parts header");
    }
    if (extraBlock == 1) {
        std::uint8_t hasUv2 = 0;
        if (! reader.read(hasUv2)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLV21 uv2 flag");
        }
        if (hasUv2 != 0) {
            std::uint16_t reserved = 0;
            std::uint8_t marker = 0;
            u32 payloadBytes = 0;
            if (! reader.read(reserved) || ! reader.read(marker) || ! reader.read(payloadBytes)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDLV21 uv2 header");
            }
            const auto expected = static_cast<std::uint64_t>(mesh.positions.size()) * 12u;
            if (payloadBytes != expected || ! reader.canRead(payloadBytes)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "invalid MDLV21 uv2 payload size");
            }
            mesh.part_uv2.resize(mesh.positions.size());
            mesh.part_uv2_pad.resize(mesh.positions.size());
            for (usize index = 0; index < mesh.positions.size(); ++index) {
                if (! reader.read(mesh.part_uv2[index]) || ! reader.read(mesh.part_uv2_pad[index])) {
                    return Result<void>::failure(ResultCode::InvalidArgument,
                                                 "truncated MDLV21 uv2 payload");
                }
            }
            (void)reserved;
            (void)marker;
        }
    } else if (extraBlock != 0) {
        return Result<void>::failure(ResultCode::NotSupported,
                                     "unsupported MDLV21 parts extra block");
    }

    std::uint8_t hasParts = 0;
    if (! reader.read(hasParts)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated MDLV21 part list flag");
    }
    if (hasParts == 0) return Result<void>::success();
    if (hasParts != 1) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "invalid MDLV21 part list flag");
    }

    u32 partBytes = 0;
    if (! reader.read(partBytes) || partBytes % 16u != 0 || ! reader.canRead(partBytes)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "invalid MDLV21 part list size");
    }
    const u32 partCount = partBytes / 16u;
    if (partCount > MAX_PART_COUNT) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "implausible MDLV21 part count");
    }

    const auto indexElementCount = static_cast<std::uint64_t>(mesh.indices.size()) * 3u;
    mesh.parts.resize(partCount);
    for (auto& part : mesh.parts) {
        u32 reserved = 0;
        if (! reader.read(part.id) || ! reader.read(reserved)
            || ! reader.read(part.start) || ! reader.read(part.size)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLV21 part record");
        }
        if (static_cast<std::uint64_t>(part.start) + part.size > indexElementCount) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "MDLV21 part range exceeds index buffer");
        }
    }
    return Result<void>::success();
}

Result<void> ParseMasks(CheckedReader& reader, WPMdlMesh& mesh) {
    u32 maskCount = 0;
    if (! reader.read(maskCount) || maskCount > MAX_MASK_COUNT) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "invalid MDLV23 mask count");
    }
    mesh.masks.resize(maskCount);
    for (auto& mask : mesh.masks) {
        u32 zeroA = 0;
        u32 zeroPadding = 0;
        if (! reader.read(mask.leading_a) || ! reader.read(zeroA)
            || ! reader.readCString(mask.material_json) || ! reader.read(zeroPadding)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated MDLV23 mask header");
        }

        u32 countA = 0;
        if (! reader.read(countA) || countA > MAX_MASK_PART_COUNT
            || ! reader.canRead(static_cast<std::uint64_t>(countA) * sizeof(u32))) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "invalid MDLV23 mask part list A");
        }
        mask.part_ids_a.resize(countA);
        for (auto& partId : mask.part_ids_a) {
            if (! reader.read(partId)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDLV23 mask part list A");
            }
        }

        u32 countB = 0;
        if (! reader.read(countB) || countB > MAX_MASK_PART_COUNT
            || ! reader.canRead(static_cast<std::uint64_t>(countB) * sizeof(u32))) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "invalid MDLV23 mask part list B");
        }
        mask.part_ids_b.resize(countB);
        for (auto& partId : mask.part_ids_b) {
            if (! reader.read(partId)) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "truncated MDLV23 mask part list B");
            }
        }
        (void)zeroA;
        (void)zeroPadding;
    }
    return Result<void>::success();
}
} // namespace

bool WPMdlUsesUint32Indices(const WPMdlHeader& header, u32 vertexCount) {
    return header.mdlv >= 23 && vertexCount > std::numeric_limits<std::uint16_t>::max();
}

Result<WPMdlMesh> ParseWPMdlMesh(fs::IBinaryStream& stream,
                                 const WPMdlHeader& header) {
    CheckedReader reader(stream);
    WPMdlMesh mesh;

    if (! reader.readCString(mesh.material_json_file) || mesh.material_json_file.empty()) {
        return Invalid("invalid MDL mesh material path");
    }
    if (! reader.read(mesh.flag_a)) return Invalid("truncated MDL mesh flags");
    if (mesh.flag_a == 2) {
        u32 value = 0;
        if (! reader.read(value)) return Invalid("truncated MDL flag-a payload");
        mesh.has_flag_a2_one = value == 1;
    }

    if (header.mdlv >= 17) {
        if (! reader.read(mesh.aabb_min) || ! reader.read(mesh.aabb_max)) {
            return Invalid("truncated MDL mesh bounds");
        }
        mesh.has_aabb = true;
    }

    mesh.flag = header.mdl_flag;
    if (header.mdlv > 14 && ! reader.read(mesh.flag)) {
        return Invalid("truncated MDL per-mesh vertex flags");
    }

    u32 vertexBytes = 0;
    if (! reader.read(vertexBytes)) return Invalid("truncated MDL vertex byte size");
    const u32 vertexStride = WPMdlVertexStride(mesh.flag);
    if (vertexStride == 0 || vertexBytes == 0 || vertexBytes % vertexStride != 0) {
        return Invalid("invalid MDL vertex byte size");
    }
    const u32 vertexCount = vertexBytes / vertexStride;
    if (vertexCount > MAX_VERTEX_COUNT || ! reader.canRead(vertexBytes)) {
        return Invalid("MDL vertex payload exceeds bounds");
    }

    mesh.positions.resize(vertexCount);
    if ((mesh.flag & WPMDL_FLAG_NORMAL) != 0) mesh.normals.resize(vertexCount);
    if ((mesh.flag & WPMDL_FLAG_TANGENT) != 0) mesh.tangents.resize(vertexCount);
    if ((mesh.flag & WPMDL_FLAG_EXTRA4) != 0) mesh.extra4.resize(vertexCount);
    if ((mesh.flag & WPMDL_FLAG_SKIN_BLEND) != 0) mesh.blend_indices.resize(vertexCount);
    if ((mesh.flag & WPMDL_FLAG_SKIN_WEIGHT) != 0) mesh.blend_weights.resize(vertexCount);
    if ((mesh.flag & (WPMDL_FLAG_UV | WPMDL_FLAG_UV2)) != 0) {
        mesh.texcoords.resize(vertexCount);
    }
    if ((mesh.flag & WPMDL_FLAG_UV2) != 0) mesh.texcoord2.resize(vertexCount);

    for (usize index = 0; index < vertexCount; ++index) {
        auto result = ReadVertex(reader, mesh.flag, mesh, index);
        if (! result) return Result<WPMdlMesh>(result.error());
    }

    u32 indexBytes = 0;
    if (! reader.read(indexBytes)) return Invalid("truncated MDL index byte size");
    const bool useUint32 = WPMdlUsesUint32Indices(header, vertexCount);
    const u32 triangleStride = useUint32 ? 3u * sizeof(u32)
                                         : 3u * sizeof(std::uint16_t);
    if (indexBytes == 0 || indexBytes % triangleStride != 0) {
        return Invalid("invalid MDL index byte size");
    }
    const u32 triangleCount = indexBytes / triangleStride;
    if (triangleCount > MAX_TRIANGLE_COUNT || ! reader.canRead(indexBytes)) {
        return Invalid("MDL index payload exceeds bounds");
    }

    mesh.indices.resize(triangleCount);
    for (auto& triangle : mesh.indices) {
        for (auto& index : triangle) {
            if (useUint32) {
                if (! reader.read(index)) return Invalid("truncated MDL uint32 index payload");
            } else {
                std::uint16_t value = 0;
                if (! reader.read(value)) return Invalid("truncated MDL uint16 index payload");
                index = value;
            }
            if (index >= vertexCount) return Invalid("MDL index exceeds vertex count");
        }
    }

    auto partsResult = ParseParts(reader, header, mesh);
    if (! partsResult) return Result<WPMdlMesh>(partsResult.error());
    if (header.mdlv > 21) {
        auto masksResult = ParseMasks(reader, mesh);
        if (! masksResult) return Result<WPMdlMesh>(masksResult.error());
    }

    return Result<WPMdlMesh>::success(std::move(mesh));
}
} // namespace wallpaper
