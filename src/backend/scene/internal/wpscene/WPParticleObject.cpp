#include "WPParticleObject.h"

#include "utils/Logging.h"
#include "fs/VFS.h"
#include "core/StringHelper.hpp"

#include <string>

using namespace wallpaper::wpscene;

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

} // namespace

bool ParticleChild::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    GET_JSON_NAME_VALUE(json, "name", name);
    GET_JSON_NAME_VALUE(json, "type", type);

    if (name.empty()) {
        return false;
    }

    nlohmann::json jParticle;
    if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + name), jParticle)) return false;

    if (! obj.FromJson(jParticle, vfs)) return false;

    GET_JSON_NAME_VALUE_NOWARN(json, "maxcount", maxcount);
    GET_JSON_NAME_VALUE_NOWARN(json, "controlpointstartindex", controlpointstartindex);
    GET_JSON_NAME_VALUE_NOWARN(json, "probability", probability);

    GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
    GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
    GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
    return true;
}

bool ParticleControlpoint::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "id", id);

    uint32_t _raw_flags { 0 };
    GET_JSON_NAME_VALUE_NOWARN(json, "flags", _raw_flags);
    flags = EFlags(_raw_flags);

    GET_JSON_NAME_VALUE_NOWARN(json, "offset", offset);
    return true;
};

bool ParticleRender::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "name", name);
    // ropetrail require subdivition, replaced
    if (name == "ropetrail") name = "spritetrail";

    if (sstart_with(name, "rope")) {
        GET_JSON_NAME_VALUE_NOWARN(json, "subdivision", subdivision);
    }
    if (name == "spritetrail" || name == "ropetrail") {
        GET_JSON_NAME_VALUE_NOWARN(json, "length", length);
        GET_JSON_NAME_VALUE_NOWARN(json, "maxlength", maxlength);
    }
    return true;
}

bool Emitter::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "name", name);
    GET_JSON_NAME_VALUE(json, "id", id);

    GET_JSON_NAME_VALUE_NOWARN(json, "speedmin", speedmin);
    GET_JSON_NAME_VALUE_NOWARN(json, "speedmax", speedmax);
    GET_JSON_NAME_VALUE_NOWARN(json, "instantaneous", instantaneous);
    GET_JSON_NAME_VALUE_NOWARN(json, "distancemax", distancemax);
    GET_JSON_NAME_VALUE_NOWARN(json, "distancemin", distancemin);
    GET_JSON_NAME_VALUE_NOWARN(json, "rate", rate);
    GET_JSON_NAME_VALUE_NOWARN(json, "directions", directions);
    GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
    GET_JSON_NAME_VALUE_NOWARN(json, "sign", sign);
    GET_JSON_NAME_VALUE_NOWARN(json, "audioprocessingmode", audioprocessingmode);
    GET_JSON_NAME_VALUE_NOWARN(json, "controlpoint", controlpoint);

    if (controlpoint >= static_cast<i32>(kParticleControlpointSlotCount))
        LOG_ERROR("wrong controlpoint %d", controlpoint);
    controlpoint = controlpoint % static_cast<i32>(kParticleControlpointSlotCount);

    uint32_t _raw_flags { 0 };
    GET_JSON_NAME_VALUE_NOWARN(json, "flags", _raw_flags);
    flags = EFlags(_raw_flags);

    std::transform(sign.begin(), sign.end(), sign.begin(), [](int32_t v) {
        if (v != 0)
            return v / std::abs(v);
        else
            return 0;
    });
    return true;
}

