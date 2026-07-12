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
| Input events | `DONE` | C ABI validates move/down/up/wheel/key/focus events and field layout; Web forwards every supported class, Scene intentionally accepts pointer position/buttons only. |
| Validation layer | `DONE` | Existing append-only render config field |
| Diagnostics JSON | `DONE` | Two-call buffer contract and error-source assertions |
| Pure C consumer | `DONE` | `we-renderer-c-abi-test` |
| Rust ABI consumer | `DONE` | `we-renderer-rust-abi-test` compiles a real `repr(C)` caller with `rustc`, links the shared library, verifies append-only layouts, legacy source prefixes, diagnostics, and source-options errors. |
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
| `23e0b14` | update shader spec uniforms | `DONE` | Added new/legacy Daytime names and source-layer effect matrix contracts with runtime matrix/inverse tests. |
| `2728164` | discover non-standard pkg names | `DONE` | Shared resolver maps `project.file` to matching package names; default `scene.pkg`, nested paths, and traversal rejection are covered by tests. |
| `3ace9c4` | add special shader names | `DONE` | Combo names with active local consumers are centralized and used by model, sprite, particle, lighting, and color-blend paths; morph-only and otherwise unconsumed names are intentionally not declared without a data path. |
| `9a7063d` | rename special names module | `EXCLUDE` | Structural rename has no behavioral value. |
| `f15539a` | remove workshop scene runtime tests | `EXCLUDE` | Local project intentionally keeps an optional workshop corpus. |

Stage 2 schema coverage is split by behavior rather than by field presence:

| Object schema | Status | Contract |
|---|---|---|
| `dependencies` | `RUNTIME` | Preserved and still consumed by scene dependency residency. |
| image `instance` | `RUNTIME` | Raw JSON is preserved and material-pass overrides still apply. |
| parent/attachment, copybackground, text visible binding | `RUNTIME` | Existing consumers remain covered by regression tests. |
| lock/mute/no-interpolation metadata | `PRESERVED` | Stored on image, particle, text, and sound objects. |
| image perspective/solid/opaque/clamp/shadow/depth/background metadata | `PRESERVED` | Parsed without claiming render behavior. |
| animation layer id/name/additive/blend transitions | `PRESERVED` | Stored separately from the existing animation id/blend runtime contract. |
| sound block alignment/spatialization/queue mode | `PRESERVED` | Stored for later media-runtime migration. |
| `disablepropagation` | `RUNTIME` | Image parents block only descendant parallax anchors; normal parent transforms and child-authored depth remain active. |

The corpus manifest retains representative scene-format 0/4/6 workshop IDs. Object-level synthetic
fixtures cover the cross-version field surface without requiring copyrighted workshop assets. Text
object parsing now has one definition in `WPTextObject.cpp`; the duplicate archive definition in
`WPTextLayer.cpp` was removed so link order can no longer select stale parsing behavior.

`corpus-parser-probe` now provides the stable parser observation surface that the version manifest alone
could not supply. Given a scene workshop directory or `project.json`, it resolves custom package names,
sorts the package file table, summarizes the scene root/object kinds, parses every TEX header and MDL
header, records valid MDL section stamps, separates unknown sections, and emits deterministic warnings.
The JSON intentionally excludes absolute paths, timestamps, pointers, and cache-dependent ordering. A
checked-in synthetic snapshot exercises PKGV0023, TEXB4/TEXS3, MDLV21, a known `MDLS` section, an unknown
`MDZZ` section, malformed TEX diagnostics, and scene object summaries. `corpus-parser-probe <project>
--compare <snapshot>` emits an RFC 6902 JSON Patch and exits nonzero when the current parser differs; the
CTest contract also proves that a changed package version becomes a single `/package/version` replace
operation.

Consumed shader combo names now have one contract in `SpecTexs.hpp`: blend mode, bone count,
skinning, sprite-sheet/NPOT handling, thick particle format, trail rendering, and lighting. Parser,
model, particle, and effect code no longer duplicates those literals. Names that only become meaningful
with a future morph buffer, advanced model attribute, or unused lighting path remain absent rather than
creating a false compatibility surface.

