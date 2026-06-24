#include "WESceneRuntimeDriver.hpp"
#include "WESceneRuntimeSurface.hpp"

#include "utils/Logging.h"
#include "looper/Looper.hpp"

#include "timer/FrameTimer.hpp"
#include "utils/FpsCounter.h"
#include "parser/WPSceneParser.hpp"
#include "scenescript/WPSceneScriptHost.hpp"
#include "scene/Scene.h"
#include "settings/WPUserProperties.hpp"
#include "text/WPTextLayer.hpp"
#include "particle/ParticleSystem.h"
#include "interface/IShaderValueUpdater.h"
#include "resources/WPJson.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

#include "fs/VFS.h"
#include "fs/PhysicalFs.h"
#include "resources/WPPkgFs.hpp"

#include "audio/SoundManager.h"

#include "rendergraph/RenderGraph.hpp"

#include "vulkanrender/SceneToRenderGraph.hpp"
#include "render/vulkanrender/VulkanRender.hpp"
#include <atomic>
#include <charconv>
#include <cmath>
#include <optional>

using namespace wallpaper;

#define CASE_CMD(cmd)      \
    case CMD::CMD_##cmd:   \
        handle_##cmd(msg); \
        break;
#define MHANDLER_CMD(cmd) void handle_##cmd(const std::shared_ptr<looper::Message>& msg)
#define MHANDLER_CMD_IMPL(cl, cmd) \
    void impl_##cl::handle_##cmd(const std::shared_ptr<looper::Message>& msg)
#define CALL_MHANDLER_CMD(cmd, msg) handle_##cmd(msg)

namespace
{
template<typename T>
void AddMsgCmd(looper::Message& msg, T cmd) {
    msg.setInt32("cmd", (int32_t)cmd);
}
template<typename T>
std::shared_ptr<looper::Message> CreateMsgWithCmd(const std::shared_ptr<looper::Handler>& handler,
                                                  T                                       cmd) {
    auto msg = looper::Message::create(0, handler);
    AddMsgCmd(*msg, cmd);
    return msg;
}

std::optional<float> ReadNumericPropertyValue(const std::shared_ptr<looper::Message>& msg) {
    float value { 0.0f };
    if (msg->findFloat("value", &value)) return value;

    int32_t int_value { 0 };
    if (msg->findInt32("value", &int_value)) return static_cast<float>(int_value);

    return std::nullopt;
}

std::optional<std::array<float, 3>> ParseColorString(std::string_view value) {
    std::array<float, 3> color {};
    size_t               start { 0 };
    for (size_t index = 0; index < color.size(); index++) {
        const size_t end = index + 1 == color.size() ? value.size() : value.find(',', start);
        if (end == std::string_view::npos) return std::nullopt;

        const std::string_view token = value.substr(start, end - start);
        float                  channel { 0.0f };
        const auto [ptr, ec] =
            std::from_chars(token.data(), token.data() + token.size(), channel);
        if (ec != std::errc() || ptr != token.data() + token.size()) return std::nullopt;

        color[index] = channel;
        start        = end + 1;
    }
    return color;
}

std::optional<std::array<float, 3>> ReadColorPropertyValue(
    const std::shared_ptr<looper::Message>& msg) {
    std::shared_ptr<std::array<float, 3>> object_value;
    if (msg->findObject("value", &object_value) && object_value) return *object_value;

    std::string string_value;
    if (msg->findString("value", &string_value)) return ParseColorString(string_value);

    return std::nullopt;
}

bool ResolveParticleRuntimeProperty(std::string_view property, std::string* target,
                                    std::string_view* attribute) {
    constexpr std::string_view kPrefix { "particle." };
    if (! property.starts_with(kPrefix)) return false;

    const std::string_view suffix = property.substr(kPrefix.size());
    const size_t           split  = suffix.find('.');
    if (split == std::string_view::npos || split == 0 || split + 1 >= suffix.size()) return false;

    *target    = std::string(suffix.substr(0, split));
    *attribute = suffix.substr(split + 1);
    return true;
}

std::vector<ParticleSubSystem*> ResolveParticleTargets(Scene& scene, std::string_view target) {
    auto appendTargets = [&](int32_t object_id, std::vector<ParticleSubSystem*>* out) {
        auto it = scene.objectRuntimeParticleSubsystems.find(object_id);
        if (it == scene.objectRuntimeParticleSubsystems.end()) return;
        out->insert(out->end(), it->second.begin(), it->second.end());
    };

    std::vector<ParticleSubSystem*> result;

    int32_t object_id { 0 };
    const auto [ptr, ec] = std::from_chars(target.data(), target.data() + target.size(), object_id);
    if (ec == std::errc() && ptr == target.data() + target.size()) {
        appendTargets(object_id, &result);
        return result;
    }

    auto name_it = scene.layerNameToId.find(std::string(target));
    if (name_it == scene.layerNameToId.end()) return result;

    appendTargets(name_it->second, &result);
    return result;
}

bool ApplyParticleRuntimeProperty(Scene& scene, std::string_view property,
                                  const std::shared_ptr<looper::Message>& msg) {
    std::string      target;
    std::string_view attribute;
    if (! ResolveParticleRuntimeProperty(property, &target, &attribute)) return false;

    auto targets = ResolveParticleTargets(scene, target);
    if (targets.empty()) return false;

    if (attribute == "rate") {
        const auto value = ReadNumericPropertyValue(msg);
        if (! value.has_value()) return false;
        for (auto* subsystem : targets) {
            if (subsystem) subsystem->SetRuntimeRateOverride(*value);
        }
        return true;
    }

    if (attribute == "size") {
        const auto value = ReadNumericPropertyValue(msg);
        if (! value.has_value()) return false;
        for (auto* subsystem : targets) {
            if (subsystem) subsystem->SetRuntimeSizeOverride(*value);
        }
        return true;
    }

    if (attribute == "color" || attribute == "colorn") {
        const auto value = ReadColorPropertyValue(msg);
        if (! value.has_value()) return false;
        for (auto* subsystem : targets) {
            if (subsystem) subsystem->SetRuntimeColorOverride(*value);
        }
        return true;
    }

    return false;
}
} // namespace

