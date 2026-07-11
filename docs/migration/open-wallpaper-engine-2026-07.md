# open-wallpaper-engine Migration — July 2026

## Scope

This migration ports applicable renderer behavior from `../open-wallpaper-engine` into the
`WallpaperSession -> ContentBackend -> Output` architecture.

It does **not** import the Waywallen plugin, release/update metadata, presenter integration, or
upstream repository structure. Behavior is migrated only when a current test or a real wallpaper
proves that the local implementation is missing or incorrect.

Working branch:

```text
migration/open-wallpaper-engine-2026-07
```

Reference branch:

```text
../open-wallpaper-engine: investigate/spectrum-3042492564-upstream
```

## Status vocabulary

| Status | Meaning |
|---|---|
| `DONE` | Implemented and covered by local tests. |
| `REVIEW` | Compare behavior and existing local implementation before changing code. |
| `PORT` | Known missing capability; implement with tests. |
| `TEST` | Likely present locally, but requires a regression test before marking done. |
| `EXCLUDE` | Waywallen/release/integration-only content; never migrate. |

## Six delivery stages

1. **Public ABI and migration baseline**
   - Versioned `we_source_v1.options_json`.
   - Initial and live scene user properties.
   - Graphviz output path.
   - Structured diagnostics JSON.
   - Pure C ABI and exported-symbol tests.
2. **Corpus, package/texture/scene formats, and modern MDL parsing**
   - Synthetic and local workshop corpus.
   - PKGV0001–0023, TEXB0001–0004, scene format 0/4/6.
   - MDLV4/13/14/16/17/21 and section-bounded MDLS/MDAT/MDLA/MDMP/MDLE parsing.
3. **Puppet, transform, parallax, and node runtime semantics**
   - MDLV21 bind/animation parent split and centroid pivots.
   - Attachment/bone override compatibility.
   - `disablepropagation`, field animation, child particle inheritance.
4. **RenderGraph, effects, and visual composition**
   - Final-writer ownership, copybackground, alpha/color blend, transparent previous pass.
   - Dynamic target extents, clipping, linked-layer residency, hidden/unresolved layers.
5. **SceneScript, dynamic assets, media, and interaction**
   - Dynamic layers/fonts, property shape, media events, audio settings, pointer state.
   - Video loop/hardware texture behavior without replacing the local video backend.
6. **MSAA, output API, host integration, and release acceptance**
   - Final-target MSAA.
   - Honest output capabilities and a concrete Texture contract.
   - C/Rust/viewer integration and full format/visual/performance acceptance.

## Stage 1 completion

| Capability | Status | Evidence |
|---|---|---|
| Versioned source options | `DONE` | `we-renderer-options-test`, `we-renderer-scene-options-test` |
| Initial user properties | `DONE` | Applied before `source`; JSON conversion test |
| Live user properties | `DONE` | `we_session_set_user_properties_json` |
| Default metadata preservation | `DONE` | `user-properties-json-test` |
| Graphviz path | `DONE` | Public contract and source options mapping |
| Pointer input | `DONE` | Existing input event ABI and regression test |
| Validation layer | `DONE` | Existing append-only render config field |
| Diagnostics JSON | `DONE` | Two-call buffer contract and error-source assertions |
| Pure C consumer | `DONE` | `we-renderer-c-abi-test` |
| Exported ABI symbols | `DONE` | `we-renderer-abi-symbols-test` |

## Recent upstream commit matrix

### Excluded integration and release commits

| Commit | Subject | Status | Reason |
|---|---|---|---|
| `ad42968` | update.json | `EXCLUDE` | Upstream update manifest. |
| `424c2f4` | merge PR #48 | `EXCLUDE` | Merge commit; review the underlying behavior commit instead. |
| `03c1c90` | v0.1.11 | `EXCLUDE` | Version marker. |
| `d3913db` | release: add update manifest | `EXCLUDE` | Release integration. |
| `e6c14ea` | v0.1.10 | `EXCLUDE` | Version marker. |
| `812f480` | plugin update URL option | `EXCLUDE` | Waywallen plugin configuration. |
| `6b3c15a` | v0.1.9 | `EXCLUDE` | Version marker. |
| `e6d1536` | constrain user property title images | `EXCLUDE` | Waywallen UI/plugin behavior. |

### Stage 2 — formats and parser compatibility

| Commit | Subject | Status | Local action |
|---|---|---|---|
| `791f273` | audio bar shader compatibility | `DONE` | Packed two-dimensional spectrum accesses are flattened before DXC; scanner and end-to-end SPIR-V compilation tests cover the legacy form. |
| `23e0b14` | update shader spec uniforms | `REVIEW` | Compare parsed uniform schema before modifying updater code. |
| `2728164` | discover non-standard pkg names | `DONE` | Shared resolver maps `project.file` to matching package names; default `scene.pkg`, nested paths, and traversal rejection are covered by tests. |
| `3ace9c4` | add special shader names | `REVIEW` | Fold only missing names into current shader compatibility table. |
| `9a7063d` | rename special names module | `EXCLUDE` | Structural rename has no behavioral value. |
| `f15539a` | remove workshop scene runtime tests | `EXCLUDE` | Local project intentionally keeps an optional workshop corpus. |

### Stage 3 — transform, puppet, and particles

