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
- Unit or regression tests cover high-risk behavior where coverage is needed; avoid adding
  tests for every small migration edit when an existing build or runtime validation already
  exercises the path.
- `cmake --build build-check -j2`, `ctest --test-dir build-check --output-on-failure`,
  `cmake --build build-standalone -j2 --target sceneviewer`, and `git diff --check`
  pass after the item.
- A code comparison against `../wallpaper-scene-renderer-new` shows no remaining
  untracked parity gap for the item.

## Workflow Constraints

- Migrate incrementally.
- Before each migration item, define the intended conventional commit.
- Migrate in coherent batches; do not run tests after tiny edits. After a substantial
  batch is complete, run the minimum validation needed to expose compile/runtime errors,
  fix those errors in one pass, and then commit.
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
   Progress:
   - `feat(scene): port scenescript layer operations` added a real scene-layer registry,
     parser population for image/particle/text layer IDs and names, and SceneScript
     `getLayer`, `getLayerCount`, `enumerateLayers`, `getLayerIndex`,
     `getInitialLayerConfig`, `sortLayer`, `destroyLayer`, and `createLayer` event
     application into `Scene`.
   - `feat(scene): port material user binding metadata` added the material-side
     `usertextures`/`usershadervalues` parse storage and preserves shader material-value
     aliases on `SceneMaterial`, giving the SceneScript material/effect bridge the same
     alias data that the reference parser exposes.
   - `feat(scene): dispatch scenescript material uniforms` makes `MaterialUniform`
     registrations write resolved user/script values into `SceneMaterial::customShader.constValues`,
     resolves authored material aliases to GLSL uniform names, and marks the mesh dirty so runtime
     material updates are visible to rendering instead of only being tracked as host diagnostics.
   Remaining:
   - Runtime-created layers are represented by a real `SceneNode` and scene registry entry,
     but full asset/config materialization still needs the reference `CreateDynamicSceneLayer`
     behavior.
   - Layer parent/children relation APIs still need the remaining reference behavior.
   - Material/effect/native object bridges still need runtime material proxy getters/setters,
     effect material indexing, and native object exposure beyond the now-functional
     `MaterialUniform` registration dispatch path.
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
   Progress:
   - `feat(scene): port material user binding metadata` parses `WPMaterial.usertextures` and
     `WPMaterial.usershadervalues`, merges them across material passes, stores shader
     `material` metadata aliases on `SceneMaterial`, and adds parser helpers for resolving
     user shader values to GLSL uniforms.
   - `feat(scene): thread user properties through parser material bindings` adds the
     reference-style parser overload that receives active `UserPropertyMap`, threads it through
     image and particle material loading, applies `usershadervalues` to cold-start material
     uniforms, and registers `MaterialUniform` bindings for live SceneScript/user-property
     dispatch.
   - `feat(scene): route project user properties through runtime loading` exposes the
     load-time/live user-property property names, stages load-time values before `source`
     triggers parsing, passes the active `UserPropertyMap` into `WPSceneParser::Parse`, and
     forwards live user-property updates to the render-thread SceneScript host.
   - `feat(scene): register effect material user shader bindings` registers image-effect pass
     `usershadervalues` as `MaterialUniform` bindings after each effect material is attached,
     so live project user-property updates reach effect shader uniforms instead of only the
     image-layer base material.
   - `feat(scene): apply user properties during json parsing` adds the parser-wide JSON
     user-property override scope used while `wpscene::WPScene::FromJson` reads authored
     scene fields, including scalar, vector, fixed-array, string, bool, and condition-gated
     `user` bindings.
   - `feat(scene): evaluate parser-time script json values` ports the remaining reference
     `WPJson` parser-time value resolution path: animated `startpaused` properties read their
     initial keyframe value, JSON `script` expressions are evaluated through the lightweight
     SceneScript runtime before `WPScene::FromJson` materializes objects, `scriptproperties`
     can receive project user-property overrides, and root orthogonal canvas size is available
     to parser-time script evaluation.
     This covers the shared `GET_JSON_*` path used by scene, object, material, effect, particle,
     text, and uniform model parsing, so authored `value`, `animation`, `script`, and `user`
     wrappers resolve before the downstream `wpscene::*::FromJson` structures are populated.
     Parser-time script evaluation remains intentionally best-effort like the reference: when the
     lightweight runtime cannot evaluate an expression, parsing falls back to the authored raw
     value so the persistent SceneScript host can still run the real callback lifecycle after load.
   - `feat(scene): consume parser text render scale` passes the scene root JSON into
     `ScopedJsonUserProperties` and stores the requested render scale on `Scene` during parse
     so text layers can build against the renderer scale known at load time.
     This removes the earlier local `(void)text_render_scale` gap and matches the reference
     parser's load-time text scale handoff without adding another test-only compatibility path.
   - `feat(scene): warm up deferred text residency` replaces the empty SceneScript host residency
     hook with real deferred text layer materialization: queued text layers are rebuilt through the
     text runtime layout path, removed from the deferred set on success, and marked dirty for render
     graph resource refresh.
   - `feat(scene): materialize scenescript text layers` routes SceneScript `createLayer` events with
     text layer config through the real text parser/runtime path instead of only creating an empty
     placeholder node: the dynamic layer receives a generated id, text primitive, scene node,
     layer registry entry, runtime text state, render-graph topology dirty flag, and text resource
     dirty flag. Hidden dynamic text config now registers the same real text layer and relies on
     layer visibility propagation instead of falling back to an empty placeholder.
   - `feat(scene): port layer visibility propagation` adds reference-style local/effective layer
     visibility state to `Scene` and `SceneNode`, propagates parent layer visibility through
     runtime nodes, seeds parser/runtime layer registrations with authored visibility, and makes
     render graph traversal skip nodes whose effective visibility is false.
   - `feat(scene): port dynamic scene layer materialization` introduces the parser-level
     `CreateDynamicSceneLayer` entry used by SceneScript `createLayer` events. Runtime-created
     image, particle, text, and light configs now go through the same object parsers as static
     scene objects, receive real layer ids, runtime nodes, visibility state, full initial config
     storage, render-graph topology invalidation, and newly-created binding/script/animation
     registrations. Configured dynamic layers that cannot be materialized no longer fall back to
     empty placeholder nodes.
   - Static image, particle, text, and light layer registration now stores the full authored object
     JSON as the layer initial config instead of the earlier `{id,name}` compatibility shell, so
     SceneScript layer enumeration and `getInitialLayerConfig` see the same config shape that
     dynamic materialization consumes.
   - `feat(scene): port sound layer runtime handles` gives sound layers real runtime residency in
     `Scene`: `SoundManager::MountStream` returns per-stream handles, streams can be unmounted and
     controlled independently, `WPSoundParser` registers authored/dynamic sound layers by handle,
     `Scene::DestroyLayer` unloads the matching sound stream, SceneScript-created sound configs are
     materialized instead of rejected, and live `volume` bindings write through to the mounted
     channel.
   Remaining:
   - Dynamic parser/materialization now receives the active user-property map for supported
     image/particle/text/light/sound configs, but still needs the full reference root-scoped scene
     JSON context for dynamic configs whose script expressions depend on broader scene metadata.
   - Sound random playback now avoids immediate repeats, but the reference min/max random ambient
     delay scheduling still needs to be migrated.
   - Full reference parser visibility contracts, dependency-aware lazy materialization,
     deferred image/particle materialization, model layers, shape/direct-draw layers, and dynamic
     `CreateDynamicSceneLayer` object construction for shape/direct-draw, model, and empty
     layers remain separate parity gaps; they are not hidden behind the parser-time JSON value
     migration above.
   Acceptance:
   - Reference modules such as `WPEffect`, `WPNodeTransformResolver`, and
     `WPImageAlignment` have equivalent current-repository implementations.
   - Deferred runtime layer IDs, materialization data, image alignment, text settings,
     and effect command parsing match reference behavior.

