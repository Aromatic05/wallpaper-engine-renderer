#include "WPTexImageParser.hpp"

#include "Type.hpp"
#include "WPCommon.hpp"
#include <cstdint>
#include <lz4.h>

#include "SpriteAnimation.hpp"
#include "utils/Algorism.h"
#include "utils/Sha.hpp"
#include "fs/VFS.h"
#include "utils/BitFlags.hpp"
#include "WPCommon.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstring>
#include <iostream>
#include <span>
using namespace wallpaper;

enum class WPTexFlagEnum : uint32_t
{
    // true for no bilinear
    noInterpolation = 0,
    // true for no repeat
    clampUVs = 1,
    sprite   = 2,

    compo1 = 20,
    compo2 = 21,
    compo3 = 22
};
using WPTexFlags = BitFlags<WPTexFlagEnum>;

namespace
{
constexpr std::string_view TEX_HEADER_DIR { "tex-headers01" };

char* Lz4Decompress(const char* src, int size, int decompressed_size) {
    char* dst       = new char[(usize)decompressed_size];
    int   load_size = LZ4_decompress_safe(src, dst, size, decompressed_size);
    if (load_size < decompressed_size) {
        LOG_ERROR("lz4 decompress failed");
        delete[] dst;
        return nullptr;
    }
    return dst;
}

bool DecodeImageContainerBytes(const char* src, int size, int& width, int& height,
                               ImageDataPtr& data) {
    int   channels = 0;
    auto* decoded  = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(src), size, &width, &height, &channels, 4);
    if (decoded == nullptr || width <= 0 || height <= 0) return false;

    data = ImageDataPtr(reinterpret_cast<uint8_t*>(decoded), [](uint8_t* ptr) {
        stbi_image_free(reinterpret_cast<unsigned char*>(ptr));
    });
    return true;
}

bool IsVideoImageContainer(const ImageHeader& header) {
    return header.isVideoTexture;
}

bool LooksLikeMp4Payload(const char* data, size_t size) {
    if (data == nullptr || size < 12) return false;
    return std::memcmp(data + 4, "ftyp", 4) == 0;
}

bool HasEmbeddedImagePayload(const ImageHeader& header) {
    return header.extraHeader.at("texb").val >= 3 && header.type != ImageType::UNKNOWN &&
           ! IsVideoImageContainer(header);
}

TextureFormat ToTexFormate(int type) {
    /*
        type
        RGBA8888 = 0,
        DXT5 = 4,
        DXT3 = 6,
        DXT1 = 7,
        RG88 = 8,
        R8 = 9,
    */
    switch (type) {
    case 0: return TextureFormat::RGBA8;
    case 4: return TextureFormat::BC3;
    case 6: return TextureFormat::BC2;
    case 7: return TextureFormat::BC1;
    case 8: return TextureFormat::RG8;
    case 9: return TextureFormat::R8;
    default:
        LOG_ERROR("ERROR::ToTexFormate Unkown image type: %d", type);
        return TextureFormat::RGBA8;
    }
}
void LoadHeader(fs::IBinaryStream& file, ImageHeader& header) {
    header.extraHeader["texv"].val = ReadTexVesion(file);
    header.extraHeader["texi"].val = ReadTexVesion(file);

    header.format = ToTexFormate(file.ReadInt32());
    WPTexFlags flags(file.ReadUint32());
    {
        header.isSprite     = flags[WPTexFlagEnum::sprite];
        header.sample.wrapS = header.sample.wrapT =
            flags[WPTexFlagEnum::clampUVs] ? TextureWrap::CLAMP_TO_EDGE : TextureWrap::REPEAT;
        header.sample.minFilter = header.sample.magFilter =
            flags[WPTexFlagEnum::noInterpolation] ? TextureFilter::NEAREST : TextureFilter::LINEAR;
        header.extraHeader["compo1"].val = flags[WPTexFlagEnum::compo1];
        header.extraHeader["compo2"].val = flags[WPTexFlagEnum::compo2];
        header.extraHeader["compo3"].val = flags[WPTexFlagEnum::compo3];
    }

    /*
        picture:
        width, height --> pow of 2 (tex size)
        mapw, maph    --> pic size
        mips
        mipw,miph     --> pow of 2

        sprites:
        width, height --> piece of sprite sheet
        mapw, maph    --> same
        1 mip
        mipw,mimp     --> tex size
    */

    header.width  = file.ReadInt32();
    header.height = file.ReadInt32();
    // in sprite this mean one pic
    header.mapWidth  = file.ReadInt32();
    header.mapHeight = file.ReadInt32();

    file.ReadInt32(); // unknown

    header.extraHeader["texb"].val = ReadTexVesion(file);

    header.count = file.ReadInt32();

    if (header.extraHeader["texb"].val >= 3) {
        header.type = static_cast<ImageType>(file.ReadInt32());
    }
    if (header.extraHeader["texb"].val >= 4) {
        header.extraHeader["texb_reserved"].val = file.ReadInt32();
    }
}

