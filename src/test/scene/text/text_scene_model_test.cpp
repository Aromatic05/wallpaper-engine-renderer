#include "backend/scene/internal/wpscene/WPTextObject.h"
#include "backend/scene/internal/scene/include/scene/SceneTextPrimitive.h"

#include "common/fs/include/fs/VFS.h"

#include <cassert>

#include <nlohmann/json.hpp>

int main() {
    wallpaper::fs::VFS vfs;
    wallpaper::wpscene::WPTextObject object;
    const bool parsed = object.FromJson(
        nlohmann::json {
            { "id", 42 },
            { "name", "Clock" },
            { "origin", "10 20 30" },
            { "scale", "1 2 1" },
            { "size", nlohmann::json::array({ 300.0f, 80.0f }) },
            { "text", { { "value", "12:34" }, { "script", "return value;" } } },
            { "font", "Inter" },
            { "padding", "4 8 12 16" },
            { "pointsize", 24.0f },
            { "horizontalalign", "center" },
            { "verticalalign", "middle" },
            { "visible", { { "value", true }, { "script", "return value;" } } },
        },
        vfs);

    assert(parsed);
    assert(object.id == 42);
    assert(object.name == "Clock");
    assert(object.text == "12:34");
    assert(object.font == "Inter");
    assert(object.size_explicit);
    assert(object.has_visible_script);
    assert(object.has_dynamic_layout_script);
    assert(object.padding == 16);
    assert(object.padding_edges == (std::array<int32_t, 4> { 4, 8, 12, 16 }));
    assert(object.horizontalalign == "center");
    assert(object.verticalalign == "middle");

    wallpaper::SceneTextPrimitive primitive;
    primitive.object = object;
    primitive.layout.visible_display_size = { 300.0f, 80.0f };
    primitive.layout.visible_source_size = { 600.0f, 160.0f };
    primitive.layout.visible_display_offset = { 5.0f, -3.0f };

    assert(primitive.VisibleDisplaySize()[0] == 300.0f);
    assert(primitive.VisibleSourceSize()[1] == 160.0f);
    assert(primitive.BackgroundLocalOffset()[0] == -5.0f);

    primitive.object.opaquebackground = true;
    assert(primitive.BackgroundLocalOffset()[0] == 0.0f);
    assert(primitive.BackgroundLocalOffset()[1] == 0.0f);

    return 0;
}
