#include "output/swapchain/ShmFrameSwapchain.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace
{
void requireAt(bool condition, const char* label) {
    if (! condition) {
        std::fprintf(stderr, "shm_frame_swapchain_test: requirement failed at %s\n", label);
        std::abort();
    }
}
} // namespace

int main() {
    wallpaper::ShmFrameSwapchain swapchain(2, 2);

    const std::vector<std::uint8_t> pixels {
        0x10, 0x20, 0x30, 0x40, 0x11, 0x21, 0x31, 0x41,
        0x12, 0x22, 0x32, 0x42, 0x13, 0x23, 0x33, 0x43,
    };

    requireAt(swapchain.publishFrame(pixels.data(), 2, 2, 8), "publishFrame");

    auto* frame = swapchain.eatFrame();
    requireAt(frame != nullptr, "eatFrame");
    requireAt(frame->isShm(), "frame kind");
    requireAt(frame->width == 2, "frame width");
    requireAt(frame->height == 2, "frame height");
    requireAt(frame->shm_stride == 8, "frame stride");
    requireAt(frame->size == pixels.size(), "frame size");
    requireAt(frame->fd >= 0, "frame fd");

    const int read_fd = ::dup(frame->fd);
    requireAt(read_fd >= 0, "dup");

    void* mapped = ::mmap(nullptr, frame->size, PROT_READ, MAP_SHARED, read_fd, 0);
    requireAt(mapped != MAP_FAILED, "mmap");
    requireAt(std::memcmp(mapped, pixels.data(), pixels.size()) == 0, "pixel copy");
    ::munmap(mapped, frame->size);
    ::close(read_fd);

    return 0;
}
