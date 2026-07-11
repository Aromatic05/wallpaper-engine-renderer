#include "backend/scene/internal/wpscene/WPImageObject.h"
#include "backend/scene/internal/wpscene/WPParticleObject.h"
#include "backend/scene/internal/wpscene/WPSoundObject.h"
#include "backend/scene/internal/wpscene/WPTextObject.h"
#include "backend/scene/internal/resources/WPJson.hpp"
#include "fs/Fs.h"
#include "fs/MemBinaryStream.h"
#include "fs/VFS.h"

#include <nlohmann/json.hpp>

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
using json = nlohmann::json;

[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr, "scene object schema test failure: %.*s\n",
                 static_cast<int>(message.size()), message.data());
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
        return files_.contains(std::string(path));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStream> Open(std::string_view path) override {
        const auto it = files_.find(std::string(path));
        if (it == files_.end()) return nullptr;
        return std::make_shared<wallpaper::fs::MemBinaryStream>(
            std::vector<std::uint8_t>(it->second.begin(), it->second.end()));
    }

    std::shared_ptr<wallpaper::fs::IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::string> files_;
};

void MountFixtures(wallpaper::fs::VFS& vfs) {
    vfs.Mount(
        "/assets",
        std::make_unique<MemoryFs>(std::unordered_map<std::string, std::string> {
            { "/models/test.json",
              R"({"material":"materials/test.json","width":64,"height":32})" },
            { "/materials/test.json",
              R"({"passes":[{"shader":"genericimage2","textures":["materials/base.tex"],"blending":"translucent","cullmode":"nocull","depthtest":"disabled","depthwrite":"disabled"}]})" },
            { "/particles/test.json",
              R"({"emitter":[{"name":"box","id":1}],"material":"materials/test.json","maxcount":8,"starttime":0})" },
        }),
        "schema-test");
}

void TestImageSchema() {
    wallpaper::fs::VFS vfs;
    MountFixtures(vfs);
    wallpaper::wpscene::WPImageObject image;
    const json source = {
        { "image", "models/test.json" },
        { "id", 10 },
        { "name", "schema-image" },
        { "origin", { 0.0f, 0.0f, 0.0f } },
        { "angles", { 0.0f, 0.0f, 0.0f } },
        { "scale", { 1.0f, 1.0f, 1.0f } },
        { "locktransforms", true },
        { "muteineditor", true },
        { "nointerpolation", true },
        { "dependencies", { 2, 3 } },
        { "instance",
          { { "textures", { "materials/override.tex" } },
            { "token", "image-instance" } } },
        { "parent", 7 },
        { "attachment", "socket" },
        { "perspective", true },
        { "copybackground", true },
        { "solid", true },
        { "opaquebackground", true },
        { "clampuvs", true },
        { "castshadow", true },
        { "disablepropagation", true },
        { "depthtest", "disabled" },
        { "backgroundcolor", { 0.1f, 0.2f, 0.3f } },
        { "backgroundbrightness", 0.5f },
        { "animationlayers",
          { { { "animation", 5 },
              { "id", 42 },
              { "name", "idle-transition" },
              { "blend", 0.75 },
              { "rate", 1.25 },
              { "visible", false },
              { "additive", true },
              { "blendin", true },
              { "blendout", true },
              { "blendtime", 0.25 } } } },
    };

    Require(image.FromJson(source, vfs), "image schema fixture should parse");
    Require(image.locktransforms && image.muteineditor && image.nointerpolation,
            "image common flags were not preserved");
    Require(image.dependencies == std::vector<std::int32_t> { 2, 3 },
            "image dependencies were not preserved");
    Require(image.instance.at("token") == "image-instance",
            "image instance JSON was not preserved");
    Require(image.material.textures.size() == 1
                && image.material.textures[0] == "materials/override.tex",
            "existing image instance material override stopped working");
    Require(image.parent == 7 && image.attachment == "socket",
            "image hierarchy metadata mismatch");
    Require(image.perspective && image.copybackground && image.solid
                && image.opaquebackground && image.clampuvs && image.castshadow
                && image.disablepropagation,
            "image feature flags were not preserved");
    Require(image.depthtest == "disabled"
                && image.backgroundcolor == std::array<float, 3> { 0.1f, 0.2f, 0.3f }
                && image.backgroundbrightness == 0.5f,
            "image render metadata mismatch");
    Require(image.puppet_layers.size() == 1,
            "animation layer count mismatch");
    const auto& layer = image.puppet_layers.front();
    Require(layer.id == 5 && layer.layer_id == 42 && layer.name == "idle-transition",
            "animation layer identifiers were not preserved");
    Require(layer.additive && layer.blendin && layer.blendout
                && layer.blendtime == 0.25 && ! layer.visible,
            "animation transition metadata mismatch");
}

