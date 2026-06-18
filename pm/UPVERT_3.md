# Switching from loro-cpp (`~/opt/loro`) to loro-c (`~/opt/loro-c`)

**Question:** How much work would it be to use the `~/opt/loro-c` library instead of `~/opt/loro`
— i.e. switch the app from the loro **C++** bindings to the loro **C-ABI** package?

**Short answer (third pass, 2026-06-17 — first pass to actually compile):** still **small and
contained, ≈ a day**. This pass stopped reasoning from header-symbol greps and instead **compiled
the whole tree against loro-c** in a throwaway build dir. Two concrete results:

1. **The five gaps the *last* pass named are now closed.** The updated loro-c header added exactly
   the wrappers that pass predicted (`EphemeralStore::get_all_states` / `remove_outdated`,
   `LoroDoc::oplog_vv`, `LoroText::convert_pos`, `enum class PosType`) — verified at method,
   signature, *and* app-call-site level (see "Now closed" below).
2. **The build does not yet compile** — but for a **different set of nine** APIs the prior
   "verified parity" lists silently missed. These only surface when you compile, because they are
   methods called on type-inferred (`auto voc = …`) objects, `ext::` free-function **overloads**, and
   methods used only in the platform/test TUs — none of which a `loro::`-name grep can see. Every one
   is backed by a C symbol already in the prebuilt archive, so the fix shape is unchanged: thin header
   wrappers, **no Rust toolchain, no archive rebuild.**

> **Why every prior pass was wrong about the remaining work.** Passes one and two reasoned from
> *symbol presence* in the headers and produced "verified parity (no changes needed)" lists that were
> incomplete each time. The only reliable test is **compiling the app's actual call sites against the
> candidate header.** This pass did that with a full-tree sweep: configure + `cmake --build
> build-loroc -- -k 0`, which compiles **every** translation unit — `basu-core` (all of `src/core` +
> `src/controller`), the Windows app TUs (`src/win`), and every test — and `-k 0` reports the complete
> error set in one pass. (The `src/linux` / `src/mac` TUs are cross-compile-only and weren't built
> here, but a grep confirms they call the same loro methods as their Windows counterparts — see the
> table.) The gap list below is that complete error set.

## The two packages share the same C++ surface (confirmed, including by a clean configure)

| | `~/opt/loro` (current, loro-cpp) | `~/opt/loro-c` (updated) |
|---|---|---|
| What it is | uniffi-bindgen-cpp generated bindings | hand-written C++20 RAII wrapper over a generated C ABI (`loro/loro.h`) |
| Doc / Text / Map / List | `LoroDoc` / `LoroText` / `LoroMap` / `LoroList` | **same** names |
| Other types | `UndoManager`, `EphemeralStore`, `VersionVector`, `Frontiers`, `Cursor`, `ContainerId`, `ValueOrContainer`, `ContainerType` | **same names** (all present — verified) |
| Ownership | `std::shared_ptr<T>` via `T::init()` | **same** — `std::shared_ptr<T>` via `static T::init()` |
| ext helpers | `loro::ext::value_from/value_like/insert_container/subscribe_*/root_*` | **same** namespace; all 18 distinct `ext::` helpers the app calls **resolve by name** (the gap is two missing *overloads*, not missing helpers) |
| Errors | throws | throws `LoroError` (app already guards loro calls) |
| Includes | `<loro.hpp>`, `<loro/loro_ext.hpp>` | **same** paths; **`loro.hpp` now `#include`s `<unordered_map>` itself** (line 41) |
| CMake | `find_package(loro CONFIG REQUIRED)` → `loro::loro` | **same** package + `loro::loro` target — **`cmake -S . -B build-loroc -Dloro_DIR=…/loro-c/lib/cmake/loro` configured cleanly with zero CMake changes** |
| Distribution | `libloro.a` + `libloro_cpp_rs.a`; needs uniffi-bindgen-cpp to rebuild | single prebuilt `libloro_c_api.a`; no uniffi toolchain |

