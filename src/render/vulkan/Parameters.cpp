#include "Parameters.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <utility>

namespace wallpaper
{
namespace vulkan
{

namespace
{
// Exported external-memory file descriptors are process-owned resources returned by
// vkGetMemoryFdKHR. Destroying the Vulkan image or device does not close them for us, so the
// ExImageParameters owner must retire every unique fd explicitly.
void CloseExternalFd(int fd) {
    if (fd < 0) return;

    if (::close(fd) == 0) {
        LOG_INFO("ExImageParameters: closed external fd=%d", fd);
        return;
    }

    const int close_errno = errno;
    LOG_ERROR("ExImageParameters: failed to close external fd=%d errno=%d message=%s",
              fd,
              close_errno,
              std::strerror(close_errno));
}

// dma-buf exports can mirror the primary fd into plane[0], so teardown has to deduplicate the
// descriptors before closing them. Walking the full array also cleans up any stale plane values
// that might survive from older move bugs.
void ReleaseExternalFds(ExImageParameters& image) {
    std::array<int, ExHandle::MAX_PLANES + 1> unique_fds {};
    std::size_t                               unique_fd_count { 0 };

    const auto remember_fd = [&](int fd) {
        if (fd < 0) return;

        for (std::size_t i = 0; i < unique_fd_count; ++i) {
            if (unique_fds[i] == fd) return;
        }

        unique_fds[unique_fd_count++] = fd;
    };

    remember_fd(image.fd);
    for (const auto& plane : image.planes) remember_fd(plane.fd);
    for (std::size_t i = 0; i < unique_fd_count; ++i) CloseExternalFd(unique_fds[i]);

    image.fd            = -1;
    image.handle_type   = ExternalFrameHandleType::NONE;
    image.drm_fourcc    = 0;
    image.drm_modifier  = ExHandle::INVALID_DRM_MODIFIER;
    image.n_planes      = 0;
    image.premultiplied = false;
    for (auto& plane : image.planes) plane.fd = -1;
}

// Move operations transfer ownership of exported fds to the destination object. The source must be
// scrubbed immediately so its eventual destructor cannot observe stale descriptor values.
void InvalidateMovedFromExternalState(ExImageParameters& image) {
    image.fd            = -1;
    image.handle_type   = ExternalFrameHandleType::NONE;
    image.drm_fourcc    = 0;
    image.drm_modifier  = ExHandle::INVALID_DRM_MODIFIER;
    image.n_planes      = 0;
    image.premultiplied = false;
    for (auto& plane : image.planes) plane.fd = -1;
}

} // namespace

VmaBufferParameters::VmaBufferParameters()  = default;
VmaBufferParameters::~VmaBufferParameters() = default;
VmaBufferParameters::VmaBufferParameters(VmaBufferParameters&& o) noexcept
    : handle(std::move(o.handle)), req_size(o.req_size) {};
VmaBufferParameters& VmaBufferParameters::operator=(VmaBufferParameters&& o) noexcept {
    handle   = std::move(o.handle);
    req_size = o.req_size;
    return *this;
};

VmaImageParameters::VmaImageParameters()  = default;
VmaImageParameters::~VmaImageParameters() = default;
VmaImageParameters::VmaImageParameters(VmaImageParameters&& o) noexcept
    : handle(std::move(o.handle)),
      view(std::move(o.view)),
      sampler(std::move(o.sampler)),
      extent(o.extent),
      mipmap_level(o.mipmap_level) {}
VmaImageParameters& VmaImageParameters::operator=(VmaImageParameters&& o) noexcept {
    handle       = std::move(o.handle);
    view         = std::move(o.view);
    sampler      = std::move(o.sampler);
    extent       = o.extent;
    mipmap_level = o.mipmap_level;
    return *this;
}

ExImageParameters::ExImageParameters() = default;
ExImageParameters::~ExImageParameters() { ReleaseExternalFds(*this); }
ExImageParameters::ExImageParameters(ExImageParameters&& o) noexcept
    : mem(std::move(o.mem)),
      mem_reqs(o.mem_reqs),
      handle(std::move(o.handle)),
      view(std::move(o.view)),
      sampler(std::move(o.sampler)),
      extent(o.extent),
      mipmap_level(o.mipmap_level),
      fd(std::exchange(o.fd, -1)),
      handle_type(o.handle_type),
      drm_fourcc(o.drm_fourcc),
      drm_modifier(o.drm_modifier),
      n_planes(o.n_planes),
      premultiplied(o.premultiplied),
      planes(o.planes) {
    InvalidateMovedFromExternalState(o);
}
ExImageParameters& ExImageParameters::operator=(ExImageParameters&& o) noexcept {
    if (this == &o) return *this;

    ReleaseExternalFds(*this);
    mem           = std::move(o.mem);
    mem_reqs      = o.mem_reqs;
    handle        = std::move(o.handle);
    view          = std::move(o.view);
    sampler       = std::move(o.sampler);
    extent        = o.extent;
    mipmap_level  = o.mipmap_level;
    fd            = std::exchange(o.fd, -1);
    handle_type   = o.handle_type;
    drm_fourcc    = o.drm_fourcc;
    drm_modifier  = o.drm_modifier;
    n_planes      = o.n_planes;
    premultiplied = o.premultiplied;
    planes        = o.planes;
    InvalidateMovedFromExternalState(o);
    return *this;
}

ImageSlots::ImageSlots()  = default;
ImageSlots::~ImageSlots() = default;
ImageSlots::ImageSlots(ImageSlots&& o) noexcept: slots(std::move(o.slots)) {}
ImageSlots& ImageSlots::operator=(ImageSlots&& o) noexcept {
    slots = std::move(o.slots);
    return *this;
}

ImageSlotsRef::ImageSlotsRef()  = default;
ImageSlotsRef::~ImageSlotsRef() = default;
ImageSlotsRef::ImageSlotsRef(const ImageSlots& o)
    : slots(std::vector<ImageParameters>(o.slots.begin(), o.slots.end())) {}

} // namespace vulkan
} // namespace wallpaper