### Stage 3 — transform, puppet, and particles

| Commit | Subject | Status | Local action |
|---|---|---|---|
| `8a07eeb` | degenerate node camera transforms | `DONE` | Node cameras safely invert repaired world frames; the same commit's hidden linked-solid passthrough condition is covered by Stage 4 private link publication and hidden-source execution tests. |
| `1a19a32` | child particle override inheritance | `DONE` | Child presets inherit only layer alpha/tint; their authored count/rate/lifetime/size/speed/control points remain independent. Local alpha already used the corrected linear scalar path. |
| `1691a07` | hidden particles and rain drag | `DONE` | Drag uses authored linear strength. Local node-visibility traversal already skips generated descendants by subtree, now covered with assertions enabled. Font fallback is tracked separately in Stage 5. |
| `ebd56ee` | node field animations | `DONE` | The shared property-animation registry already covers node origin/angles/scale across SceneNode-backed layers; an end-to-end light fixture verifies parser registration and midpoint runtime writes. Sound transforms remain part of Stage 5 spatialization. |
| `8234617` | particle random frame motion | `DONE` | Stable per-particle random frames, rate-independent emitter cadence, and owner-basis conversion for world-space gravity are implemented. |
| `673a2b6` | image alignment in local geometry | `DONE` | Image card, effect-final, and puppet/static mesh positions carry the alignment offset; node origin remains the script/child pivot and aligned cursor bounds follow visible geometry. |

Stage 3 now separates authored, bind, and animation bone relationships as `file_parent`,
`bind_parent`, and `anim_parent`. Normal models keep the original hierarchy for both bind and
animation. MDLV21 flattens only the bind hierarchy, computes area-weighted vertex centroid pivots,
and still inherits animated parent deltas. SceneScript local/model transform conversion follows the
same parent contract. MDLE world-bind matrices remain preserved but observational because their
runtime meaning is not yet validated. Synthetic MDLV17/21 tests cover bind-pose identity, centroid
pivots, inherited root motion, and ordinary-model non-regression.
The hidden linked-solid condition bundled into `8a07eeb` is no longer outstanding: linked solid layers
without authored effects receive a synthetic passthrough only when they are dependency sources, and
hidden sources continue executing their private publication path without writing into the visible frame.

Image-layer `disablepropagation` is collected into Scene-owned metadata before layer materialization
and reused by deferred/dynamic parsing. Both physically parented nodes and routed effect/world nodes
retain their authored transform binding while omitting the parent parallax anchor. Parser integration
tests compare enabled and disabled parents; resolver tests verify that child-local depth still moves
independently.

Node-attached cameras now derive their view from the inverse complete `ModelTrans`, so parent chains
and authored scale no longer leak into camera-local clip space. A missing Z axis is reconstructed from
X/Y when possible; non-finite or still-singular frames fall back to identity before inversion. Tests
cover parented scale, zero Z, fully collapsed scale, and NaN input.

Particle instance overrides now use a shared production/test resolver. Root subsystems preserve every
authored override, while child presets receive only enabled/alpha/color/colorn state. Regression tests
execute with assertions enabled and verify both the copied tint result and the reset child-owned
emission, lifetime, size, velocity, and control-point fields.

Particle drag now evaluates `-speed * strength * density`. Scalar and vector regression assertions
lock the authored coefficient and direction, preventing the previous doubled damping from returning.

Render-plan traversal stops at an invisible node before visiting children, so anonymous/generated
particle descendants do not need layer IDs to be elided. The render-graph regression now toggles a
hidden particle parent with an ID-less child and verifies both passes disappear and return together.
The target explicitly undefines `NDEBUG`, making its existing and new assertions effective.

Node field animations reuse the existing `propertyAnimationRegistrations` and SceneScriptHost writer
instead of introducing a second curve state on `SceneNode`. A synthetic light layer proves authored
origin, angles, and scale are registered by the parser and reach their expected midpoint after a
0.5-second runtime step.

Random-frame particles now receive an independent scalar at spawn time and reuse it for their entire
lifetime. This avoids identical initializer output collapsing multiple particles onto one atlas frame.
Regression coverage verifies spawn range/diversity, stable resolution, and safe clamping below 1.0.

