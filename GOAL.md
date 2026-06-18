# Migration Goal

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

## Migration Checklist

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
