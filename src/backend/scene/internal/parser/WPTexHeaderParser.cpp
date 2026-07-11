#include "WPTexHeaderParser.hpp"

#include "fs/IBinaryStream.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace wallpaper
{
namespace
{
constexpr i32 MAX_IMAGE_COUNT = 65'536;
constexpr i32 MAX_MIP_COUNT = 256;
constexpr i32 MAX_SPRITE_FRAME_COUNT = 1'000'000;

class CheckedReader {
public:
    explicit CheckedReader(fs::IBinaryStream& stream)
        : m_stream(stream) {}

    template<typename T>
    bool read(T& value) {
        return m_stream.Read(&value, sizeof(value)) == sizeof(value);
    }

    bool readBytes(void* data, usize size) {
        return size == 0 || m_stream.Read(data, size) == size;
    }

    bool skip(idx size) {
        if (size < 0 || ! canRead(size)) return false;
        return m_stream.SeekCur(size);
    }

    bool canRead(idx size) const {
        if (size < 0) return false;
        const auto position = m_stream.Tell();
        const auto streamSize = m_stream.Size();
        return position >= 0 && streamSize >= position && size <= streamSize - position;
    }

private:
    fs::IBinaryStream& m_stream;
};

Result<ImageHeader> Invalid(std::string message) {
    return Result<ImageHeader>::failure(ResultCode::InvalidArgument, std::move(message));
}

Result<ImageHeader> Unsupported(std::string message) {
    return Result<ImageHeader>::failure(ResultCode::NotSupported, std::move(message));
}

bool ReadVersion(CheckedReader& reader, std::string_view expected, i32& version) {
    std::array<char, 9> stamp {};
    if (! reader.readBytes(stamp.data(), stamp.size())) return false;
    if (std::memcmp(stamp.data(), expected.data(), expected.size()) != 0) return false;
    if (stamp[8] != '\0') return false;

    const char* first = stamp.data() + 4;
    const char* last = stamp.data() + 8;
    const auto [ptr, error] = std::from_chars(first, last, version);
    return error == std::errc {} && ptr == last && version > 0;
}

bool ToTextureFormat(i32 raw, TextureFormat& format) {
    switch (raw) {
    case 0: format = TextureFormat::RGBA8; return true;
    case 4: format = TextureFormat::BC3; return true;
    case 6: format = TextureFormat::BC2; return true;
    case 7: format = TextureFormat::BC1; return true;
    case 8: format = TextureFormat::RG8; return true;
    case 9: format = TextureFormat::R8; return true;
    default: return false;
    }
}

bool LooksLikeMp4Payload(const char* data, usize size) {
    return data != nullptr && size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0;
}

bool IsPowOfTwo(u32 value) {
    return value > 1 && (value & (value - 1)) == 0;
}

void SetHeaderPow2(ImageHeader& header, i32 width, i32 height) {
    header.mipmap_pow2 = IsPowOfTwo(static_cast<u32>(width))
                         || IsPowOfTwo(static_cast<u32>(height));
    const auto mipArea = static_cast<std::int64_t>(width) * height;
    const auto mapArea = static_cast<std::int64_t>(header.mapWidth) * header.mapHeight;
    header.mipmap_larger = mipArea > mapArea;
}

struct SlotDimensions {
    i32 width { 0 };
    i32 height { 0 };
};

Result<void> ReadMipPayload(CheckedReader& reader,
                            i32 texb,
                            bool inspectPayload,
                            ImageHeader& header) {
    i32 compressed = 0;
    i32 decompressedSize = 0;
    if (texb >= 2) {
        if (! reader.read(compressed) || ! reader.read(decompressedSize)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated TEXB compression fields");
        }
        if ((compressed != 0 && compressed != 1) || decompressedSize < 0) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "invalid TEXB compression fields");
        }
    }

    i32 sourceSize = 0;
    if (! reader.read(sourceSize) || sourceSize < 0) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "invalid TEXB mip payload size");
    }
    if (! reader.canRead(sourceSize)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "TEXB mip payload exceeds stream bounds");
    }

    constexpr usize probeSize = 16;
    if (inspectPayload && compressed == 0 && header.type == ImageType::UNKNOWN
        && sourceSize > 0) {
        std::array<char, probeSize> probe {};
        const auto bytesToRead = static_cast<usize>(std::min<i32>(sourceSize, probeSize));
        if (! reader.readBytes(probe.data(), bytesToRead)) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated TEXB mip payload");
        }
        if (LooksLikeMp4Payload(probe.data(), bytesToRead)) {
            header.isVideoTexture = true;
            header.extraHeader["texb_is_video_mp4"].val = 1;
        }
        if (! reader.skip(sourceSize - static_cast<i32>(bytesToRead))) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "truncated TEXB mip payload");
        }
    } else if (! reader.skip(sourceSize)) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "truncated TEXB mip payload");
    }

    return Result<void>::success();
}
} // namespace