void SetHeaderPow2(ImageHeader& header, i32 mip_0_w, i32 mip_0_h) {
    header.mipmap_pow2   = algorism::IsPowOfTwo((u32)mip_0_w) || algorism::IsPowOfTwo((u32)mip_0_h);
    header.mipmap_larger = mip_0_w * mip_0_h > header.mapWidth * header.mapHeight;
}

template<typename T>
bool ReadPod(fs::IBinaryStream& file, T& value) {
    return file.Read(&value, sizeof(value)) == sizeof(value);
}

template<typename T>
void WritePod(fs::IBinaryStreamW& file, const T& value) {
    file.Write(&value, sizeof(value));
}

void WriteString(fs::IBinaryStreamW& file, std::string_view value) {
    file.WriteUint32(static_cast<u32>(value.size()));
    if (! value.empty()) file.Write(value.data(), value.size());
}

bool ReadString(fs::IBinaryStream& file, std::string& value) {
    const auto size = file.ReadUint32();
    value.resize(size);
    if (size > 0) file.Read(value.data(), size);
    return true;
}

std::string GetTextureHeaderCachePath(std::string_view cache_namespace, std::string_view name) {
    std::string key;
    key.reserve(cache_namespace.size() + name.size() + 16);
    key.append("tex-header-v2\n");
    key.append(cache_namespace);
    key.push_back('\n');
    key.append(name);

    const auto sha = utils::genSha1(std::span<const char>(key.data(), key.size()));
    return std::string("/cache/") + std::string(cache_namespace) + "/" + std::string(TEX_HEADER_DIR) +
           "/" + sha + ".wpth";
}

bool LoadExtraHeaderMap(std::unordered_map<std::string, ImageExtra>& extra_header,
                        fs::IBinaryStream&                         file) {
    extra_header.clear();

    const auto count = file.ReadUint32();
    for (u32 i = 0; i < count; i++) {
        std::string key;
        if (! ReadString(file, key)) return false;

        ImageExtra extra;
        if (! ReadPod(file, extra.val)) return false;
        extra_header.emplace(std::move(key), extra);
    }
    return true;
}

void SaveExtraHeaderMap(const std::unordered_map<std::string, ImageExtra>& extra_header,
                        fs::IBinaryStreamW&                              file) {
    file.WriteUint32(static_cast<u32>(extra_header.size()));
    for (const auto& [key, extra] : extra_header) {
        WriteString(file, key);
        WritePod(file, extra.val);
    }
}

bool LoadSpriteAnimation(SpriteAnimation& animation, fs::IBinaryStream& file) {
    animation = {};

    const auto count = file.ReadUint32();
    for (u32 i = 0; i < count; i++) {
        SpriteFrame frame;
        if (! ReadPod(file, frame.imageId)) return false;
        if (! ReadPod(file, frame.frametime)) return false;
        if (! ReadPod(file, frame.x)) return false;
        if (! ReadPod(file, frame.y)) return false;
        if (! ReadPod(file, frame.width)) return false;
        if (! ReadPod(file, frame.height)) return false;
        if (! ReadPod(file, frame.rate)) return false;
        if (! ReadPod(file, frame.xAxis[0])) return false;
        if (! ReadPod(file, frame.xAxis[1])) return false;
        if (! ReadPod(file, frame.yAxis[0])) return false;
        if (! ReadPod(file, frame.yAxis[1])) return false;
        animation.AppendFrame(frame);
    }
    return true;
}

void SaveSpriteAnimation(const SpriteAnimation& animation, fs::IBinaryStreamW& file) {
    file.WriteUint32(static_cast<u32>(animation.Frames().size()));
    for (const auto& frame : animation.Frames()) {
        WritePod(file, frame.imageId);
        WritePod(file, frame.frametime);
        WritePod(file, frame.x);
        WritePod(file, frame.y);
        WritePod(file, frame.width);
        WritePod(file, frame.height);
        WritePod(file, frame.rate);
        WritePod(file, frame.xAxis[0]);
        WritePod(file, frame.xAxis[1]);
        WritePod(file, frame.yAxis[0]);
        WritePod(file, frame.yAxis[1]);
    }
}

