#include "WPImageObject.h"
#include "utils/Logging.h"
#include "fs/VFS.h"
#include "WPTexImageParser.hpp"

using namespace wallpaper::wpscene;

// Shared effect parsing lives in WPEffect.cpp. This file now owns only image-object parsing, which
// keeps WPTextObject free from image-object implementation details while preserving image behavior.
namespace
{

void ReadVisibleBinding(const nlohmann::json& json, wallpaper::VisibleBinding* binding) {
    if (! json.is_object()) return;

    GET_JSON_NAME_VALUE_NOWARN(json, "value", binding->value);
    if (! json.contains("user") || json.at("user").is_null()) return;

    const auto& user = json.at("user");
    if (user.is_string()) {
        GET_JSON_VALUE(user, binding->user.name);
        return;
    }

    if (! user.is_object()) return;

    GET_JSON_NAME_VALUE_NOWARN(user, "name", binding->user.name);
    GET_JSON_NAME_VALUE_NOWARN(user, "condition", binding->user.condition);
}

void ReadVisibleProperty(const nlohmann::json& json, bool* visible,
                         wallpaper::VisibleBinding* binding) {
    if (visible == nullptr || binding == nullptr || ! json.contains("visible") ||
        json.at("visible").is_null()) {
        return;
    }

    const auto& visible_json = json.at("visible");
    if (visible_json.is_object()) {
        // Wallpaper Engine stores conditional visibility as an object with both the authored
        // fallback value and the user-property binding. Keep those pieces separate here so the
        // parser can later evaluate the binding contract without trying to coerce the full object
        // into a boolean and logging a bogus conversion error.
        ReadVisibleBinding(visible_json, binding);
        *visible = binding->value;
        return;
    }

    bool parsed_visible = *visible;
    if (GET_JSON_VALUE_NOWARN(visible_json, parsed_visible)) {
        *visible = parsed_visible;
        binding->value = parsed_visible;
    }
}

void ApplyImageInstanceMaterialOverride(const nlohmann::json& object_json, WPMaterial& material) {
    if (! object_json.contains("instance") || object_json.at("instance").is_null()) return;
    const auto& instance_json = object_json.at("instance");
    if (! instance_json.is_object()) return;

    // Instanced image models keep their base material in the model asset and their per-layer
    // texture bindings on the scene object. Media cover layers use that override to replace the
    // model's `util/white` placeholder with `$mediaThumbnail`, so it must be merged before the
    // material is handed to LoadMaterial.
    WPMaterialPass instance_override;
    if (instance_override.FromJson(instance_json)) {
        material.MergePass(instance_override);
    }
}

} // namespace

