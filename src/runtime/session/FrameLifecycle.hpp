#pragma once

namespace wallpaper
{
// Runtime-wide frame contract:
// 1. drain session commands
// 2. apply pending properties
// 3. dispatch input events
// 4. backend tick
// 5. backend updates its output source / render plan state
// 6. renderer executes if a frame is needed
// 7. output presents/submits
// 8. diagnostics are aggregated
struct FrameLifecycle final {
    bool contentStateChanged { false };
    bool outputStateChanged { false };
    bool diagnosticsChanged { false };
    bool frameRequested { false };
};
} // namespace wallpaper
