# Plan: make `loro-c`'s C++ API a faithful `loro-cpp` drop-in (replace `loro.hpp` in place)

## Context

An app (~1,150 references across ~70 files; heavy `LoroMap`/`LoroText` use) currently
builds against **loro-cpp** (uniffi-bindgen-cpp output at `../loro-cpp/`) and wants to
switch to **loro-c** (this repo) to drop the uniffi toolchain and ship a single prebuilt
static archive. `pm/UPVERT.md` concludes the two are *different APIs* sharing only the
`loro` namespace name, so a swap today is a multi-week, ~1,150-site rewrite.

Decision (chosen): **rewrite `include/loro/loro.hpp` in place** so loro-c's C++ wrapper
*becomes* the loro-cpp-shaped API — the same names, `shared_ptr` ownership, typed
`LoroValue`, callback interfaces, and `loro::ext` helpers. There is no parallel `compat/`
header. Because there is then only one set of `loro::` types, the in-namespace collision
problem disappears along with all the machinery that would have managed it. The C ABI
(`loro.h`) and the Rust crate stay as the foundation, extended only additively.

This is a **breaking change** to loro-c's current clean API (deliberately accepted at
v0.1.0): loro-c's own `examples/` (6) and `tests/` (13) are rewritten to the new shape,
and any current downstream user of `loro::Doc` must migrate. Goal: an app TU that compiles
against loro-cpp today compiles **and behaves identically** against the new loro-c, with no
value-type corruption.

## The one hard truth that shapes the design

**The JSON value bridge is provably lossy** — verified against loro's serde
(`loro-common-1.13.1/src/value.rs`): `Binary` serializes to a JSON array and comes back as
a `List` (binary destroyed); integer-valued doubles (`2.0`) collapse to `I64`;
container-vs-string relies on a `cid:` magic prefix. loro-cpp's typed `LoroValue`
distinguishes all of these, and the app + conformance tests depend on it. So the new
`loro.hpp` cannot route values through loro-c's existing JSON `loro_map_insert(key,json)`
path — it needs a **typed-value C ABI** so values cross the boundary without JSON.

(The current clean wrapper's `value.rs` comment claiming `LoroValue` "round-trips cleanly
through serde_json" is true for inspection but **false** for faithful typed reconstruction —
binary, integer-doubles, and cid-prefixed strings are the counterexamples.)

## Recommended approach

Rewrite the public C++ header to the loro-cpp shape, add the small typed-value surface to
the Rust crate, and migrate loro-c's own examples/tests onto the new API.

### Files

```
include/loro/loro.h                  C ABI (cbindgen) — extended additively, see below
include/loro/loro.hpp                REWRITTEN: loro-cpp-shaped API in namespace loro
include/loro/loro_ext.hpp            NEW: loro::ext helpers (port of ../loro-cpp/include/loro/loro_ext.hpp)
loro-c-api/src/value_typed.rs        NEW: opaque typed LoroValue surface (no JSON)
loro-c-api/src/value_or_container.rs EXTEND: typed as_value()
loro-c-api/src/diff_typed.rs         NEW: typed diff-walker (owned ValueOrContainers)
loro-c-api/src/doc.rs / undo.rs      EXTEND: ImportStatus return, typed get_deep_value, undo cursor channel (if needed)
examples/*, tests/*                  REWRITTEN to the new API (largely adopted from ../loro-cpp/{examples,tests})
CMakeLists.txt, tests/CMakeLists.txt update install + test wiring
```

Glue (`detail::check`, `detail::Bytes`, the `extern "C"` callback trampolines) stays in
`loro::detail` exactly as today — there is no second namespace to worry about. Header
include spellings should **match loro-cpp's installed layout** (confirm against
`../loro-cpp` install — e.g. `<loro.hpp>` for the main header, `<loro/loro_ext.hpp>` for
ext) so the app changes only its CMake package / include dir, not its `#include` lines or
its `loro::LoroDoc` call sites.

### What the new `loro.hpp` reproduces (exact signatures from `../loro-cpp/build/generated/loro.hpp`)