Particle `rate` now scales simulation time, lifetime, and operators without changing emitter cadence.
Regression coverage compares slow and fast simulations with the same emitter: both produce the same
particle count while their lifetimes advance at the configured rates.

World-space particle gravity is authored in scene coordinates and converted through the inverse owner
node linear transform before local simulation. Local-space particles retain the original vector.
Regression coverage rotates the owner basis by 90 degrees and verifies the local acceleration maps
back to the same authored world-down direction.

Image alignment is now written into image geometry rather than `SceneNode::AlignmentOffset`. Ordinary
cards, effect final cards, cropped final quads, and MDL-backed image meshes share the same local
offset. Dynamic image-size animation rewrites aligned vertices, meshless effect cursor bounds use the
same offset, and child resolved transforms remain anchored to the authored parent origin. Text keeps
its independent node-alignment layout contract.

### Stage 4 — RenderGraph and visual composition

| Commit | Subject | Status | Local action |
|---|---|---|---|
| `c50269f` | solid layer link composites | `DONE` | Explicit private link publication plus visible/hidden final-owner regression. |
| `5431b87` | preserve text color in glyph seed | `DONE` | First-class TextPass preserves authored glyph RGB independently from background brightness. |
| `f721e8c` | composite final perspective effects | `DONE` | Perspective shaders are classified as authored final writers. |
| `f7406a4` | media thumbnail image fallback | `DONE` | Eligible hidden static-image fallbacks are indexed before visibility pruning and verified pixel-for-pixel. |
| `ba993dd` | image color blend final owner | `DONE` | BLENDMODE is applied to the actual layer/puppet/final-effect writer. |
| `9165e37` | seed text effects with background | `DONE` | Framebuffer RGB/zero-alpha prefill precedes one private glyph seed. |
| `1a6251c` | unresolved layers script-safe | `DONE` | Logical layers without SceneNode ownership expose stable origin/angles/scale defaults through SceneScript. |
| `c52a28e` | resize dynamic text effect targets | `DONE` | Runtime text rebuild grows bridge/FBO resources and marks only affected targets dirty. |
| `a03db48` | color blend alpha compositing | `DONE` | Legacy 0–100 image alpha is normalized and RGB-only passes preserve destination alpha. |
| `7556026` | transparent previous effects | `DONE` | TRANSPARENCY plus a sampler annotated as `previous` may composite directly. |
| `1362b6d` | simplify copybackground color blend | `DONE` | Consolidated into final-owner planning; no dedicated additive workaround remains. |
| `fdce1ee` | copybackground effect color blend | `DONE` | COPYBG is propagated as a shader combo through authored and synthetic final effects. |
| `680b3d8` | dynamic effect text clipping | `DONE` | Cropped visible source metrics drive bridge targets and final mesh placement. |
| `26fc451` | atmosphere effect rendering | `DONE` | Legacy atmosphere aliases, light combo, uniform seeds, shader initialization, and passthrough target sizing are covered. |
| `40c60bb` | spin effect final quad center | `DONE` | Position-annotated spin/transform uniforms use effect-space values internally and authored values on direct final quads. |
| `73342e3` | clock highlight rendering | `DONE` | Unique FBOs, microsecond frame timing, and the existing dynamic text bridge path cover the relevant rendering behavior. |
| `9c7ef47` | text alpha composition | `DONE` | Private glyph source uses straight RGBA; translucent final composite applies alpha once. |
| `e366bc0` | copybackground video blend | `DONE` | COPYBG shader composition and existing getVideoTexture runtime controls are covered together. |
| `c79db16` | scroll effect final composite | `DONE` | Scroll shaders remain authored final writers. |
| `fdfd195` | copybackground color blend | `DONE` | Superseded fixed-additive experiment replaced by shader-owned blend math. |
| `f0eb7c1` | clamp image effect render targets | `DONE` | Primary targets, cameras, effect layers, and scaled FBOs use non-zero extents; passthrough uses active-camera size. |
| `77bcea2` | transform effect final composite | `DONE` | Transform shaders remain authored final writers; ordinary filters publish through the neutral composite. |
| `4cab330` | dynamic text effect sizing | `DONE` | Camera, final mesh, source target, and dependent FBOs synchronize after text layout changes. |
| `082ba2c` | image alpha visibility | `DONE` | Layer visibility remains a topology/residency flag and never rewrites material alpha; hide/show retention is covered. |
| `faf188d` | image alpha user bindings | `DONE` | Object-form initial values and live updates reach direct, deferred, source, and authored final-effect materials. |

