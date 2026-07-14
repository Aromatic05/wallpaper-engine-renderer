#include "backend/scene/internal/parser/WPSceneParser.hpp"
#include "backend/scene/internal/parser/WPSoundParser.hpp"
#include "backend/scene/internal/scene/include/scene/Scene.h"
#include "backend/scene/internal/settings/WPUserProperties.hpp"
#include "backend/scene/internal/wpscene/WPSoundObject.h"
#include "common/fs/include/fs/VFS.h"
#include "host/audio/include/audio/SoundManager.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace
{
[[noreturn]] void Fail(std::string_view message) {
    std::fprintf(stderr,
                 "sound playback policy test failure: %.*s\n",
                 static_cast<int>(message.size()),
                 message.data());
    std::abort();
}

void Require(bool condition, std::string_view message) {
    if (! condition) Fail(message);
}

using PlaybackMode = wallpaper::WPSoundPlaybackMode;

void TestPlaybackModeMatrix() {
    struct TestCase {
        const char*  authored_mode;
        bool         autoplay;
        bool         force_audio_loop;
        PlaybackMode expected_mode;
    };

    const TestCase cases[] = {
        { "single", true, false, PlaybackMode::Single },
        { "single", true, true, PlaybackMode::Loop },
        { "single", false, true, PlaybackMode::Single },
        { "loop", true, true, PlaybackMode::Loop },
        { "loop", false, true, PlaybackMode::Loop },
        { "random", true, true, PlaybackMode::Random },
        { "random", false, true, PlaybackMode::Random },
    };

    wallpaper::wpscene::WPSoundObject object;
    for (const auto& test : cases) {
        object.playbackmode = test.authored_mode;
        const auto policy = wallpaper::WPSoundParser::ResolvePlaybackPolicy(
            object, test.autoplay, test.force_audio_loop);
        Require(policy.autoplay == test.autoplay,
                "resolved policy changed the parser-provided autoplay decision");
        Require(policy.mode == test.expected_mode,
                "resolved playback mode did not preserve the authored/forced contract");
    }
}

wallpaper::UserProperty BooleanProperty(bool value) {
    return wallpaper::UserProperty {
        .value      = wallpaper::ShaderValue(value ? 1.0f : 0.0f),
        .condition  = {},
        .is_boolean = true,
    };
}

struct Fixture {
    // Scene retains a raw pointer to the SoundManager, so destroy Scene first.
    std::unique_ptr<wallpaper::audio::SoundManager> sound_manager;
    std::shared_ptr<wallpaper::Scene>               scene;
};

Fixture ParseScene() {
    wallpaper::WPSceneParser parser;
    auto vfs           = std::make_unique<wallpaper::fs::VFS>();
    auto sound_manager = std::make_unique<wallpaper::audio::SoundManager>();

    wallpaper::UserPropertyMap properties;
    properties.emplace("enable_bound_sound", BooleanProperty(false));

    auto scene = parser.Parse(
        "sound-playback-policy",
        R"({
            "camera":{"center":[0,0,0],"eye":[0,0,1],"up":[0,1,0]},
            "general":{
                "clearcolor":[0,0,0],
                "orthogonalprojection":{"width":64,"height":64},
                "zoom":1
            },
            "objects":[
                {"id":1,"name":"VisibleSingle","sound":["tone.wav"],"playbackmode":"single","visible":true},
                {"id":2,"name":"VisibleLoop","sound":["tone.wav"],"playbackmode":"loop","visible":true},
                {"id":3,"name":"VisibleRandom","sound":["tone.wav"],"playbackmode":"random","visible":true},
                {"id":4,"name":"StaticHidden","sound":["tone.wav"],"playbackmode":"single","visible":false},
                {"id":5,"name":"StartSilent","sound":["tone.wav"],"playbackmode":"single","visible":true,"startsilent":true},
                {
                    "id":6,
                    "name":"UserBoundHidden",
                    "sound":["tone.wav"],
                    "playbackmode":"single",
                    "visible":{"value":true,"user":"enable_bound_sound"}
                },
                {
                    "id":7,
                    "name":"ScriptBoundHidden",
                    "sound":["tone.wav"],
                    "playbackmode":"single",
                    "visible":{"value":false,"script":"export function update() { return false; }"}
                }
            ]
        })",
        *vfs,
        *sound_manager,
        &properties,
        1.0,
        true);
    if (scene != nullptr) scene->vfs = std::move(vfs);
    return { .sound_manager = std::move(sound_manager), .scene = std::move(scene) };
}

wallpaper::audio::SoundHandle Handle(const wallpaper::Scene& scene, int32_t layer_id) {
    const auto it = scene.objectRuntimeSoundHandles.find(layer_id);
    Require(it != scene.objectRuntimeSoundHandles.end(), "expected sound layer was not mounted");
    return it->second;
}

void TestSceneAutoplayUsesInitialEffectiveVisibility() {
    auto fixture = ParseScene();
    Require(fixture.scene != nullptr, "synthetic sound scene failed to parse");

    Require(fixture.sound_manager->IsPlaying(Handle(*fixture.scene, 1)),
            "visible single sound should autoplay");
    Require(fixture.sound_manager->IsPlaying(Handle(*fixture.scene, 2)),
            "visible loop sound should autoplay");
    Require(fixture.sound_manager->IsPlaying(Handle(*fixture.scene, 3)),
            "visible random sound should autoplay");
    Require(! fixture.scene->objectRuntimeSoundHandles.contains(4),
            "static hidden sound should remain pruned");
    Require(! fixture.sound_manager->IsPlaying(Handle(*fixture.scene, 5)),
            "start-silent sound should not autoplay");
    Require(! fixture.sound_manager->IsPlaying(Handle(*fixture.scene, 6)),
            "initially hidden user-bound sound should not autoplay");
    Require(! fixture.sound_manager->IsPlaying(Handle(*fixture.scene, 7)),
            "initially hidden script-bound sound should not autoplay");
}

wallpaper::audio::SoundHandle CreateDynamicSound(Fixture& fixture, int32_t id,
                                                 bool visible, bool start_silent) {
    const nlohmann::json object = {
        { "id", id },
        { "name", "DynamicSound" },
        { "sound", { "tone.wav" } },
        { "playbackmode", "single" },
        { "visible", visible },
        { "startsilent", start_silent },
    };

    int32_t created_id = 0;
    Require(wallpaper::CreateDynamicSceneLayer(
                *fixture.scene, object, nullptr, nullptr, nullptr, nullptr, nullptr, &created_id),
            "dynamic sound layer was not created");
    Require(created_id == id, "dynamic sound layer id changed unexpectedly");
    return Handle(*fixture.scene, created_id);
}

void TestDynamicSoundAutoplay() {
    auto fixture = ParseScene();
    Require(fixture.scene != nullptr, "dynamic sound fixture failed to parse");

    const auto visible = CreateDynamicSound(fixture, 101, true, false);
    const auto hidden = CreateDynamicSound(fixture, 102, false, false);
    const auto start_silent = CreateDynamicSound(fixture, 103, true, true);

    Require(fixture.sound_manager->IsPlaying(visible),
            "visible dynamic sound should autoplay");
    Require(! fixture.sound_manager->IsPlaying(hidden),
            "hidden dynamic sound should not autoplay");
    Require(! fixture.sound_manager->IsPlaying(start_silent),
            "start-silent dynamic sound should not autoplay");
}
} // namespace

int main() {
    TestPlaybackModeMatrix();
    TestSceneAutoplayUsesInitialEffectiveVisibility();
    TestDynamicSoundAutoplay();
    return 0;
}
