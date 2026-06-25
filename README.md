# wallpaper-engine-renderer

`wallpaper-engine-renderer` is a session-oriented wallpaper runtime that is being
refactored from a scene-centric renderer into:

```text
WallpaperSession -> ContentBackend -> Output
```

Current status:

- `WallpaperSession` is the top-level API surface.
- `scene` is one backend, not the architectural center.
- `render` is responsible for how frames are drawn.
- `output` is responsible for where frames are presented.
- `host` provides platform capabilities to the runtime and backends.
- current architecture notes live in [`wpdoc/runtime-architecture.md`](./wpdoc/runtime-architecture.md).

The existing scene pipeline is still used internally through a scene backend
engine shim while the runtime contracts are being lifted to the top level.

Validation commands:

```bash
cmake -S . -B build-check -DBUILD_TESTING=ON
cmake --build build-check -j2
ctest --test-dir build-check --output-on-failure
cmake --build build-check -j2 --target sceneviewer
```