Linked solid layers without authored effects now receive a neutral passthrough only when they are
used as offscreen dependency sources. The authored/synthetic final effect remains on a private local
target; RenderGraph publishes that result explicitly as `_rt_link_<id>` and consumers bind that
publication instead of copying the cumulative `_rt_default` framebuffer. A separate neutral final
composite writes the source back to the scene only while its world node is visible. Hidden dependency
sources still execute their private shader passes through `execute_when_hidden`, so link consumers
remain live without leaking the source layer into the visible frame. The parser-to-RenderGraph test
covers instance texture normalization, private source ownership, consumer binding, and both visibility
states.

Effect-backed text now has one explicit source pipeline: copy the accumulated framebuffer RGB with
zero alpha into the first private target, draw the glyph atlas once into that target, execute the
authored effect chain, then publish through the translucent final composite. Offscreen glyph passes
store straight RGBA with `Normal` blending, while direct scene text and the final publisher retain
source-over blending, so text alpha is not multiplied twice. Glyph color remains the authored RGB and
is no longer coupled to background brightness. The first-class text runtime continues to use cropped
visible-source metrics for clipping and effect sizing; dynamic text rebuilds update the bridge camera,
final mesh, ping-pong targets, and authored effect FBOs, then mark only those resources dirty. A
parser-to-RenderGraph regression verifies prefill/glyph/effect/final order, one glyph seed, background
sampling, blend contracts, and target growth after a live text update.

Effect final ownership is now capability-driven rather than assuming every last authored pass can
write the visible framebuffer. Shader preprocessing preserves sampler material annotations in its
versioned cache, allowing transparent shaders that explicitly sample `previous` to be recognized.
Generic image/passthrough, transform, scroll, and perspective shaders remain direct final writers.
All other terminal filters stay on a private ping-pong target and are published by the neutral final
composite. Tests cover annotation parsing, direct and private topology, linked-layer publication, and
text bridges.

Image color blending is assigned to the real final owner. Layers without an effect chain apply
`BLENDMODE` to their image material; effect-backed layers append one synthetic final compositor; an
animated puppet keeps the combo on its layer-surface writer. Local technical effects such as linked
solid passthroughs count as an effect chain, preventing the combo from being stranded on a replaced
source material. `copybackground` is carried as `COPYBG=1` on every effect material, including the
synthetic color-blend writer, and no fixed additive material blend is used. Legacy image alpha in the
0–100 range is normalized at object parse time. Passes that preserve destination alpha now zero only
their alpha blend factors while retaining authored RGB blending. Integration tests cover direct,
effect-backed, linked-solid, shader-combo, alpha-write, and existing video-texture control paths.

Effect compatibility now shares one bounded extent contract: invalid or sub-pixel primary dimensions
clamp to 1, ordinary targets use authored effect-source size, and fullscreen/passthrough effects use
the active camera extent without changing layer geometry. Authored `unique` FBOs are non-reusable,
including text-effect FBOs. Frame pacing computes the requested interval directly in microseconds.
The legacy atmosphere material receives its historical aliases, `g_ViewForward`, default light index,
and initialized fragment-shader locals without affecting unrelated shaders. Shader metadata also keeps
`position:true` uniforms in its versioned cache. Spin/transform MODE=1 passes receive normalized
effect-space centers on every graph build, while a direct final writer restores the authored center;
private writers remain normalized. The unrelated particle-rate reversal included in the clock commit
is intentionally not adopted because Stage 3 already has a tested, newer local rate contract.

