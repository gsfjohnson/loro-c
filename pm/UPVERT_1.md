# Switching from loro-cpp (`~/opt/loro`) to loro-c (`~/opt/loro-c`)

**Question:** How much work would it be to use the `~/opt/loro-c` library instead of `~/opt/loro`
— i.e. switch the app from the loro **C++** bindings to the loro **C-ABI** package?

**Short answer (reassessed 2026-06-17):** **small and contained — close to a drop-in swap.**
The loro-c C++ interface has been reshaped to mirror loro-cpp's API (same class names, same
`shared_ptr` ownership, same construction, same CMake target). The two blockers the original
analysis flagged are **gone**. One real seam remains: the byte-indexed text mutators the app
relies on aren't surfaced in the new C++ wrapper and need a thin shim over the C ABI.

> **Why this conclusion changed.** The first version of this analysis was written against an
> earlier loro-c that used `Doc`/`Text`/`Map` class names and **move-only** ownership. That
> package has since been updated: the hand-written C++ layer now deliberately matches loro-cpp.
> The findings below are verified against the headers currently in `~/opt/loro-c`.

## The two packages now share the same C++ surface

| | `~/opt/loro` (current, loro-cpp) | `~/opt/loro-c` (updated) |
|---|---|---|
| What it is | uniffi-bindgen-cpp generated bindings | hand-written C++20 RAII wrapper over a generated C ABI (`loro/loro.h`) |
| Doc / Text / Map / List | `LoroDoc` / `LoroText` / `LoroMap` / `LoroList` | **same** — `LoroDoc` / `LoroText` / `LoroMap` / `LoroList` |
| Other types | `UndoManager`, `EphemeralStore`, `VersionVector`, `Frontiers`, `Cursor`, `ContainerId` | **same names** |
| Ownership | `std::shared_ptr<T>` via `T::init()` | **same** — `std::shared_ptr<T>` via `static T::init()` |
| Construction | `LoroDoc::init()`, `UndoManager::init(doc)`, `EphemeralStore::init(ms)` | **identical signatures** |
| Errors | throws | throws `LoroError` (app already guards loro calls) |
| Includes | `<loro.hpp>`, `<loro/loro_ext.hpp>` | **same paths** |
| CMake | `find_package(loro CONFIG REQUIRED)` → `loro::loro` | **same** package + target |
| Distribution | `libloro.a` + `libloro_cpp_rs.a`; needs uniffi-bindgen-cpp to rebuild | single prebuilt `libloro_c_api.a`; no uniffi toolchain |

The original analysis's two headline claims are **no longer true**:

- ~~"The class names are literally different (`LoroDoc`→`Doc`, …)"~~ — they are now **identical**.
- ~~"shared-ownership → move-only mismatch"~~ — the updated wrapper is **`shared_ptr`-based**, so
  `LoroDocText`'s `std::shared_ptr<loro::LoroDoc>` / `std::shared_ptr<loro::UndoManager>` members
  work **unchanged**. No `shared_ptr` layer to add.

## Verified parity (no changes needed at these call sites)

- **Persistence / sync:** `export_snapshot`, `export_snapshot_at`, `export_updates(from_vv)`,
  `import`, `import_with`, `import_batch` — all present, same shapes.
- **Construction:** `LoroDoc::init()` (26 sites), `UndoManager::init(doc)` (2), `EphemeralStore::init(ms)` (2).
- **UndoManager:** `record_new_checkpoint`, `set_on_push`, `set_on_pop`, `can_undo/redo`, `undo/redo`,
  `set_merge_interval`, `add_exclude_origin_prefix`, `group_start/end`.
- **EphemeralStore:** `set`, `get`, `keys`, `delete_`, `encode_all`, `apply`, `subscribe`,
  `get_all_states`, `remove_outdated` (the multi-cursor TTL path — see `reference_loro_ephemeral_ttl`).
- **Text read/transform:** `slice`, `splice`, `to_delta`, `len_utf8`, `len_unicode`, `get_cursor`.
- **Containers / values:** `LoroMap`, `LoroList`, `ValueOrContainer`, `get_deep_value`, plus the
  `loro_ext.hpp` helpers (`insert_container`, `value_like`, `subscribe_*`, `on_local_update`, …).

