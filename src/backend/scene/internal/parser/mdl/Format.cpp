#include "Format.hpp"

#include "fs/IBinaryStream.h"

#include <array>
#include <charconv>
#include <cstring>
#include <string>

namespace wallpaper
{
namespace
{
constexpr u32 MAX_MESH_COUNT = 65'536;

bool CanRead(const fs::IBinaryStream& stream, idx bytes) {
    if (bytes < 0) return false;
    const auto position = stream.Tell();
    const auto size = stream.Size();
    return position >= 0 && size >= position && bytes <= size - position;
}

Result<WPMdlHeader> Invalid(std::string message) {
    return Result<WPMdlHeader>::failure(ResultCode::InvalidArgument, std::move(message));
}

Result<WPMdlHeader> Unsupported(std::string message) {
    return Result<WPMdlHeader>::failure(ResultCode::NotSupported, std::move(message));
}
} // namespace

u32 WPMdlVertexStride(u32 flags) {
    u32 stride = 3u * sizeof(float);
    if ((flags & WPMDL_FLAG_NORMAL) != 0) stride += 3u * sizeof(float);
    if ((flags & WPMDL_FLAG_TANGENT) != 0) stride += 4u * sizeof(float);
    if ((flags & WPMDL_FLAG_EXTRA4) != 0) stride += 4u * sizeof(std::uint8_t);
    if ((flags & WPMDL_FLAG_SKIN_BLEND) != 0) stride += 4u * sizeof(u32);
    if ((flags & WPMDL_FLAG_SKIN_WEIGHT) != 0) stride += 4u * sizeof(float);
    if ((flags & (WPMDL_FLAG_UV | WPMDL_FLAG_UV2)) != 0) stride += 2u * sizeof(float);
    if ((flags & WPMDL_FLAG_UV2) != 0) stride += 2u * sizeof(float);
    return stride;
}

Result<WPMdlHeader> ParseWPMdlHeader(fs::IBinaryStream& stream) {
    constexpr idx headerBytes = 9 + static_cast<idx>(sizeof(u32) * 3);
    if (! CanRead(stream, headerBytes)) return Invalid("truncated MDL header");

    std::array<char, 9> stamp {};
    if (stream.Read(stamp.data(), stamp.size()) != stamp.size()
        || std::memcmp(stamp.data(), "MDLV", 4) != 0 || stamp[8] != '\0') {
        return Invalid("invalid MDLV stamp");
    }

    i32 version = 0;
    const auto [end, error] = std::from_chars(stamp.data() + 4, stamp.data() + 8, version);
    if (error != std::errc {} || end != stamp.data() + 8 || version <= 0) {
        return Invalid("invalid MDLV version digits");
    }
    if (version > 23) return Unsupported("unsupported MDLV version");

    WPMdlHeader header;
    header.mdlv = version;
    header.mdl_flag = stream.ReadUint32();
    header.unk_a = stream.ReadUint32();
    header.mesh_count = stream.ReadUint32();

    if (header.mesh_count == 0 || header.mesh_count > MAX_MESH_COUNT) {
        return Invalid("invalid MDL mesh count");
    }
    if (WPMdlVertexStride(header.mdl_flag) < 3u * sizeof(float)) {
        return Invalid("invalid MDL vertex layout");
    }

    return Result<WPMdlHeader>::success(header);
}
} // namespace wallpaper
