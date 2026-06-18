#pragma once
#include <vector>
#include <list>
#include <memory>
#include <cstdint>
#include <string>
#include "core/Literals.hpp"
#include "Type.hpp"

namespace wallpaper
{

class SceneNode;
class SceneMesh;

struct SceneImageEffectNode {
    std::string                authored_output;
    std::string                output; // resolved render target for current build
    std::vector<std::string>   authored_textures;
    std::shared_ptr<SceneNode> sceneNode;
    std::string                camera_override;
    bool                       clear_before_draw { false };
    bool                       force_alpha_write { false };
    bool                       premultiplied_source_blend { false };
};

struct SceneImageEffect {
    enum class CmdType
    {
        Copy,
    };
    struct Command {
        CmdType     cmd { CmdType::Copy };
        std::string authored_dst;
        std::string authored_src;
        std::string dst;
        std::string src;
        i32         afterpos { 0 }; // start at 1, 0 for begin at all
    };
    std::vector<Command>            commands;
    std::list<SceneImageEffectNode> nodes;

    void SetLocalVisible(bool visible) { m_local_visible = visible; }
    bool LocalVisible() const { return m_local_visible; }
    void SetBypassTargets(std::string src, std::string dst) {
        m_bypass_src = std::move(src);
        m_bypass_dst = std::move(dst);
    }
    const std::string& BypassSource() const { return m_bypass_src; }
    const std::string& BypassTarget() const { return m_bypass_dst; }
    void SetFinalBypassTarget(std::string value) { m_final_bypass_target = std::move(value); }
    const std::string& FinalBypassTarget() const { return m_final_bypass_target; }

private:
    bool        m_local_visible { true };
    std::string m_bypass_src;
    std::string m_bypass_dst;
    std::string m_final_bypass_target;
};

class SceneImageEffectLayer {
public:
    SceneImageEffectLayer(SceneNode* node, float w, float h, std::string_view pingpong_a,
                          std::string_view pingpong_b);

    void AddEffect(const std::shared_ptr<SceneImageEffect>& node) { m_effects.push_back(node); }
    std::size_t EffectCount() const { return m_effects.size(); }
    auto&       GetEffect(std::size_t index) { return m_effects.at(index); }
    const auto& FirstTarget() const { return m_pingpong_a; }
    SceneMesh&  FinalMesh() const { return *m_final_mesh; }
    SceneNode&  FinalNode() const { return *m_final_node; }
    void        SetFinalBlend(BlendMode m) { m_final_blend = m; }
    void        SetFullscreen(bool value) { fullscreen = value; }
    bool        HasFinalComposite() const;
    void        SetFinalCompositeSource(std::string source);

    void ResolveEffect(const SceneMesh& defualt_mesh, std::string_view effect_cam);

private:
    SceneNode*  m_worldNode;
    std::string m_pingpong_a;
    std::string m_pingpong_b;

    bool fullscreen { false };
    //    std::vector<float> m_size;
    std::unique_ptr<SceneMesh> m_final_mesh;
    std::unique_ptr<SceneNode> m_final_node;
    BlendMode                  m_final_blend;
    std::string                m_final_composite_source;

    std::vector<std::shared_ptr<SceneImageEffect>> m_effects;
};
} // namespace wallpaper
