#include "backend/scene/internal/scene/include/scene/SceneCamera.h"
#include "backend/scene/internal/scene/include/scene/SceneNode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>

namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "camera transform test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

bool Near(const Eigen::Matrix4d& lhs, const Eigen::Matrix4d& rhs, double epsilon = 1e-8) {
    return (lhs - rhs).cwiseAbs().maxCoeff() <= epsilon;
}

void TestCompleteWorldFrame() {
    auto parent = std::make_shared<wallpaper::SceneNode>();
    parent->SetTranslate({ 10.0f, -3.0f, 2.0f });
    parent->SetScale({ 1.5f, 0.5f, 2.0f });

    auto child = std::make_shared<wallpaper::SceneNode>();
    child->SetTranslate({ 4.0f, 6.0f, -2.0f });
    child->SetScale({ 2.0f, 3.0f, 4.0f });
    parent->AppendChild(child);
    child->UpdateTrans();
    const Eigen::Matrix4d authored_world = child->ModelTrans();

    wallpaper::SceneCamera camera(320, 180, -1.0f, 1.0f);
    camera.AttatchNode(child);

    const auto view = camera.GetViewMatrix();
    Require(view.allFinite(), "node camera view must remain finite");
    Require(Near(view * authored_world, Eigen::Matrix4d::Identity()),
            "node camera must invert the complete parented/scaled world frame");
    Require(camera.GetViewProjectionMatrix().allFinite(),
            "node camera view-projection matrix must remain finite");
}

void TestMissingZAxisRepair() {
    auto node = std::make_shared<wallpaper::SceneNode>();
    node->SetTranslate({ 7.0f, 8.0f, 9.0f });
    node->SetScale({ 2.0f, 3.0f, 0.0f });

    wallpaper::SceneCamera camera(320, 180, -1.0f, 1.0f);
    camera.AttatchNode(node);

    const auto view = camera.GetViewMatrix();
    Require(view.allFinite(), "zero-Z node camera must be repaired before inversion");
    Require(std::abs(view.determinant()) > 1e-10,
            "repaired zero-Z node camera view must be invertible");

    const Eigen::Vector4d authored_origin { 7.0, 8.0, 9.0, 1.0 };
    const auto camera_origin = view * authored_origin;
    Require(camera_origin.head<3>().cwiseAbs().maxCoeff() <= 1e-8,
            "repaired node camera must preserve the authored camera position");
}

void TestFullyDegenerateFallback() {
    auto node = std::make_shared<wallpaper::SceneNode>();
    node->SetTranslate({ 5.0f, 6.0f, 7.0f });
    node->SetScale(Eigen::Vector3f::Zero());

    wallpaper::SceneCamera camera(320, 180, -1.0f, 1.0f);
    camera.AttatchNode(node);

    Require(Near(camera.GetViewMatrix(), Eigen::Matrix4d::Identity()),
            "fully degenerate node camera must fall back to identity");
    Require(camera.GetViewProjectionMatrix().allFinite(),
            "fully degenerate node camera view-projection must remain finite");
}

void TestNonFiniteFallback() {
    auto node = std::make_shared<wallpaper::SceneNode>();
    node->SetScale({ std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f });

    wallpaper::SceneCamera camera(320, 180, -1.0f, 1.0f);
    camera.AttatchNode(node);

    Require(Near(camera.GetViewMatrix(), Eigen::Matrix4d::Identity()),
            "non-finite node camera must fall back to identity");
    Require(camera.GetViewProjectionMatrix().allFinite(),
            "non-finite node camera view-projection must remain finite");
}
} // namespace

int main() {
    TestCompleteWorldFrame();
    TestMissingZAxisRepair();
    TestFullyDegenerateFallback();
    TestNonFiniteFallback();
    return 0;
}
