# Text Scene Scaling Bugfix

## Summary

`Wallpaper Engine` scene text was consistently rendered too small on Linux. The issue reproduced in both this repository and the older `../wallpaper-scene-renderer-new` reference build, which ruled out a migration-only regression and pointed to a shared runtime contract mismatch.

## Symptom

- Text looked sharper after earlier density work, but still materially smaller than the original Wallpaper Engine output.
- The gap was large enough that small percentage tweaks were not credible as a final fix.
- Preview images from workshop content showed the intended authored text occupying much more screen space than either Linux renderer produced.

## Investigation

- Parser-side propagation was already present: `textRenderScale` was being carried from runtime into scene state.
- Runtime-side relayout was already present: text primitives could rebuild after render scale changes.
- Viewer defaults were misleading for diagnosis because standalone runs used `render_scale = 1.0` and a fixed window size.
- The important finding was that our text raster density path and the final on-screen text size path were not the same thing.

To close the gap, the original Wallpaper Engine text path was reverse engineered with `Ghidra` headless analysis against `wallpaper64.exe`.

Recovered path highlights:

- Text property registration: `FUN_14020d3c0`
- Runtime text rebuild path: `FUN_14020ca20`, `FUN_1401805a0`
- Font layout / font description sizing path: `FUN_14017d860`

Key recovered behavior:

```cpp
fVar60 = *(float *)((longlong)param_2 + 0xc);
if (*(char *)(param_2 + 2) != '\0')
    fVar60 = fVar60 * (*(float *)(*param_1 + 0x78) / 768.0);
FUN_1400dfec0(..., fVar60 * 64.0);
```

That was the first hard evidence that Wallpaper Engine does not use authored `pointsize` directly. It scales text size by scene height relative to `768` before constructing the final font description.

## Root Cause

The Linux renderer had been treating authored `pointsize` as effectively final layout size, while Wallpaper Engine scales it by scene orthographic height. Because both this repo and the older Linux reference shared that assumption, both rendered text too small.

## Fix

- Thread scene orthographic height into text primitive construction and rasterization.
- Apply the same `scene_height / 768.0` scaling rule before Pango font sizing.
- Keep raster density logic separate from final display size logic.

Implementation landed in:

- [src/backend/scene/internal/parser/WPSceneParser.cpp](/home/aromatic/Applications/OwnProject/we-new/wallpaper-engine-renderer/src/backend/scene/internal/parser/WPSceneParser.cpp)
- [src/backend/scene/internal/text/WPTextLayer.cpp](/home/aromatic/Applications/OwnProject/we-new/wallpaper-engine-renderer/src/backend/scene/internal/text/WPTextLayer.cpp)
- [src/backend/scene/internal/text/WPTextLayer.hpp](/home/aromatic/Applications/OwnProject/we-new/wallpaper-engine-renderer/src/backend/scene/internal/text/WPTextLayer.hpp)
- [src/test/migrated_features_regression_test.cpp](/home/aromatic/Applications/OwnProject/we-new/wallpaper-engine-renderer/src/test/migrated_features_regression_test.cpp)

## Validation

- `cmake --build build-standalone -j2 --target text-layer-runtime-test migrated-features-regression-test sceneviewer`
- `./build-standalone/scenebackend/src/test/text-layer-runtime-test`
- `./build-standalone/scenebackend/src/test/migrated-features-regression-test`

The regression coverage now checks that equivalent text renders substantially larger at scene height `2160` than at `768`, matching the recovered engine contract.

## Takeaways

- Text raster density and final display size must be treated as separate contracts.
- When both the migration target and the old Linux reference behave the same way, the next check should be the original engine, not more local tuning.
- For Wallpaper Engine text, scene ortho height is a first-order layout input, not just a rendering environment detail.
