#pragma once
#include "TripleSwapchain.hpp"
#include <array>
#include <cstdint>
#include <limits>

namespace wallpaper
{

enum class TexTiling
{
    OPTIMAL,
    LINEAR
};

enum class ExternalFrameExportMode
{
    OPAQUE_FD,
    DMA_BUF
};

enum class ExternalFrameHandleType
{
    NONE,
    OPAQUE_FD,
    DMA_BUF
};

struct ExPlane {
    int      fd { -1 };
    uint32_t stride { 0 };
    uint32_t offset { 0 };
};

struct ExHandle {
    constexpr static std::size_t MAX_PLANES { 4 };
    constexpr static uint64_t    INVALID_DRM_MODIFIER { std::numeric_limits<uint64_t>::max() };

    ExternalFrameHandleType          handle_type { ExternalFrameHandleType::NONE };
    int         fd { -1 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    std::size_t size { 0 };
    uint32_t    drm_fourcc { 0 };
    uint64_t    drm_modifier { INVALID_DRM_MODIFIER };
    uint32_t    n_planes { 0 };
    bool        premultiplied { false };
    std::array<ExPlane, MAX_PLANES> planes {};

    ExHandle() = default;
    ExHandle(int id): m_id(id) {};

    int32_t id() const { return m_id; }
    bool    isOpaqueFd() const { return handle_type == ExternalFrameHandleType::OPAQUE_FD; }
    bool    isDmabuf() const { return handle_type == ExternalFrameHandleType::DMA_BUF; }
    int     primaryFd() const {
        if (n_planes > 0 && planes[0].fd >= 0) return planes[0].fd;
        return fd;
    }

private:
    int32_t m_id { 0 };
};

// class ExSwapchain : public TripleSwapchain<ExHandle> {};
using ExSwapchain = TripleSwapchain<ExHandle>;
} // namespace wallpaper
