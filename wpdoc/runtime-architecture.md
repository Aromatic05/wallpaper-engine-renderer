# Runtime Architecture

This repository is being refactored from a scene-centered renderer into a
session-centered wallpaper runtime.

The top-level axis is now:

```text
WallpaperSession -> ContentBackend -> Output
```

## Layer Roles

- `WallpaperSession`
  - owns the public lifecycle
  - routes properties and input
  - synchronizes session state from backend readiness
  - aggregates diagnostics
- `ContentBackend`
  - responds to session commands
  - owns content-specific loading and update logic
  - exposes frame lifecycle hooks: `update()`, `tick()`, `produceFrame()`, `acquireOutput()`
- `output`
  - binds an `OutputTarget`
  - consumes backend-produced `OutputSource`
  - consumes generic `RenderPlan` contracts
- `host`
  - provides platform services such as cache resolution, filesystem factories,
    audio factories, loopers, timers, and diagnostics publication

## Current Backends

- `scene`
  - implemented as `src/backend/scene`
  - legacy scene execution remains behind `src/backend/scene/internal/engine`
  - the old scene pipeline is not a public API surface
- `web`
  - implemented as `src/backend/web`
  - CEF-backed BrowserHost under `src/backend/web/internal/cef/`
  - the C++-internal target is gated on `-DBUILD_WEWEB=ON`; the C
    ABI's `BackendType::Web` returns a clear "not built" error in
    the default build so consumers can detect the missing CEF
    runtime without dereferencing a null backend
  - the public include surface (`include/wallpaper/web/**`) is
    CEF-free; only the implementation in `internal/cef/` pulls in
    `libcef_dll` headers, preserving the architecture boundary

## Runtime vs Engine

- top-level `runtime/`
  - session state machine
  - backend coordination
  - diagnostics aggregation
  - input/property routing
- backend-internal `engine/`
  - legacy scene execution details
  - renderer-driver glue that has not yet been replaced
- backend-internal `cef/`
  - CEF handler classes (App / Client / Osr / UserProperties) and
    the CEF-driven `BrowserHost` implementation
  - the public `WebBrowserHost` (`include/wallpaper/web/`) is the
    CEF-free pImpl; the Impl lives in `internal/cef/BrowserHost.cpp`

`src/backend/scene/internal/runtime` is intentionally no longer used. Scene
internal execution code lives under `internal/engine` to avoid conflicting with
the shared runtime concept. Web backend internals live under
`src/backend/web/internal/cef/` to keep the CEF include surface off the
shared runtime concept.

## Output Contract

- backends produce `OutputSource`
- `RenderPlanSource` exposes a generic `RenderPlan`
- `OutputController` consumes the `RenderPlan`
- `RenderPlan` now defines:
  - `requiredBindingKind()`
  - `revision()`
  - `bindOutput(const OutputTarget&)`

This means `OutputController` no longer depends on a scene-specific render plan
type in order to consume render plans.

## Public API Boundary

The public include surface is `include/wallpaper/**`.

- `src/api/**` now forwards to `include/wallpaper/**`
- public headers must not include `src/` internals
- architecture tests enforce that boundary

## Validation

The repository currently carries automated checks for:

- session lifecycle contract
- public API include surface
- output controller contract
- web backend transitional honesty
- host service routing defaults
- architecture boundary guards