- **Reference types, all `shared_ptr`-held, `init()`-constructed.** `LoroDoc::init()`,
  `LoroText/Map/List/MovableList/Tree/Counter::init()`,
  `UndoManager::init(shared_ptr<LoroDoc>)`, `EphemeralStore::init(int64)`,
  `VersionVector::init()/decode(bytes)`, `Frontiers`, `Cursor`, `Subscription`,
  `DiffBatch`, `Configure`, `ValueOrContainer`. Each wraps a raw C handle with a
  `loro_*_free` deleter; `init()` = `make_shared`. **Validated:** loro-c handles are strong
  co-owners that outlive their Doc and free in any order
  ([loro.hpp:12-17](include/loro/loro.hpp#L12)), so `make_shared` child handles are
  independently valid with no doc back-pointer — maps cleanly onto loro-cpp's `shared_ptr`
  model.
- **Typed `LoroValue`** = `std::variant<kNull,kBool,kDouble,kI64,kBinary,kString,kList,kMap,
  kContainer>` (member names verbatim), `LoroValueLike` interface, `ValueOrContainer` with
  `as_value()/is_container()/as_loro_text()/...`. Built on `value_typed.rs`, not JSON.
- **`ContainerId`** variant `{kRoot{name,ContainerType}, kNormal{peer,counter,ContainerType}}`
  + `ContainerIdLike` interface; converts to/from loro-c's `"cid:..."` strings C++-side.
- **Callback interfaces** as `shared_ptr<AbstractBase>`: `Subscriber{on_diff}`,
  `OnPush`/`OnPop`, `EphemeralSubscriber`, `LocalUpdateCallback`, `PreCommitCallback`,
  `ChangeAncestorsTraveler`, etc.; `subscribe(...) -> shared_ptr<Subscription>` with
  `unsubscribe()`/`detach()`.
- **`loro::ext` (`loro_ext.hpp`)**: port `../loro-cpp/include/loro/loro_ext.hpp`
  near-verbatim — `on_diff`/`on_undo_push`/... adapters, `value_from`/`value_like_from`/
  `value_as_*`, `root`/`root_text`/..., templated `insert_container<T>`, `Result<T>`/
  `try_call`, `subscribe(doc,cid,lambda)` free functions. Pure C++ over the new header.
- **`LoroError : std::runtime_error`** with loro-cpp's variant subclasses; map loro-c's
  `LoroStatus` onto the right subclass (the app may `catch` specific ones).

### uniffi member-name quirks — reproduce exactly

`delete_()`, `Index::kSeq` (not `seq`), `ContainerType::kUnknown{uint8}`,
`ListDiffItem::kDelete{delete_}`, etc. Diff the generated header field-by-field; don't
eyeball, or app code won't compile.

### loro-c extras with no loro-cpp equivalent

The current clean API has features loro-cpp's surface lacks (type-erased `Container`,
`ensure_mergeable_*`, some G6 attribution helpers). Replacing the header means conforming to
loro-cpp; these are **dropped** unless a specific one is worth keeping as an additive
`loro::ext` extension. Decide per-feature during Phase 2; default is drop.

## Phasing (build to de-risk; gate each on loro-cpp's tests)

- **Phase 0 — Spike (0.5–1d).** Stub the new `loro.hpp` with just
  `LoroDoc::init/get_text/insert/to_string` + minimal `LoroValue`, and compile
  `../loro-cpp/tests/test_smoke.cpp` + `test_text.cpp` against it. Proves the shape and the
  build wiring before investing.
- **Phase 1 — Typed value core (2–4d, the de-risker).** `value_typed.rs` + `LoroValue`/
  `LoroValueLike`/`value_like` + `ValueOrContainer::as_value`. Gate: `test_map`,
  `test_ephemeral_store`, `test_ext` value helpers. Do not proceed until solid — everything
  downstream reuses it.
- **Phase 2 — Containers + accessors (2–3d).** All six container types, `insert_*_container`,
  `get_or_create_*`, typed `get_deep_value`, `keys/values`, `ContainerId`↔cid-string,
  `root`/`ContainerIdLike`. Decide the fate of loro-c-only extras here. Gate:
  `test_text/list/movable_list/tree/counter/doc`, `test_ext`.
- **Phase 3 — Subscriptions (3–7d, long pole).** Ship the **envelope** first
  (`triggered_by`/`origin`/`current_target` + per-container `target`/`path`/`is_unknown`/
  `kind`), which is all `test_subscriptions` checks. Then, **gated on an audit of the app's
  `on_diff` handlers**, add typed `Diff` payloads (`diff_typed.rs`: list/text/map/tree items
  with owned `ValueOrContainer`s). loro-cpp's `DiffEvent` is an owned struct whose handles
  outlive the callback (the app stashes `current_target`), which is why this cannot reuse
  loro-c's callback-scoped diff views ([loro.hpp:1254](include/loro/loro.hpp#L1254)).
- **Phase 4 — Undo + commit hooks (1–2d).** `UndoManager` on_push/on_pop with **empty**
  `UndoItemMeta.cursors` (passes `test_undo`); real `ImportStatus` return. Add the undo
  cursor channel only if the app's on_push populates cursors.
- **Phase 5 — Versions/frontiers/cursors/jsonpath/fractional index (2–3d).** Mostly
  shape-translation; loro-c already has the primitives. Gate: `test_version_vector`,
  `test_fractional_index`, `test_jsonpath`.
- **Phase 6 — Awareness (0–2d, conditional).** Only if the app uses `Awareness` over
  `EphemeralStore` (both libs mark it legacy). Gate: `test_awareness`.
- **Phase 7 — Migrate loro-c's own examples/ + tests/. (done)** The spike header was promoted
  in place: `conformance/include/loro.hpp` → `include/loro.hpp` and
  `conformance/include/loro/loro_ext.hpp` → `include/loro/loro_ext.hpp` (installed as `<loro.hpp>`
  / `<loro/loro_ext.hpp>`), the old clean-API `include/loro/loro.hpp` was deleted, and `conformance/`
  + the `LORO_BUILD_CONFORMANCE` option were retired. `tests/` now holds the self-contained
  loro-cpp-shaped suite (loro-cpp's 16 `test_*.cpp` + `test_helpers.hpp`, copied in, plus the
  loro-c-authored `test_value_fidelity` and `test_cursor`, plus the pure-C `test_c_only`). `examples/`
  adopts loro-cpp's three and ports loro-c's `rich_text` / `cursor_tracking` / `json_sync` to the new
  API. This is loro-c's permanent test suite.

**Explicit stubs (documented; throw `LoroError` if unexpectedly hit):**
`UndoItemMeta.cursors` empty until proven needed; typed `Diff` payloads behind the envelope;
any loro-cpp method with zero app references.

## Risks to keep front-of-mind

- **#1 silent value drift, not compile errors.** `binary→list` and `2.0→i64` compile clean
  and pass shallow tests but corrupt data — why Phase 1 (typed ABI) is mandatory.
- **DiffEvent payload consumption** is the schedule's largest variance — audit the app's
  RenderTree `on_diff` handlers early to learn whether the envelope suffices.
- **Handle identity:** loro-c returns a *fresh* `LoroMap*` per `get_map`, so two facade
  handles to the same root have different raw pointers. Grep the app for `==` on handles or
  handles used as keys in pointer-keyed containers.
- **Subscription threading:** loro-c callbacks may fire from any mutating thread and must be
  reentrant ([loro.hpp:19-21](include/loro/loro.hpp#L19)); confirm the app doesn't assume
  main-thread delivery.
- **Error subclass identity:** map `LoroStatus` → the specific `LoroError` subclass the app
  catches, or those `catch` blocks silently miss.

## Critical files

- loro-c: [include/loro/loro.hpp](include/loro/loro.hpp) (the rewrite target),
  [include/loro/loro.h](include/loro/loro.h), `loro-c-api/src/value.rs`,
  `loro-c-api/src/value_or_container.rs`, `loro-c-api/src/container/{map,list}.rs`,
  `CMakeLists.txt`, `tests/CMakeLists.txt`.
- loro-cpp (the contract): `../loro-cpp/build/generated/loro.hpp`,
  `../loro-cpp/include/loro/loro_ext.hpp`, `../loro-cpp/tests/*.cpp`,
  `../loro-cpp/examples/*.cpp`.

## Verification

1. **Conformance gate:** a CMake target compiles loro-cpp's ~17 `tests/*.cpp` against the
   new `loro.hpp` and runs them under CTest. Green = signature + behavior match. (These also
   seed loro-c's own rewritten test suite in Phase 7.)
2. **Fidelity tests (the conformance suite's blind spots):** add four targeted tests —
   `kDouble{2.0}` round-trip, `kBinary` round-trip, `ImportStatus.success/pending`,
   `UndoItemMeta.cursors` — since loro-cpp's own tests only check `3.5`, empty cursors, and
   "import throws".
3. **App spike:** point the app's build `loro_DIR` at the new loro-c and compile one
   representative TU (the `LoroDocText` class + a RenderTree walker) — covers idioms the
   loro-cpp tests don't (the app's 1,150 call sites).
4. **Header-regeneration guard:** the existing CI step that regenerates `loro.h` via cbindgen
   and fails on diff must stay green after the typed-value/diff/import C ABI additions.