| Commit | Subject | Status | Local action |
|---|---|---|---|
| `8a07eeb` | degenerate node camera transforms | `TEST` | Add zero/degenerate transform regression before comparing behavior. |
| `1a19a32` | child particle override inheritance | `REVIEW` | Compare with local runtime override inheritance. |
| `1691a07` | hidden particles and rain drag | `REVIEW` | Validate hidden-state simulation and drag independently. |
| `ebd56ee` | node field animations | `REVIEW` | Compare with local property animation path. |
| `8234617` | particle random frame motion | `REVIEW` | Add deterministic seed fixture. |
| `673a2b6` | image alignment in local geometry | `REVIEW` | Validate local geometry versus parent transform. |

Modern MDL/MDLV21 support is required by the wider upstream history even though it is not isolated
in these 55 commit subjects. It remains a Stage 2/3 blocking deliverable.

### Stage 4 — RenderGraph and visual composition

| Commit | Subject | Status | Local action |
|---|---|---|---|
| `c50269f` | solid layer link composites | `REVIEW` | Linked-source/final-owner fixture. |
| `5431b87` | preserve text color in glyph seed | `REVIEW` | Text seed pass color assertion. |
| `f721e8c` | composite final perspective effects | `REVIEW` | Perspective effect final-writer test. |
| `f7406a4` | media thumbnail image fallback | `TEST` | Local implementation exists; add/retain exact fallback regression. |
| `ba993dd` | image color blend final owner | `REVIEW` | Validate final render target ownership. |
| `9165e37` | seed text effects with background | `REVIEW` | Distinguish clear/previous/background seed modes. |
| `1a6251c` | unresolved layers script-safe | `REVIEW` | Validate deferred materialization and unresolved references. |
| `c52a28e` | resize dynamic text effect targets | `REVIEW` | Dynamic text extent and rerasterization test. |
| `a03db48` | color blend alpha compositing | `REVIEW` | Pixel/ROI comparison. |
| `7556026` | transparent previous effects | `REVIEW` | Transparent intermediate target fixture. |
| `1362b6d` | simplify copybackground color blend | `REVIEW` | Behavior only; do not copy structural simplification blindly. |
| `fdce1ee` | copybackground effect color blend | `REVIEW` | Copybackground and alpha ownership fixture. |
| `680b3d8` | dynamic effect text clipping | `REVIEW` | Logical versus physical extent test. |
| `26fc451` | atmosphere effect rendering | `REVIEW` | Real wallpaper and synthetic pass graph. |
| `40c60bb` | spin effect final quad center | `REVIEW` | Transform center regression. |
| `73342e3` | clock highlight rendering | `REVIEW` | Calendar/clock visual fixture. |
| `9c7ef47` | text alpha composition | `REVIEW` | Text alpha and parent opacity fixture. |
| `e366bc0` | copybackground video blend | `REVIEW` | Video-backed copybackground fixture. |
| `c79db16` | scroll effect final composite | `REVIEW` | Final writer and UV bounds test. |
| `fdfd195` | copybackground color blend | `REVIEW` | Consolidate with the other copybackground cases. |
| `f0eb7c1` | clamp image effect render targets | `REVIEW` | Oversized/negative target extent fixture. |
| `77bcea2` | transform effect final composite | `REVIEW` | Transform final writer test. |
| `4cab330` | dynamic text effect sizing | `REVIEW` | Consolidate with dynamic target extent work. |
| `082ba2c` | image alpha visibility | `REVIEW` | Visibility and alpha must remain separate. |
| `faf188d` | image alpha user bindings | `REVIEW` | User property binding plus final alpha fixture. |

### Stage 5 — SceneScript, dynamic assets, audio, and media

| Commit | Subject | Status | Local action |
|---|---|---|---|
| `707f4ef` | expose image effect visibility | `REVIEW` | Compare script field exposure and runtime updates. |
| `e1b47fe` | preserve field value shape | `REVIEW` | Scalar/vector/object shape tests. |
| `ebb7031` | dynamic audio setting | `REVIEW` | Compare with local audio property route. |
| `182340f` | initial user property strings | `DONE` | Stage 1 JSON path accepts strings and applies before source load. |
| `3198eff` | calendar text layer scripts | `REVIEW` | Script/text integration fixture. |
| `a66f60d` | looped texture frames | `REVIEW` | Local GStreamer loop boundary test. |
| `08da17e` | hardware video textures | `REVIEW` | Compare behavior, keep local VA/CUDA/SHM architecture. |
| `c4cb8bb` | dynamic asset layers | `PORT` | Complete dynamic font/image asset lifetime and creation path. |
| `07b26a1` | dynamic text loading | `REVIEW` | Consolidate with dynamic fonts/assets. |
| `898d2b4` | MPRIS media events | `REVIEW` | Compare host media event contract; do not import Waywallen bridge. |

## Migration rules

1. A commit is not marked `DONE` from code similarity alone.
2. Parser changes require a checked-in synthetic fixture or stable snapshot.
3. Visual changes require frame capture and quantified comparison against the original reported effect.
4. Existing local architecture wins over upstream structure when behavior is equivalent.
5. Waywallen presenter/plugin/release code never enters the renderer library.
6. Each stage is completed through multiple focused commits, followed by full build and test evidence.