The class-rename and shared-vs-move-ownership blockers from the *original* analysis remain **gone**:
names are identical and the wrapper is `shared_ptr`-based, so `LoroDocText`'s
`std::shared_ptr<loro::LoroDoc>` / `…UndoManager` members compile unchanged.

## Now closed — last pass's "five gaps," verified present + matching the app's call sites

Each is present in the updated `loro.hpp` with the exact loro-cpp shape, and each app call site
compiles against it:

| API (loro-c) | Shape | App call sites — verified compiling |
|---|---|---|
| `EphemeralStore::get_all_states()` | `std::unordered_map<std::string, LoroValue>` (loro.hpp:3377) | editor_controller.cpp:4945, 4997 — consumed as `[string key, LoroValue value]` (the `Awareness` `unordered_map<uint64_t,…>` confusion the last pass made is gone) |
| `EphemeralStore::remove_outdated()` | `void` (loro.hpp:3370) | editor_controller.cpp:5024 |
| `LoroDoc::oplog_vv()` | `std::shared_ptr<VersionVector>` (loro.hpp:2696) | app_controller.cpp:3121, 4571 |
| `LoroText::convert_pos(pos, from, to)` | `std::optional<uint32_t>` (loro.hpp:1335) | loro_doc_text.cpp:83, 125 |
| `loro::PosType` (`kBytes`/`kUnicode`/`kUtf16`) | `enum class` + `to_c_pos_type` map (loro.hpp:140, 564) | loro_doc_text.cpp:83-84, 126-127 |

(The byte-indexed `insert_utf8` / `delete_utf8` / `mark_utf8` trio that pass *one* called the blocker
also remains wrapped. None of these need any further work.)

## The actual remaining seam: nine APIs the compiler flags as missing

Captured from the full-tree `cmake --build build-loroc -- -k 0` (complete error set). Grouped by which
TUs need each — the first five block `basu-core` (so they break **all** platforms); the next three are
the inspector views (win/linux/mac, identical usage); the last is a test.

**In `basu-core` (`src/core` + `src/controller`) — breaks every platform:**

| # | Missing in loro-c | Shape the app expects | App call sites | Backing C symbol (in archive) |
|---|---|---|---|---|
| 1 | `ValueOrContainer::container_type()` | `std::optional<ContainerType>` (checked via `->get_variant()` + `holds_alternative<ContainerType::kText>` …) | loro_schema.cpp:26, 37, 49, 60; loro_walk.cpp:60 | `loro_value_or_container_container_type` ✓ (non-destructive; source handle unaffected) |
| 2 | `ValueOrContainer::is_value()` | `bool` | loro_schema.cpp:18 | none needed — it is literally `!is_container()` |
| 3 | `LoroDoc::set_next_commit_origin(const std::string&)` | `void` | editor_controller.cpp:7124 | `loro_doc_set_next_commit_origin` ✓ |
| 4 | `loro::ext::subscribe(EphemeralStore&, fn)` **overload** | `std::shared_ptr<Subscription>` | editor_controller.cpp:4888 | `loro_ephemeral_store_subscribe` ✓ — and `EphemeralStore::subscribe(shared_ptr<EphemeralSubscriber>)` **already exists**; only the ext lambda-adapter overload is missing |
| 5 | `loro::ext::subscribe_local_update(EphemeralStore&, fn)` **overload** | `std::shared_ptr<Subscription>` | editor_controller.cpp:4898; app_controller.cpp:435 | `loro_ephemeral_store_subscribe_local_updates` ✓ — needs a new `EphemeralStore::subscribe_local_updates` method **and** the ext overload |

**In the inspector views (`src/win` built here; `src/linux` / `src/mac` grep-confirmed identical):**

