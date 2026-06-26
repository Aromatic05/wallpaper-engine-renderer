#pragma once

// Internal accessor for the C ABI's we_runtime_init-saved argv.
// Lives in src/abi/ rather than include/wallpaper/abi/ because it is
// not part of the public ABI surface — only the web backend's
// start() path needs to read the saved argv to call
// CefExecuteProcess. WeRenderer.cpp owns the storage.

namespace wallpaper::abi
{
// Returns true and writes the saved argc/argv out-params if
// we_runtime_init has been called with a non-zero argc and a
// non-null argv at least once in this process. Returns false
// otherwise. The pointer pair is process-static and never freed —
// it aliases the host's main() argv.
bool TryGetRuntimeArgs(int& argc, char**& argv);
} // namespace wallpaper::abi
