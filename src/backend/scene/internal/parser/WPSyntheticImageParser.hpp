#pragma once

#include "interface/IImageParser.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace wallpaper
{

class WPSyntheticImageParser : public IImageParser {
public:
    explicit WPSyntheticImageParser(std::unique_ptr<IImageParser> fallback)
        : m_fallback(std::move(fallback)) {}
    ~WPSyntheticImageParser() override;

    std::shared_ptr<Image> Parse(const std::string& name) override;
    ImageHeader            ParseHeader(const std::string& name) override;

    void   RegisterImage(std::string key, std::shared_ptr<Image> image);
    void   UnregisterImage(const std::string& key);
    size_t TrackedImageCount() const;
    size_t TrackedBytes() const;

private:
    std::unique_ptr<IImageParser>                         m_fallback;
    std::unordered_map<std::string, std::shared_ptr<Image>> m_images;
    mutable std::mutex                                    m_mutex;
};

WPSyntheticImageParser* AsSyntheticImageParser(IImageParser* parser);

} // namespace wallpaper