namespace wallpaper
{
class RenderHandler;

class MainHandler : public looper::Handler {
public:
    enum class CMD
    {
        CMD_LOAD_SCENE,
        CMD_SET_PROPERTY,
        CMD_STOP,
        CMD_FIRST_FRAME,
        CMD_NO
    };

public:
    MainHandler(std::shared_ptr<HostServices> hostServices,
                std::shared_ptr<WESceneEngineServices> engineServices);
    virtual ~MainHandler() {};

    bool init();
    auto renderHandler() const { return m_render_handler; }
    bool inited() const { return m_inited; }

public:
    void onMessageReceived(const std::shared_ptr<looper::Message>& msg) override {
        int32_t cmd_int = (int32_t)CMD::CMD_NO;
        if (msg->findInt32("cmd", &cmd_int)) {
            CMD cmd = static_cast<CMD>(cmd_int);
            switch (cmd) {
                CASE_CMD(SET_PROPERTY);
                CASE_CMD(LOAD_SCENE);
                CASE_CMD(STOP);
                CASE_CMD(FIRST_FRAME);
            default: break;
            }
        }
    }

    void sendCmdLoadScene();
    void sendFirstFrameOk();
    bool isGenGraphviz() const { return m_gen_graphviz; }
    const auto& audioSamples() const { return m_audio_samples; }

private:
    void loadScene();

    MHANDLER_CMD(LOAD_SCENE);
    MHANDLER_CMD(SET_PROPERTY);
    MHANDLER_CMD(STOP);
    MHANDLER_CMD(FIRST_FRAME);
    MHANDLER_CMD(CAPTURE_FRAME);

private:
    bool m_inited { false };
    std::shared_ptr<HostServices>          m_hostServices;
    std::shared_ptr<WESceneEngineServices> m_engineServices;
    std::shared_ptr<Scene>                 m_scene;

    std::string m_assets;
    std::string m_source;
    std::string m_cache_path;
    bool        m_gen_graphviz { false };

