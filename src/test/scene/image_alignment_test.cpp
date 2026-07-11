#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/scenescript/WPSceneScriptHost.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneMesh.h"
#include "backend/scene/internal/SpecTexs.hpp"
#include "backend/scene/internal/shader/WPShaderValueUpdater.hpp"
#include "backend/scene/internal/transform/WPNodeTransformResolver.hpp"
#include "common/fs/include/fs/Fs.h"
#include "common/fs/include/fs/MemBinaryStream.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"

#include <array>
#include <cmath>
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
                 "image alignment test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

bool Near(double lhs, double rhs, double epsilon = 0.0001) {
    return std::abs(lhs - rhs) <= epsilon;
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

void MountAssets(wallpaper::fs::VFS& vfs) {
    Require(vfs.Mount(
                "/assets",
                std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
                    { "/parent.json", R"({ "material": "materials/card.json" })" },
                    { "/child.json",
                      R"({ "width": 2, "height": 2, "material": "materials/card.json" })" },
                    { "/materials/card.json",
                      R"({
                          "passes": [
                              {
                                  "shader": "alignment_contract",
                                  "textures": []
                              }
                          ]
                      })" },
                    { "/shaders/alignment_contract.vert",
                      R"(
                          attribute vec3 a_Position;
                          attribute vec2 a_TexCoord;
                          varying vec2 v_TexCoord;
                          void main() {
                              gl_Position = vec4(a_Position, 1.0);
                              v_TexCoord = a_TexCoord;
                          }
                      )" },
                    { "/shaders/alignment_contract.frag",
                      R"(
                          varying vec2 v_TexCoord;
                          void main() {
                              gl_FragColor = vec4(v_TexCoord, 0.0, 1.0);
                          }
                      )" },
                }),
                "image-alignment-assets"),
            "failed to mount synthetic image assets");
}

std::shared_ptr<wallpaper::Scene> ParseScene() {
    wallpaper::WPSceneParser parser;
    wallpaper::fs::VFS vfs;
    wallpaper::audio::SoundManager sound_manager;
    MountAssets(vfs);

    const std::string scene_json = R"({
        "camera": {
            "center": [0, 0, 0],
            "eye": [0, 0, 1],
            "up": [0, 1, 0]
        },
        "general": {
            "clearcolor": [0, 0, 0],
            "orthogonalprojection": {
                "width": 256,
                "height": 256
            },
            "zoom": 1
        },
        "objects": [
            {
                "id": 10,
                "name": "AlignedParent",
                "image": "parent.json",
                "origin": [100, 50, 0],
                "angles": [0, 0, 0],
                "scale": [1, 1, 1],
                "alignment": "topleft",
                "size": {
                    "value": [20, 10],
                    "animation": {
                        "options": {
                            "fps": 10,
                            "length": 10,
                            "mode": "single",
                            "name": "aligned-size"
                        },
                        "c0": [
                            { "frame": 0, "value": 20 },
                            { "frame": 10, "value": 40 }
                        ],
                        "c1": [
                            { "frame": 0, "value": 10 },
                            { "frame": 10, "value": 30 }
                        ]
                    }
                },
                "visible": true
            },
            {
                "id": 11,
                "name": "Child",
                "image": "child.json",
                "parent": 10,
                "origin": [3, 4, 0],
                "angles": [0, 0, 0],
                "scale": [1, 1, 1],
                "visible": true
            }
        ]
    })";

    return parser.Parse("image-alignment", scene_json, vfs, sound_manager);
}

std::array<float, 8> ReadQuadXY(const wallpaper::SceneMesh& mesh) {
    Require(mesh.VertexCount() > 0, "image mesh has no vertex array");
    const auto& vertex = mesh.GetVertexArray(0);
    Require(vertex.VertexCount() == 4, "image mesh must contain four vertices");
    const auto offsets = vertex.GetAttrOffsetMap();
    Require(offsets.contains(wallpaper::WE_IN_POSITION.data()),
            "image mesh position attribute is missing");
    const auto position_offset =
        offsets.at(wallpaper::WE_IN_POSITION.data()).offset / sizeof(float);

    std::array<float, 8> xy {};
    for (std::size_t index = 0; index < 4; ++index) {
        const float* position = vertex.Data() + index * vertex.OneSize() + position_offset;
        xy[index * 2] = position[0];
        xy[index * 2 + 1] = position[1];
    }
    return xy;
}

