#include "wallpaper/OutputTargetBinding.hpp"
#include "wallpaper/TextureOutput.hpp"
#include "wallpaper/swapchain/ExSwapchain.hpp"
#include <drm/drm_fourcc.h>

#include <unistd.h>


namespace wallpaper
{
namespace
{
bool ResolveDmaBufTextureFormat(std::uint32_t drmFourcc, TexturePixelFormat* format) {
    if (format == nullptr) return false;
    switch (drmFourcc) {
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
        *format = TexturePixelFormat::Rgba8Unorm;
        return true;
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        *format = TexturePixelFormat::Bgra8Unorm;
        return true;
    default:
        return false;
    }
}
} // namespace

OwnedFileDescriptor::OwnedFileDescriptor(int descriptor)
    : m_descriptor(descriptor) {}

OwnedFileDescriptor::~OwnedFileDescriptor() {
    reset();
}

OwnedFileDescriptor::OwnedFileDescriptor(OwnedFileDescriptor&& other) noexcept
    : m_descriptor(other.release()) {}

OwnedFileDescriptor& OwnedFileDescriptor::operator=(OwnedFileDescriptor&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
}

int OwnedFileDescriptor::release() {
    const int descriptor = m_descriptor;
    m_descriptor = -1;
    return descriptor;
}

void OwnedFileDescriptor::reset(int descriptor) {
    if (m_descriptor >= 0) ::close(m_descriptor);
    m_descriptor = descriptor;
}

bool TextureFrame::valid() const {
    if (revision == 0 || extent.width == 0 || extent.height == 0 || planeCount == 0
        || planeCount > planes.size()) {
        return false;
    }
    for (std::uint32_t index = 0; index < planeCount; ++index) {
        if (! planes[index].descriptor.valid() || planes[index].stride == 0) return false;
    }
    if (exportKind == TextureExportKind::SharedMemory) {
        return storageLayout == TextureStorageLayout::LinearRows && planeCount == 1 && shmSize > 0;
    }
    return storageLayout == TextureStorageLayout::DrmModifier && drmFourcc != 0;
}


Result<TextureFrame> OutputTargetBinding::acquireTexture() {
    std::scoped_lock lock(m_textureSwapchainMutex);
    auto* swapchain = m_textureSwapchain;
    if (swapchain == nullptr) {
        return Result<TextureFrame>::failure(ResultCode::InvalidState,
                                             "output binding has no attached texture swapchain");
    }

    const ExHandle* handle = swapchain->eatFrame();
    if (handle == nullptr) {
        return Result<TextureFrame>::failure(ResultCode::NotFound,
                                             "output binding has no new texture frame");
    }
    if (handle->width <= 0 || handle->height <= 0) {
        return Result<TextureFrame>::failure(ResultCode::InternalError,
                                             "texture frame has an invalid extent");
    }

    TextureFrame frame;
    frame.extent.width = static_cast<std::uint32_t>(handle->width);
    frame.extent.height = static_cast<std::uint32_t>(handle->height);
    frame.premultiplied = handle->premultiplied;

    if (handle->isShm()) {
        if (handle->fd < 0 || handle->size == 0 || handle->shm_stride == 0) {
            return Result<TextureFrame>::failure(ResultCode::InternalError,
                                                 "shared-memory texture frame is incomplete");
        }
        const int descriptor = ::dup(handle->fd);
        if (descriptor < 0) {
            return Result<TextureFrame>::failure(ResultCode::InternalError,
                                                 "failed to duplicate shared-memory texture descriptor");
        }

        frame.exportKind = TextureExportKind::SharedMemory;
        frame.format = TexturePixelFormat::Bgra8Unorm;
        frame.storageLayout = TextureStorageLayout::LinearRows;
        frame.planeCount = 1;
        frame.shmSize = static_cast<std::uint64_t>(handle->size);
        frame.planes[0].descriptor.reset(descriptor);
        frame.planes[0].stride = handle->shm_stride;
    } else if (handle->isDmabuf()) {
        if (handle->drm_fourcc == 0 || handle->n_planes == 0
            || handle->n_planes > TextureFrame::MaxPlanes) {
            return Result<TextureFrame>::failure(ResultCode::InternalError,
                                                 "DMA-BUF texture frame is incomplete");
        }

        frame.exportKind = TextureExportKind::DmaBuf;
        if (! ResolveDmaBufTextureFormat(handle->drm_fourcc, &frame.format)) {
            return Result<TextureFrame>::failure(ResultCode::NotSupported,
                                                 "DMA-BUF texture format is not supported");
        }
        frame.storageLayout = TextureStorageLayout::DrmModifier;
        frame.drmFourcc = handle->drm_fourcc;
        frame.drmModifier = handle->drm_modifier;
        frame.planeCount = handle->n_planes;
        for (std::uint32_t index = 0; index < handle->n_planes; ++index) {
            if (handle->planes[index].fd < 0 || handle->planes[index].stride == 0) {
                return Result<TextureFrame>::failure(ResultCode::InternalError,
                                                     "DMA-BUF texture plane is incomplete");
            }
            const int descriptor = ::dup(handle->planes[index].fd);
            if (descriptor < 0) {
                return Result<TextureFrame>::failure(ResultCode::InternalError,
                                                     "failed to duplicate DMA-BUF texture descriptor");
            }
            frame.planes[index].descriptor.reset(descriptor);
            frame.planes[index].offset = handle->planes[index].offset;
            frame.planes[index].stride = handle->planes[index].stride;
        }
    } else {
        return Result<TextureFrame>::failure(ResultCode::NotSupported,
                                             "output frame is not exportable as a public texture");
    }

    frame.revision = m_textureRevision.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (! frame.valid()) {
        return Result<TextureFrame>::failure(ResultCode::InternalError,
                                             "exported texture frame failed contract validation");
    }
    return Result<TextureFrame>::success(std::move(frame));
}
} // namespace wallpaper