Result<ImageHeader> ParseWPTexHeader(fs::IBinaryStream& stream) {
    CheckedReader reader(stream);
    ImageHeader header;

    i32 texv = 0;
    i32 texi = 0;
    if (! ReadVersion(reader, "TEXV", texv) || ! ReadVersion(reader, "TEXI", texi)) {
        return Invalid("invalid or truncated TEXV/TEXI stamps");
    }
    if (texv != 5 || texi != 1) {
        return Unsupported("unsupported TEXV/TEXI version combination");
    }
    header.extraHeader["texv"].val = texv;
    header.extraHeader["texi"].val = texi;

    i32 rawFormat = 0;
    u32 rawFlags = 0;
    if (! reader.read(rawFormat) || ! reader.read(rawFlags)) {
        return Invalid("truncated TEX format and flags");
    }
    if (! ToTextureFormat(rawFormat, header.format)) {
        return Unsupported("unsupported TEX texture format");
    }

    header.isSprite = (rawFlags & (1u << 2)) != 0;
    header.sample.wrapS = header.sample.wrapT = (rawFlags & (1u << 1)) != 0
        ? TextureWrap::CLAMP_TO_EDGE
        : TextureWrap::REPEAT;
    header.sample.minFilter = header.sample.magFilter = (rawFlags & 1u) != 0
        ? TextureFilter::NEAREST
        : TextureFilter::LINEAR;
    header.extraHeader["compo1"].val = (rawFlags & (1u << 20)) != 0;
    header.extraHeader["compo2"].val = (rawFlags & (1u << 21)) != 0;
    header.extraHeader["compo3"].val = (rawFlags & (1u << 22)) != 0;

    i32 unknown = 0;
    if (! reader.read(header.width) || ! reader.read(header.height)
        || ! reader.read(header.mapWidth) || ! reader.read(header.mapHeight)
        || ! reader.read(unknown)) {
        return Invalid("truncated TEX dimensions");
    }
    if (header.width <= 0 || header.height <= 0 || header.mapWidth <= 0
        || header.mapHeight <= 0) {
        return Invalid("invalid TEX dimensions");
    }

    i32 texb = 0;
    if (! ReadVersion(reader, "TEXB", texb)) {
        return Invalid("invalid or truncated TEXB stamp");
    }
    if (texb < 1 || texb > 4) {
        return Unsupported("unsupported TEXB version");
    }
    header.extraHeader["texb"].val = texb;

    if (! reader.read(header.count) || header.count < 0 || header.count > MAX_IMAGE_COUNT) {
        return Invalid("invalid TEX image count");
    }

    if (texb >= 3) {
        i32 rawType = 0;
        if (! reader.read(rawType)) return Invalid("truncated TEXB image type");
        if (rawType < static_cast<i32>(ImageType::UNKNOWN)
            || rawType > static_cast<i32>(ImageType::RAW)) {
            return Unsupported("unsupported TEXB image container type");
        }
        header.type = static_cast<ImageType>(rawType);
    }
    if (texb >= 4) {
        i32 reserved = 0;
        if (! reader.read(reserved)) return Invalid("truncated TEXB reserved field");
        header.extraHeader["texb_reserved"].val = reserved;
    }

    std::vector<SlotDimensions> slotDimensions(static_cast<size_t>(header.count));
    for (i32 imageIndex = 0; imageIndex < header.count; ++imageIndex) {
        i32 mipCount = 0;
        if (! reader.read(mipCount) || mipCount < 0 || mipCount > MAX_MIP_COUNT) {
            return Invalid("invalid TEX mip count");
        }
        for (i32 mipIndex = 0; mipIndex < mipCount; ++mipIndex) {
            i32 width = 0;
            i32 height = 0;
            if (! reader.read(width) || ! reader.read(height) || width <= 0 || height <= 0) {
                return Invalid("invalid TEX mip dimensions");
            }
            if (mipIndex == 0) {
                slotDimensions[static_cast<size_t>(imageIndex)] = { width, height };
                if (imageIndex == 0) SetHeaderPow2(header, width, height);
            }
            auto payloadResult = ReadMipPayload(reader,
                                                texb,
                                                imageIndex == 0 && mipIndex == 0,
                                                header);
            if (! payloadResult) return Result<ImageHeader>(payloadResult.error());
        }
    }

    if (! header.isSprite) return Result<ImageHeader>::success(std::move(header));

    i32 texs = 0;
    if (! ReadVersion(reader, "TEXS", texs)) {
        return Invalid("invalid or truncated TEXS stamp");
    }
    if (texs < 1 || texs > 3) return Unsupported("unsupported TEXS version");
    header.extraHeader["texs"].val = texs;

    i32 frameCount = 0;
    if (! reader.read(frameCount) || frameCount < 0 || frameCount > MAX_SPRITE_FRAME_COUNT) {
        return Invalid("invalid TEXS frame count");
    }
    if (texs == 3) {
        i32 atlasWidth = 0;
        i32 atlasHeight = 0;
        if (! reader.read(atlasWidth) || ! reader.read(atlasHeight)
            || atlasWidth <= 0 || atlasHeight <= 0) {
            return Invalid("invalid TEXS atlas dimensions");
        }
    }

    for (i32 frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        SpriteFrame frame;
        if (! reader.read(frame.imageId) || frame.imageId < 0 || frame.imageId >= header.count) {
            return Invalid("invalid TEXS image id");
        }
        const auto dimensions = slotDimensions[static_cast<size_t>(frame.imageId)];
        if (dimensions.width <= 0 || dimensions.height <= 0) {
            return Invalid("TEXS frame references an image without mip zero");
        }
        if (! reader.read(frame.frametime) || ! std::isfinite(frame.frametime)) {
            return Invalid("invalid TEXS frame time");
        }

        if (texs == 1) {
            i32 x = 0;
            i32 y = 0;
            i32 x0 = 0;
            i32 x1 = 0;
            i32 y0 = 0;
            i32 y1 = 0;
            if (! reader.read(x) || ! reader.read(y) || ! reader.read(x0) || ! reader.read(x1)
                || ! reader.read(y0) || ! reader.read(y1)) {
                return Invalid("truncated TEXS integer frame");
            }
            frame.x = static_cast<float>(x) / static_cast<float>(dimensions.width);
            frame.y = static_cast<float>(y) / static_cast<float>(dimensions.height);
            frame.xAxis = { static_cast<float>(x0), static_cast<float>(x1) };
            frame.yAxis = { static_cast<float>(y0), static_cast<float>(y1) };
        } else {
            if (! reader.read(frame.x) || ! reader.read(frame.y)
                || ! reader.read(frame.xAxis[0]) || ! reader.read(frame.xAxis[1])
                || ! reader.read(frame.yAxis[0]) || ! reader.read(frame.yAxis[1])) {
                return Invalid("truncated TEXS floating frame");
            }
            frame.x /= static_cast<float>(dimensions.width);
            frame.y /= static_cast<float>(dimensions.height);
        }

        frame.width = std::hypot(frame.xAxis[0], frame.xAxis[1]);
        frame.height = std::hypot(frame.yAxis[0], frame.yAxis[1]);
        if (! std::isfinite(frame.width) || ! std::isfinite(frame.height) || frame.width <= 0.0f) {
            return Invalid("invalid TEXS frame axes");
        }
        frame.xAxis[0] /= static_cast<float>(dimensions.width);
        frame.xAxis[1] /= static_cast<float>(dimensions.width);
        frame.yAxis[0] /= static_cast<float>(dimensions.height);
        frame.yAxis[1] /= static_cast<float>(dimensions.height);
        frame.rate = frame.height / frame.width;
        header.spriteAnim.AppendFrame(frame);
    }

    return Result<ImageHeader>::success(std::move(header));
}
} // namespace wallpaper
