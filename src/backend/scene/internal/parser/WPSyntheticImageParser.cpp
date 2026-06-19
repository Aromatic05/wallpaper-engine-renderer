#include "WPSyntheticImageParser.hpp"

#include "utils/Logging.h"

using namespace wallpaper;

namespace
{
size_t EstimateImageBytes(const std::shared_ptr<Image>& image) {
    if (image == nullptr) return 0;

    size_t total = 0;
    for (const auto& slot : image->slots) {
        for (const auto& mipmap : slot.mipmaps) {
            if (mipmap.size > 0) {
                total += static_cast<size_t>(mipmap.size);
            } else {
                total += static_cast<size_t>(std::max(mipmap.width, 0)) *
                         static_cast<size_t>(std::max(mipmap.height, 0)) * 4;
            }
        }
    }
    return total;
}
} // namespace

WPSyntheticImageParser::~WPSyntheticImageParser() = default;

std::shared_ptr<Image> WPSyntheticImageParser::Parse(const std::string& name) {
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_images.find(name); it != m_images.end()) return it->second;
    }
    return m_fallback ? m_fallback->Parse(name) : nullptr;
}

ImageHeader WPSyntheticImageParser::ParseHeader(const std::string& name) {
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_images.find(name); it != m_images.end() && it->second != nullptr) {
            return it->second->header;
        }
    }
    return m_fallback ? m_fallback->ParseHeader(name) : ImageHeader {};
}

void WPSyntheticImageParser::RegisterImage(std::string key, std::shared_ptr<Image> image) {
    if (key.empty() || image == nullptr) return;
    image->key = key;
    std::lock_guard lock(m_mutex);
    m_images[std::move(key)] = std::move(image);
}

void WPSyntheticImageParser::UnregisterImage(const std::string& key) {
    if (key.empty()) return;
    std::lock_guard lock(m_mutex);
    m_images.erase(key);
}

size_t WPSyntheticImageParser::TrackedImageCount() const {
    std::lock_guard lock(m_mutex);
    return m_images.size();
}

size_t WPSyntheticImageParser::TrackedBytes() const {
    std::lock_guard lock(m_mutex);
    size_t total_bytes = 0;
    for (const auto& [key, image] : m_images) {
        (void)key;
        total_bytes += EstimateImageBytes(image);
    }
    return total_bytes;
}

WPSyntheticImageParser* wallpaper::AsSyntheticImageParser(IImageParser* parser) {
    return dynamic_cast<WPSyntheticImageParser*>(parser);
}
