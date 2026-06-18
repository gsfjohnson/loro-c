# Switching from loro-cpp (`~/opt/loro`) to loro-c (`~/opt/loro-c`)

**Question:** How much work would it be to use the `~/opt/loro-c` library instead of `~/opt/loro`
— i.e. switch the app from the loro **C++** bindings to the loro **C-ABI** package?

**Short answer (re-verified 2026-06-17, second pass against the updated headers):** still **small
and contained** — but the specific seam has **moved**. The prior pass named the byte-indexed text
mutators (`insert_utf8` / `delete_utf8` / `mark_utf8`) as "the one real seam." The updated loro-c
wrapper now surfaces those directly (loro.hpp:1207-1264), so **that seam is closed**. In its place,
the loro-c C++ wrapper is **missing five other APIs the app actually calls**. Every one is backed by
a C-ABI function already compiled into the prebuilt archive, so each is a few-line inline wrapper —
the same fix shape the utf8 trio already received. **The effort estimate (≈ a day) is unchanged**,
and the fix shape is now *proven* rather than projected.

> **Why this pass differs from the last.** The previous reassessment (also dated 2026-06-17) got the
> direction right but its "verified parity (no changes needed)" list was **not actually verified at
> the method level** — it was wrong about `EphemeralStore` and silent about three other gaps. This
> pass cross-checked every loro API the app calls against the loro-c headers (`loro.hpp`,
> `loro/loro_ext.hpp`) and confirmed each missing method's backing C symbol is present in
> `libloro_c_api.a` via `nm`. The findings below are method-level verified.

## The two packages share the same C++ surface (unchanged from last pass — and true)

| | `~/opt/loro` (current, loro-cpp) | `~/opt/loro-c` (updated) |
|---|---|---|
| What it is | uniffi-bindgen-cpp generated bindings | hand-written C++20 RAII wrapper over a generated C ABI (`loro/loro.h`) |
| Doc / Text / Map / List | `LoroDoc` / `LoroText` / `LoroMap` / `LoroList` | **same** names |
| Other types | `UndoManager`, `EphemeralStore`, `VersionVector`, `Frontiers`, `Cursor`, `ContainerId` | **same names** |
| Ownership | `std::shared_ptr<T>` via `T::init()` | **same** — `std::shared_ptr<T>` via `static T::init()` |
| ext helpers | `loro::ext::value_from/value_like/insert_container/subscribe_*/set_on_push/pop/root_*` | **same** — `loro_ext.hpp` is "ported near-verbatim from loro-cpp" (header comment); same `loro::ext` namespace the app's 202 call sites use |
| Errors | throws | throws `LoroError` (app already guards loro calls) |
| Includes | `<loro.hpp>`, `<loro/loro_ext.hpp>` | **same** paths |
| CMake | `find_package(loro CONFIG REQUIRED)` → `loro::loro` | **same** package + `loro::loro` INTERFACE target (loroConfig.cmake:54, loro-targets.cmake:59) |
| Distribution | `libloro.a` + `libloro_cpp_rs.a`; needs uniffi-bindgen-cpp to rebuild | single prebuilt `libloro_c_api.a` (40 MB, 452 exported `loro_*` symbols); no uniffi toolchain |

The class-rename and shared-vs-move-ownership blockers from the *original* (first) analysis remain
**gone**: names are identical and the wrapper is `shared_ptr`-based, so `LoroDocText`'s
`std::shared_ptr<loro::LoroDoc>` / `std::shared_ptr<loro::UndoManager>` members compile unchanged.

## The actual seam now: five app-used APIs the loro-c wrapper doesn't expose

Each is **present in the C ABI** (`loro/loro.h`) and **defined in the prebuilt archive** (verified
with `nm libloro_c_api.a` — all show as `T`), but is **not wrapped** in loro-c's `loro.hpp` C++
layer. loro-cpp wraps all five, which is why the app compiles today.

