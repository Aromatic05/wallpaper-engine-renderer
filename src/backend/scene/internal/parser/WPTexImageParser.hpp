#pragma once
#include "interface/IImageParser.h"
#include "wallpaper/Result.hpp"
#include "fs/VFS.h"

namespace wallpaper
{

class WPTexImageParser : public IImageParser {
public:
    WPTexImageParser(fs::VFS* vfs, std::string cache_namespace = {})
        : m_vfs(vfs), m_cache_namespace(std::move(cache_namespace)) {}
    virtual ~WPTexImageParser() = default;

    std::shared_ptr<Image> Parse(const std::string&) override;
    ImageHeader            ParseHeader(const std::string&) override;
    Result<ImageHeader>    ParseHeaderChecked(const std::string&);

private:
    fs::VFS* m_vfs;
    std::string m_cache_namespace;
};
} // namespace wallpaper
