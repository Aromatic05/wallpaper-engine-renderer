#include "wallpaper/OutputTargetBinding.hpp"
#include "wallpaper/TextureOutput.hpp"
#include "wallpaper/swapchain/ExSwapchain.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <type_traits>
#include <fcntl.h>
#include <unistd.h>

namespace
{
class TestSwapchain final : public wallpaper::ExSwapchain {
public:
    TestSwapchain() {
        m_presented.store(&m_handles[0]);
        m_ready.store(&m_handles[1]);
        m_inprogress.store(&m_handles[2]);
    }

    std::uint32_t width() const override { return 64; }
    std::uint32_t height() const override { return 32; }

protected:
    std::atomic<wallpaper::ExHandle*>& presented() override { return m_presented; }
    std::atomic<wallpaper::ExHandle*>& ready() override { return m_ready; }
    std::atomic<wallpaper::ExHandle*>& inprogress() override { return m_inprogress; }

private:
    std::array<wallpaper::ExHandle, 3> m_handles {};
    std::atomic<wallpaper::ExHandle*> m_presented { nullptr };
    std::atomic<wallpaper::ExHandle*> m_ready { nullptr };
    std::atomic<wallpaper::ExHandle*> m_inprogress { nullptr };
};

class TestBinding final : public wallpaper::OutputTargetBinding {
public:
    wallpaper::OutputTargetBindingKind kind() const override {
        return wallpaper::OutputTargetBindingKind::VulkanRenderTarget;
    }

    void attach(wallpaper::ExSwapchain* swapchain) { attachTextureSwapchain(swapchain); }
};

int MakeReadableFd() {
    int pipe_fds[2] { -1, -1 };
    assert(::pipe(pipe_fds) == 0);
    ::close(pipe_fds[1]);
    return pipe_fds[0];
}

void RequireClosed(int fd) {
    errno = 0;
    assert(::fcntl(fd, F_GETFD) == -1);
    assert(errno == EBADF);
}
} // namespace

static_assert(! std::is_copy_constructible_v<wallpaper::TextureFrame>);
static_assert(! std::is_copy_assignable_v<wallpaper::TextureFrame>);
static_assert(std::is_move_constructible_v<wallpaper::TextureFrame>);

int main() {
    TestBinding binding;
    auto missing = binding.acquireTexture();
    assert(! missing);
    assert(missing.error().code == wallpaper::ResultCode::InvalidState);

    TestSwapchain swapchain;
    binding.attach(&swapchain);

    const int shm_source_fd = MakeReadableFd();
    auto* shm = swapchain.getInprogress();
    assert(shm != nullptr);
    shm->handle_type = wallpaper::ExternalFrameHandleType::SHM;
    shm->fd = shm_source_fd;
    shm->width = 64;
    shm->height = 32;
    shm->size = 64u * 32u * 4u;
    shm->shm_stride = 64u * 4u;
    shm->premultiplied = true;
    swapchain.renderFrame();

    int owned_shm_fd = -1;
    {
        auto acquired = binding.acquireTexture();
        assert(acquired);
        auto frame = std::move(acquired.value());
        assert(frame.valid());
        assert(frame.exportKind == wallpaper::TextureExportKind::SharedMemory);
        assert(frame.format == wallpaper::TexturePixelFormat::Bgra8Unorm);
        assert(frame.storageLayout == wallpaper::TextureStorageLayout::LinearRows);
        assert(frame.ownership == wallpaper::TextureOwnership::OwnedFileDescriptors);
        assert(frame.acquireSync == wallpaper::TextureSyncMode::Implicit);
        assert(frame.releaseSync == wallpaper::TextureSyncMode::Implicit);
        assert(frame.extent.width == 64);
        assert(frame.extent.height == 32);
        assert(frame.planeCount == 1);
        assert(frame.shmSize == 64u * 32u * 4u);
        assert(frame.planes[0].stride == 64u * 4u);
        assert(frame.premultiplied);
        assert(frame.revision == 1);
        owned_shm_fd = frame.planes[0].descriptor.get();
        assert(owned_shm_fd >= 0);
        assert(owned_shm_fd != shm_source_fd);
    }
    RequireClosed(owned_shm_fd);
    ::close(shm_source_fd);

    auto empty = binding.acquireTexture();
    assert(! empty);
    assert(empty.error().code == wallpaper::ResultCode::NotFound);

    const int unsupported_source_fd = MakeReadableFd();
    auto* unsupported = swapchain.getInprogress();
    assert(unsupported != nullptr);
    unsupported->handle_type = wallpaper::ExternalFrameHandleType::DMA_BUF;
    unsupported->width = 32;
    unsupported->height = 32;
    unsupported->drm_fourcc = 0xdeadbeefu;
    unsupported->drm_modifier = 0;
    unsupported->n_planes = 1;
    unsupported->planes[0].fd = unsupported_source_fd;
    unsupported->planes[0].stride = 128;
    swapchain.renderFrame();

    auto unsupported_frame = binding.acquireTexture();
    assert(! unsupported_frame);
    assert(unsupported_frame.error().code == wallpaper::ResultCode::NotSupported);
    ::close(unsupported_source_fd);

    const int dma_source_fd = MakeReadableFd();
    auto* dma = swapchain.getInprogress();
    assert(dma != nullptr);
    dma->handle_type = wallpaper::ExternalFrameHandleType::DMA_BUF;
    dma->width = 128;
    dma->height = 72;
    dma->drm_fourcc = 0x34325241u;
    dma->drm_modifier = 0x1122334455667788ull;
    dma->n_planes = 1;
    dma->planes[0].fd = dma_source_fd;
    dma->planes[0].offset = 128;
    dma->planes[0].stride = 512;
    swapchain.renderFrame();

    int owned_dma_fd = -1;
    {
        auto acquired = binding.acquireTexture();
        assert(acquired);
        auto frame = std::move(acquired.value());
        assert(frame.exportKind == wallpaper::TextureExportKind::DmaBuf);
        assert(frame.format == wallpaper::TexturePixelFormat::Bgra8Unorm);
        assert(frame.storageLayout == wallpaper::TextureStorageLayout::DrmModifier);
        assert(frame.extent.width == 128);
        assert(frame.extent.height == 72);
        assert(frame.drmFourcc == 0x34325241u);
        assert(frame.drmModifier == 0x1122334455667788ull);
        assert(frame.planeCount == 1);
        assert(frame.planes[0].offset == 128);
        assert(frame.planes[0].stride == 512);
        assert(frame.revision == 2);
        owned_dma_fd = frame.planes[0].descriptor.get();
        assert(owned_dma_fd >= 0);
        assert(owned_dma_fd != dma_source_fd);
    }
    RequireClosed(owned_dma_fd);
    ::close(dma_source_fd);

    binding.attach(nullptr);
    auto detached = binding.acquireTexture();
    assert(! detached);
    assert(detached.error().code == wallpaper::ResultCode::InvalidState);
    return 0;
}