bool ParticleInstanceoverride::FromJosn(const nlohmann::json& json) {
    enabled = true;
    GET_JSON_NAME_VALUE_NOWARN(json, "alpha", alpha);
    GET_JSON_NAME_VALUE_NOWARN(json, "size", size);
    GET_JSON_NAME_VALUE_NOWARN(json, "lifetime", lifetime);
    GET_JSON_NAME_VALUE_NOWARN(json, "rate", rate);
    GET_JSON_NAME_VALUE_NOWARN(json, "speed", speed);
    GET_JSON_NAME_VALUE_NOWARN(json, "count", count);
    if (json.contains("color")) {
        GET_JSON_NAME_VALUE(json, "color", color);
        overColor = true;
    } else if (json.contains("colorn")) {
        GET_JSON_NAME_VALUE(json, "colorn", colorn);
        overColorn = true;
    }

    // Wallpaper Engine stores per-layer particle control point overrides as
    // `instanceoverride.controlpointN`. Parse them into an optional table so the scene parser can
    // merge only authored layer overrides while preserving the particle asset's default slots.
    for (std::size_t index = 0; index < controlpointOffsets.size(); index++) {
        const std::string key = "controlpoint" + std::to_string(index);
        if (! json.contains(key) || json.at(key).is_null()) continue;

        std::array<float, 3> offset { 0.0f, 0.0f, 0.0f };
        GET_JSON_NAME_VALUE(json, key, offset);
        controlpointOffsets[index] = offset;
    }
    return true;
};

bool Particle::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    if (! json.contains("emitter")) {
        LOG_ERROR("particle no emitter");
        return false;
    }
    for (const auto& el : json.at("emitter")) {
        Emitter emi;
        emi.FromJson(el);
        emitters.push_back(emi);
    }
    if (json.contains("renderer")) {
        for (const auto& el : json.at("renderer")) {
            ParticleRender pr;
            pr.FromJson(el);
            renderers.push_back(pr);
        }
    }
    // add sprite if no renderers
    if (renderers.empty()) {
        ParticleRender pr;
        pr.name = "sprite";
        renderers.push_back(pr);
    }
    if (json.contains("initializer")) {
        for (const auto& el : json.at("initializer")) {
            initializers.push_back(el);
        }
    }
    if (json.contains("operator")) {
        for (const auto& el : json.at("operator")) {
            operators.push_back(el);
        }
    }
    if (json.contains("controlpoint")) {
        for (const auto& el : json.at("controlpoint")) {
            ParticleControlpoint pc;
            pc.FromJson(el);
            controlpoints.push_back(pc);
        }
    }

    if (json.contains("children")) {
        for (const auto& el : json.at("children")) {
            ParticleChild child;
            if (child.FromJson(el, vfs)) {
                children.push_back(child);
            }
        }
    }
    if (json.contains("material")) {
        std::string matPath;
        GET_JSON_NAME_VALUE(json, "material", matPath);
        nlohmann::json jMat;
        if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) return false;
        material.FromJson(jMat);
    } else {
        LOG_ERROR("particle object no material");
        return false;
    }

    GET_JSON_NAME_VALUE_NOWARN(json, "animationmode", animationmode);
    GET_JSON_NAME_VALUE_NOWARN(json, "sequencemultiplier", sequencemultiplier);
    GET_JSON_NAME_VALUE(json, "maxcount", maxcount);
    GET_JSON_NAME_VALUE(json, "starttime", starttime);

    uint32_t rawflags { 0 };
    GET_JSON_NAME_VALUE_NOWARN(json, "flags", rawflags);
    flags = EFlags(rawflags);

    return true;
}

bool WPParticleObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    GET_JSON_NAME_VALUE(json, "particle", particle);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    if (json.contains("visible")) ReadVisibleBinding(json.at("visible"), &visible_binding);

    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
    GET_JSON_NAME_VALUE_NOWARN(json, "attachment", attachment);
    GET_JSON_NAME_VALUE(json, "origin", origin);
    GET_JSON_NAME_VALUE(json, "angles", angles);
    GET_JSON_NAME_VALUE(json, "scale", scale);
    GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);

    if (json.contains("instanceoverride") && ! json.at("instanceoverride").is_null()) {
        instanceoverride.FromJosn(json.at("instanceoverride"));
    }

    nlohmann::json jParticle;
    if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + particle), jParticle)) return false;
    if (! particleObj.FromJson(jParticle, vfs)) return false;
    return true;
}
