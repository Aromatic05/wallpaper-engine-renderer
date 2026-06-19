# SceneScript Cursor Input Bugfix

## Summary

Workshop scene `3351163962` exposed a cursor-follow interaction bug: the current renderer produced cleaner logs than the old Linux reference, but mouse-follow behavior was visibly broken. This turned out to be an input-to-SceneScript routing gap rather than a script execution failure.

## Symptom

- Current build looked healthier in logs than the reference build.
- Reference build still emitted repeated QuickJS errors such as `TypeError: cannot read property 'x' of undefined`.
- Despite that, the reference build appeared to preserve mouse-follow interaction better for this scene.

This made the problem "log-insensitive": visual interaction was wrong even though the runtime looked quieter.

## Investigation

The SceneScript host already exposed the expected cursor-facing state:

- `input.cursorWorldPosition`
- `input.cursorScreenPosition`
- `input.cursorLeftDown`
- `HandleCursorMove()`
- `HandleCursorButton(bool)`

So the missing piece was not object model support inside `WPSceneScriptHost`.

Comparing against `../wallpaper-scene-renderer-new` showed the older path updated multiple scene subsystems on pointer input:

- normalized mouse position
- shader value updater mouse input
- particle system mouse position
- SceneScript cursor callbacks

In this repository, normal session input only updated stored mouse position for rendering. Regular pointer move / down / up events never called the SceneScript cursor handlers.

## Root Cause

The new runtime shell had introduced a routing gap:

- `WESceneBackend::sendInput()` forwarded pointer coordinates.
- `WESceneRuntimeDriver::mouseInput()` only updated render-side mouse state.
- Scene state, particle state, shader mouse state, and SceneScript cursor callbacks were not being updated from the ordinary runtime input path.

As a result, scenes that depended on cursor-driven script updates looked inert even though the scripting runtime itself was not crashing.

## Fix

Restore the full cursor input chain inside the scene runtime driver:

- Route pointer move events through render-thread commands instead of a local position setter.
- Update `scene->mousePositionNormalized`.
- Feed mouse position into `shaderValueUpdater`.
- Feed mouse position into `paritileSys`.
- Call `scriptHost->HandleCursorMove()`.
- Add explicit left-button routing and call `scriptHost->HandleCursorButton(down)`.
- Update `scene->cursorLeftDown` on button transitions.

Implementation landed in:

- [src/backend/scene/internal/engine/WESceneBackend.cpp](/home/aromatic/Applications/OwnProject/we-new/wallpaper-engine-renderer/src/backend/scene/internal/engine/WESceneBackend.cpp)
- [src/backend/scene/internal/engine/WESceneRuntimeDriver.cpp](/home/aromatic/Applications/OwnProject/we-new/wallpaper-engine-renderer/src/backend/scene/internal/engine/WESceneRuntimeDriver.cpp)
- [src/backend/scene/internal/engine/WESceneRuntimeDriver.hpp](/home/aromatic/Applications/OwnProject/we-new/wallpaper-engine-renderer/src/backend/scene/internal/engine/WESceneRuntimeDriver.hpp)

## Validation

- `cmake --build build-standalone -j2 --target sceneviewer migrated-features-regression-test scenescript-runtime-test`
- `./build-standalone/scenebackend/src/test/scenescript-runtime-test`
- Visual verification on workshop scene `3351163962`

The key lesson from this bug is that QuickJS error volume was not a reliable proxy for user-visible correctness in this case. The decisive signal was interactive behavior under a real scene.