void TestParticleSchema() {
    wallpaper::fs::VFS vfs;
    MountFixtures(vfs);
    wallpaper::wpscene::WPParticleObject particle;
    const json source = {
        { "particle", "particles/test.json" },
        { "id", 20 },
        { "name", "schema-particle" },
        { "origin", { 0.0f, 0.0f, 0.0f } },
        { "angles", { 0.0f, 0.0f, 0.0f } },
        { "scale", { 1.0f, 1.0f, 1.0f } },
        { "locktransforms", true },
        { "muteineditor", true },
        { "nointerpolation", true },
        { "dependencies", { 11, 12 } },
        { "instance", { { "kind", "particle-instance" } } },
        { "parent", 9 },
        { "attachment", "particle-socket" },
    };

    Require(particle.FromJson(source, vfs), "particle schema fixture should parse");
    Require(particle.locktransforms && particle.muteineditor && particle.nointerpolation,
            "particle common flags were not preserved");
    Require(particle.dependencies == std::vector<std::int32_t> { 11, 12 }
                && particle.instance.at("kind") == "particle-instance",
            "particle common metadata mismatch");
    Require(particle.parent == 9 && particle.attachment == "particle-socket",
            "particle hierarchy metadata mismatch");
}

void TestTextSchema() {
    wallpaper::fs::VFS vfs;
    wallpaper::wpscene::WPTextObject text;
    const json source = {
        { "id", 30 },
        { "name", "schema-text" },
        { "text", "hello" },
        { "locktransforms", true },
        { "muteineditor", true },
        { "nointerpolation", true },
        { "dependencies", { 21, 22 } },
        { "instance", { { "kind", "text-instance" } } },
        { "parent", 4 },
        { "attachment", "text-socket" },
    };

    Require(text.FromJson(source, vfs), "text schema fixture should parse");
    Require(text.locktransforms, "text locktransforms was not preserved");
    Require(text.muteineditor, "text muteineditor was not preserved");
    Require(text.nointerpolation, "text nointerpolation was not preserved");
    Require(text.dependencies == std::vector<std::int32_t> { 21, 22 }
                && text.instance.at("kind") == "text-instance",
            "text common metadata mismatch");
    Require(text.parent == 4 && text.attachment == "text-socket",
            "text hierarchy metadata mismatch");
}

void TestSoundSchema() {
    wallpaper::fs::VFS vfs;
    wallpaper::wpscene::WPSoundObject sound;
    const json source = {
        { "id", 40 },
        { "name", "schema-sound" },
        { "volume", 0.75f },
        { "playbackmode", "loop" },
        { "sound", { "sounds/test.ogg" } },
        { "locktransforms", true },
        { "muteineditor", true },
        { "nointerpolation", true },
        { "parent", 6 },
        { "dependencies", { 31, 32 } },
        { "instance", { { "kind", "sound-instance" } } },
        { "blockalign", true },
        { "spatialization", true },
        { "queuemode", "replace" },
    };

    Require(sound.FromJson(source, vfs), "sound schema fixture should parse");
    Require(sound.locktransforms && sound.muteineditor && sound.nointerpolation,
            "sound common flags were not preserved");
    Require(sound.parent == 6
                && sound.dependencies == std::vector<std::int32_t> { 31, 32 }
                && sound.instance.at("kind") == "sound-instance",
            "sound common metadata mismatch");
    Require(sound.blockalign && sound.spatialization && sound.queuemode == "replace",
            "sound queue/spatial metadata mismatch");
}
void TestDependencyArrayValidation() {
    std::vector<std::int32_t> dependencies { 9 };
    Require(wallpaper::ReadJsonIntArray(
                json { { "dependencies", { 1, 2, 3 } } },
                "dependencies",
                dependencies),
            "valid dependency array should parse");
    Require(dependencies == std::vector<std::int32_t> { 1, 2, 3 },
            "valid dependency array mismatch");

    const auto unchanged = dependencies;
    Require(! wallpaper::ReadJsonIntArray(
                json { { "dependencies", { 1, 2.5 } } },
                "dependencies",
                dependencies),
            "fractional dependency must be rejected");
    Require(dependencies == unchanged,
            "invalid dependency array must not partially update the destination");

    Require(! wallpaper::ReadJsonIntArray(
                json { { "dependencies", { 1, 4'294'967'295ULL } } },
                "dependencies",
                dependencies),
            "out-of-range dependency must be rejected");
    Require(dependencies == unchanged,
            "out-of-range dependency must not update the destination");
}

} // namespace

int main() {
    TestImageSchema();
    TestParticleSchema();
    TestTextSchema();
    TestSoundSchema();
    TestDependencyArrayValidation();
    return 0;
}
