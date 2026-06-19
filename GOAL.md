# Full Feature Migration Goal

## Objective

Migrate the scene renderer feature set from `../wallpaper-scene-renderer-new` into this
repository completely. The prior compatibility-phase checklist is not the finish line:
the target is feature parity for the migrated scene runtime, parser, SceneScript host,
text rendering, video texture handling, render graph translation, and render residency
behavior.

Do not accept placeholder, stub, no-op, or test-only compatibility behavior as complete
when the reference implementation has real behavior.

## Reference Repository

- `../wallpaper-scene-renderer-new`

## Completion Standard

A migration item is complete only when all of the following are true:

- The current implementation covers the corresponding reference behavior, not only the
  public API shape.
- Any intentionally missing platform/backend capability is represented by an explicit
  runtime fallback, diagnostic, or feature gate instead of silent no-op behavior.
- Unit or regression tests cover the behavior added in this repository.
- `cmake --build build-check -j2`, `ctest --test-dir build-check --output-on-failure`,
  `cmake --build build-standalone -j2 --target sceneviewer`, and `git diff --check`
  pass after the item.
- A code comparison against `../wallpaper-scene-renderer-new` shows no remaining
  untracked parity gap for the item.

## Workflow Constraints

- Migrate incrementally.
- Before each migration item, define the intended conventional commit.
- After each migration item, run tests before committing.
- Do not leave large batches of unrelated files in the worktree.
- Keep the worktree clean after each commit before starting the next item.
- Use Conventional Commits for every commit.

## Commit Prefixes

- `chore(build): ...`
- `feat(scene): ...`
- `feat(render): ...`
- `refactor(scene): ...`
- `test(scene): ...`
- `docs(runtime): ...`

## Migration Baseline

Status: complete

Baseline verification commands:

```bash
cmake -S . -B build-check -DBUILD_TESTING=ON
cmake --build build-check -j2
ctest --test-dir build-check --output-on-failure
cmake -S standalone_view -B build-standalone
cmake --build build-standalone -j2 --target sceneviewer
```

Baseline verification results:

- `build-check` configure: passed
- `build-check` build: passed
- `ctest --test-dir build-check --output-on-failure`: 9/9 tests passed
- `build-standalone` configure: passed
- `sceneviewer` build: passed

Planned commit:

- `docs(runtime): record migration baseline`

## Completed Compatibility-Phase Checklist

1. Build dependencies and feature toggles
   Planned commit:
   - `chore(build): add scenescript and text rendering dependencies`
2. Dynamic value and user setting foundation
   Planned commit:
   - `feat(scene): add dynamic value and user setting foundation`
3. Property animation foundation
   Planned commit:
   - `feat(scene): add property animation support`
4. SceneScript runtime core
   Planned commit:
   - `feat(scene): add scenescript runtime core`
5. SceneScript host framework
   Planned commit:
   - `feat(scene): wire scenescript host lifecycle into runtime`
6. User properties and general settings dispatch
   Planned commit:
   - `feat(scene): support user property and general setting bindings`
7. Parser support for script bindings and animated settings
   Planned commit:
   - `feat(scene): extend parser for script bindings and animated settings`
8. Synthetic image parsing
   Planned commit:
   - `feat(scene): add synthetic image parsing`
9. Text layer scene model
   Planned commit:
   - `feat(scene): add text layer scene model`
10. Text layer runtime support
    Planned commit:
    - `feat(scene): add text layer runtime support`
11. Text and clear render passes
    Planned commit:
    - `feat(render): add text and clear passes`
12. Scene to render graph translation
    Planned commit:
    - `feat(render): expand scene to render graph translation`
13. Video texture cache
    Planned commit:
    - `feat(render): add video texture cache support`
14. SceneScript media integration
    Planned commit:
    - `feat(scene): support media events in scenescript`
15. Regression coverage for migrated features
    Planned commit:
    - `test(scene): add regression coverage for migrated features`
16. Residency and non-blocking runtime improvements
    Planned commit:
    - split into small focused commits during implementation

## Full Parity Checklist

1. SceneScript native host parity
   Planned commits:
   - `feat(scene): port scenescript native media support`
   - `feat(scene): port scenescript native layer operations`
   - `feat(scene): port scenescript native material and effect bindings`
   Acceptance:
   - No `createLayer`, `destroyLayer`, `sortLayer`, media, material, or object bridge
     path remains as a silent stub when the reference has behavior.
   - Runtime-created layers can be materialized, sorted, destroyed, and rendered.
   - Media callbacks and video texture calls are observable through tests.

2. Parser and scene model parity
   Planned commits:
   - `feat(scene): port effect scene model`
   - `feat(scene): port node transform resolver`
   - `feat(scene): port image alignment handling`
   - `feat(scene): complete parser parity for runtime and effect layers`
   Acceptance:
   - Reference modules such as `WPEffect`, `WPNodeTransformResolver`, and
     `WPImageAlignment` have equivalent current-repository implementations.
   - Deferred runtime layer IDs, materialization data, image alignment, text settings,
     and effect command parsing match reference behavior.

3. Text rendering parity
   Planned commits:
   - `feat(scene): port text layout and glyph rasterization`
   - `feat(render): render text glyph atlases in text pass`
   Acceptance:
   - `TextPass::execute` performs real rendering work or emits an explicit unsupported
     diagnostic behind a disabled feature gate.
   - Text layout, font selection, glyph atlas/cache behavior, alignment, color, and
     effects match the reference implementation within the available backend.

4. Video texture parity
   Planned commits:
   - `feat(render): port video decoding texture pipeline`
   - `feat(render): upload video frames through video texture cache`
   Acceptance:
   - `VideoTextureCache::Poll` and `RecordUploads` are real implementations.
   - Playback state, seeking, pause, frame upload, cache accounting, and release behavior
     are covered by tests.

5. Render graph and residency parity
   Planned commits:
   - `feat(render): port scene to render graph parity`
   - `feat(render): port deferred pass preparation`
   - `feat(render): port residency refresh and resource warmup`
   Acceptance:
   - Render graph construction covers the reference `SceneToRenderGraph` behavior.
   - `VulkanPass` subclasses expose deferred prepare, refresh, warmup, and residency
     state absorption behavior where the reference does.
   - Runtime layer materialization and pass preparation are non-blocking where the
     reference is non-blocking.

6. Final parity audit
   Planned commit:
   - `test(scene): add full parity regression coverage`
   Acceptance:
   - A file/module comparison against `../wallpaper-scene-renderer-new` has no unexplained
     missing scene/runtime/render modules.
   - All stub/no-op searches are either gone or documented as explicit unsupported
     platform fallbacks with tests.
   - Full build, tests, standalone viewer build, and diff checks pass.