void RequireTopLeftQuad(const wallpaper::SceneMesh& mesh,
                        float width,
                        float height,
                        std::string_view message) {
    const auto xy = ReadQuadXY(mesh);
    const std::array<float, 8> expected {
        0.0f, -height,
        0.0f, 0.0f,
        width, -height,
        width, 0.0f,
    };
    for (std::size_t index = 0; index < xy.size(); ++index) {
        if (! Near(xy[index], expected[index])) Fail(message);
    }
}

Eigen::Matrix4d ResolveRawModelTransform(wallpaper::Scene& scene,
                                         wallpaper::SceneNode* parent,
                                         wallpaper::SceneNode* child) {
    auto* updater = dynamic_cast<wallpaper::WPShaderValueUpdater*>(scene.shaderValueUpdater.get());
    Require(updater != nullptr, "scene did not expose the Wallpaper Engine shader updater");
    const auto* parent_data = updater->GetNodeData(parent);
    const auto* child_data = updater->GetNodeData(child);
    Require(parent_data != nullptr && child_data != nullptr,
            "aligned parent/child transform data is missing");

    wallpaper::Map<void*, wallpaper::WPShaderValueData> node_data;
    node_data[parent] = *parent_data;
    node_data[child] = *child_data;
    wallpaper::Map<void*, Eigen::Matrix4d> model_cache;
    wallpaper::Map<void*, Eigen::Vector3f> parallax_cache;
    wallpaper::Map<void*, Eigen::Affine3f> attachment_cache;
    std::array<float, 2> mouse { 0.5f, 0.5f };
    wallpaper::WPCameraParallax parallax {};
    wallpaper::WPNodeTransformResolver resolver(
        scene,
        parallax,
        node_data,
        model_cache,
        parallax_cache,
        attachment_cache,
        nullptr,
        mouse,
        0);
    return resolver.ResolveRawModelTransform(child);
}
} // namespace

int main() {
    auto scene = ParseScene();
    Require(scene != nullptr, "synthetic aligned image scene failed to parse");
    Require(scene->layerNodes.contains(10) && scene->layerNodes.contains(11),
            "aligned parent/child layers were not materialized");

    auto* parent = scene->layerNodes.at(10);
    auto* child = scene->layerNodes.at(11);
    Require(parent != nullptr && parent->Mesh() != nullptr && child != nullptr,
            "aligned image runtime nodes are incomplete");
    Require(parent->AlignmentOffset().isZero(0.0001f),
            "image alignment must not be stored in the SceneNode transform");
    RequireTopLeftQuad(*parent->Mesh(), 20.0f, 10.0f,
                       "cold-parsed top-left quad geometry mismatch");

    parent->UpdateTrans();
    Require(Near(parent->ModelTrans()(0, 3), 100.0)
                && Near(parent->ModelTrans()(1, 3), 50.0),
            "image origin must remain the node transform pivot");
    const auto child_model = ResolveRawModelTransform(*scene, parent, child);
    Require(Near(child_model(0, 3), 103.0) && Near(child_model(1, 3), 54.0),
            "child transform must not inherit image alignment geometry offset");

    auto host = std::make_shared<wallpaper::WPSceneScriptHost>(scene.get());
    for (const auto& registration : scene->propertyAnimationRegistrations) {
        Require(host->RegisterPropertyAnimation(registration),
                "image size property animation registration failed");
    }
    host->Initialize();
    host->FrameBegin(0.5);

    RequireTopLeftQuad(*parent->Mesh(), 30.0f, 20.0f,
                       "runtime-resized top-left quad geometry mismatch");
    parent->UpdateTrans();
    Require(Near(parent->ModelTrans()(0, 3), 100.0)
                && Near(parent->ModelTrans()(1, 3), 50.0),
            "runtime image size must not move the authored pivot");
    const auto resized_child_model = ResolveRawModelTransform(*scene, parent, child);
    Require(Near(resized_child_model(0, 3), 103.0)
                && Near(resized_child_model(1, 3), 54.0),
            "runtime image size must not move descendants");

    return 0;
}