    WPSceneParser                        m_scene_parser;
    std::unique_ptr<audio::SoundManager> m_sound_manager;
    FirstFrameCallback                   m_first_frame_callback;
    UserPropertyMap                      m_user_properties;
    std::shared_ptr<std::vector<float>>  m_audio_samples;
    int32_t                              m_capture_frame_number { 1 };

private:
    std::shared_ptr<looper::Looper> m_main_loop;
    std::shared_ptr<looper::Looper> m_render_loop;
    std::shared_ptr<RenderHandler>  m_render_handler;
};
// for macro
using impl_MainHandler = MainHandler;

class RenderHandler : public looper::Handler {
public:
    enum class CMD
    {
        CMD_INIT_VULKAN,
        CMD_SET_SCENE,
        CMD_SET_FILLMODE,
        CMD_SET_SPEED,
        CMD_MOUSE_INPUT,
        CMD_MOUSE_LEFT_BUTTON,
        CMD_APPLY_USER_PROPERTIES,
        CMD_APPLY_AUDIO_SAMPLES,
        CMD_CAPTURE_FRAME,
        CMD_STOP,
        CMD_DRAW,
        CMD_NO
    };
    MainHandler& main_handler;
    RenderHandler(MainHandler& m, std::shared_ptr<WESceneEngineServices> engineServices)
        : main_handler(m)
        , m_frameTimer(engineServices && engineServices->createFrameTimer
                           ? engineServices->createFrameTimer()
                           : std::make_unique<FrameTimer>())
        , m_render(std::make_unique<vulkan::VulkanRender>()) {}
    virtual ~RenderHandler() {
        m_frameTimer->Stop();
        m_render->destroy();
        LOG_INFO("render handler deleted");
    }

    void onMessageReceived(const std::shared_ptr<looper::Message>& msg) override {
        int32_t cmd_int = (int32_t)CMD::CMD_NO;
        if (msg->findInt32("cmd", &cmd_int)) {
            CMD cmd = static_cast<CMD>(cmd_int);
            switch (cmd) {
                CASE_CMD(DRAW);
                CASE_CMD(STOP);
                CASE_CMD(SET_FILLMODE);
                CASE_CMD(SET_SCENE);
                CASE_CMD(SET_SPEED);
                CASE_CMD(MOUSE_INPUT);
                CASE_CMD(MOUSE_LEFT_BUTTON);
                CASE_CMD(APPLY_USER_PROPERTIES);
                CASE_CMD(APPLY_AUDIO_SAMPLES);
                CASE_CMD(CAPTURE_FRAME);
                CASE_CMD(INIT_VULKAN);
            default: break;
            }
        }
    }

    ExSwapchain* exSwapchain() const { return m_render->exSwapchain(); }

    bool renderInited() const { return m_render->inited(); }

    double textRenderScale() const { return std::max(1.0, m_render_scale); }

