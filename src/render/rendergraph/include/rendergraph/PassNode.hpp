#pragma once
#include "DependencyGraph.hpp"
#include <string>

namespace wallpaper
{
namespace rg
{

class TexNode;
class PassNode : public DependencyGraph::Node {
public:
    enum class Type {
        CustomShader,
        Clear,
        // Text is a dedicated first-class render-graph pass. Keeping it explicit here lets scene
        // text travel through its own primitive pipeline instead of being folded into generic
        // image/custom-shader passes.
        Text,
        Copy,
        Virtual // for mark a virual writer to update version
    };
    static PassNode* addPassNode(DependencyGraph& dg, Type type);

    Type type() const;
    std::string_view name() const;

    void setName(std::string_view);

    std::string ToGraphviz() const override; 


private:
    Type m_type;
    std::string m_name { "unknown pass" };
};
}
} 
