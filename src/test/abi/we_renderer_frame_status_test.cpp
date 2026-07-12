#include "abi/WeRendererFrameStatus.hpp"
#include "wallpaper/OutputTargetBinding.hpp"

#include <cassert>

namespace
{
class PendingOutputBinding final : public wallpaper::OutputTargetBinding {
public:
    wallpaper::OutputTargetBindingKind kind() const override {
        return wallpaper::OutputTargetBindingKind::VulkanRenderTarget;
    }
};
} // namespace

int main() {
    using wallpaper::MapTextureAcquireErrorToAbiStatus;
    using wallpaper::ResultCode;

    const auto not_found = MapTextureAcquireErrorToAbiStatus(ResultCode::NotFound);
    assert(not_found.abiStatus == 1);
    assert(! not_found.publishDiagnostic);

    const auto initializing = MapTextureAcquireErrorToAbiStatus(ResultCode::InvalidState);
    assert(initializing.abiStatus == 1);
    assert(! initializing.publishDiagnostic);

    // Binding creation happens synchronously in we_session_set_render_config,
    // while its swapchain is attached asynchronously on the render thread.
    // Acquiring in that interval must be observable as a no-frame poll.
    PendingOutputBinding pending_binding;
    const auto pending_frame = pending_binding.acquireTexture();
    assert(! pending_frame);
    assert(pending_frame.error().code == ResultCode::InvalidState);
    const auto before_first_frame = MapTextureAcquireErrorToAbiStatus(pending_frame.error().code);
    assert(before_first_frame.abiStatus == 1);
    assert(! before_first_frame.publishDiagnostic);

    const auto unsupported = MapTextureAcquireErrorToAbiStatus(ResultCode::NotSupported);
    assert(unsupported.abiStatus == -2);
    assert(! unsupported.publishDiagnostic);

    const auto invalid_argument = MapTextureAcquireErrorToAbiStatus(ResultCode::InvalidArgument);
    assert(invalid_argument.abiStatus == -1);
    assert(invalid_argument.publishDiagnostic);

    const auto internal_error = MapTextureAcquireErrorToAbiStatus(ResultCode::InternalError);
    assert(internal_error.abiStatus == -1);
    assert(internal_error.publishDiagnostic);
    return 0;
}