bool WPImageObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    GET_JSON_NAME_VALUE(json, "image", image);
    ReadVisibleProperty(json, &visible, &visible_binding);
    GET_JSON_NAME_VALUE_NOWARN(json, "alignment", alignment);
    nlohmann::json jImage;
    const std::string imagePath = "/assets/" + image;
    if (!vfs.Contains(imagePath)) {
        // Report missing authored image models as missing assets, not as JSON parse failures on an
        // empty string returned by the VFS read helper. This keeps broken workshop references
        // obvious without hiding or rewriting the bad path.
        LOG_ERROR("ImageObject: image json asset not found: %s", image.c_str());
        return false;
    }
    if(!PARSE_JSON(fs::GetFileContent(vfs, imagePath), jImage)) {
        LOG_ERROR("Can't load image json: %s", image.c_str());
        return false;
    }
    GET_JSON_NAME_VALUE_NOWARN(jImage, "fullscreen", fullscreen);
    GET_JSON_NAME_VALUE_NOWARN(jImage, "autosize", autosize);
    GET_JSON_NAME_VALUE_NOWARN(jImage, "solidlayer", solidlayer);
    // Project-layer is authored in the utility model JSON. Reading it here keeps the later parser
    // decision tied to the resolved asset metadata instead of relying only on a hard-coded path.
    GET_JSON_NAME_VALUE_NOWARN(jImage, "projectlayer", projectlayer);
	GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
	GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
    GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
    GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
    ReadJsonIntArray(json, "dependencies", dependencies);
    if (json.contains("instance") && ! json.at("instance").is_null()) {
        instance = json.at("instance");
    }
	GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
	GET_JSON_NAME_VALUE_NOWARN(json, "attachment", attachment);
	GET_JSON_NAME_VALUE_NOWARN(json, "colorBlendMode", colorBlendMode);
    GET_JSON_NAME_VALUE_NOWARN(json, "copybackground", copybackground);
    GET_JSON_NAME_VALUE_NOWARN(json, "perspective", perspective);
    GET_JSON_NAME_VALUE_NOWARN(json, "solid", solid);
    GET_JSON_NAME_VALUE_NOWARN(json, "opaquebackground", opaquebackground);
    GET_JSON_NAME_VALUE_NOWARN(json, "clampuvs", clampuvs);
    GET_JSON_NAME_VALUE_NOWARN(json, "castshadow", castshadow);
    GET_JSON_NAME_VALUE_NOWARN(json, "disablepropagation", disablepropagation);
    GET_JSON_NAME_VALUE_NOWARN(json, "depthtest", depthtest);
    GET_JSON_NAME_VALUE_NOWARN(json, "backgroundcolor", backgroundcolor);
    GET_JSON_NAME_VALUE_NOWARN(json, "backgroundbrightness", backgroundbrightness);
	if(!fullscreen) {
		GET_JSON_NAME_VALUE(json, "origin", origin);	
		GET_JSON_NAME_VALUE(json, "angles", angles);	
		GET_JSON_NAME_VALUE(json, "scale", scale);	
        // Preserve whether the field was present before parsing it. Missing parallaxDepth and an
        // explicit "0 0" both produce the same numeric array, but they have different import
        // semantics when a root image layer is meant to participate in scene camera parallax.
        parallaxDepthAuthored = json.contains("parallaxDepth") && ! json.at("parallaxDepth").is_null();
		GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
    }
    GET_JSON_NAME_VALUE_NOWARN(jImage, "nopadding", nopadding);
    GET_JSON_NAME_VALUE_NOWARN(json, "color", color);
    GET_JSON_NAME_VALUE_NOWARN(json, "alpha", alpha);
    GET_JSON_NAME_VALUE_NOWARN(json, "brightness", brightness);

	GET_JSON_NAME_VALUE_NOWARN(jImage, "puppet", puppet);	
    if(jImage.contains("material")) {
        std::string matPath;
		GET_JSON_NAME_VALUE(jImage, "material", matPath);	
        nlohmann::json jMat;
        const std::string materialPath = "/assets/" + matPath;
        if (!vfs.Contains(materialPath)) {
            // Keep material lookup diagnostics at the asset layer as well; otherwise a missing
            // material follows the same empty-string JSON parse path and obscures the real author
            // reference that needs to be fixed.
            LOG_ERROR("ImageObject: material json asset not found: %s", matPath.c_str());
            return false;
        }
        if(!PARSE_JSON(fs::GetFileContent(vfs, materialPath), jMat)) {
            LOG_ERROR("Can't load material json: %s", matPath.c_str());
            return false;
        }
        material.FromJson(jMat);
        ApplyImageInstanceMaterialOverride(json, material);
    } else {
        LOG_INFO("image object no material");
        return false;
    }
    if (!fullscreen) {
        if (jImage.contains("width")) {
            int32_t w, h;
            GET_JSON_NAME_VALUE(jImage, "width", w);
            GET_JSON_NAME_VALUE(jImage, "height", h);
            size = { (float)w, (float)h };
        } else if (json.contains("size")) {
            GET_JSON_NAME_VALUE(json, "size", size);
        } else if (autosize) {
            const auto texture_it =
                std::find_if(material.textures.begin(), material.textures.end(), [](const auto& name) {
                    return !name.empty();
                });
            if (texture_it != material.textures.end()) {
                WPTexImageParser texture_parser(&vfs);
                const auto header = texture_parser.ParseHeader(*texture_it);
                if (header.isSprite && !header.spriteAnim.Frames().empty()) {
                    const auto& first_frame = header.spriteAnim.Frames().front();
                    if (first_frame.width > 0.0f && first_frame.height > 0.0f) {
                        size = { first_frame.width, first_frame.height };
                    } else if (header.width > 0 && header.height > 0) {
                        size = { static_cast<float>(header.width), static_cast<float>(header.height) };
                    } else if (header.mapWidth > 0 && header.mapHeight > 0) {
                        size = {
                            static_cast<float>(header.mapWidth),
                            static_cast<float>(header.mapHeight),
                        };
                    } else {
                        size = { origin.at(0) * 2, origin.at(1) * 2 };
                    }
                } else if (header.width > 0 && header.height > 0) {
                    size = { static_cast<float>(header.width), static_cast<float>(header.height) };
                } else if (header.mapWidth > 0 && header.mapHeight > 0) {
                    size = {
                        static_cast<float>(header.mapWidth),
                        static_cast<float>(header.mapHeight),
                    };
                } else {
                    size = { origin.at(0) * 2, origin.at(1) * 2 };
                }
            } else {
                size = { origin.at(0) * 2, origin.at(1) * 2 };
            }
        } else {
            size = { origin.at(0) * 2, origin.at(1) * 2 };
        }
    }
    if(json.contains("effects")) {
        for(const auto& jE:json.at("effects")) {
            WPImageEffect wpeff;
            wpeff.FromJson(jE, vfs);
            effects.push_back(std::move(wpeff));
        }
    }
    if(json.contains("animationlayers")) {
        for(const auto& jLayer:json.at("animationlayers")) {
             WPPuppetLayer::AnimationLayer layer;
             GET_JSON_NAME_VALUE(jLayer, "animation", layer.id);
             GET_JSON_NAME_VALUE(jLayer, "blend", layer.blend);
             GET_JSON_NAME_VALUE(jLayer, "rate", layer.rate);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "visible", layer.visible);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "id", layer.layer_id);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "name", layer.name);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "additive", layer.additive);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "blendin", layer.blendin);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "blendout", layer.blendout);
             GET_JSON_NAME_VALUE_NOWARN(jLayer, "blendtime", layer.blendtime);
             puppet_layers.push_back(layer);
        }
    }
    if(json.contains("config")) {
        const auto& jConf = json.at("config");
        GET_JSON_NAME_VALUE_NOWARN(jConf, "passthrough", config.passthrough);
    }
    return true;
}