| # | Missing in loro-c | Shape the app expects | App call sites | Backing C symbol (in archive) |
|---|---|---|---|---|
| 6 | `LoroDoc::len_ops()` | `uintptr_t` | loro_inspector_view_{win:527, linux:534, mac:361} | `loro_doc_len_ops` ✓ |
| 7 | `LoroDoc::is_detached()` | `bool` | loro_inspector_view_{win:528, linux:535, mac:362}; peer_inspector_view_{win:405, linux:404, mac:443} | `loro_doc_is_detached` ✓ |
| 8 | `LoroDoc::oplog_frontiers()` | `std::shared_ptr<Frontiers>` (used as `fr->to_vec()`) | peer_inspector_view_{win:384, linux:383, mac:422} | `loro_doc_oplog_frontiers` ✓ (mirror the existing `state_frontiers()` wrapper) |

**In the test suite:**

| # | Missing in loro-c | Shape the app expects | App call sites | Backing C symbol (in archive) |
|---|---|---|---|---|
| 9 | `LoroText::get_richtext_value()` | `LoroValue` (fed to `ext::value_to_string`) | test_loro_schema.cpp:502 | `loro_text_get_richtext_value` ✓ (JSON delta bytes → `LoroValue`, same idiom as `get_all_states` / `to_delta`) |

Why each prior pass missed these:

- **#1 / #2** are methods on `ValueOrContainer`, reached through `auto voc = m.get(key);` — the type
  name never appears at the call site, so it isn't in any `loro::`-symbol grep. loro-c's
  `ValueOrContainer` exposes `is_container` / `as_value` / `as_loro_*` but not `is_value` /
  `container_type`.
- **#3 / #6 / #7 / #8 / #9** are plain method-name misses on `LoroDoc` / `LoroText`. #6–9 additionally
  never entered earlier passes because they live in the platform/test TUs, which no pass compiled.
- **#4 / #5** are *overload* gaps. loro-c's `ext::subscribe` / `subscribe_local_update` exist — but
  only for `LoroDoc&` and the container types. The app passes an `EphemeralStore&` (errors read
  "no matching function" and "cannot bind `LoroDoc&` to `EphemeralStore`"). The presence-grep saw
  the name and called it parity.

## The fix is a header-only edit in loro-c — but it is the dependency, not the app

All nine live in `~/opt/loro-c`'s `loro.hpp` / `loro_ext.hpp`, and each C-ABI backing function is
**already compiled into `libloro_c_api.a`** — so **no Rust, no uniffi, no archive rebuild.** Six are
one-liners, one is a small JSON parse, and two (the ephemeral subscribe adapters) need a callback
trampoline that already has a twin in the header. The edits mirror wrappers already present:

- `ValueOrContainer::is_value()` → one-liner (`!is_container()`).
- `ValueOrContainer::container_type()` → call `loro_value_or_container_container_type`, map the
  `LoroContainerType` enum (`LORO_CONTAINER_TEXT=2`, …) to `ContainerType`'s variant, return
  `std::nullopt` for value/unknown.
- `LoroDoc::set_next_commit_origin()` → `detail::check(loro_doc_set_next_commit_origin(raw_, s.data(), s.size()))`.
- `LoroDoc::len_ops()` → one-liner returning `loro_doc_len_ops(raw_)`.
- `LoroDoc::is_detached()` → one-liner returning `loro_doc_is_detached(raw_)`.
- `LoroDoc::oplog_frontiers()` → copy the existing `state_frontiers()` wrapper, swapping in
  `loro_doc_oplog_frontiers`.
- `LoroText::get_richtext_value()` → call `loro_text_get_richtext_value`, parse the JSON delta bytes
  to a `LoroValue` (same `detail::parse_json` / `json_to_value` idiom as `get_all_states` / `to_delta`).
- `ext::subscribe(EphemeralStore&, fn)` → an `on_ephemeral(fn)` adapter (same pattern as the
  existing `on_local_update` / `on_diff`) feeding `EphemeralStore::subscribe`.
