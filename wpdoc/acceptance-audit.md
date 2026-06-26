# Acceptance Audit

This document maps the acceptance gaps to the current code and verification
artifacts in the repository.

## P0: OutputSource direction was reversed

Expected:

```text
Backend produces OutputSource
OutputController consumes OutputSource
```

Current evidence:

- `src/output/OutputSource.hpp`
  - `OutputSource` exposes `renderPlan()`
  - there is no `bind(const OutputTarget&)` on `OutputSource`
- `src/output/RenderPlan.hpp`
  - `RenderPlan` is the consumable contract
  - `bindOutput(const OutputTarget&)` is on `RenderPlan`, not `OutputSource`
- `src/output/OutputController.cpp`
  - `OutputController::bind()` validates target/capabilities
  - `OutputController::bindRenderPlan()` fetches the backend-produced render plan
  - the controller calls `plan->bindOutput(target)`

Verification:

- `src/test/output_controller_contract_test.cpp`
- `ctest --test-dir build-check --output-on-failure`

## P0: OutputTarget type safety

Expected:

- no `shared_ptr<void>`-based output target binding

Current evidence:

- `include/wallpaper/OutputTargetBinding.hpp`
  - typed `OutputTargetBinding`
  - explicit `OutputTargetBindingKind`
- `include/wallpaper/OutputTarget.hpp`
  - `OutputTarget` stores `OutputTargetBindingPtr`
- `src/output/OutputController.cpp`
  - validates binding kind
  - render plans consume typed `OutputTarget`

Verification:

- `src/test/output_controller_contract_test.cpp`

## P0: RenderPlanSource had no real RenderPlan contract

Expected:

- render-plan output should expose a real contract, not a renamed scene runtime entrypoint

Current evidence:

- `src/output/RenderPlan.hpp`
  - `requiredBindingKind()`
  - `revision()`
  - `bindOutput(const OutputTarget&)`
- `src/output/RenderPlanSource.hpp`
  - `RenderPlanSource` returns `RenderPlanPtr`
- `src/output/OutputController.hpp`
  - controller tracks `boundRenderPlanRevision()`
- `src/output/OutputController.cpp`
  - `revision()` is consumed after a successful bind
- `src/backend/scene/internal/engine/WESceneBackend.cpp`
  - scene backend now implements a generic `RenderPlan`

Verification:

- `src/test/output_controller_contract_test.cpp`

## P1: unified frame lifecycle only existed as comments

Expected:

- top-level runtime must be able to drive backend frame lifecycle hooks

Current evidence:

- `src/runtime/backend/ContentBackend.hpp`
  - `update()`
  - `tick()`
  - `produceFrame()`
  - `acquireOutput()`
- `include/wallpaper/FrameLifecycle.hpp`
  - concrete lifecycle result struct
- `src/runtime/session/WallpaperSession.cpp`
  - session tick drives backend update, output acquire, backend tick, state sync

Verification:

- `src/test/wallpaper_session_contract_test.cpp`

## P1: web backend capability mismatch

Expected:

- web backend should not claim unsupported surface/runtime behavior

Current evidence (pre-port):

- `src/backend/web/internal/WebBackend.cpp`
  - `supportsSurfaceOutput = false`
  - `start()` returns `NotSupported`
  - `tick()/update()/produceFrame()/acquireOutput()` are honest skeleton hooks

Verification (pre-port):

- `src/test/web_backend_contract_test.cpp` (asserted `play() ==
  NotSupported`)

Current evidence (post-port):

- `src/backend/web/internal/WebBackend.cpp` is the real CEF-backed
  implementation. Lifecycle / properties / input / Pump all route
  through the CEF BrowserHost; capabilities now include
  `supportsRenderPlan = true` and the `WebOutputSource` exposes the
  `WebRenderPlan` to the `OutputController`.
- The C ABI's `we_session_set_render_config` dispatches on
  `state->sourceKind` (SCENE -> `BindWESceneOutput`; WEB -> new
  `MakeWebOutputBinding` + `session->bindOutput`). The
  `we_session_acquire_frame` central `dynamic_cast` to
  `WESceneOutputBinding` / `WebOutputBinding` keeps the swapchain
  read path unified.
- The web backend is gated on `-DBUILD_WEWEB=ON`; the C ABI's
  `BackendType::Web` returns a clear "not built" error in the
  default build.
- The web backend now relies on a dedicated `we-cef-helper`
  executable configured through CEF's `browser_subprocess_path`
  instead of exposing helper argv plumbing in the public C ABI.

Verification (post-port):

