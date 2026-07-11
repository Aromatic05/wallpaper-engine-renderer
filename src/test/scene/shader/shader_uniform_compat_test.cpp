#include "backend/scene/internal/SpecTexs.hpp"
#include "backend/scene/internal/shader/WPShaderValueUpdater.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"

#include <Eigen/Dense>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace
{
using Matrix4f = Eigen::Matrix<float, 4, 4, Eigen::ColMajor>;

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "shader uniform compatibility test failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

bool NearlyEqual(float lhs, float rhs, float epsilon = 0.0001f) {
    return std::abs(lhs - rhs) <= epsilon;
}

Matrix4f ToMatrix(const wallpaper::ShaderValue& value) {
    Require(value.size() == 16, "matrix uniform must contain 16 floats");
    return Eigen::Map<const Matrix4f>(value.data());
}

void RequireMatrixNear(const Matrix4f& actual,
                       const Matrix4f& expected,
                       std::string_view message) {
    if (! actual.isApprox(expected, 0.0001f)) Fail(message);
}

void TestEffectLayerUniforms() {
    wallpaper::Scene scene;
    scene.ortho[0] = 1000;
    scene.ortho[1] = 800;

    auto cameraNode = std::make_shared<wallpaper::SceneNode>(
        Eigen::Vector3f(500.0f, 400.0f, 0.0f),
        Eigen::Vector3f::Ones(),
        Eigen::Vector3f::Zero(),
        "GlobalCamera");
    auto camera = std::make_shared<wallpaper::SceneCamera>(1000, 800, -1.0f, 1.0f);
    camera->AttatchNode(cameraNode);
    cameraNode->UpdateTrans();
    camera->Update();
    scene.cameras["global"] = camera;
    scene.activeCamera = camera.get();
    scene.sceneGraph->AppendChild(cameraNode);

    auto layerNode = std::make_shared<wallpaper::SceneNode>(
        Eigen::Vector3f(700.0f, 500.0f, 0.0f),
        Eigen::Vector3f::Ones(),
        Eigen::Vector3f::Zero(),
        "AuthoredLayer");
    auto effectHelper = std::make_shared<wallpaper::SceneNode>(
        Eigen::Vector3f(20.0f, 30.0f, 0.0f),
        Eigen::Vector3f::Ones(),
        Eigen::Vector3f::Zero(),
        "EffectHelper");
    scene.sceneGraph->AppendChild(layerNode);
    scene.sceneGraph->AppendChild(effectHelper);
    layerNode->UpdateTrans();
    effectHelper->UpdateTrans();

    wallpaper::WPShaderValueData layerData;
    wallpaper::WPShaderValueData effectData;
    effectData.effect_projection_node = layerNode.get();

    wallpaper::WPShaderValueUpdater updater(&scene);
    updater.SetNodeData(layerNode.get(), layerData);
    updater.SetNodeData(effectHelper.get(), effectData);
    updater.InitUniforms(effectHelper.get(), [](std::string_view name) {
        return name == wallpaper::G_M || name == wallpaper::G_AM ||
               name == wallpaper::G_LMM || name == wallpaper::G_EM ||
               name == wallpaper::G_EMVP || name == wallpaper::G_EMVPI ||
               name == wallpaper::G_DAYTIME || name == wallpaper::G_DAYTIME_LEGACY;
    });

    wallpaper::sprite_map_t sprites;
    std::unordered_map<std::string, wallpaper::ShaderValue> uniforms;
    updater.UpdateUniforms(
        effectHelper.get(),
        sprites,
        [&](std::string_view name, wallpaper::ShaderValue value) {
            uniforms.emplace(std::string(name), std::move(value));
        });

    const auto findUniform = [&](std::string_view name) -> const wallpaper::ShaderValue& {
        const auto it = uniforms.find(std::string(name));
        Require(it != uniforms.end(), "expected shader uniform was not emitted");
        return it->second;
    };

    const Matrix4f helperModel = effectHelper->ModelTrans().cast<float>();
    const Matrix4f layerModel = layerNode->ModelTrans().cast<float>();
    const Matrix4f viewProjection = camera->GetViewProjectionMatrix().cast<float>();
    const Matrix4f layerMvp = viewProjection * layerModel;

    RequireMatrixNear(ToMatrix(findUniform(wallpaper::G_M)),
                      helperModel,
                      "g_ModelMatrix must retain the effect helper transform");
    RequireMatrixNear(ToMatrix(findUniform(wallpaper::G_AM)),
                      helperModel,
                      "g_AltModelMatrix must be emitted when requested alone");
    RequireMatrixNear(ToMatrix(findUniform(wallpaper::G_LMM)),
                      layerModel,
                      "g_LayerModelMatrix must use the authored layer transform");
    RequireMatrixNear(ToMatrix(findUniform(wallpaper::G_EM)),
                      layerModel,
                      "g_EffectModelMatrix must use the authored layer transform");
    RequireMatrixNear(ToMatrix(findUniform(wallpaper::G_EMVP)),
                      layerMvp,
                      "g_EffectModelViewProjectionMatrix must use the authored layer transform");

    const Matrix4f effectMvpInverse = ToMatrix(findUniform(wallpaper::G_EMVPI));
    RequireMatrixNear(layerMvp * effectMvpInverse,
                      Matrix4f::Identity(),
                      "effect MVP inverse must invert the emitted effect MVP");

    const auto& daytime = findUniform(wallpaper::G_DAYTIME);
    const auto& legacyDaytime = findUniform(wallpaper::G_DAYTIME_LEGACY);
    Require(daytime.size() == 1 && legacyDaytime.size() == 1,
            "daytime uniforms must be scalar");
    Require(NearlyEqual(daytime[0], legacyDaytime[0]),
            "new and legacy daytime spellings must receive the same value");
}
} // namespace

int main() {
    TestEffectLayerUniforms();
    return 0;
}
