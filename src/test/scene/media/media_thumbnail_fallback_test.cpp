#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/parser/media/Fallback.hpp"
#include "backend/scene/internal/scenescript/WPSceneScriptMedia.hpp"
#include "backend/scene/internal/interface/IImageParser.h"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/wpscene/WPImageObject.h"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "media thumbnail fallback test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

class MemoryFs final : public wallpaper::fs::Fs {
public:
    explicit MemoryFs(std::unordered_map<std::string, std::string> files)
        : files_(std::move(files)) {}

    bool Contains(std::string_view path) const override {
        return files_.contains(std::string(path));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStream> Open(std::string_view path) override {
        const auto it = files_.find(std::string(path));
        if (it == files_.end()) return nullptr;
        return std::make_shared<wallpaper::fs::MemBinaryStream>(
            std::vector<uint8_t>(it->second.begin(), it->second.end()));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::string> files_;
};

template<typename T>
void AppendLE(std::vector<uint8_t>& bytes, T value) {
    using Unsigned = std::make_unsigned_t<T>;
    const auto converted = static_cast<Unsigned>(value);
    for (size_t index = 0; index < sizeof(T); index++) {
        bytes.push_back(static_cast<uint8_t>((converted >> (8 * index)) & 0xffU));
    }
}

void AppendTexVersion(std::vector<uint8_t>& bytes, char prefix, int version) {
    bytes.insert(bytes.end(), { 'T', 'E', 'X', static_cast<uint8_t>(prefix) });
    bytes.push_back(static_cast<uint8_t>('0' + ((version / 1000) % 10)));
    bytes.push_back(static_cast<uint8_t>('0' + ((version / 100) % 10)));
    bytes.push_back(static_cast<uint8_t>('0' + ((version / 10) % 10)));
    bytes.push_back(static_cast<uint8_t>('0' + (version % 10)));
    bytes.push_back('\0');
}

std::string BuildMinimalTexAsset(std::array<uint8_t, 4> rgba) {
    std::vector<uint8_t> bytes;
    AppendTexVersion(bytes, 'V', 1);
    AppendTexVersion(bytes, 'I', 1);
    AppendLE<int32_t>(bytes, 0);
    AppendLE<uint32_t>(bytes, 0);
    AppendLE<int32_t>(bytes, 1);
    AppendLE<int32_t>(bytes, 1);
    AppendLE<int32_t>(bytes, 1);
    AppendLE<int32_t>(bytes, 1);
    AppendLE<int32_t>(bytes, 0);
    AppendTexVersion(bytes, 'B', 1);
    AppendLE<int32_t>(bytes, 1);
    AppendLE<int32_t>(bytes, 1);
    AppendLE<int32_t>(bytes, 1);
    AppendLE<int32_t>(bytes, 1);
    AppendLE<int32_t>(bytes, 4);
    bytes.insert(bytes.end(), rgba.begin(), rgba.end());
    return std::string(bytes.begin(), bytes.end());
}

std::shared_ptr<wallpaper::Scene> ParseScene() {
    wallpaper::WPSceneParser parser;
    wallpaper::fs::VFS vfs;
    wallpaper::audio::SoundManager sound_manager;

    const std::string vertex_shader = R"(
        attribute vec3 a_Position;
        attribute vec2 a_TexCoord;
        varying vec2 v_TexCoord;
        void main() {
            gl_Position = vec4(a_Position, 1.0);
            v_TexCoord = a_TexCoord;
        }
    )";
    const std::string fragment_shader = R"(
        uniform sampler2D g_Texture0;
        varying vec2 v_TexCoord;
        void main() {
            gl_FragColor = texture(g_Texture0, v_TexCoord);
        }
    )";

    Require(vfs.Mount(
                "/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/album.json",
                      R"({"width":16,"height":16,"material":"materials/album.json"})" },
                    { "/consumer.json",
                      R"({"width":16,"height":16,"material":"materials/consumer.json"})" },
                    { "/materials/album.json",
                      R"({"passes":[{"shader":"thumbnail","textures":["fallback_cover"]}]})" },
                    { "/materials/consumer.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "thumbnail",
                                  "textures": ["_rt_imageLayerComposite_21"],
                                  "usertextures": [
                                      { "name": "$mediaThumbnail", "type": "system" }
                                  ]
                              }
                          ]
                      })" },
                    { "/materials/fallback_cover.tex",
                      BuildMinimalTexAsset({ 0x11, 0x22, 0x33, 0x44 }) },
                    { "/shaders/thumbnail.vert", vertex_shader },
                    { "/shaders/thumbnail.frag", fragment_shader },
                }),
                "media-thumbnail-assets"),
            "failed to mount media fallback assets");

    return parser.Parse("media-thumbnail-fallback",
                        R"({
                            "camera": {
                                "center": [0, 0, 0],
                                "eye": [0, 0, 1],
                                "up": [0, 1, 0]
                            },
                            "general": {
                                "clearcolor": [0, 0, 0],
                                "orthogonalprojection": { "width": 64, "height": 64 },
                                "zoom": 1
                            },
                            "objects": [
                                {
                                    "id": 21,
                                    "name": "HiddenAlbumArt",
                                    "visible": false,
                                    "image": "album.json",
                                    "origin": [16, 16, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1]
                                },
                                {
                                    "id": 22,
                                    "name": "ThumbnailConsumer",
                                    "visible": true,
                                    "image": "consumer.json",
                                    "origin": [32, 32, 0],
                                    "angles": [0, 0, 0],
                                    "scale": [1, 1, 1]
                                }
                            ]
                        })",
                        vfs,
                        sound_manager);
}
} // namespace

int main() {
    wallpaper::wpscene::WPImageObject image;
    Require(wallpaper::CanUseImageAsSystemMediaFallback(image),
            "simple static image was rejected as media fallback");
    image.puppet = "mesh.mdl";
    Require(!wallpaper::CanUseImageAsSystemMediaFallback(image),
            "puppet image was accepted as media fallback");
    image.puppet.clear();
    image.fullscreen = true;
    Require(!wallpaper::CanUseImageAsSystemMediaFallback(image),
            "fullscreen image was accepted as media fallback");
    image.fullscreen = false;
    image.config.passthrough = true;
    Require(!wallpaper::CanUseImageAsSystemMediaFallback(image),
            "passthrough image was accepted as media fallback");
    image.config.passthrough = false;
    image.effects.emplace_back();
    Require(!wallpaper::CanUseImageAsSystemMediaFallback(image),
            "effect-backed image was accepted as media fallback");

    auto scene = ParseScene();
    Require(scene != nullptr, "media fallback scene failed to parse");
    auto fallback = scene->imageParser->Parse(
        std::string(wallpaper::WP_SCENE_SCRIPT_MEDIA_THUMBNAIL_TEXTURE));
    Require(fallback != nullptr && fallback->slots.size() == 1 &&
                fallback->slots.front().mipmaps.size() == 1,
            "media thumbnail fallback image was not registered");
    const auto& mipmap = fallback->slots.front().mipmaps.front();
    Require(mipmap.width == 1 && mipmap.height == 1 && mipmap.size == 4 &&
                mipmap.data != nullptr,
            "media thumbnail fallback has the wrong extent");
    Require(mipmap.data.get()[0] == 0x11 && mipmap.data.get()[1] == 0x22 &&
                mipmap.data.get()[2] == 0x33 && mipmap.data.get()[3] == 0x44,
            "media thumbnail fallback did not preserve the source pixels");
    return 0;
}
