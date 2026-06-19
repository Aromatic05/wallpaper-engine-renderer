#include "CreateScenePackageFs.hpp"

#include "WPPkgFs.hpp"

namespace wallpaper
{
std::unique_ptr<fs::Fs> CreateScenePackageFs(std::string_view pkgPath) {
    return std::unique_ptr<fs::Fs>(fs::WPPkgFs::CreatePkgFs(pkgPath).release());
}
}
