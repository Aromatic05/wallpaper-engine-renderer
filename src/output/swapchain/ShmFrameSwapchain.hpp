#pragma once

#include "wallpaper/swapchain/ExSwapchain.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace wallpaper
{
class ShmFrameSwapchain : public ExSwapchain {
    using atomic_ = std::atomic<ExHandle*>;

public:
    explicit ShmFrameSwapchain(std::uint32_t width, std::uint32_t height)
        : m_presented(&m_handles[0]), m_ready(&m_handles[1]), m_inprogress(&m_handles[2]),
          m_width(width), m_height(height) {}

    ~ShmFrameSwapchain() override {
        for (auto& handle : m_handles) {
            resetHandle(handle);
        }
    }

    bool publishFrame(const void* buffer,
                      std::uint32_t width,
                      std::uint32_t height,
                      std::uint32_t stride_bytes,
                      bool          premultiplied = false) {
        if (! buffer || width == 0 || height == 0 || stride_bytes < width * 4u) {
            return false;
        }

        auto* slot = getInprogress();
        if (! slot) return false;
        resetHandle(*slot);

        const std::size_t shm_size = static_cast<std::size_t>(stride_bytes) * height;
        if (shm_size == 0) return false;

        const int memfd = ::memfd_create("we-shm-frame", 0);
        if (memfd < 0) return false;
        if (::ftruncate(memfd, static_cast<off_t>(shm_size)) != 0) {
            ::close(memfd);
            return false;
        }

        void* mapped = ::mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (mapped == MAP_FAILED) {
            ::close(memfd);
            return false;
        }
        std::memcpy(mapped, buffer, shm_size);
        ::munmap(mapped, shm_size);

        slot->handle_type = ExternalFrameHandleType::SHM;
        slot->fd = memfd;
        slot->width = static_cast<std::int32_t>(width);
        slot->height = static_cast<std::int32_t>(height);
        slot->size = shm_size;
        slot->shm_stride = stride_bytes;
        slot->premultiplied = premultiplied;

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
        if (handle.fd >= 0) {
            ::close(handle.fd);
            handle.fd = -1;
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

protected:
    std::array<ExHandle, 3> m_handles {};
    atomic_                 m_presented { nullptr };
    atomic_                 m_ready { nullptr };
    atomic_                 m_inprogress { nullptr };
    std::uint32_t           m_width { 1 };
    std::uint32_t           m_height { 1 };
};
} // namespace wallpaper
