#pragma once

#include <cerrno>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

namespace wallpaper
{
class RendererFrameReadySignal {
public:
    RendererFrameReadySignal()
        : m_descriptor(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

    ~RendererFrameReadySignal() {
        if (m_descriptor >= 0) ::close(m_descriptor);
    }

    RendererFrameReadySignal(const RendererFrameReadySignal&) = delete;
    RendererFrameReadySignal& operator=(const RendererFrameReadySignal&) = delete;

    bool valid() const noexcept { return m_descriptor >= 0; }
    int descriptor() const noexcept { return m_descriptor; }

    void notify() const noexcept {
        if (m_descriptor < 0) return;
        const std::uint64_t increment = 1;
        while (::write(m_descriptor, &increment, sizeof(increment)) < 0) {
            if (errno == EINTR) continue;
            break;
        }
    }

    void consume() const noexcept {
        if (m_descriptor < 0) return;
        std::uint64_t count = 0;
        while (true) {
            const ssize_t result = ::read(m_descriptor, &count, sizeof(count));
            if (result == static_cast<ssize_t>(sizeof(count))) continue;
            if (result < 0 && errno == EINTR) continue;
            break;
        }
    }

private:
    int m_descriptor { -1 };
};
} // namespace wallpaper
