#include "abi/WeRendererFrameStatus.hpp"

#include <cassert>

int main() {
    using wallpaper::MapTextureAcquireErrorToAbiStatus;
    using wallpaper::ResultCode;

    const auto not_found = MapTextureAcquireErrorToAbiStatus(ResultCode::NotFound);
    assert(not_found.abiStatus == 1);
    assert(! not_found.publishDiagnostic);

    const auto initializing = MapTextureAcquireErrorToAbiStatus(ResultCode::InvalidState);
    assert(initializing.abiStatus == 1);
    assert(! initializing.publishDiagnostic);

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
