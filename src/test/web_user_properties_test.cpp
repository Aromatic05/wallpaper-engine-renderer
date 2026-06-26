#include "backend/web/internal/cef/UserProperties.hpp"

#include <cassert>
#include <string>

int main() {
    const std::string bootstrap = wallpaper::BuildPropertyListenerBootstrapSnippet();
    assert(! bootstrap.empty());
    assert(bootstrap.find("__weweb_applyUserProperties") != std::string::npos);
    assert(bootstrap.find("wallpaperPropertyListener") != std::string::npos);
    assert(bootstrap.find("setInterval") != std::string::npos);

    const std::string initial =
        wallpaper::BuildPropertyListenerApplySnippet("{\"color\":{\"type\":\"combo\",\"value\":\"red\"}}");
    assert(! initial.empty());
    assert(initial.find("__weweb_applyUserProperties") != std::string::npos);
    assert(initial.find("\"color\"") != std::string::npos);
    assert(initial.find("\"red\"") != std::string::npos);

    const std::string patch =
        wallpaper::BuildApplyUserPropertySnippet("audio", "{\"value\":0.7000}");
    assert(! patch.empty());
    assert(patch.find("__weweb_applyUserProperties") != std::string::npos);
    assert(patch.find("\"audio\"") != std::string::npos);
    assert(patch.find("0.7000") != std::string::npos);

    const std::string empty = wallpaper::BuildPropertyListenerApplySnippet({});
    assert(empty.empty());

    return 0;
}
