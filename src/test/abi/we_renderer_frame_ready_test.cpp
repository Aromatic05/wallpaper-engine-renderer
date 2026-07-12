#include "abi/WeRendererFrameReady.hpp"

#include <cassert>
#include <fcntl.h>
#include <poll.h>

int main() {
    wallpaper::RendererFrameReadySignal signal;
    assert(signal.valid());
    assert((::fcntl(signal.descriptor(), F_GETFL) & O_NONBLOCK) != 0);
    assert((::fcntl(signal.descriptor(), F_GETFD) & FD_CLOEXEC) != 0);

    pollfd descriptor {
        .fd = signal.descriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    assert(::poll(&descriptor, 1, 0) == 0);

    signal.notify();
    signal.notify();
    assert(::poll(&descriptor, 1, 0) == 1);
    assert((descriptor.revents & POLLIN) != 0);

    signal.consume();
    descriptor.revents = 0;
    assert(::poll(&descriptor, 1, 0) == 0);
    return 0;
}