3. Text rendering parity
   Planned commits:
   - `chore(build): enable text rasterization dependencies`
   - `feat(scene): port text layout and glyph rasterization`
   - `feat(scene): port text primitive runtime geometry updates`
   - `feat(render): render text glyph atlases in text pass`
   Progress:
   - `feat(render): render text glyph atlases in text pass` now routes TextPass through
     scene-owned text primitives, dedicated text shaders, atlas page textures, dynamic
     mesh uploads, and real background/glyph draw submission.
   - `feat(scene): port text layout and glyph rasterization` enabled the real text
     rasterization dependencies and ported reference Pango/Cairo/fontconfig/FreeType
     layout, font resolution, glyph bitmap caching, atlas packing, canonical text
     primitive meshes, runtime relayout, screen-anchor placement, bridge sizing, and
     regression coverage for non-empty atlas pages and glyph meshes.
   Remaining:
   - Audit parser/effect integration against the reference after render-graph resource
     refresh consumes text dirty keys.
   Acceptance:
   - `TextPass::execute` performs real rendering work.
   - Text layout, font selection, glyph atlas/cache behavior, alignment, color, and
     effects match the reference implementation within the available backend.
   - No text layer path silently falls back to rectangular placeholder geometry when
     reference behavior can rasterize the text.