| # | Missing C++ method/type (loro-c) | loro-cpp shape (what the app expects) | App call sites | Backing C symbol (in archive ✓) |
|---|---|---|---|---|
| 1 | `EphemeralStore::get_all_states()` | `std::unordered_map<std::string, LoroValue>` (loro-cpp loro.hpp:1157) | editor_controller.cpp:4945, 4997 | `loro_ephemeral_store_get_all_states` — emits JSON object `{"<key>": <value>}` |
| 2 | `EphemeralStore::remove_outdated()` | `void` (loro-cpp loro.hpp:1159) | editor_controller.cpp:5024 | `loro_ephemeral_store_remove_outdated` |
| 3 | `LoroDoc::oplog_vv()` | `std::shared_ptr<VersionVector>` (loro-cpp loro.hpp:2066) | app_controller.cpp:3121, 4571; peer_inspector_view_{win,linux,mac}.cpp | `loro_doc_oplog_vv` |
| 4 | `LoroText::convert_pos(pos, from, to)` | `std::optional<uint32_t>` (loro-cpp loro.hpp:2702) | loro_doc_text.cpp:83, 125 | `loro_text_convert_pos` |
| 5 | `loro::PosType` enum (`kBytes`/`kUnicode`/`kUtf16`) | enum (loro-cpp loro.hpp:4781) | loro_doc_text.cpp:83-84, 126-127 | C enum `LoroPosType` (`LORO_POS_BYTES=0`, …) |

Notes:
- **#1 / #2 — the previous pass got these backwards.** It listed `get_all_states` / `remove_outdated`
  as "verified parity, no changes needed," and its spot-check note called `get_all_states()`
  "`unordered_map<uint64_t, PeerInfo>`." That signature belongs to loro-c's **`Awareness`** struct
  (loro.hpp:3437) — the legacy presence type the app **does not use** (zero `loro::Awareness` call
  sites). The app uses **`EphemeralStore`**, whose loro-c wrapper (loro.hpp:3294-3354) exposes
  `set/get/keys/delete_/encode_all/apply/subscribe` but **not** `get_all_states` / `remove_outdated`.
  The app needs the **string-keyed, `LoroValue`-valued** EphemeralStore variant; the wrapper for it
  must parse the C ABI's JSON object (the header already has `detail::parse_json` / `json_to_value`,
  used by `to_delta`).
