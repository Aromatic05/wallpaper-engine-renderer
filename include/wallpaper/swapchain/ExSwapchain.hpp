#pragma once

#include "TripleSwapchain.hpp"

#include <cstddef>
#include <cstdint>

namespace wallpaper
{
enum class TexTiling
{
    OPTIMAL,
    LINEAR
};

struct ExHandle {
    int         fd;
    std::int32_t width;
    std::int32_t height;
    std::size_t size;

    ExHandle() = default;
    explicit ExHandle(int id)
        : m_id(id) {}

    std::int32_t id() const { return m_id; }

private:
    std::int32_t m_id { 0 };
};

using ExSwapchain = TripleSwapchain<ExHandle>;
} // namespace wallpaper