    void setMousePos(double x, double y) { m_mouse_pos.store(std::array { (float)x, (float)y }); }

private:
    MHANDLER_CMD(MOUSE_INPUT) {
        float x { 0.5f };
        float y { 0.5f };
        if (! msg->findFloat("x", &x) || ! msg->findFloat("y", &y)) return;

        m_mouse_pos.store(std::array { x, y });
        if (! m_scene) return;

        m_scene->mousePositionNormalized = { x, y };
        m_scene->shaderValueUpdater->MouseInput(x, y);
        m_scene->paritileSys->SetMousePos(x, y);
        if (m_scene->scriptHost) {
            m_scene->scriptHost->HandleCursorMove();
        }
    }
    MHANDLER_CMD(MOUSE_LEFT_BUTTON) {
        bool down { false };
        if (! msg->findBool("down", &down)) return;
        if (! m_scene) return;

        m_scene->cursorLeftDown = down;
        if (m_scene->scriptHost) {
            m_scene->scriptHost->HandleCursorButton(down);
        }
    }
    MHANDLER_CMD(STOP) {
        bool stop { false };
        if (msg->findBool("value", &stop)) {
            m_render->setPaused(stop);
            if (stop)
                m_frameTimer->Stop();
            else
                m_frameTimer->Run();
        }
    }
    MHANDLER_CMD(DRAW) {
        m_frameTimer->FrameBegin();
        if (m_rg) {
            m_scene->PassFrameTime(m_frameTimer->IdeaTime() * m_speed);
            if (m_scene->scriptHost) {
                m_scene->scriptHost->FrameBegin(m_scene->frameTime);
            }
            // LOG_INFO("frame info, fps: %.1f, frametime: %.1f", 1.0f, 1000.0f*m_scene->frameTime);
            m_scene->shaderValueUpdater->FrameBegin();
            {
                auto pos = m_mouse_pos.load();
                m_scene->shaderValueUpdater->MouseInput(pos[0], pos[1]);
            }
            m_scene->paritileSys->Emitt();

            if (m_scene->renderGraphTopologyDirty) {
                if (m_rg) m_render->clearLastRenderGraph();
                m_rg = sceneToRenderGraph(*m_scene);
                if (main_handler.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
                m_render->compileRenderGraph(*m_scene, *m_rg);
                m_scene->ClearRenderGraphDirty();
            } else if (m_scene->renderGraphResourcesDirty) {
                m_render->compileRenderGraph(*m_scene, *m_rg, true);
                m_scene->ClearRenderGraphDirty();
            }

            m_render->drawFrame(*m_scene);

            m_scene->shaderValueUpdater->FrameEnd();
            // fps_counter.RegisterFrame();

            if (! m_scene->first_frame_ok) {
                m_scene->first_frame_ok = true;
                main_handler.sendFirstFrameOk();
            }
        }
        m_frameTimer->FrameEnd();
    }
    MHANDLER_CMD(SET_FILLMODE) {
        int32_t value;
        if (msg->findInt32("value", &value)) {
            m_fillmode = (FillMode)value;
            if (m_scene && renderInited()) {
                m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
            }
        }
    }
    MHANDLER_CMD(SET_SCENE) {
        if (msg->findObject("scene", &m_scene)) {
            const double requested_text_render_scale = textRenderScale();
            const double parsed_text_render_scale    = m_scene->textRenderScale;
            m_scene->textRenderScale                 = requested_text_render_scale;
            const bool requires_initial_text_rerender =
                std::abs(parsed_text_render_scale - requested_text_render_scale) > 0.001;
            for (const auto& [layer_id, _] : m_scene->textLayers) {
                if (m_scene->deferredRuntimeTextLayerIds.count(layer_id) != 0) continue;
                if (! requires_initial_text_rerender) continue;
                RebuildTextLayerSceneLayout(*m_scene, layer_id);
            }

#if WP_ENABLE_SCENESCRIPT_RUNTIME
            m_scene->scriptHost = std::make_shared<WPSceneScriptHost>(m_scene.get());
            for (const auto& registration : m_scene->bindingRegistrations) {
                m_scene->scriptHost->RegisterPropertyBinding(registration);
            }
            for (const auto& registration : m_scene->propertyAnimationRegistrations) {
                m_scene->scriptHost->RegisterPropertyAnimation(registration);
            }
            for (const auto& registration : m_scene->scriptRegistrations) {
                m_scene->scriptHost->RegisterPropertyScript(registration);
            }
            m_scene->scriptHost->Initialize();
            if (main_handler.audioSamples()) {
                m_scene->scriptHost->ApplyAudioSamples(*main_handler.audioSamples());
            }
            m_scene->scriptHost->MaterializeDeferredRuntimeLayersForResidency();
#endif
            if (m_rg) m_render->clearLastRenderGraph();
            {
                auto warmup_rg = sceneToPipelineWarmupRenderGraph(*m_scene);
                m_render->warmupRenderGraphPipelines(*m_scene, *warmup_rg);
            }
            m_rg = sceneToRenderGraph(*m_scene);

            if (main_handler.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
            m_render->compileRenderGraph(*m_scene, *m_rg);
            m_scene->ClearRenderGraphDirty();
            m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
        }
    }
    MHANDLER_CMD(SET_SPEED) { msg->findFloat("value", &m_speed); }
    MHANDLER_CMD(APPLY_AUDIO_SAMPLES) {
        std::shared_ptr<std::vector<float>> audio_samples;
        if (! msg->findObject("value", &audio_samples) || ! m_scene || ! m_scene->scriptHost ||
            ! audio_samples)
            return;

        m_scene->scriptHost->ApplyAudioSamples(*audio_samples);
    }
    MHANDLER_CMD(CAPTURE_FRAME) {
        std::string path;
        if (!msg->findString("value", &path) || path.empty()) return;
        int32_t frame_number { 1 };
        msg->findInt32("frame_number", &frame_number);

        std::string error_message;
        if (!m_render->captureNextOffscreenFrame(path, frame_number, &error_message)) {
            LOG_ERROR("frame capture request rejected: path=%s frame=%d error=%s",
                      path.c_str(),
                      frame_number,
                      error_message.c_str());
        } else {
            LOG_INFO("frame capture requested: path=%s frame=%d", path.c_str(), frame_number);
        }
    }
    MHANDLER_CMD(APPLY_USER_PROPERTIES) {
        std::shared_ptr<UserPropertyMap> user_properties;
        if (! msg->findObject("value", &user_properties) || ! user_properties || ! m_scene) return;

#if WP_ENABLE_SCENESCRIPT_RUNTIME
        if (m_scene->scriptHost) {
            m_scene->scriptHost->ApplyUserProperties(*user_properties, false);
            return;
        }
#endif
        m_scene->userProperties = *user_properties;
    }
    MHANDLER_CMD(INIT_VULKAN) {
        std::shared_ptr<RenderInitInfo> info;
        if (msg->findObject("info", &info)) {
            m_render_scale = std::max(1.0, info->render_scale);
            m_render->init(*info);

            // inited, callback to laod scene
            main_handler.sendCmdLoadScene();
        }
    }

public:
    FrameTimer& frameTimer() { return *m_frameTimer; }
    FpsCounter  fps_counter;

private:
    std::unique_ptr<FrameTimer> m_frameTimer;
    std::shared_ptr<Scene> m_scene { nullptr };
    float                  m_speed { 1.0f };
    double                 m_render_scale { 1.0 };

    std::unique_ptr<vulkan::VulkanRender> m_render;
    std::unique_ptr<rg::RenderGraph>      m_rg { nullptr };

    FillMode m_fillmode { FillMode::ASPECTCROP };

    std::atomic<std::array<float, 2>> m_mouse_pos { std::array { 0.5f, 0.5f } };
};
} // namespace wallpaper

WESceneRuntimeDriver::WESceneRuntimeDriver(std::shared_ptr<HostServices> hostServices,
                                           std::shared_ptr<WESceneEngineServices> engineServices)
    : m_hostServices(hostServices ? std::move(hostServices) : CreateDefaultHostServices())
    , m_engineServices(std::move(engineServices))
    , m_main_handler(std::make_shared<MainHandler>(m_hostServices, m_engineServices)) {}

WESceneRuntimeDriver::~WESceneRuntimeDriver() {
    /*
    if(m_offscreen) {
        // no wait
        auto msg = looper::Message::create(0, m_main_handler);
        msg->setObject("self_clean", m_main_handler);
        msg->setCleanAfterDeliver(true);
        m_main_handler = nullptr;
        msg->post();
    }
    */
}

bool WESceneRuntimeDriver::inited() const { return m_main_handler->inited(); }

bool WESceneRuntimeDriver::init() { return m_main_handler->init(); }

void WESceneRuntimeDriver::initVulkan(const RenderInitInfo& info) {
    m_offscreen                             = info.offscreen;
    std::shared_ptr<RenderInitInfo> sp_info = std::make_shared<RenderInitInfo>(info);
    auto                            msg =
        CreateMsgWithCmd(m_main_handler->renderHandler(), RenderHandler::CMD::CMD_INIT_VULKAN);
    msg->setObject("info", sp_info);
    msg->post();
}

void WESceneRuntimeDriver::play() {
    auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_STOP);
    msg->setBool("value", false);
    msg->post();
}
void WESceneRuntimeDriver::pause() {
    auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_STOP);
    msg->setBool("value", true);
    msg->post();
}