bool LoadCachedImageHeader(ImageHeader& header, uint64_t& file_size, fs::IBinaryStream& file) {
    if (ReadVersion("WTHD", file) != 2) return false;
    if (! ReadPod(file, file_size)) return false;

    if (! ReadPod(file, header.width)) return false;
    if (! ReadPod(file, header.height)) return false;
    if (! ReadPod(file, header.mapWidth)) return false;
    if (! ReadPod(file, header.mapHeight)) return false;
    if (! ReadPod(file, header.mipmap_larger)) return false;
    if (! ReadPod(file, header.mipmap_pow2)) return false;
    if (! ReadPod(file, header.isVideoTexture)) return false;

    int32_t type = 0;
    int32_t format = 0;
    int32_t wrap_s = 0;
    int32_t wrap_t = 0;
    int32_t mag_filter = 0;
    int32_t min_filter = 0;

    if (! ReadPod(file, type)) return false;
    if (! ReadPod(file, format)) return false;
    if (! ReadPod(file, header.count)) return false;
    if (! ReadPod(file, header.isSprite)) return false;
    if (! ReadPod(file, wrap_s)) return false;
    if (! ReadPod(file, wrap_t)) return false;
    if (! ReadPod(file, mag_filter)) return false;
    if (! ReadPod(file, min_filter)) return false;

    header.type             = static_cast<ImageType>(type);
    header.format           = static_cast<TextureFormat>(format);
    header.sample.wrapS     = static_cast<TextureWrap>(wrap_s);
    header.sample.wrapT     = static_cast<TextureWrap>(wrap_t);
    header.sample.magFilter = static_cast<TextureFilter>(mag_filter);
    header.sample.minFilter = static_cast<TextureFilter>(min_filter);

    if (! LoadExtraHeaderMap(header.extraHeader, file)) return false;
    if (! LoadSpriteAnimation(header.spriteAnim, file)) return false;
    if (! header.isVideoTexture) {
        const auto it = header.extraHeader.find("texb_is_video_mp4");
        header.isVideoTexture = it != header.extraHeader.end() && it->second.val == 1;
    }
    return true;
}

void SaveCachedImageHeader(const ImageHeader& header, uint64_t file_size, fs::IBinaryStreamW& file) {
    WriteVersion("WTHD", file, 2);
    WritePod(file, file_size);

    WritePod(file, header.width);
    WritePod(file, header.height);
    WritePod(file, header.mapWidth);
    WritePod(file, header.mapHeight);
    WritePod(file, header.mipmap_larger);
    WritePod(file, header.mipmap_pow2);
    WritePod(file, header.isVideoTexture);

    const auto type       = static_cast<int32_t>(header.type);
    const auto format     = static_cast<int32_t>(header.format);
    const auto wrap_s     = static_cast<int32_t>(header.sample.wrapS);
    const auto wrap_t     = static_cast<int32_t>(header.sample.wrapT);
    const auto mag_filter = static_cast<int32_t>(header.sample.magFilter);
    const auto min_filter = static_cast<int32_t>(header.sample.minFilter);

    WritePod(file, type);
    WritePod(file, format);
    WritePod(file, header.count);
    WritePod(file, header.isSprite);
    WritePod(file, wrap_s);
    WritePod(file, wrap_t);
    WritePod(file, mag_filter);
    WritePod(file, min_filter);

    SaveExtraHeaderMap(header.extraHeader, file);
    SaveSpriteAnimation(header.spriteAnim, file);
}