4. Video texture parity
   Planned commits:
   - `feat(render): port video decoding texture pipeline`
   - `feat(render): upload video frames through video texture cache`
   Progress:
   - `feat(render): upload video frames through video texture cache` replaced the silent
     `Poll`/`RecordUploads` no-ops with a real CPU-pipeline state path: playback advances,
     pause/stop is honored, seek requests are applied, pending uploads are queued, recorded
     uploads are accounted, cache entry/byte release is covered, and missing GPU/decoder
     capability emits an explicit diagnostic instead of failing silently.
   Remaining:
   - The reference GStreamer/VA/NVIDIA decoder pipeline and actual decoded-frame pixel
     replacement/upload still need to be migrated before video texture parity is complete.
   Acceptance:
   - `VideoTextureCache::Poll` and `RecordUploads` are real implementations.
   - Playback state, seeking, pause, frame upload, cache accounting, and release behavior
     are covered by tests.

5. Render graph and residency parity
   Planned commits:
   - `feat(render): port scene to render graph parity`
   - `feat(render): port deferred pass preparation`
   - `feat(render): port residency refresh and resource warmup`
   Progress:
   - `feat(render): add text pass residency refresh hooks` adds pass-level
     `refreshResources`, `warmupPipeline`, render-target/text-layer reference queries,
     residency graph-state absorption, and TextPass-specific reuse/reference behavior.
   - `feat(render): port targeted render graph resource refresh` makes
     `VulkanRender` consume `Scene::dirtyRenderTargetKeys`, `dirtyTextLayerIds`, and
     `renderGraphAllResourcesDirty`, refreshing only affected render-target/text-layer
     dependencies and using a real destroy/prepare refresh path for generic passes.
   - `feat(render): port render graph pass residency handoff` keeps reusable prepared
     pass objects alive across topology rebuilds through `canReuseForResidency` and
     `absorbResidencyGraphState`, replaces graph pass references with resident
     instances, and destroys retired pass instances.
   - `feat(render): move render graph uploads onto frame command path` removes the
     compile-time staging-buffer submit/`DeviceWaitIdle`, records prepared static and
     dynamic uploads in the frame command buffer before pass execution, exposes pipeline
     warmup over the current render graph, and gives ClearPass/CopyPass real
     render-target reference, refresh, and residency-gate handoff behavior.
   - `feat(render): port deferred render graph preparation queue` adds the pass-level
     deferred prepare/resource-wait contract, queues cold non-copy passes on topology
     refresh when a resident graph already exists, advances that queue during `drawFrame`
     with a frame budget, and keeps CopyPass dependencies synchronously prepared so
     reused shader passes can bind copy targets.
   - `feat(render): port texture cache deferred graph activation` keeps transient
     render-target alias release intents behind the deferred graph activation fence,
     replays them only after queued passes become resident, and fixes multi-key
     `MarkShareReady` so shared query textures are reusable only after every logical key
     reaches its last read.
   - `fix(scene): port stock render targets and shader compatibility fixes` registers
     Wallpaper Engine's built-in `_rt_shadowAtlas`, maps `_alias_lightCookie` to the
     stock cookie texture, fixes default render-target content dimensions, and ports the
     post-preprocessor GLSL compatibility needed by stock/zcompat shaders that mix WE's
     integer, float, and bool conventions.
   Remaining:
   - Real pass-specific resource-wait states still need their reference behavior.
   - Pipeline warmup still needs the reference hidden-layer warmup render graph, not only
     the public warmup entry over the active graph.
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