void WESceneRuntimeDriver::mouseInput(double x, double y) {
    auto msg = CreateMsgWithCmd(m_main_handler->renderHandler(), RenderHandler::CMD::CMD_MOUSE_INPUT);
    msg->setFloat("x", static_cast<float>(x));
    msg->setFloat("y", static_cast<float>(y));
    msg->post();
}

void WESceneRuntimeDriver::mouseButton(bool down) {
    auto msg =
        CreateMsgWithCmd(m_main_handler->renderHandler(), RenderHandler::CMD::CMD_MOUSE_LEFT_BUTTON);
    msg->setBool("down", down);
    msg->post();
}

#define BASIC_TYPE(NAME, TYPENAME)                                                       \
    void WESceneRuntimeDriver::setProperty##NAME(std::string_view name, TYPENAME value) { \
        auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_SET_PROPERTY); \
        msg->setString("property", std::string(name));                                   \
        msg->set##NAME("value", value);                                                  \
        msg->post();                                                                     \
    }

BASIC_TYPE(Bool, bool);
BASIC_TYPE(Int32, int32_t);
BASIC_TYPE(Float, float);
BASIC_TYPE(String, std::string);
BASIC_TYPE(Object, std::shared_ptr<void>);

ExSwapchain* WESceneRuntimeDriver::exSwapchain() const {
    return m_main_handler->renderHandler()->exSwapchain();
}

