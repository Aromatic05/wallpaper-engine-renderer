#pragma once

namespace wallpaper
{
// Runtime-wide frame contract:
// 1. drain session commands
// 2. apply pending properties
// 3. dispatch input events
// 4. backend tick
// 5. backend produces OutputSource update
// 6. render executes if needed
// 7. output presents/submits
// 8. diagnostics flushed
struct FrameLifecycle final {};
} // namespace wallpaper