- **#4 / #5 are a pair** — `convert_pos` takes `PosType` args; both are absent. The app uses them in
  `LoroDocText`'s undo cursor round-trip to convert byte offsets ↔ codepoint indices before/after
  building a loro `Cursor` (cursors are codepoint-addressed; the app's model is byte-addressed).

## These five must be fixed in loro-c's header — but it's a pure header edit

The previous pass offered "a project-local header" as a fix option. For these five that is **not
possible**: they are methods on the wrapper structs (`EphemeralStore`, `LoroDoc`, `LoroText`) and the
C-ABI calls need each struct's **private `raw_` handle**, which app code can't reach. So the fix
**belongs in loro-c's `loro.hpp`** — exactly where the now-present `insert_utf8` / `state_vv`
wrappers live.

That is cheap because:
- The backing C functions are **already in the prebuilt `libloro_c_api.a`** (verified: all four
  `loro_*` symbols resolve as `T`). **No Rust toolchain, no `uniffi`, no archive rebuild.**
- Each addition is a few inline lines mirroring an existing wrapper (`remove_outdated` ≈
  `state_vv`'s one-liner; `get_all_states` ≈ the `to_delta` JSON-parse idiom; `convert_pos` is a
  thin `loro_text_convert_pos` call returning `std::optional`; `PosType` is a 3-value `enum class`
  + a `to_c` mapping).
- `~/opt/loro-c` is a locally-maintained prefix, so editing its header is in-scope, not an upstream
  PR dependency.

(Alternatively, all five could be upstreamed into loro-c's `loro.hpp`/`loro_ext.hpp` properly — same
code, just contributed back. Either way the app source needs **no** change at these call sites once
the methods exist, because the names/shapes match loro-cpp.)

## Verified parity — present in loro-c, app needs no change (this pass, method-level)

- **Construction:** `LoroDoc::init()`, `UndoManager::init(doc)`, `EphemeralStore::init(ms)`.
- **Persistence / sync:** `export_snapshot`, `export_snapshot_at`, `export_updates(from_vv)`,
  `import`, `import_with`, `import_batch`, `import_json_updates`, `export_json_updates`,
  **`state_vv`** (but **not** `oplog_vv` — gap #3).
- **UndoManager:** `record_new_checkpoint`, `set_on_push`, `set_on_pop`, `can_undo/redo`,
  `undo/redo`, `set_merge_interval`, `add_exclude_origin_prefix`, `group_start/end`.
- **EphemeralStore:** `set`, `get`, `keys`, `delete_`, `encode_all`, `apply`, `subscribe`
  (gaps: `get_all_states`, `remove_outdated`).
- **Text:** `insert`, **`insert_utf8`**, `delete_`, **`delete_utf8`**, `splice`, `slice`,
  **`mark_utf8`**, `unmark`, `to_delta`, `len_utf8` / `len_unicode` / `len_utf16`, `get_cursor`,
  `id`, `is_attached` (gap: `convert_pos` + `PosType`). The byte-indexed trio that was the *prior*
  pass's blocker is now wrapped (loro.hpp:1211, 1221, 1258).
- **Containers / values:** `LoroMap`, `LoroList`, `ValueOrContainer`, `get_text`/`get_map`/`get_list`
  (same `ContainerIdLike` signature), and the `loro::ext` helpers (`value_from`, `value_like`,
  `value_as_*`, `root_*`, `insert_container<T>`, `get_or_create_container<T>`, `subscribe_*`,
  `on_local_update`, `set_on_push/pop`).
- **Container attachment model** matches: `T::init()` returns a **detached** container, attached by
  passing it to a parent's `insert_*_container` — same as loro-cpp, same as the app's
  `ext::insert_container<LoroText>(...)` / `get_text(...)` usage. Low risk; confirm at the spike.

## Build switch is still trivial

The app already does `find_package(loro CONFIG REQUIRED)` and links `loro::loro` (CMakeLists.txt:179,
410). loro-c exports the **same** package and the **same** `loro::loro` INTERFACE target (which pulls
in `loro_c_api-static` plus the baked-in system libs). **No CMake code changes** — only repoint the
search prefix:

```bash
cmake -S . -B build -Dloro_DIR=$HOME/opt/loro-c/lib/cmake/loro
# (or -DCMAKE_PREFIX_PATH=$HOME/opt/loro-c)
```

Include paths line up (`<loro.hpp>`, `<loro/loro_ext.hpp>` exist in both). The app never includes the
old `loro_scaffolding.hpp` (uniffi internals), so nothing is lost.

## Residual risks (small, localized)

- **The five wrapper additions** (above) are the bulk of the work. The byte/codepoint correctness
  that the prior pass worried about is now split: the `_utf8` mutators are already byte-correct in the
  wrapper; `convert_pos` must map `PosType` → the right `LoroPosType` enum value. Cover with a
  non-ASCII (emoji/CJK) text test — `test_loro_smoke` / `test_editor_controller` are the place.
- **Signature-shape spot-checks** beyond the five: the `to_delta()` / `TextDelta` shape and
  `get_cursor`/`Cursor` round-trip are present but worth a compile-time check. Any divergence is a
  local adapter, not a rewrite.
- **Exceptions** — both libraries throw; the codebase already guards loro calls
  (see `reference_loro_throws_clone_kinds`). Unchanged, not a new hazard.

## Revised effort

**Still roughly a day**, now dominated by adding the **five wrapper methods/types to loro-c's
`loro.hpp`** (not an app-side shim) and running the `test_loro_*` suite. The reshape moved the seam
from the utf8 trio to this set of five, but the *shape and size* of the work are the same — thin
inline wrappers over C-ABI functions already in the prebuilt archive.

1. Repoint the build at `~/opt/loro-c` (one CMake variable). Compile `test_loro_smoke`.
2. Add the five wrappers to loro-c's `loro.hpp`:
   `EphemeralStore::get_all_states` / `::remove_outdated`, `LoroDoc::oplog_vv`,
   `LoroText::convert_pos`, and `enum class PosType` (+ its `to_c` mapping). All backing C symbols
   are present — no archive rebuild.
3. Fix any further signature-shape divergences the compiler surfaces (expected few).
4. Build everything; run `ctest --test-dir build`, with a non-ASCII text test guarding the
   byte/codepoint paths.

The spike (steps 1-2) will confirm this in well under a day: with names and ownership aligned, the
remaining compiler errors should reduce to exactly these five undefined members plus a handful of
signature tweaks.