ImageHeader ParseHeaderUncached(fs::IBinaryStream& file) {
    ImageHeader header;
    LoadHeader(file, header);
    if (header.count < 0) return header;

    usize image_count = (usize)header.count;

    if (header.isSprite) {
        std::vector<std::vector<float>> imageDatas(image_count);
        for (usize i_image = 0; i_image < image_count; i_image++) {
            int mipmap_count = file.ReadInt32();
            for (int32_t i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
                int32_t width  = file.ReadInt32();
                int32_t height = file.ReadInt32();
                if (i_mipmap == 0) {
                    imageDatas.at(i_image) = { (float)width, (float)height };
                    header.mipmap_pow2     = algorism::IsPowOfTwo((u32)(width * height));
                }
                if (header.extraHeader["texb"].val > 1) {
                    int32_t LZ4_compressed    = file.ReadInt32();
                    int32_t decompressed_size = file.ReadInt32();
                    (void)LZ4_compressed;
                    (void)decompressed_size;
                }
                long src_size = file.ReadInt32();
                file.SeekCur(src_size);
            }
        }

        int32_t texs       = ReadTexVesion(file);
        int32_t framecount = file.ReadInt32();
        if (texs > 3) {
            LOG_ERROR("Unkown texs version");
        }
        if (texs == 3) {
            i32 width  = file.ReadInt32();
            i32 height = file.ReadInt32();
            (void)width;
            (void)height;
        }

        for (int32_t i = 0; i < framecount; i++) {
            SpriteFrame sf;
            sf.imageId = file.ReadInt32();
            if (sf.imageId < 0) {
                LOG_ERROR("get neg imageid");
            }
            float spriteWidth  = imageDatas.at((usize)sf.imageId)[0];
            float spriteHeight = imageDatas.at((usize)sf.imageId)[1];

            sf.frametime = file.ReadFloat();
            if (texs == 1) {
                sf.x        = (float)file.ReadInt32() / spriteWidth;
                sf.y        = (float)file.ReadInt32() / spriteHeight;
                sf.xAxis[0] = (float)file.ReadInt32();
                sf.xAxis[1] = (float)file.ReadInt32();
                sf.yAxis[0] = (float)file.ReadInt32();
                sf.yAxis[1] = (float)file.ReadInt32();
            } else {
                sf.x        = file.ReadFloat() / spriteWidth;
                sf.y        = file.ReadFloat() / spriteHeight;
                sf.xAxis[0] = file.ReadFloat();
                sf.xAxis[1] = file.ReadFloat();
                sf.yAxis[0] = file.ReadFloat();
                sf.yAxis[1] = file.ReadFloat();
            }
            sf.width  = (float)std::sqrt(std::pow(sf.xAxis[0], 2) + std::pow(sf.xAxis[1], 2));
            sf.height = (float)std::sqrt(std::pow(sf.yAxis[0], 2) + std::pow(sf.yAxis[1], 2));
            sf.xAxis[0] /= spriteWidth;
            sf.xAxis[1] /= spriteWidth;
            sf.yAxis[0] /= spriteHeight;
            sf.yAxis[1] /= spriteHeight;
            sf.rate = sf.height / sf.width;
            header.spriteAnim.AppendFrame(sf);
        }
    } else {
        i32 mipmap_count = file.ReadInt32();
        for (i32 i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
            i32 width  = file.ReadInt32();
            i32 height = file.ReadInt32();
            if (i_mipmap == 0) SetHeaderPow2(header, width, height);

            bool    LZ4_compressed    = false;
            int32_t decompressed_size = 0;
            if (header.extraHeader["texb"].val > 1) {
                LZ4_compressed    = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }
            (void)decompressed_size;

            const i32 src_size = file.ReadInt32();
            if (src_size <= 0) continue;

            constexpr usize probe_size = 16;
            const auto      to_read = static_cast<usize>(std::min<i32>(src_size, probe_size));
            char            probe[probe_size] {};
            const auto      read = file.Read(probe, to_read);
            if (i_mipmap == 0 && !LZ4_compressed && header.type == ImageType::UNKNOWN &&
                LooksLikeMp4Payload(probe, read)) {
                header.isVideoTexture = true;
                header.extraHeader["texb_is_video_mp4"].val = 1;
            }
            if (src_size > static_cast<i32>(read)) file.SeekCur(src_size - static_cast<i32>(read));
        }
    }
    return header;
}

} // namespace