## The one real seam: byte-indexed text mutators

The app addresses text by **UTF-8 byte offset** (its caret model is `BlockLocation {containerId,
localOffset}` in bytes). It currently calls the loro-cpp byte-indexed methods:

| Method | App call sites | In updated C++ wrapper? | In C ABI (`loro/loro.h`)? |
|---|---|---|---|
| `insert_utf8` | 28 | ❌ no | ✅ `loro_text_insert_utf8` |
| `delete_utf8` | 20 | ❌ no | ✅ `loro_text_delete_utf8` |
| `mark_utf8` | 10 | ❌ no | ✅ `loro_text_mark_utf8` |
| `len_utf8` | 23 | ✅ yes | ✅ |

The updated C++ wrapper only surfaces `insert` / `delete_` / `mark`, and those take **Unicode
codepoint indices** (per the C header: *"Inserts … at Unicode codepoint index `pos`"*). So a naive
rename `insert_utf8` → `insert` would be a **correctness bug**, not a no-op: for any non-ASCII text
(emoji, accents, CJK) byte offsets and codepoint indices diverge and positions would corrupt. The
app must keep byte addressing.

**Fix:** the byte-indexed functions exist in the C ABI — they're just not wrapped. Add a small,
contained shim (a project-local header, or upstream three inline methods into `loro_ext.hpp`) that
calls `loro_text_insert_utf8` / `loro_text_delete_utf8` / `loro_text_mark_utf8`. `len_utf8` already
exists. This is the bulk of the actual migration work, and it's localized to ~58 call sites that
all funnel through `LoroDocText` / `EditorController`.

## Build switch is trivial

The app already does `find_package(loro CONFIG REQUIRED)` and links `loro::loro`
(CMakeLists.txt:410). The updated loro-c package exports the **same** `loro::loro` target
(an INTERFACE target that pulls in `loro_c_api-static` plus the baked-in system libs
`bcrypt advapi32 kernel32 ntdll userenv ws2_32 dbghelp unwind`). No CMake **code** changes are
needed — only repoint the search prefix:

```bash
cmake -S . -B build -Dloro_DIR=$HOME/opt/loro-c/lib/cmake/loro
# (or -DCMAKE_PREFIX_PATH=$HOME/opt/loro-c)
```

Include paths line up too: `<loro.hpp>` and `<loro/loro_ext.hpp>` exist in both. The app never
includes the old `loro_scaffolding.hpp` (uniffi internals), so nothing is lost.

## Residual risks (small, localized)

- **utf8/unicode index seam** — the shim above must be byte-correct; cover it with a non-ASCII
  test (the existing `test_loro_smoke` / `test_editor_controller` suites are the place).
- **Signature-shape spot-checks** — confirm a few return types match the app's expectations,
  e.g. `EphemeralStore::get_all_states()` (now `unordered_map<uint64_t, PeerInfo>`) and the
  `to_delta()` / `TextDelta` shape. Any divergence is a local adapter, not a rewrite.
- **Exceptions** — both libraries throw; the codebase already guards loro calls
  (see `reference_loro_throws_clone_kinds`), so this is unchanged, not a new hazard.

The class-rename and ownership-rewrite risks from the original estimate **no longer apply**.

## Revised effort

**Roughly a day**, dominated by writing the `_utf8` shim and running the `test_loro_*` suite —
not the multi-day-to-multi-week refactor the first analysis projected.

1. Repoint the build at `~/opt/loro-c` (one CMake variable). Try to compile `test_loro_smoke`.
2. Add the `insert_utf8` / `delete_utf8` / `mark_utf8` shim over the C ABI.
3. Fix any signature-shape divergences the compiler surfaces (expected to be few).
4. Build everything; run `ctest --test-dir build`, with a non-ASCII text test guarding the seam.

The spike (step 1) will confirm this in well under an hour: with the names and ownership now
aligned, the compiler errors should reduce to the byte-indexed methods and a handful of
signature tweaks.
