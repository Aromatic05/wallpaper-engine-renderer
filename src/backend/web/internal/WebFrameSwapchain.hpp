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
        const int coded_width = std::max(frame.coded_width, 0);
        const int coded_height = std::max(frame.coded_height, 0);
        const int visible_x = std::max(frame.visible_x, 0);
        const int visible_y = std::max(frame.visible_y, 0);
        const int visible_width =
            frame.visible_width > 0 ? frame.visible_width : frame.coded_width;
        const int visible_height =
            frame.visible_height > 0 ? frame.visible_height : frame.coded_height;
        if (visible_width <= 0 || visible_height <= 0 || frame.plane_count <= 0) {
            return false;
        }
        if (coded_width > 0 && visible_x + visible_width > coded_width) {
            return false;
        }
        if (coded_height > 0 && visible_y + visible_height > coded_height) {
            return false;
        }
        if ((visible_x != 0 || visible_y != 0) && frame.plane_count != 1) {
            return false;
        }

        const std::uint32_t width = static_cast<std::uint32_t>(visible_width);
        const std::uint32_t height = static_cast<std::uint32_t>(visible_height);
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

        const std::uint32_t x_offset_bytes =
            static_cast<std::uint32_t>(visible_x) * bytesPerPixel(frame.format);
        const std::uint32_t y_offset_bytes =
            static_cast<std::uint32_t>(visible_y) * frame.planes[0].stride;

        for (std::uint32_t i = 0; i < slot->n_planes; ++i) {
            const int dup_fd = ::dup(frame.planes[i].fd);
            if (dup_fd < 0) {
                resetHandle(*slot);
                return false;
            }
            slot->planes[i].fd = dup_fd;
            slot->planes[i].stride = frame.planes[i].stride;
            slot->planes[i].offset = static_cast<std::uint32_t>(frame.planes[i].offset);
            if (i == 0) {
                slot->planes[i].offset += x_offset_bytes + y_offset_bytes;
            }
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

    static std::uint32_t bytesPerPixel(DmaBufFormat format) {
        switch (format) {
        case DmaBufFormat::BGRA8_UNORM:
        case DmaBufFormat::RGBA8_UNORM: return 4;
        }
        return 4;
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