std::shared_ptr<Image> WPTexImageParser::Parse(const std::string& name) {
    std::string            path    = "/assets/materials/" + name + ".tex";
    std::shared_ptr<Image> img_ptr = std::make_shared<Image>();
    auto&                  img     = *img_ptr;
    img.key                        = name;
    // std::ifstream file = fs::GetFileFstream(vfs, path);
    auto pfile = m_vfs->Open(path);
    if (! pfile) return nullptr;
    auto& file     = *pfile;
    auto  startpos = file.Tell();
    LoadHeader(file, img.header);

    // image
    i32 _image_count = img.header.count;
    if (_image_count < 0) return nullptr;
    usize image_count = (usize)_image_count;

    img.slots.resize(image_count);
    for (usize i_image = 0; i_image < image_count; i_image++) {
        auto& img_slot = img.slots[i_image];
        auto& mipmaps  = img_slot.mipmaps;

        usize mipmap_count = (usize)std::max<i32>(file.ReadInt32(), 0);
        mipmaps.resize(mipmap_count);
        // load image
        for (usize i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
            auto& mipmap  = mipmaps.at(i_mipmap);
            mipmap.width  = file.ReadInt32();
            mipmap.height = file.ReadInt32();
            if (i_mipmap == 0) {
                img_slot.width  = mipmap.width;
                img_slot.height = mipmap.height;
                SetHeaderPow2(img.header, mipmap.width, mipmap.height);
            }

            bool    LZ4_compressed    = false;
            int32_t decompressed_size = 0;
            // check compress
            if (img.header.extraHeader["texb"].val > 1) {
                LZ4_compressed    = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }

            i32 src_size = file.ReadInt32();
            if (src_size <= 0 || mipmap.width <= 0 || mipmap.height <= 0 || decompressed_size < 0)
                return nullptr;

            char* result;
            result = new char[(usize)src_size];
            file.Read(result, (usize)src_size);

            // is LZ4 compress
            if (LZ4_compressed) {
                char* decompressed_char = Lz4Decompress(result, src_size, decompressed_size);
                src_size                = decompressed_size;
                if (decompressed_char != nullptr) {
                    delete[] result;
                    result = decompressed_char;
                } else {
                    LOG_ERROR("lz4 decompress failed");
                    delete[] result;
                    return nullptr;
                }
            }
            if (i_image == 0 && i_mipmap == 0 && img.header.type == ImageType::UNKNOWN &&
                LooksLikeMp4Payload(result, static_cast<size_t>(src_size))) {
                img.header.isVideoTexture = true;
                img.header.extraHeader["texb_is_video_mp4"].val = 1;
            }
            // TEXB0003/TEXB0004 textures may store an image container payload instead of raw RGBA
            // bytes.
            if (HasEmbeddedImagePayload(img.header)) {
                int          decoded_width  = 0;
                int          decoded_height = 0;
                ImageDataPtr decoded_data {};
                const bool   decoded_from_tex = DecodeImageContainerBytes(
                    result, src_size, decoded_width, decoded_height, decoded_data);

                if (! decoded_from_tex) {
                    delete[] result;
                    return nullptr;
                }

                mipmap.width      = decoded_width;
                mipmap.height     = decoded_height;
                mipmap.data       = std::move(decoded_data);
                src_size          = decoded_width * decoded_height * 4;
                img.header.format = TextureFormat::RGBA8;

                if (i_mipmap == 0) {
                    img_slot.width  = decoded_width;
                    img_slot.height = decoded_height;
                    SetHeaderPow2(img.header, decoded_width, decoded_height);
                }
            } else {
                mipmap.data = ImageDataPtr(new uint8_t[(usize)src_size], [](uint8_t* data) {
                    delete[] data;
                });
                std::copy(result, result + src_size, mipmap.data.get());
            }
            mipmap.size = src_size * (i32)sizeof(uint8_t);
            delete[] result;
        }
    }
    return img_ptr;
}

ImageHeader WPTexImageParser::ParseHeader(const std::string& name) {
    std::string path  = "/assets/materials/" + name + ".tex";
    auto        pfile = m_vfs->Open(path);
    if (! pfile) return {};
    auto& file = *pfile;

    const auto file_size = static_cast<uint64_t>(file.Usize());
    if (m_vfs->IsMounted("cache") && ! m_cache_namespace.empty()) {
        const auto cache_path = GetTextureHeaderCachePath(m_cache_namespace, name);
        if (m_vfs->Contains(cache_path)) {
            ImageHeader cached_header;
            uint64_t    cached_size { 0 };
            auto        cache_file = m_vfs->Open(cache_path);
            if (cache_file && LoadCachedImageHeader(cached_header, cached_size, *cache_file) &&
                cached_size == file_size) {
                return cached_header;
            }
        }

        auto header = ParseHeaderUncached(file);
        if (auto cache_file = m_vfs->OpenW(cache_path); cache_file) {
            SaveCachedImageHeader(header, file_size, *cache_file);
        }
        return header;
    }

    return ParseHeaderUncached(file);
}
