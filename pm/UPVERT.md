# Switching from loro-cpp (`~/opt/loro`) to loro-c (`~/opt/loro-c`)

**Question:** How much work would it be to use the `~/opt/loro-c` library instead of `~/opt/loro`
— i.e. switch the app from the loro **C++** bindings to the loro **C-ABI** package?

**Short answer:** a large, invasive migration — not a drop-in swap.

## The two packages are different APIs

They share only the `loro` namespace *name*. Everything else differs:

| | `~/opt/loro` (current, loro-cpp) | `~/opt/loro-c` (the new one) |
|---|---|---|
| What it is | uniffi-bindgen-cpp generated bindings | hand-written C++20 RAII wrapper over a generated C ABI (`loro.h`) |
| Doc class | `loro::LoroDoc` | `loro::Doc` |
| Text / Map / List | `loro::LoroText` / `LoroMap` / `LoroList` | `loro::Text` / `Map` / `List` |
| Ownership | `std::shared_ptr<loro::LoroDoc>` everywhere | **move-only** `unique_ptr`-style RAII (no shared ownership) |
| Errors | throws (uniffi) | throws `loro::Error` on every fallible call |
| Value exchange | uniffi value types | `std::string` / `std::vector` / `loro::Bytes`, `std::optional` |
| Distribution | `libloro.a` + `libloro_cpp_rs.a`; needs uniffi-bindgen-cpp to rebuild | single prebuilt `libloro_c_api.a` (windows-x86_64-gnullvm zip); no uniffi toolchain |

The class names you call are literally different (`LoroDoc`→`Doc`, `LoroText`→`Text`,
`LoroMap`→`Map`), and the ownership model is incompatible — `LoroDocText` holds
`std::shared_ptr<loro::LoroDoc>` and `std::shared_ptr<loro::UndoManager>`, which the
move-only wrapper does not support without adding a `shared_ptr` layer on top.

## Scale of the change

Direct app-side references to the core types (src + tests only, excluding `pm/` docs):

| Type | References |
|---|---|
| `LoroMap` | 539 |
| `LoroText` | 260 |
| `LoroDoc` | 230 |
| `LoroList` | 60 |
| `EphemeralStore` | 28 |
| `UndoManager` | 26 |
| `VersionVector` | 6 |

**~1,150 references across ~70 files**, spanning core, controller, all three platform
view layers, and tests.

These are not pure renames: per the architecture, `RenderTree` leaves *carry*
`LoroText` / `LoroMap` handles, and the caret model is structural
(`BlockLocation {containerId, localOffset}`), so the handle types are threaded through
the rendering and editing hot paths — not hidden behind a single facade.

## Feature parity exists (no hard blockers)

The loro-c `loro.hpp` exposes everything the app depends on:

- `export_snapshot`, `export_updates`, `import` — persistence + sync
- `insert_utf8` / `delete_utf8` / `mark` — structural mutation
- `UndoManager` with `record_new_checkpoint` / `set_on_push` / `set_on_pop`
- `EphemeralStore`, `get_cursor`, `subscribe`, `get_deep_value` — presence/multi-cursor, inspector

It also ships as a single prebuilt static archive and drops the uniffi-bindgen-cpp build
dependency, which may be the actual motivation for considering the switch.

## Realistic effort

A sustained **multi-day-to-multi-week** refactor with meaningful risk — not a quick task.
Risk areas:

- **Shared-ownership → move-only mismatch.** Handles will likely need wrapping in a
  `shared_ptr` to preserve the current `LoroDocText` ownership model.
- **Throwing ops vs no-exceptions house style.** loro ops throw; the codebase is
  no-exceptions (bool/optional returns), so every new call site needs careful guarding
  (see `reference_loro_throws_clone_kinds`).
- **Schema walkers.** `clone_block_into` and the schema/render walkers must cover every
  container kind under the new handle types.

## Recommended approach

Do **not** rename ~1,150 sites by hand. Contain the change:

1. **Compatibility shim** — a header that `using`-aliases the new classes to the old
   names (`using LoroDoc = Doc;` etc.) plus `shared_ptr` factory helpers, **or** push the
   swap entirely behind `loro_schema.*` / `loro_render.*` / `loro_walk.*` / `LoroDocText`
   and adapt only the structural call sites that leak `LoroText` / `LoroMap` into
   `RenderTree`.
2. **Spike first** — point the build's `loro_DIR` at `~/opt/loro-c` and try to compile a
   single TU (`test_loro_smoke`). That surfaces the real diff in ~an hour and tells you
   whether the shim approach holds or whether the ownership model forces a deeper change.

The spike is the fastest way to turn this estimate into a concrete number before
committing to the migration.