System media thumbnail fallback indexing now happens before static hidden-layer pruning, so an authored
hidden cover image can seed `$mediaThumbnail` even when it never becomes a renderable layer. Only simple
static images qualify: puppet, fullscreen, passthrough, effect-backed, and special-target sources are
rejected. A dedicated parser test validates both eligibility boundaries and exact fallback pixels.
SceneScript logical-layer proxies also preserve transform value shape before a deferred or nonvisual
layer owns a `SceneNode`: origin and angles return zero vectors and scale returns a unit vector.

Image visibility and image alpha intentionally remain separate in the local runtime. Visibility updates
only change layer graph residency and `SceneNode::LayerVisible`; opacity stays in material uniforms and
therefore survives hide/show cycles. Existing generic property registrations already parse object-form
alpha bindings, clamp them to the normalized domain, materialize deferred images before the remaining
same-dispatch bindings run, and synchronize direct/source/authored-final effect materials. A dedicated
integration test covers initial overrides, deferred visibility, same-dispatch alpha, hidden updates,
reshow retention, and direct-final effect ownership. No parallel `SceneNode` alpha state is introduced.

### Stage 5 — SceneScript, dynamic assets, audio, and media

| Commit | Subject | Status | Local action |
|---|---|---|---|
| `707f4ef` | expose image effect visibility | `DONE` | Layer proxies resolve effects by name or index and reuse the existing effect visibility proxy/bypass path. |
| `e1b47fe` | preserve field value shape | `DONE` | Cross-frame scalar returns are coerced back to the registered vector shape before the next script update. |
| `ebb7031` | dynamic audio setting | `NOT_APPLICABLE` | Upstream change is confined to the Waywallen plugin/host audio gate; renderer ABI/source volume and mute remain host-neutral. |
| `182340f` | initial user property strings | `DONE` | Stage 1 JSON path accepts strings and applies before source load. |
| `3198eff` | calendar text layer scripts | `DONE` | Cross-layer text writes rebuild the first-class text primitive, atlas, and auto-sized layout immediately. |
| `a66f60d` | looped texture frames | `DONE` | Embedded H.264 playback reaches EOS, performs the flushing loop seek, uploads a new sample, and retains the same sampled image/view/sampler across the boundary. |
| `08da17e` | hardware video textures | `DONE` | Keep the local GStreamer architecture; AMD VA VAMemory is imported through DMA-BUF and copied into the stable Vulkan sampled image, with a forced CPU RGBA fallback regression. NVIDIA CUDA paths remain structurally covered but were not executable on this AMD host. |
| `c4cb8bb` | dynamic asset layers | `DONE` | Registered image-model assets create real runtime layers through the existing parser, and destroy/recreate uses normal ownership and resource release rather than an upstream fixed clone pool. |
| `07b26a1` | dynamic text loading | `DONE` | Scene-owned text nodes/primitives avoid the upstream raw-pointer callback lifetime issue; VertexArray append/move ownership, index resource IDs, shader annotation leading-zero numbers, and dynamic text destruction are covered directly. |
| `898d2b4` | MPRIS media events | `DONE` | Renderer-local thumbnail/properties/playback callbacks, change de-duplication, and current/previous thumbnail texture registration are covered; the Waywallen MPRIS bridge remains host-side and is not imported. |

SceneScript layer proxies now expose `getEffect(nameOrIndex)` and resolve authored effect names to the
same index-based proxy already used by effect property scripts. Visibility writes therefore retain the
existing conditional-pass and bypass-copy topology instead of rebuilding a second effect model. Field
scripts continue to read the applied node state before every frame; a scalar result targeting a Vec3 is
broadcast to all components and is observed as that Vec3 on the following update. Cross-layer calendar
scripts can assign another layer's `text` property, which immediately rebuilds glyphs, increments the
atlas revision, expands auto-sized layout, and marks the text resource dirty. The upstream dynamic-audio
commit only adds a Waywallen plugin setting and daemon bridge call, so it is deliberately excluded from
the renderer library and its stable host-neutral ABI.