MHANDLER_CMD_IMPL(MainHandler, LOAD_SCENE) {
    if (m_render_handler->renderInited()) {
        loadScene();
    }
}

MHANDLER_CMD_IMPL(MainHandler, SET_PROPERTY) {
    std::string property;
    if (msg->findString("property", &property)) {
        if (property == PROPERTY_SOURCE) {
            msg->findString("value", &m_source);
            LOG_INFO("source: %s user-properties=%zu",
                     m_source.c_str(),
                     m_user_properties.size());
            CALL_MHANDLER_CMD(LOAD_SCENE, msg);
        } else if (property == PROPERTY_ASSETS) {
            msg->findString("value", &m_assets);
        } else if (property == PROPERTY_FPS) {
            int32_t fps { 15 };
            msg->findInt32("value", &fps);
            if (fps >= 5) {
                m_render_handler->frameTimer().SetRequiredFps((uint8_t)fps);
            }
        } else if (property == PROPERTY_FILLMODE) {
            int32_t value;
            if (msg->findInt32("value", &value)) {
                auto nmsg =
                    CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_FILLMODE);
                nmsg->setInt32("value", value);
                nmsg->post();
            }
        } else if (property == PROPERTY_GRAPHIVZ) {
            msg->findBool("value", &m_gen_graphviz);
        } else if (property == PROPERTY_MUTED) {
            bool muted { false };
            msg->findBool("value", &muted);
            m_sound_manager->SetMuted(muted);
        } else if (property == PROPERTY_VOLUME) {
            float volume { 1.0f };
            msg->findFloat("value", &volume);
            m_sound_manager->SetVolume(volume);
        } else if (property == PROPERTY_CACHE_PATH) {
            std::string path;
            msg->findString("value", &path);
            m_cache_path = path;
        } else if (property == PROPERTY_FIRST_FRAME_CALLBACK) {
            std::shared_ptr<FirstFrameCallback> cb;
            msg->findObject("value", &cb);
            m_first_frame_callback = *cb;
        } else if (property == PROPERTY_LOAD_USER_PROPERTIES) {
            std::shared_ptr<UserPropertyMap> user_properties;
            if (msg->findObject("value", &user_properties) && user_properties) {
                m_user_properties = *user_properties;
            } else {
                m_user_properties.clear();
            }
            LOG_INFO("staged load user-properties count=%zu", m_user_properties.size());
        } else if (property == PROPERTY_USER_PROPERTIES) {
            std::shared_ptr<UserPropertyMap> user_properties;
            if (msg->findObject("value", &user_properties) && user_properties) {
                m_user_properties = *user_properties;
            } else {
                m_user_properties.clear();
            }
            LOG_INFO("live user-properties count=%zu", m_user_properties.size());
            auto nmsg =
                CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_APPLY_USER_PROPERTIES);
            nmsg->setObject("value", std::make_shared<UserPropertyMap>(m_user_properties));
            nmsg->post();
        } else if (property == PROPERTY_AUDIO_SAMPLES) {
            msg->findObject("value", &m_audio_samples);
            auto nmsg =
                CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_APPLY_AUDIO_SAMPLES);
            nmsg->setObject("value", m_audio_samples);
            nmsg->post();
        } else if (property == PROPERTY_CAPTURE_FRAME) {
            std::string path;
            if (msg->findString("value", &path) && !path.empty()) {
                auto nmsg =
                    CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_CAPTURE_FRAME);
                nmsg->setString("value", path);
                nmsg->setInt32("frame_number", m_capture_frame_number);
                nmsg->post();
            }
        } else if (property == PROPERTY_CAPTURE_FRAME_NUMBER) {
            int32_t frame_number { 1 };
            if (msg->findInt32("value", &frame_number)) {
                m_capture_frame_number = std::max(1, frame_number);
            }
        } else if (property == PROPERTY_SPEED) {
            float speed { 1.0f };
            if (msg->findFloat("value", &speed)) {
                auto nmsg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_SPEED);
                nmsg->setFloat("value", speed);
                nmsg->post();
            }
        } else if (m_scene && ApplyParticleRuntimeProperty(*m_scene, property, msg)) {
            return;
        }
    }
}

