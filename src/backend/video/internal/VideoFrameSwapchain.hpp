#pragma once

#include "output/swapchain/ShmFrameSwapchain.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unistd.h>

namespace wallpaper
{
struct VideoDmabufPlane {
    int           fd { -1 };
    std::uint32_t stride { 0 };
    std::uint32_t offset { 0 };
};

struct VideoDmabufFrame {
    std::array<VideoDmabufPlane, 4> planes {};
    std::uint32_t plane_count { 0 };
    std::uint32_t width { 0 };
    std::uint32_t height { 0 };
    std::uint32_t drm_fourcc { 0 };
    std::uint64_t modifier { ExHandle::INVALID_DRM_MODIFIER };
    bool          premultiplied { false };
};

class VideoFrameSwapchain final : public ShmFrameSwapchain {
public:
    using ShmFrameSwapchain::publishFrame;

    explicit VideoFrameSwapchain(std::uint32_t width, std::uint32_t height)
        : ShmFrameSwapchain(width, height) {}

    bool publishFrame(const VideoDmabufFrame& frame) {
        if (frame.width == 0 || frame.height == 0 || frame.plane_count == 0) return false;

        auto* slot = getInprogress();
        if (! slot) return false;
        resetDmabufHandle(*slot);

        slot->handle_type = ExternalFrameHandleType::DMA_BUF;
        slot->width = static_cast<std::int32_t>(frame.width);
        slot->height = static_cast<std::int32_t>(frame.height);
        slot->drm_fourcc = frame.drm_fourcc;
        slot->drm_modifier = frame.modifier;
        slot->n_planes = std::min<std::uint32_t>(frame.plane_count, 4);
        slot->premultiplied = frame.premultiplied;

        for (std::uint32_t i = 0; i < slot->n_planes; ++i) {
            const int dup_fd = ::dup(frame.planes[i].fd);
            if (dup_fd < 0) {
                resetDmabufHandle(*slot);
                return false;
            }
            slot->planes[i].fd = dup_fd;
            slot->planes[i].stride = frame.planes[i].stride;
            slot->planes[i].offset = frame.planes[i].offset;
        }

        m_width = frame.width;
        m_height = frame.height;
        renderFrame();
        return true;
    }

private:
    static void resetDmabufHandle(ExHandle& handle) {
        for (auto& plane : handle.planes) {
            if (plane.fd >= 0) {
                ::close(plane.fd);
                plane.fd = -1;
            }
            plane.offset = 0;
            plane.stride = 0;
        }
        handle.handle_type = ExternalFrameHandleType::NONE;
        handle.width = 0;
        handle.height = 0;
        handle.size = 0;
        handle.shm_stride = 0;
        handle.drm_fourcc = 0;
        handle.drm_modifier = ExHandle::INVALID_DRM_MODIFIER;
        handle.n_planes = 0;
        handle.premultiplied = false;
    }
};
} // namespace wallpaper