Dynamic asset creation remains a genuine parser/runtime operation locally. `engine.registerAsset()`
handles for image model JSON can be passed directly to `thisScene.createLayer()`, then destroyed and
recreated from the same asset without a preallocated clone pool. Font handles are accepted inside a
dynamic text configuration, for example `{ text: "...", font: engine.registerAsset("fonts/x.ttf") }`.
Nested handles retain their optional workshop namespace and are resolved through the scene VFS before
the normal text parser runs, so `.ttf`/`.otf` layers reuse the existing FreeType, Fontconfig, Pango,
glyph-atlas, effect-target resize, and cache path instead of introducing a script-only font loader. The
dynamic-font regression creates, destroys, and recreates a text layer from a workshop-scoped font handle
and verifies that the surviving runtime object and primitive retain the resolved asset path.

The image lifecycle regression verifies that a removed image no longer owns a runtime layer while the
recreated image keeps its scripted transform. Dynamic text destruction removes the logical layer, scene
nodes, initial configuration, text runtime state, scene-owned primitive, atlas dirty state, and name
index. Local text nodes and primitives are held by scene-owned `shared_ptr` objects, so the raw-pointer
callback lifetime repair from upstream is structural rather than copied. `SceneVertexArray` now appends
through the active tail, accepts the final capacity slot, and transfers all owned option state during
move construction/assignment; both `SceneVertexArray` and `SceneIndexArray` start with an explicit
unassigned ID so moving a newly-created geometry buffer never reads indeterminate state. Shader metadata
parsing normalizes malformed numeric literals such as `[0,01]` outside strings before JSON parsing.

The media cluster keeps the local renderer pipeline rather than adopting upstream's device and bridge
layout. A checked-in 64x64 two-frame H.264 fixture exercises real GStreamer playback. On the current AMD
host, the requested VA path decodes to VAMemory, exports/imports the frame through DMA-BUF, records the GPU
copy into the stable sampled image, and repeats the same operation after the EOS loop seek without changing
the image, view, or sampler handles. A second Vulkan device deliberately omits external-memory extensions
and forces the CPU RGBA path; MP4 AVC input is normalized to H.264 byte-stream before `decodebin`, allowing
`openh264dec` to work on systems without `gst-libav`. The NVIDIA CUDA and stateless CUDA paths are retained
unchanged and participate in the same status/upload contract, but they were not run on this AMD machine.

Media-state regression uses the renderer's host-neutral `ApplyMediaState` boundary. It verifies independent
`mediaThumbnailChanged`, `mediaPropertiesChanged`, and `mediaPlaybackChanged` delivery, suppresses duplicate
events when state is unchanged, and confirms current/previous RGBA thumbnails are registered in the
synthetic image parser and marked dirty. Upstream Waywallen MPRIS discovery and daemon bridge code remains
outside the renderer library.

### Stage 6 — MSAA, public output, and host integration

The renderer ABI now exposes append-only `we_render_config_v1.msaa_samples`. Legacy callers whose
structure ends at `allow_shm_fallback` remain valid and resolve to the historical one-sample path;
zero and one also mean 1x. The ABI parser rejects zero-sized outputs and dimensions that cannot be
represented by the renderer's existing 16-bit Vulkan output contract instead of silently truncating
them. A requested MSAA count is clamped downward to the highest count no greater than the request in
the intersection of `framebufferColorSampleCounts` and the final present format's
`VkImageFormatProperties::sampleCounts`. Counts above 1x are selected only when the physical device
also supports and explicitly enables `sampleRateShading`; otherwise the renderer logs the reason and
stays on 1x rather than exposing a multisample attachment whose fullscreen draw would be visually
identical to the legacy path.
The generic ABI rejects values above 1 for Web and Video backends with `NotSupported`; those output
pipelines do not use the scene renderer's `FinPass`, so silently accepting the option would be false
capability advertising.
The standalone Wayland viewer maps every product-facing control through the C ABI. `--user-properties
FILE` and `--graphviz FILE` build versioned `options_json`; `--valid-layer` and `--msaa N` populate the
append-only render config; `--mouse-position X,Y` sends normalized pointer events and prevents real
pointer callbacks from overriding the locked coordinates; Wayland pointer-axis events map to ABI wheel
deltas; `--diagnostics` reads and prints structured
diagnostics JSON. The viewer never includes scene-runtime, RenderGraph, or user-property implementation
types. Its parser validates positive MSAA counts and normalized pointer coordinates before creating a
session.

