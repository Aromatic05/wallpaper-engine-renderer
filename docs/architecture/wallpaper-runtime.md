# Wallpaper Runtime Top-Level Design

## Goal

This repository is migrating its top-level axis from:

```text
Scene -> Renderer
```

to:

```text
WallpaperSession -> ContentBackend -> Output
```

The purpose of this stage is not to rewrite `WPSceneParser`, `VulkanRender`, or the internal WE scene execution model. The purpose is to establish a runtime shell that can host the existing Wallpaper Engine scene path as one backend and leave room for future backends such as `web`.

## Architectural Boundaries

Allowed dependency flow:

```text
api
  -> runtime public API

runtime
  -> backend interface
  -> output interface
  -> host interface
  -> common

backend/scene
  -> runtime backend interface
  -> host interface
  -> render
  -> common

backend/web
  -> runtime backend interface
  -> host interface
  -> output source interface
  -> common

render
  -> common
  -> host GPU/platform abstraction if needed

output
  -> output source interface
  -> render public result types
  -> host platform interface
  -> common

host
  -> platform/system libraries
  -> common

common
  -> no project-specific dependency
```

Forbidden:

```text
runtime include backend/scene or backend/web concrete implementations
backend/scene depends on backend/web
backend/web depends on backend/scene
render depends on output
host depends on runtime/backend/render business objects
api exposes backend/render/host internal types
```

## Runtime Responsibilities

`WallpaperSession` is the only external lifecycle owner.

`runtime/` owns:

- session state machine
- backend creation through `BackendFactory`
- property routing
- input routing
- output binding coordination
- diagnostic aggregation

Backends do not own the external lifecycle. They only respond to `load/start/pause/resume/stop/setProperty/sendInput`.

## Output Model

`render` decides how to draw.

`output` decides where the produced content goes.

The current public output-source contract exposes one kind:

- `RenderPlan`

All built-in backends produce a `RenderPlan`. A render plan may target either a native surface or an
offscreen/export binding, so presentation destination remains an `OutputTarget` concern rather than a
second source type.

`Texture` is intentionally not public yet. A future texture source must first define format, extent,
ownership, image layout, acquire/release synchronization, lifetime/revision, and exportability.
`Surface` is not planned as a renderer source contract: hosts own Wayland/native presentation while the
renderer produces frames or render plans.

## Frame Lifecycle

The runtime frame contract is:

```text
Frame Begin
  1. drain session commands
  2. apply pending properties
  3. dispatch input events
  4. backend tick
  5. backend produces OutputSource update
  6. render executes if needed
  7. output presents/submits
  8. diagnostics flushed
Frame End
```

The current repository does not yet implement the full centralized frame pump. This phase introduces the interface contract and a shim path so the existing WE scene flow can live behind `WallpaperSession`.

## Transitional Strategy

This stage intentionally uses shim layers:

- `backend/scene/WESceneBackend` wraps the existing `SceneWallpaper`
- `backend/scene/compatibility/WESceneRenderInit.hpp` owns scene-output init types for new code
- `api/scene/WEScene.hpp` is the preferred include for WE scene session compatibility helpers
- `output/OutputController` binds runtime sessions to backend-provided `OutputSource`
- `api/WallpaperRuntime.hpp` becomes the top-level entry for new integrations

The old `SceneWallpaper` API remains present for compatibility while the repository migrates callers to the new runtime entrypoint.
`SceneWallpaperSurface.hpp` remains only as a compatibility forwarding header and should not be the preferred include for new code.
