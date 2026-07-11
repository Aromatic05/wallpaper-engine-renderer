#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/shader/WPShaderValueUpdater.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "parallax propagation test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

class MemoryFs final : public wallpaper::fs::Fs {
public:
    explicit MemoryFs(std::unordered_map<std::string, std::string> files)
        : files_(std::move(files)) {}

    bool Contains(std::string_view path) const override {
        return files_.count(std::string(path)) != 0;
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

void MountAssets(wallpaper::fs::VFS& vfs) {
    Require(vfs.Mount(
                "/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/parent.json",
                      R"({
                          "width": 32,
                          "height": 32,
                          "material": "materials/card.json"
                      })" },
                    { "/child.json",
                      R"({
                          "width": 32,
                          "height": 32,
                          "material": "materials/card.json"
                      })" },
                    { "/materials/card.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "parallax_contract",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/shaders/parallax_contract.vert",
                      R"(
                          attribute vec3 a_Position;
                          attribute vec2 a_TexCoord;
                          varying vec2 v_TexCoord;
                          void main() {
                              gl_Position = vec4(a_Position, 1.0);
                              v_TexCoord = a_TexCoord;
                          }
                      )" },
                    { "/shaders/parallax_contract.frag",
                      R"(
                          varying vec2 v_TexCoord;
                          void main() {
                              gl_FragColor = vec4(v_TexCoord, 0.0, 1.0);
                          }
                      )" },
                }),
                "parallax-contract-assets"),
            "failed to mount synthetic image assets");
}

std::shared_ptr<wallpaper::Scene> ParseScene(bool disable_propagation) {
    wallpaper::WPSceneParser parser;
    wallpaper::fs::VFS vfs;
    wallpaper::audio::SoundManager sound_manager;
    MountAssets(vfs);

    const std::string scene_json = std::string(R"({
        "camera": {
            "center": [0, 0, 0],
            "eye": [0, 0, 1],
            "up": [0, 1, 0]
        },
        "general": {
            "clearcolor": [0, 0, 0],
            "orthogonalprojection": {
                "width": 64,
                "height": 64
            },
            "cameraparallax": true,
            "cameraparallaxamount": 1,
            "zoom": 1
        },
        "objects": [
            {
                "id": 10,
                "name": "Parent",
                "image": "parent.json",
                "origin": [10, 20, 0],
                "angles": [0, 0, 0],
                "scale": [1, 1, 1],
                "parallaxDepth": [0.1, 0.2],
                "disablepropagation": )")
        + (disable_propagation ? "true" : "false")
        + R"( 
            },
            {
                "id": 11,
                "name": "Child",
                "image": "child.json",
                "parent": 10,
                "origin": [2, 3, 0],
                "angles": [0, 0, 0],
                "scale": [1, 1, 1],
                "parallaxDepth": )"
        + (disable_propagation ? "[0.5, 0.25]" : "[0, 0]")
        + R"(
            }
        ]
    })";

    return parser.Parse("parallax-propagation", scene_json, vfs, sound_manager);
}

void VerifyContract(const std::shared_ptr<wallpaper::Scene>& scene,
                    bool expect_parent_parallax) {
    Require(scene != nullptr, "synthetic scene failed to parse");
    Require(scene->layerNodes.count(10) == 1 && scene->layerNodes.count(11) == 1,
            "synthetic parent and child layers were not materialized");

    auto* updater = dynamic_cast<wallpaper::WPShaderValueUpdater*>(scene->shaderValueUpdater.get());
    Require(updater != nullptr, "scene did not expose the Wallpaper Engine shader updater");

    auto* parent_node = scene->layerNodes.at(10);
    auto* child_node = scene->layerNodes.at(11);
    const auto* child_data = updater->GetNodeData(child_node);
    Require(parent_node != nullptr && child_node != nullptr && child_data != nullptr,
            "parent/child shader transform data is missing");
    Require(child_data->transform_binding.mode ==
                wallpaper::WPNodeTransformBindingMode::InheritParent,
            "child must retain normal parent transform inheritance");
    Require(child_data->transform_binding.parent == parent_node,
            "child transform binding must point at the authored parent");

    if (expect_parent_parallax) {
        Require(child_data->parallax_anchor == parent_node,
                "normal parent layer must remain the child parallax anchor");
        Require(scene->parallaxPropagationDisabledLayerIds.count(10) == 0,
                "normal parent must not enter the disabled-propagation set");
    } else {
        Require(child_data->parallax_anchor == nullptr,
                "disablepropagation parent must not become the child parallax anchor");
        Require(scene->parallaxPropagationDisabledLayerIds.count(10) == 1,
                "disablepropagation parent id was not retained by the Scene");
        Require(child_data->parallaxDepth[0] == 0.5f && child_data->parallaxDepth[1] == 0.25f,
                "child authored parallax depth must survive propagation blocking");
    }
}
} // namespace

int main() {
    VerifyContract(ParseScene(false), true);
    VerifyContract(ParseScene(true), false);
    return 0;
}
