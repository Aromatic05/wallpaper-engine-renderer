#include "backend/scene/internal/parser/WPSyntheticImageParser.hpp"

#include <cassert>
#include <memory>
#include <string>

namespace
{
class FallbackImageParser final : public wallpaper::IImageParser {
public:
    std::shared_ptr<wallpaper::Image> Parse(const std::string& name) override {
        if (name != "fallback") return nullptr;
        auto image           = std::make_shared<wallpaper::Image>();
        image->key           = name;
        image->header.width  = 8;
        image->header.height = 4;
        return image;
    }

    wallpaper::ImageHeader ParseHeader(const std::string& name) override {
        wallpaper::ImageHeader header;
        if (name == "fallback") {
            header.width  = 8;
            header.height = 4;
        }
        return header;
    }
};

std::shared_ptr<wallpaper::Image> MakeSyntheticImage() {
    auto image           = std::make_shared<wallpaper::Image>();
    image->header.width  = 2;
    image->header.height = 2;
    image->slots.resize(1);
    image->slots[0].width  = 2;
    image->slots[0].height = 2;
    wallpaper::ImageData mipmap;
    mipmap.width  = 2;
    mipmap.height = 2;
    mipmap.size   = 16;
    image->slots[0].mipmaps.push_back(std::move(mipmap));
    return image;
}
} // namespace

int main() {
    wallpaper::WPSyntheticImageParser parser(std::make_unique<FallbackImageParser>());
    assert(wallpaper::AsSyntheticImageParser(&parser) == &parser);

    auto synthetic = MakeSyntheticImage();
    parser.RegisterImage("synthetic", synthetic);

    assert(parser.TrackedImageCount() == 1);
    assert(parser.TrackedBytes() == 16);
    assert(parser.Parse("synthetic") == synthetic);
    assert(synthetic->key == "synthetic");

    const auto synthetic_header = parser.ParseHeader("synthetic");
    assert(synthetic_header.width == 2);
    assert(synthetic_header.height == 2);

    const auto fallback_header = parser.ParseHeader("fallback");
    assert(fallback_header.width == 8);
    assert(fallback_header.height == 4);
    assert(parser.Parse("fallback") != nullptr);

    parser.UnregisterImage("synthetic");
    assert(parser.TrackedImageCount() == 0);
    assert(parser.Parse("synthetic") == nullptr);

    return 0;
}
