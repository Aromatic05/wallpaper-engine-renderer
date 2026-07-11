#include "WeRendererOptions.hpp"

#include "wallpaper/scene/WESceneContract.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace
{
using Json = nlohmann::json;

wallpaper::Result<Json> ParseObject(std::string_view text, std::string_view label) {
    auto value = Json::parse(text.begin(), text.end(), nullptr, false, true);
    if (value.is_discarded()) {
        return wallpaper::Result<Json>::failure(wallpaper::ResultCode::InvalidArgument,
                                                std::string(label) + " is not valid JSON");
    }
    if (! value.is_object()) {
        return wallpaper::Result<Json>::failure(wallpaper::ResultCode::InvalidArgument,
                                                std::string(label) + " must be a JSON object");
    }
    return wallpaper::Result<Json>::success(std::move(value));
}

wallpaper::Result<void> ValidateUserPropertyValue(std::string_view name, const Json& value) {
    if (value.is_boolean() || value.is_number() || value.is_string()) {
        return wallpaper::Result<void>::success();
    }
    if (value.is_array() && ! value.empty()) {
        for (const auto& component : value) {
            if (! component.is_number()) {
                return wallpaper::Result<void>::failure(
                    wallpaper::ResultCode::InvalidArgument,
                    "user property '" + std::string(name) + "' array must contain only numbers");
            }
        }
        return wallpaper::Result<void>::success();
    }
    return wallpaper::Result<void>::failure(
        wallpaper::ResultCode::InvalidArgument,
        "user property '" + std::string(name)
            + "' must be a boolean, number, string, or non-empty numeric array");
}

wallpaper::Result<void> ValidateUserProperties(const Json& properties) {
    if (! properties.is_object()) {
        return wallpaper::Result<void>::failure(wallpaper::ResultCode::InvalidArgument,
                                                "scene.userProperties must be a JSON object");
    }
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        auto result = ValidateUserPropertyValue(it.key(), it.value());
        if (! result) return result;
    }
    return wallpaper::Result<void>::success();
}
} // namespace

namespace wallpaper
{
Result<std::string> NormalizeUserPropertiesJson(std::string_view jsonText) {
    auto parsed = ParseObject(jsonText, "user properties");
    if (! parsed) return Result<std::string>(parsed.error());

    auto valid = ValidateUserProperties(parsed.value());
    if (! valid) return Result<std::string>(valid.error());

    return Result<std::string>::success(parsed.value().dump());
}

Result<void> ApplyRendererSourceOptionsJson(std::string_view jsonText, WallpaperSource& source) {
    if (jsonText.empty()) return Result<void>::success();

    auto parsed = ParseObject(jsonText, "source options");
    if (! parsed) return Result<void>(parsed.error());
    const auto& root = parsed.value();

    const auto versionIt = root.find("version");
    if (versionIt == root.end() || ! versionIt->is_number_integer()) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "source options must contain integer version 1");
    }
    if (versionIt->get<int>() != 1) {
        return Result<void>::failure(ResultCode::NotSupported,
                                     "unsupported source options version");
    }

    if (source.type != BackendType::WEScene) return Result<void>::success();

    const auto sceneIt = root.find("scene");
    if (sceneIt == root.end()) return Result<void>::success();
    if (! sceneIt->is_object()) {
        return Result<void>::failure(ResultCode::InvalidArgument,
                                     "source options scene field must be an object");
    }

    const auto& scene = *sceneIt;
    const auto userPropertiesIt = scene.find("userProperties");
    if (userPropertiesIt != scene.end()) {
        auto valid = ValidateUserProperties(*userPropertiesIt);
        if (! valid) return valid;
        source.initialProperties[std::string(WE_SCENE_PROPERTY_LOAD_USER_PROPERTIES_JSON)] =
            userPropertiesIt->dump();
    }

    const auto graphvizIt = scene.find("graphviz");
    if (graphvizIt != scene.end()) {
        if (! graphvizIt->is_object()) {
            return Result<void>::failure(ResultCode::InvalidArgument,
                                         "scene.graphviz must be an object");
        }

        bool enabled = true;
        const auto enabledIt = graphvizIt->find("enabled");
        if (enabledIt != graphvizIt->end()) {
            if (! enabledIt->is_boolean()) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "scene.graphviz.enabled must be a boolean");
            }
            enabled = enabledIt->get<bool>();
        }
        source.initialProperties[std::string(WE_SCENE_PROPERTY_GRAPHVIZ)] = enabled;

        const auto pathIt = graphvizIt->find("path");
        if (pathIt != graphvizIt->end()) {
            if (! pathIt->is_string() || pathIt->get_ref<const std::string&>().empty()) {
                return Result<void>::failure(ResultCode::InvalidArgument,
                                             "scene.graphviz.path must be a non-empty string");
            }
            source.initialProperties[std::string(WE_SCENE_PROPERTY_GRAPHVIZ_PATH)] =
                pathIt->get<std::string>();
        }
    }

    return Result<void>::success();
}
} // namespace wallpaper