- `ext::subscribe_local_update(EphemeralStore&, fn)` → add `EphemeralStore::subscribe_local_updates`
  (reuses the existing `detail::LocalUpdateHolder` + `loro_conf_local_update_invoke/free`
  trampoline) and an ext overload over `on_local_update`.

> **Not done in this pass.** Applying these edits touches the `~/opt/loro-c` *install* (a dependency
> outside this repo), which is out of scope for "reassess the analysis" and was blocked by the
> sandbox. The wrappers are specified above; applying them + recompiling is the spike that would turn
> "compiles cleanly" from projected to proven. Either edit the local prefix or upstream them to
> loro-c — either way **the app source needs no change at any call site**, because the names/shapes
> match loro-cpp.

## Build switch verified

`find_package(loro CONFIG REQUIRED)` + `loro::loro` resolve from loro-c with **zero CMake changes** —
a fresh configure against `-Dloro_DIR=$HOME/opt/loro-c/lib/cmake/loro` succeeded and the full tree
compiled up to the nine gaps above. To switch, only repoint the prefix:

```bash
cmake -S . -B build -Dloro_DIR=$HOME/opt/loro-c/lib/cmake/loro
# (or -DCMAKE_PREFIX_PATH=$HOME/opt/loro-c)
```

Minor cleanup available, not required: loro-c's `loro.hpp` is self-contained for `std::unordered_map`
(includes it at line 41), so the `target_compile_options(basu-core PUBLIC -include unordered_map)`
workaround in CMakeLists.txt:448 — added for loro-cpp's non-self-contained generated header — can be
dropped after the switch.

## Residual risks (small, localized)

- **The nine wrapper additions** above are the bulk of the work, but seven are one-liners or a small
  parse. #4/#5 (the ephemeral subscribe adapters) are the most involved — they need callback
  trampolines — but loro-c already contains the exact pattern (the `EphemeralStore::subscribe` event
  trampoline and the `LoroDoc` local-update trampoline), so it is mirror-and-rename, not new design.
- **Coverage of this spike.** The full-tree `-k 0` build compiled `basu-core` (every `src/core` +
  `src/controller` loro call site), the Windows app TUs (`src/win`), and every test — surfacing all
  nine errors at once. Not exercised: the final **link** (can't, until the nine wrappers exist), and
  the `src/linux` / `src/mac` TUs (cross-compile-only on this machine). A grep confirms linux/mac call
  exactly the same loro methods as their Windows twins (#6–8), so no extra gaps are expected there,
  but the linux/mac builds should still be run after the fix to be sure.
- **byte/codepoint correctness** (`convert_pos` ↔ `PosType`) is now in-wrapper and present; still
  worth a non-ASCII (emoji/CJK) test — `test_loro_smoke` / `test_editor_controller`.
- **Exceptions** — both libraries throw; the codebase already guards loro calls
  (see `reference_loro_throws_clone_kinds`). Unchanged, not a new hazard.

## Revised effort

**Still roughly a day.** The reshape moved the seam again — from last pass's (now-closed) five to a
new nine — but the *shape and size* are the same: thin inline wrappers over C-ABI functions already
in the prebuilt archive (seven of the nine are one-liners or a small parse). The estimate is now
compile-grounded across the whole tree, not projected.

1. Repoint the build at `~/opt/loro-c` (one CMake variable). — **done in this pass; configures clean.**
2. Add the nine wrappers to loro-c's `loro.hpp` / `loro_ext.hpp` (list above). All backing C symbols
   present — no archive rebuild.
3. Build everything (full app + tests, all three platforms); fix any residual signature-shape
   divergences the compiler surfaces (none expected beyond the nine — linux/mac mirror Windows).
4. `ctest --test-dir build`, with a non-ASCII text test guarding the byte/codepoint paths.
5. Optional: drop the `-include unordered_map` workaround (CMakeLists.txt:448).

The remaining unknown is whether step 2's edits compile clean on the first try; given all nine backing
symbols are confirmed present and the wrapper patterns already exist in the header, the risk is low.
