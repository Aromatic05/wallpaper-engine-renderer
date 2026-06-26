#include "backend/web/internal/Manifest.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

namespace wallpaper::web
{
namespace
{
std::string LowerAscii(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
} // namespace

std::optional<WebManifestData> LoadWebManifest(const std::filesystem::path& workshop_dir) {
    auto          pj_path = workshop_dir / "project.json";
    std::ifstream is(pj_path);
    if (! is) {
        std::fprintf(stderr, "web: cannot open %s\n", pj_path.c_str());
        return std::nullopt;
    }

    auto j = nlohmann::json::parse(is,
                                   /*callback=*/nullptr,
                                   /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    if (j.is_discarded()) {
        std::fprintf(stderr, "web: invalid JSON in %s\n", pj_path.c_str());
        return std::nullopt;
    }

    auto type_it = j.find("type");
    if (type_it == j.end() || ! type_it->is_string()) {
        std::fprintf(stderr, "web: %s is missing a string \"type\" field\n", pj_path.c_str());
        return std::nullopt;
    }
    // WE corpus has both "web" and "Web" for the type field; fold case.
    std::string type = LowerAscii(type_it->get<std::string>());
    if (type != "web") {
        std::fprintf(stderr,
                     "web: %s has type=\"%s\", expected \"web\"\n",
                     pj_path.c_str(),
                     type.c_str());
        return std::nullopt;
    }

    WebManifestData m;
    m.entry_html = j.value("file", std::string { "index.html" });
    m.title      = j.value("title", std::string { "Wallpaper" });

    if (auto gen = j.find("general"); gen != j.end() && gen->is_object()) {
        auto props = gen->find("properties");
        if (props != gen->end() && props->is_object()) {
            // Verbatim dump — the page side preserves its own {type, value}
            // shape and downstream JavaScript drives conversion. We do not
            // pre-parse to WallpaperSource::initialProperties because
            // Wallpaper Engine's web property model (combo / vec3 / color /
            // slider / ...) does not map onto the runtime's PropertyValue
            // variant.
            m.user_props_json  = props->dump();
            m.has_user_props   = true;
        }
    }

    return m;
}
} // namespace wallpaper::web
