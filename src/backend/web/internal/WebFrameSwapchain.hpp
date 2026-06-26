#pragma once

#include "wallpaper/swapchain/ExSwapchain.hpp"
#include "wallpaper/web/WebTypes.hpp"

#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <unistd.h>
#include <drm/drm_fourcc.h>

namespace wallpaper
{
class WebFrameSwapchain final : public ExSwapchain {
    using atomic_ = std::atomic<ExHandle*>;

public:
    explicit WebFrameSwapchain(std::uint32_t width, std::uint32_t height)
        : m_presented(&m_handles[0]), m_ready(&m_handles[1]), m_inprogress(&m_handles[2]),
          m_width(width), m_height(height) {}

    ~WebFrameSwapchain() override {
        for (auto& handle : m_handles) {
            resetHandle(handle);
        }
    }

    bool publishFrame(const DmaBufFrame& frame) {
        const std::uint32_t width =
            static_cast<std::uint32_t>(std::max(frame.visible_width, frame.coded_width));
        const std::uint32_t height =
            static_cast<std::uint32_t>(std::max(frame.visible_height, frame.coded_height));
        if (width == 0 || height == 0 || frame.plane_count <= 0) {
            return false;
        }

        auto* slot = getInprogress();
        if (! slot) return false;
        resetHandle(*slot);

        slot->handle_type = ExternalFrameHandleType::DMA_BUF;
        slot->width = static_cast<std::int32_t>(width);
        slot->height = static_cast<std::int32_t>(height);
        slot->drm_fourcc = toDrmFourcc(frame.format);
        slot->drm_modifier = frame.modifier == DRM_FORMAT_MOD_INVALID
            ? static_cast<std::uint64_t>(DRM_FORMAT_MOD_LINEAR)
            : frame.modifier;
        slot->n_planes = static_cast<std::uint32_t>(std::min(frame.plane_count, 4));
        slot->premultiplied = false;

        for (std::uint32_t i = 0; i < slot->n_planes; ++i) {
            const int dup_fd = ::dup(frame.planes[i].fd);
            if (dup_fd < 0) {
                resetHandle(*slot);
                return false;
            }
            slot->planes[i].fd = dup_fd;
            slot->planes[i].stride = frame.planes[i].stride;
            slot->planes[i].offset = static_cast<std::uint32_t>(frame.planes[i].offset);
        }

        m_width = width;
        m_height = height;
        renderFrame();
        return true;
    }

    std::uint32_t width() const override { return m_width; }
    std::uint32_t height() const override { return m_height; }

protected:
    atomic_& presented() override { return m_presented; }
    atomic_& ready() override { return m_ready; }
    atomic_& inprogress() override { return m_inprogress; }

private:
    static void resetHandle(ExHandle& handle) {
        for (auto& plane : handle.planes) {
            if (plane.fd >= 0) {
                ::close(plane.fd);
                plane.fd = -1;
            }
            plane.offset = 0;
            plane.stride = 0;
        }
        handle.handle_type = ExternalFrameHandleType::NONE;
        handle.fd = -1;
        handle.width = 0;
        handle.height = 0;
        handle.size = 0;
        handle.drm_fourcc = 0;
        handle.drm_modifier = ExHandle::INVALID_DRM_MODIFIER;
        handle.n_planes = 0;
        handle.premultiplied = false;
    }

    static std::uint32_t toDrmFourcc(DmaBufFormat format) {
        switch (format) {
        case DmaBufFormat::BGRA8_UNORM: return DRM_FORMAT_ARGB8888;
        case DmaBufFormat::RGBA8_UNORM: return DRM_FORMAT_ABGR8888;
        }
        return DRM_FORMAT_ABGR8888;
    }

private:
    std::array<ExHandle, 3> m_handles {};
    atomic_                 m_presented { nullptr };
    atomic_                 m_ready { nullptr };
    atomic_                 m_inprogress { nullptr };
    std::uint32_t           m_width { 1 };
    std::uint32_t           m_height { 1 };
};
} // namespace wallpaper