MSAA is deliberately confined to `FinPass`. Scene render targets, text bridges, model depth, particles,
and authored effect FBOs remain single-sample, preserving their established render-graph semantics and
memory profile. For counts above 1x, `FinPass` enables full per-sample fragment shading, samples the
resolved scene texture independently at each sample position, writes a private multisample color
attachment, and resolves into the existing single-sample swapchain/export image. The DMA-BUF,
opaque-fd, SHM readback, and frame-capture contracts therefore remain unchanged. Pipeline multisample
state, render-pass attachment descriptions, framebuffer attachments, and resolve state all derive from
one selected sample count. This is final-output per-sample resampling, not full geometry MSAA: it can
improve final scaling/subpixel sampling, but it does not recreate subpixel coverage already lost while
the scene and authored effect graph were rendered into single-sample targets.

Pure contract tests cover request clamping, feature-based 1x fallback, per-sample pipeline state, and the
one- versus two-attachment render-pass plan. C and C++ ABI tests cover append-only layout, old-structure
parsing, the new tail field, invalid dimensions, and unsupported ABI versions. A real Vulkan regression
clears `_rt_default` to a known color, executes both the legacy 1x and selected MSAA final passes, copies
the single-sample result back to the CPU, and requires identical center pixels. On the current AMD Radeon
610M host, `sampleRateShading` is enabled, the `R8G8B8A8_UNORM` format mask is `0xf`, and the selected
path is 4x. The same test presents a 24x12 image through a device initialized at 16x16, proving that the
private MSAA color attachment is recreated for a changed output extent while the pipeline/render pass
remains reusable.

#### Public RenderPlan and Texture contracts

The public C++ backend source contract exposes only `RenderPlan`. `OutputSourceType` and the old
`Texture`/`Surface` source variants were removed, as were the empty internal `TextureSource` and
`SurfaceSource` placeholders. `OutputSource::renderPlan()` is mandatory; native surfaces remain valid
presentation targets, but no backend claims to produce a separately owned native surface.

Texture output is now a separate, real contract on `OutputTargetBinding`, not a second backend source
type. Scene, Web, and Video bindings attach their completed offscreen swapchain to the same
`acquireTexture()` operation. It returns a move-only `TextureFrame` with explicit RGBA/BGRA format,
extent, DRM fourcc/modifier or linear SHM layout, per-plane offset/stride, premultiplication, owned file
descriptors, and a binding-local monotonic revision. The current producers expose implicit acquire and
release synchronization: a slot is published only after GPU completion, CEF frame delivery, or CPU copy,
and no unimplemented fence/semaphore handle is advertised. Destruction of the frame closes every owned
descriptor; callers can explicitly transfer descriptor ownership when adapting to another ABI.

The C ABI now consumes this same contract instead of dynamic-casting Scene/Web/Video bindings and reading
their private swapchains. `we_frame_v1` receives the moved descriptors while preserving its independent
session-monotonic serial and existing `we_frame_release()` ownership rule. Dedicated tests cover SHM and
DMA-BUF metadata, DRM-to-pixel-format mapping, move-only descriptor lifetime, empty/detached acquisition,
revision increments, Web software and accelerated frames, and the real Video C ABI acquire/release path.

Render-plan output is no longer a capability bit. `ContentBackend::outputSource()` is mandatory, so a
`supportsRenderPlan=false` state was contradictory. The architecture guard rejects reintroduction of
source-kind branching, false output capability flags, and the old placeholder headers.

## Migration rules

1. A commit is not marked `DONE` from code similarity alone.
2. Parser changes require a checked-in synthetic fixture or stable snapshot.
3. Visual changes require frame capture and quantified comparison against the original reported effect.
4. Existing local architecture wins over upstream structure when behavior is equivalent.
5. Waywallen presenter/plugin/release code never enters the renderer library.
6. Each stage is completed through multiple focused commits, followed by full build and test evidence.