- `src/test/web_backend_contract_test.cpp` — mock contract test
  (BUILD_WEWEB=ON) that drives the full lifecycle with a
  `MockWebBrowserHost` and asserts the recorded BrowserHost call
  sequence matches the contract for load / bind / play /
  setProperty / sendInput / pause / resume / update / stop.
- `src/test/web_cef_integration_test.cpp` — optional real-CEF
  smoke test gated on `-DWP_ENABLE_CEF_INTEGRATION_TEST=ON` that
  drives a minimal HTML project and asserts CefDoMessageLoopWork
  does not deadlock on it. The test self-skips when no CEF
  runtime is present.
- `src/test/web/MockWebBrowserHost.hpp` — recording fake used by
  the contract test.

## P1: Session state lied about load completion

Expected:

- distinguish loading, loaded, output-ready, and running states

Current evidence:

- `include/wallpaper/SessionState.hpp`
  - `Loading`
  - `Loaded`
  - `OutputReady`
  - `Playing`
- `src/runtime/backend/BackendReadyState.hpp`
  - `Loading`
  - `Loaded`
  - `OutputReady`
- `src/runtime/session/WallpaperSession.cpp`
  - session state synchronizes from backend readiness
  - `bindOutput()` can advance the session to `OutputReady`
- `src/backend/scene/internal/engine/WESceneBackend.cpp`
  - scene backend readiness is advanced through first-frame callback and output binding notification

Verification:

- `src/test/wallpaper_session_contract_test.cpp`

## P1: HostServices had not really landed

Expected:

- host should be a capability provider, not a loose folder grouping

Current evidence:

- `src/host/HostServices.hpp`
  - `FileSystemService`
  - `AudioService`
  - `MediaService`
  - `TimerService`
  - `PlatformService`
  - `CacheService`
  - `DiagnosticsService`
- `src/host/HostServices.cpp`
  - default implementations wired
- `src/backend/scene/CreateWESceneBackend.cpp`
  - scene package fs is injected through host services
- `src/backend/scene/internal/engine/WESceneRuntimeDriver.cpp`
  - sound manager, loopers, frame timer, VFS, physical fs, package fs, cache fs all route through host services
- `src/runtime/session/WallpaperSession.cpp`
  - cache root and diagnostics publication route through host services

Verification:

- `src/test/host_services_contract_test.cpp`

## P1: public API boundary was not protected

Expected:

- `include/wallpaper/**` is the formal public surface
- public headers must not depend on `src/` internals

Current evidence:

- `src/CMakeLists.txt`
  - target public include path is `../include`
- `include/wallpaper/**`
  - self-contained public headers
- `src/api/**`
  - forwards into `include/wallpaper/**`
- `src/test/architecture_guard_test.cmake`
  - fails if public headers include `src/`
  - fails if `src/backend/scene/internal/runtime` reappears

Verification:

- `src/test/wallpaper_public_api_test.cpp`
- `src/test/architecture_guard_test.cmake`

## P2: docs and project identity were outdated

Current evidence:

- `README.md`
  - describes the session-centered runtime
  - documents validation commands
- `wpdoc/runtime-architecture.md`
  - documents runtime vs engine
  - documents output/render-plan contract
- `CMakeLists.txt`
  - project is `wallpaper-engine-renderer`

## P2: backend internal runtime naming conflict

Expected:

- backend-internal execution code should live under `engine/`, not `runtime/`

Current evidence:

- scene internal execution code is under `src/backend/scene/internal/engine`
- `src/test/architecture_guard_test.cmake` fails if `src/backend/scene/internal/runtime` exists

## Build and test evidence

Validated commands:

```bash
cmake -S . -B build-check -DBUILD_TESTING=ON
cmake --build build-check -j2
ctest --test-dir build-check --output-on-failure
cmake --build build-check -j2 --target sceneviewer
```

Optional web backend:

```bash
cmake -S . -B build-check -DBUILD_WEWEB=ON -DCEF_ROOT=/path/to/cef
cmake --build build-check -j2
ctest --test-dir build-check --output-on-failure
# additionally:
cmake -S . -B build-check -DBUILD_WEWEB=ON -DCEF_ROOT=... -DWP_ENABLE_CEF_INTEGRATION_TEST=ON
```

Current verified results (default -DBUILD_WEWEB=OFF):

- main library builds
- `sceneviewer` builds
- test suite passes (pre-existing 2 segfaults in
  `resource-correctness-test` / `scenescript-host-test` are
  baseline issues unrelated to the web port):
  - `wallpaper-session-contract-test`
  - `wallpaper-public-api-test`
  - `output-controller-contract-test`
  - `host-services-contract-test`
  - `wallpaper-architecture-guard-test`
