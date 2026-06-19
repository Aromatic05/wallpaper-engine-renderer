#include "backend/scene/internal/WPImageAlignment.hpp"
#include "backend/scene/internal/WPSceneScriptMedia.hpp"
#include "backend/scene/internal/wpscene/WPEffect.h"
#include "scene/Image.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

int main() {
    {
        const auto offset =
            wallpaper::ResolveImageAlignmentOffset("topright", std::array<float, 2> { 200.0f, 80.0f });
        assert(offset.x() == -100.0f);
        assert(offset.y() == -40.0f);
        assert(offset.z() == 0.0f);

        Eigen::Matrix4d model = Eigen::Matrix4d::Identity();
        model(0, 3) = 15.0;
        const auto without_alignment =
            wallpaper::RemoveImageAlignmentOffsetFromModel(model, Eigen::Vector3f { 5.0f, 0.0f, 0.0f });
        assert(without_alignment(0, 3) == 10.0);
    }

    {
        const auto solid = wallpaper::CreateSceneScriptSolidImage("solid", { 1, 2, 3, 4 });
        assert(solid != nullptr);
        assert(solid->key == "solid");
        assert(solid->revision > 0);
        assert(solid->header.width == 1);
        assert(solid->header.height == 1);
        assert(solid->header.extraHeader.at("compo1").val == 1);
        assert(solid->slots.size() == 1);
        assert(solid->slots[0].mipmaps.size() == 1);
        assert(solid->slots[0].mipmaps[0].size == 4);

        const std::array<uint8_t, 8> pixels { 255, 0, 0, 255, 0, 255, 0, 255 };
        const auto rgba = wallpaper::CreateSceneScriptRgbaImage("rgba", 2, 1, pixels);
        assert(rgba != nullptr);
        assert(rgba->revision > solid->revision);
        assert(rgba->header.width == 2);
        assert(rgba->header.height == 1);
        assert(rgba->slots[0].mipmaps[0].size == 8);

        const auto invalid = wallpaper::CreateSceneScriptRgbaImage("bad", 2, 2, pixels);
        assert(invalid == nullptr);
    }

    {
        wallpaper::wpscene::WPEffectFbo fit_fbo;
        fit_fbo.name = "fit";
        fit_fbo.fit = 512;
        const auto fit_size = fit_fbo.ResolveSize({ 1024.0f, 256.0f });
        assert(fit_size[0] == 512);
        assert(fit_size[1] == 128);

        wallpaper::wpscene::WPEffectFbo scaled_fbo;
        scaled_fbo.name = "scaled";
        scaled_fbo.scale = 4;
        const auto scaled_size = scaled_fbo.ResolveSize({ 100.0f, 50.0f });
        assert(scaled_size[0] == 25);
        assert(scaled_size[1] == 13);
    }

    {
        wallpaper::wpscene::WPImageEffect effect;
        wallpaper::wpscene::WPEffectFbo feedback_fbo;
        feedback_fbo.name = "history";
        effect.fbos.push_back(feedback_fbo);

        wallpaper::wpscene::WPMaterialPass pass;
        pass.bind.push_back({ "history", 0 });
        pass.target = "history";
        pass.combos["DIRECTDRAW"] = 1;
        effect.passes.push_back(pass);

        const auto feedback = effect.FeedbackFboNames();
        assert(feedback.count("history") == 1);
        assert(effect.HasEnabledCombo("DIRECTDRAW"));
        assert(! effect.HasEnabledCombo("MISSING"));
    }

    return 0;
}
