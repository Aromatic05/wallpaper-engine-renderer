#pragma once

#include "wallpaper/Result.hpp"

#include <cstdint>

namespace wallpaper
{
struct RendererFrameAcquireStatus {
    std::int32_t abiStatus { -1 };
    bool         publishDiagnostic { true };
};

constexpr RendererFrameAcquireStatus MapTextureAcquireErrorToAbiStatus(ResultCode code) {
    switch (code) {
    case ResultCode::NotFound:
    case ResultCode::InvalidState:
        // Frame acquisition is a polling API. During asynchronous output initialization, and after
        // an output has been detached, there is simply no frame available yet. Treat those states
        // like an empty swapchain instead of flooding diagnostics on every poll.
        return RendererFrameAcquireStatus { .abiStatus = 1, .publishDiagnostic = false };
    case ResultCode::NotSupported:
        return RendererFrameAcquireStatus { .abiStatus = -2, .publishDiagnostic = false };
    default:
        return RendererFrameAcquireStatus { .abiStatus = -1, .publishDiagnostic = true };
    }
}
} // namespace wallpaper