MHANDLER_CMD_IMPL(MainHandler, STOP) {
    bool stop { false };
    if (msg->findBool("value", &stop)) {
        if (stop) {
            m_sound_manager->Pause();
        } else {
            m_sound_manager->Play();
        }

        auto msg_r = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_STOP);
        msg_r->setBool("value", stop);
        msg_r->post();
    }
}

MHANDLER_CMD_IMPL(MainHandler, FIRST_FRAME) {
    if (m_first_frame_callback) m_first_frame_callback();
}

void MainHandler::loadScene() {
    if (m_source.empty() || m_assets.empty()) return;

    LOG_INFO("loading scene: %s user-properties=%zu",
             m_source.c_str(),
             m_user_properties.size());

    if (! m_sound_manager->IsInited()) {
        m_sound_manager->Init();
        m_sound_manager->Play();
    } else {
        m_sound_manager->UnMountAll();
    }

    std::shared_ptr<Scene> scene { nullptr };

    // mount assets dir
    if (! m_engineServices || ! m_engineServices->createVfs || ! m_engineServices->createPhysicalFs) {
        LOG_ERROR("scene engine services are incomplete");
        return;
    }

    std::unique_ptr<fs::VFS> pVfs = m_engineServices->createVfs();
    auto&                    vfs  = *pVfs;
    if (! vfs.IsMounted("assets")) {
        auto assetsFs = m_engineServices->createPhysicalFs(m_assets, false);
        bool sus = vfs.Mount("/assets", std::move(assetsFs), "assets");
        if (! sus) {
            LOG_ERROR("Mount assets dir failed");
            return;
        }
    }
    std::filesystem::path pkgPath_fs { m_source };
    pkgPath_fs.replace_extension("pkg");
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgEntry = pkgPath_fs.filename().replace_extension("json").native();
    std::string pkgDir   = pkgPath_fs.parent_path().native();
    std::string scene_id = pkgPath_fs.parent_path().filename().native();

    // Wallpaper Engine scene packages ship a project.json beside the scene pkg. Its
    // general.properties block defines every user-facing property with default values and
    // optional visibility conditions. When the host application did not supply user properties
    // before load, seed them from those defaults so scene scripts that read g_zoom, av_count,
    // color_av, etc. do not run against an empty property map.
    if (m_user_properties.empty()) {
        std::filesystem::path projectJsonPath = pkgPath_fs.parent_path() / "project.json";
        if (std::filesystem::exists(projectJsonPath)) {
            std::ifstream pf(projectJsonPath, std::ios::binary);
            if (pf) {
                std::string project_src((std::istreambuf_iterator<char>(pf)),
                                        std::istreambuf_iterator<char>());
                nlohmann::json project_json;
                if (PARSE_JSON(project_src, project_json)) {
                    auto general = project_json.value("general", nlohmann::json::object());
                    auto props   = general.value("properties", nlohmann::json::object());
                    UserPropertyMap defaults;
                    for (auto it = props.begin(); it != props.end(); ++it) {
                        const auto& prop = it.value();
                        UserProperty user_prop;
                        user_prop.condition = prop.value("condition", "");
                        user_prop.is_boolean = prop.value("type", "") == "bool";
                        const auto& value = prop.value("value", nlohmann::json());
                        if (value.is_boolean()) {
                            user_prop.value = ShaderValue(value.get<bool>() ? 1.0f : 0.0f);
                        } else if (value.is_number()) {
                            user_prop.value = ShaderValue(static_cast<float>(value.get<double>()));
                        } else if (value.is_string()) {
                            user_prop.value = value.get<std::string>();
                        } else {
                            continue;
                        }
                        defaults.emplace(it.key(), std::move(user_prop));
                    }
                    m_user_properties = std::move(defaults);
                    LOG_INFO("seed user-properties from project.json: count=%zu", m_user_properties.size());
                }
            }
        }
    }

    // load pkgfile
    std::unique_ptr<fs::Fs> pkgFs;
    if (m_engineServices->createPackageFs) {
        pkgFs = m_engineServices->createPackageFs(pkgPath);
    }
    if (! pkgFs || ! vfs.Mount("/assets", std::move(pkgFs))) {
        LOG_INFO("load pkg file %s failed, fallback to use dir", pkgPath.c_str());
        // load pkg dir
        auto pkgDirFs = m_engineServices->createPhysicalFs(pkgDir, false);
        if (! vfs.Mount("/assets", std::move(pkgDirFs))) {
            LOG_ERROR("can't load pkg directory: %s", pkgDir.c_str());
            return;
        }
    }
    if (! m_cache_path.empty()) {
        if (m_hostServices && m_hostServices->fileSystem.createDirectories) {
            const bool ready =
                m_hostServices->fileSystem.createDirectories(std::filesystem::path(m_cache_path));
            if (! ready) {
                LOG_ERROR("can't prepare cache folder: %s", m_cache_path.c_str());
            }
        }
        auto cacheFs = m_engineServices->createPhysicalFs(m_cache_path, true);
        if (! vfs.Mount("/cache", std::move(cacheFs), "cache")) {
            LOG_ERROR("can't load cache folder: %s", m_cache_path.c_str());
        } else {
            LOG_INFO("cache folder: %s", m_cache_path.c_str());
        }
    }

    {
        std::string       scene_src;
        const std::string base { "/assets/" };
        {
            std::string scenePath = base + pkgEntry;
            if (vfs.Contains(scenePath)) {
                auto f = vfs.Open(scenePath);
                if (f) scene_src = f->ReadAllStr();
            }
        }
        if (scene_src.empty()) {
            LOG_ERROR("Not supported scene type");
            return;
        }
        scene = m_scene_parser.Parse(scene_id,
                                     scene_src,
                                     vfs,
                                     *m_sound_manager,
                                     &m_user_properties,
                                     m_render_handler->textRenderScale());
        m_scene = scene;
        scene->vfs.swap(pVfs);
    }

    {
        auto msg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_SCENE);
        msg->setObject("scene", scene);
        msg->post();
    }

    // draw first frame
    {
        auto msg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_DRAW);
        msg->post();
    }
}
void MainHandler::sendCmdLoadScene() {
    auto msg = CreateMsgWithCmd(shared_from_this(), MainHandler::CMD::CMD_LOAD_SCENE);
    msg->post();
}
void MainHandler::sendFirstFrameOk() {
    auto msg = CreateMsgWithCmd(shared_from_this(), MainHandler::CMD::CMD_FIRST_FRAME);
    msg->post();
}

bool MainHandler::init() {
    if (m_inited) return true;
    m_main_loop->setName("main");
    m_render_loop->setName("render");

    m_main_loop->start();
    m_render_loop->start();

    m_main_loop->registerHandler(shared_from_this());
    m_render_loop->registerHandler(m_render_handler);

    {
        auto  msg        = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_DRAW);
        auto& frameTimer = m_render_handler->frameTimer();
        frameTimer.SetCallback([msg]() {
            msg->post();
        });
        frameTimer.SetRequiredFps(15);
        frameTimer.Run();
    }

    m_inited = true;
    return true;
}
MainHandler::MainHandler(std::shared_ptr<HostServices> hostServices,
                         std::shared_ptr<WESceneEngineServices> engineServices)
    : m_hostServices(std::move(hostServices)),
      m_engineServices(std::move(engineServices)),
      m_sound_manager(m_engineServices->createSoundManager()),
      m_main_loop(m_engineServices->createLooper()),
      m_render_loop(m_engineServices->createLooper()),
      m_render_handler(std::make_shared<RenderHandler>(*this, m_engineServices)) {}
