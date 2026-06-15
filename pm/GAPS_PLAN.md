# Plan: Close the `loro-ffi` parity gaps (`loro-c`)

## Context

[CBINDGEN_PLAN.md](CBINDGEN_PLAN.md) (M1–M5) shipped a strong **core subset** of `loro-ffi`:
the document model, all six containers (basic ops), events/subscriptions, Awareness,
EphemeralStore, UndoManager, Frontiers, FractionalIndex, JSON Path, and commit hooks. That
plan's stated v1 goal was *"full parity with `loro-ffi`"*, and the README still claims it —
but an audit against the upstream [`loro.udl`](https://github.com/loro-dev/loro-ffi/blob/main/src/loro.udl)
shows the real coverage is roughly **70%**. `LoroDoc` alone exposes ~80 methods upstream vs
~40 here, and several flagship feature areas are entirely absent.

This plan stages the remaining work into milestones **G1–G6**, ordered by user impact. It
**supersedes the "full parity" wording** in [CBINDGEN_PLAN.md:9](CBINDGEN_PLAN.md#L9) and
[README.md:7](README.md#L7): step 0 below is to make those honest.

### Step 0 — correct the parity claim (do first, ~10 min)

- [README.md:7](README.md#L7): replace "full `loro-ffi` parity" with an accurate coverage
  statement ("core document model + all six containers + events + the advanced subsystems;
  rich text, cursors, JSON-update sync, diff/patch, and the doc-utility long tail are
  tracked in `pm/GAPS_PLAN.md`").
- [CBINDGEN_PLAN.md:9](CBINDGEN_PLAN.md#L9): note that v1 landed a subset; link here.

## Verified gap inventory

| Area | Upstream surface | Local | Milestone |
|---|---|---|---|
| **Rich text** | `mark`/`unmark`(+`_utf8`/`_utf16`), `get_richtext_value`, `to_delta`/`slice_delta`, `apply_delta`, `update`/`update_by_line`, `splice`, `slice`, `char_at`, `insert_utf16`/`delete_utf16`, `len_utf16`, `convert_pos`, `push_str`; doc `config_text_style`/`config_default_text_style` | insert/delete(+`_utf8`), len_unicode/utf8, is_empty, to_string | **G1** |
| **Cursors** | `Cursor` encode/decode, `get_cursor` (text/list/movable), `doc.get_cursor_pos` → `PosQueryResult`, `Side`, `PosType` | none | **G2** |
| **JSON-update sync** | `import_json_updates`, `export_json_updates`(+`_without_peer_compression`), `export_json_in_id_span`, `redact_json_updates` | none | **G3** |
| **Export modes / shallow** | unified `export(ExportMode)`, `export_shallow_snapshot`, `export_snapshot_at`, `export_state_only`, `export_updates_in_range`, `import_batch`, `import_with`, `is_shallow`, `shallow_since_vv` | only `export_snapshot`/`export_updates`/`export_updates_from`, `import` | **G3** |
| **Diff / patch** | `diff(a,b)`→`DiffBatch`, `apply_diff`, `revert_to` | read-only DiffEvents only | **G4** |
| **Value representation** | structured `LoroValue` enum + `ValueOrContainer` typed accessors; `get_by_path`/`get_by_str_path` | JSON-string marshalling (`*_to_json`, `get_deep_value_json`) | **G5** (decision) |
| **Doc utilities** | `config()`/`Configure`, `get_change`, `fork_at`, `cmp_with_frontiers`, `minimize_frontiers`, `len_ops`/`len_changes`, `has_container`, `get_path_to_container`, `get_changed_containers_in`, `get_pending_txn_len`, history-cache controls, `try_get_*`, `set_hide_empty_root_containers`, `delete_root_container` | none | **G6** |
| **Attribution** | map `get_last_editor`; movable `get_creator_at`/`get_last_mover_at`/`get_last_editor_at`; tree `get_last_move_id`/`parent`/`nodes`/`get_value_with_meta`/`disable_fractional_index` | none | **G6** |
| **Per-container** | `is_deleted`, `is_attached`/`get_attached`, `doc()`, `subscribe`, `values()`/`to_vec()` | none | **G6** |
| **VersionVector algebra** | `merge`, `diff`, `get_missing_span`, `intersect_span`, `extend_to_include_vv`, `try_update_last`, `set_end`, `to_hashmap` | encode/decode, get/set_last, includes_*, compare, to_frontiers | **G6** |
| **UndoManager extras** | `peer`, `top_undo_meta`/`top_redo_meta`, `top_undo_value`/`top_redo_value` | most of the rest | **G6** |

## Prerequisite check (before G1)

Confirm the pinned `loro = "=1.13.1"` ([loro-c-api/Cargo.toml](loro-c-api/Cargo.toml))
actually exposes each Rust API this plan calls (`mark`, `get_cursor`, `export(ExportMode)`,
`import_json_updates`, `diff`/`apply_diff`, `Configure`, the attribution getters). These all
exist in the 1.x line that `loro-ffi` itself targets, but the local pin must be re-verified;
bump the pin in lockstep with `loro-ffi`'s if any are gated behind a newer minor.

## Cross-cutting conventions (apply to every milestone)

These are already established by M1–M5 — reuse them, don't reinvent:

- **FFI shape.** Fallible fn → `LoroStatus`; owned output → `*mut LoroBytes` written only on
  `LORO_OK`; null/long-lived handles are opaque `Box<…>` raw pointers with a `loro_*_free`.
  Wrap every body in `ffi_guard!` and use `deref_or!` (see
  [container/text.rs](loro-c-api/src/container/text.rs)).
- **Values stay JSON.** Per the M1 decision ([value.rs](loro-c-api/src/value.rs)), `LoroValue`
  inputs/outputs cross as JSON via `value_from_json` / `value_to_json_bytes`. New compound
  types below (TextDelta, DiffBatch) also marshal as JSON for consistency — see each
  milestone. The structured-value question is deferred to **G5**.
- **Callbacks.** Reuse the uniform `{invoke, user_data, free_user_data}` triple and
  `CCallback<…>` from [callbacks.rs](loro-c-api/src/callbacks.rs); subscriptions return a
  `LoroSubscription*` that unsubscribes on free.
- **Header is generated + committed.** After Rust changes run `cmake --build build --target
  regenerate-header`, commit [include/loro/loro.h](include/loro/loro.h); the CI drift guard
  (`git diff --exit-code`) enforces it.
- **Two test surfaces.** Every new area gets coverage in both a C++ test and the pure-C
  [tests/test_c_only.c](tests/test_c_only.c) path, plus a C++ RAII wrapper in
  [include/loro/loro.hpp](include/loro/loro.hpp). Use the loro Rust crate's own integration
  tests as the behavior oracle.

---

## G1 — Rich text *(highest impact)*

Loro's headline feature; today the text container can't carry formatting at all.

**Rust — [container/text.rs](loro-c-api/src/container/text.rs):**

| New C function | Maps to | Marshalling |
|---|---|---|
| `loro_text_mark` / `_mark_utf8` / `_mark_utf16` | `LoroText::mark` | `(from,to,key,len,value_json,value_len)`; value via `value_from_json` |
| `loro_text_unmark` / `_unmark_utf16` | `LoroText::unmark` | `(from,to,key,len)` |
| `loro_text_get_richtext_value` | `get_richtext_value` | → `LoroBytes` JSON (delta-with-attributes array) |
| `loro_text_to_delta` | `to_delta` | → `LoroBytes` JSON array of `{retain/insert/delete, attributes?}` |
| `loro_text_apply_delta` | `apply_delta` | in: JSON delta array → parse to `Vec<TextDelta>` |
| `loro_text_update` / `_update_by_line` | `update` | `(s,len, UpdateOptions{timeout_ms, use_refined_diff})` as plain fields |
| `loro_text_splice` | `splice` | `(pos,len,s,slen)` → `LoroBytes` removed slice |
| `loro_text_slice` | `slice` | `(start,end)` → `LoroBytes` |
| `loro_text_char_at` | `char_at` | `(pos)` → `LoroBytes` (one UTF-8 char) |
| `loro_text_insert_utf16` / `_delete_utf16` / `_len_utf16` | corresponding | mirror existing utf8 fns |
| `loro_text_convert_pos` | `convert_pos` | `(index, PosType from, PosType to)` → `usize` out + status |
| `loro_text_push_str` | `push_str` | `(s,len)` |

**Doc-level style config** (needed for marks to round-trip expand semantics) —
[doc.rs](loro-c-api/src/doc.rs): `loro_doc_config_text_style` /
`loro_doc_config_default_text_style`, taking a `LoroStyleConfig { LoroExpandType expand }`
where `LoroExpandType ∈ {Before, After, Both, None}`. A `StyleConfigMap` is passed as
repeated `(key, LoroStyleConfig)` pairs or a small builder handle — prefer a tiny opaque
`LoroStyleConfigMap*` with `_new`/`_insert`/`_free` to avoid a variadic C signature.

**New enums (cbindgen-emitted):** `LoroExpandType`, `LoroPosType { Bytes, Unicode, Utf16 }`.

**C++ wrapper:** `Text::mark/unmark` (value via the existing JSON helper), `Text::to_delta()`
→ a parsed delta vector or raw JSON string, `Text::update`, `Text::splice`, utf16 overloads.

**Tests:** mark a range bold → `get_richtext_value` shows the attribute; insert inside vs at
the boundary of a mark exercises each `ExpandType`; concurrent marks on two docs merge;
`apply_delta` round-trips `to_delta`; pure-C mark/unmark path.

**Effort:** ~16 C functions + 2 enums + 1 small handle type. Largest milestone.

---

## G2 — Cursors *(second highest)*

Stable positions that survive concurrent edits — needed by any real editor integration.

**New file `loro-c-api/src/cursor.rs`** (register in [lib.rs](loro-c-api/src/lib.rs)):

| New C function | Maps to |
|---|---|
| `loro_cursor_free` | drop `Box<Cursor>` |
| `loro_cursor_encode` | `Cursor::encode` → `LoroBytes` |
| `loro_cursor_decode` | `Cursor::decode(bytes)` → `LoroCursor*` |
| `loro_text_get_cursor` / `loro_list_get_cursor` / `loro_movable_list_get_cursor` | `get_cursor(pos, Side)` → `LoroCursor*` (nullable) |
| `loro_doc_get_cursor_pos` | `get_cursor_pos(&cursor)` → `LoroPosQueryResult { side, abs_pos }` out-param + status (`CannotFindRelativePosition` → error) |

**New enum:** `LoroSide { Left, Middle, Right }`. `LoroCursor` is an opaque `Box<loro::cursor::Cursor>`.

**C++ wrapper:** `Cursor` RAII type with `encode()`/`Cursor::decode()`; `Text::get_cursor`,
`Doc::get_cursor_pos` returning a small `PosQueryResult` struct.

**Tests:** place cursor at pos 3, a remote peer inserts at pos 0, after merge
`get_cursor_pos` reports the shifted absolute position; encode→decode round-trip; deletion of
the cursor's anchor resolves to the configured `Side`.

**Effort:** ~7 C functions + 1 enum + 1 handle type. Self-contained.

---

## G3 — Sync surface: JSON updates + export modes

Two related sync gaps. JSON updates are how you interop with the JS/other-language peers
that don't speak the binary format; export modes give efficient/partial/shallow sync.

**Rust — [doc.rs](loro-c-api/src/doc.rs):**

*JSON updates:*
- `loro_doc_import_json_updates(doc, json, len)` → `LoroImportStatus` (or status + JSON
  out describing success/pending ranges).
- `loro_doc_export_json_updates(doc, start_vv, end_vv)` → `LoroBytes` JSON; plus
  `_without_peer_compression` variant.
- `loro_doc_export_json_in_id_span(doc, IdSpan)` → `LoroBytes` (JSON array).
- `loro_doc_redact_json_updates(doc, json, len, VersionRange)` → `LoroBytes`.

*Export modes / shallow:*
- `loro_doc_export(doc, LoroExportMode, out)` — a tagged C struct
  `LoroExportMode { kind; union { Frontiers* at; IdSpan* range; VersionVector* from; } }`,
  covering `Snapshot`/`Updates`/`ShallowSnapshot`/`SnapshotAt`/`StateOnly`/`UpdatesInRange`.
  Keep the existing thin `export_snapshot`/`export_updates` as convenience wrappers.
- Direct helpers (simpler than the union for common cases): `loro_doc_export_shallow_snapshot`,
  `_export_snapshot_at`, `_export_state_only`, `_export_updates_in_range`.
- `loro_doc_import_batch(doc, bytes_array, count)`, `loro_doc_import_with(doc, bytes, origin)`.
- `loro_doc_is_shallow`, `loro_doc_shallow_since_vv` → `LoroVersionVector*`.

**New types:** `LoroExportMode` (tagged union) + `LoroExportModeKind` enum; `LoroIdSpan`,
`LoroVersionRange` plain structs (mirror the existing `LoroId`/`LoroChangeMeta` style in the
header). Decide: union vs. one-function-per-mode — recommend shipping the per-mode helpers
first (no union ABI risk) and adding the union only if callers want runtime-selected modes.

**C++ wrapper:** `Doc::import_json` / `export_json`, `Doc::export(ExportMode)` with an
`ExportMode` variant type, shallow-snapshot helpers.

**Tests:** export_json_updates from doc A → import_json_updates into doc B → states match;
shallow snapshot then import into a fresh doc preserves current state but drops deep history
(`is_shallow` true, `shallow_since_vv` non-empty); `import_batch` of several updates.

**Effort:** ~12 C functions + a few POD structs. Medium.

---

## G4 — Diff / patch API ✅ *(landed)*

Programmatic diffing and time-travel beyond read-only events. Shipped in
[diff.rs](loro-c-api/src/diff.rs).

**Design change from the original sketch.** The plan first specced diffs as JSON in *both*
directions (`apply_diff(doc, diff_json, len)` parsing JSON → `DiffBatch`). That is not viable:
the pinned `loro` 1.13.1 `DiffBatch`/`Diff`/`ListDiffItem`/`MapDelta`/`TreeDiff` do **not** derive
serde `Deserialize`, and a diff can carry a *live* nested container (`ValueOrContainer::Container`)
that JSON flattens to a plain value and can't faithfully reconstruct — so a JSON-parsed
`apply_diff` would be lossy and require ~4 hand-written deserializers. G4 instead represents a
diff as an **opaque `LoroDiffBatch` handle** (mirroring upstream `loro-ffi`'s `DiffBatch` object),
which is lossless; JSON is kept for read-only inspection only.

**Rust — [diff.rs](loro-c-api/src/diff.rs):**
- `loro_doc_diff(doc, from, to, out: LoroDiffBatch**)` → status; writes a new opaque
  `LoroDiffBatch*` (`LORO_ERR_NOT_FOUND` if a frontiers is unknown).
- `loro_doc_apply_diff(doc, batch)` → status (batch cloned, not consumed).
- `loro_diff_batch_to_json(batch, out)` → `LoroBytes` JSON object keyed by container-id, for
  inspection — reuses the existing event diff codec (`diff_to_json`/`write_json` in
  [event.rs](loro-c-api/src/event.rs), now `pub(crate)`, plus a new `diff_batch_to_json`).
- `loro_doc_revert_to(doc, frontiers)` → status.
- `loro_diff_batch_free(batch)`.

**C++ wrapper:** `loro::DiffBatch` RAII type with `to_json()`; `Doc::diff(a,b)`,
`Doc::apply_diff(batch)`, `Doc::revert_to(frontiers)`.

**Tests:** [tests/test_diff.cpp](tests/test_diff.cpp) + `test_diff_c()` in
[tests/test_c_only.c](tests/test_c_only.c): edit doc, capture frontiers F0; edit more; `diff(F0,
now)` then `apply_diff` onto a clone of the F0 state reproduces `now`; a diff that *creates a
nested container* round-trips losslessly (the case JSON couldn't); `revert_to(F0)` returns content
to the F0 snapshot.

**Effort:** 5 C functions + 1 opaque handle, reusing the existing diff codec.

---

## G5 — Value navigation ✅ *(landed)*

The deepest divergence: upstream exposes a typed `LoroValue` enum + `ValueOrContainer` with
`as_loro_text()`-style accessors; we marshal everything as JSON strings. JSON round-trips
plain data fine, so this was **lower priority than G1–G4**. The one thing JSON genuinely
*cannot* express is "this value is a live nested container" — a JSON dump flattens it and
loses the handle. G5 closed exactly that gap, per the plan's recommendation, without porting
the full tagged-union `LoroValue`. Shipped in
[value_or_container.rs](loro-c-api/src/value_or_container.rs).

**Design decision (refines the original POD sketch).** The plan first sketched a POD
`LoroValueOrContainer { bool is_container; LoroContainerType type; }` returned alongside a live
handle. G5 instead ships `LoroValueOrContainer` as an **opaque handle + accessors**, mirroring
the existing [jsonpath.rs](loro-c-api/src/jsonpath.rs) `LoroJsonPathResults` pattern (which
solves the identical value-or-container problem losslessly) — the same kind of refinement G4
made when it dropped JSON in favour of an opaque `LoroDiffBatch`.

**Rust — [value_or_container.rs](loro-c-api/src/value_or_container.rs):**
- `loro_doc_get_by_path(doc, path: *const LoroPathComponent, count)` → `LoroValueOrContainer*`
  (null if a component is invalid UTF-8 or the path doesn't resolve). `LoroPathComponent` is a
  tagged POD (`LoroPathComponentKind ∈ {Key, Seq, Node}`) mirroring `loro::Index`, reusing the
  existing `LoroTreeID` POD for the tree-node case.
- `loro_doc_get_by_str_path(doc, path, len)` → `LoroValueOrContainer*` (null if unresolved).
- Accessors on the handle: `loro_value_or_container_is_container`, `_container_type`
  (→ `LoroContainerType`, reusing a factored `container_type_of` in
  [container/any.rs](loro-c-api/src/container/any.rs)), `_get_container`
  (→ a live `LoroContainer*`, the case JSON can't express), `_get_value_json`
  (→ deep-value JSON, like the jsonpath accessor), and `_free`.

**C++ wrapper:** `loro::ValueOrContainer` RAII type (`is_container()`/`container_type()`/
`container()` → `std::optional<Container>`/`value_json()`); `loro::PathComponent` builder
(`key`/`seq`/`node`); `Doc::get_by_str_path` / `Doc::get_by_path` returning
`std::optional<ValueOrContainer>`.

**Tests:** [tests/test_navigate.cpp](tests/test_navigate.cpp) + `test_get_by_path_c()` in
[tests/test_c_only.c](tests/test_c_only.c): navigate to a nested container, prove the handle is
*live* (a write through it shows on the parent doc after commit — the thing JSON can't
round-trip); navigate to a leaf value (`get_value_json`); reach the same node via explicit
`{Key, Seq}` path components; a non-resolving path returns null/`std::nullopt`.

A full structured `LoroValue` remains intentionally deferred (see the intentional-omissions
table); revisit only if profiling shows JSON (de)serialization is hot, as flagged in
[CBINDGEN_PLAN.md:274](CBINDGEN_PLAN.md#L274).

---

## G6 — Doc utilities, attribution & long tail

The breadth that takes `LoroDoc` from ~40 to ~80 methods and fills in per-container/version
accessors. Independent, low-risk, batchable. A full design pass against the pinned
`loro = "=1.13.1"` confirmed **every** underlying Rust method exists and refined the surface to
**~99 new C functions + 4 new types** — the original "~60" undercounted the per-container uniform
set (×6 containers) and `ensure_mergeable_*` (×6). The work is staged into the grouped sub-steps
**G6.1–G6.6** below; do them in order, **one commit per group**, following the per-group recipe at
the end of this section.

**New types (4):**

| Type | File | Kind | Note |
|---|---|---|---|
| `LoroConfigure(loro::Configure)` | [doc.rs](loro-c-api/src/doc.rs) | opaque, owned clone | `Configure` is `Clone` over `Arc<Atomic*>`; a clone **shares** the doc's live config (tracks later changes, not a snapshot). Free `loro_configure_free`. |
| `LoroChangeMetaOwned(loro::ChangeMeta)` | [version.rs](loro-c-api/src/version.rs) | opaque, owned | distinct from the borrowed callback-scoped `LoroChangeMeta`; ships `_free` + `loro_change_meta_owned_as_ref` that reinterprets to `*const LoroChangeMeta` so it reuses the existing 5 accessors. |
| `LoroCommitOptions` | [commit.rs](loro-c-api/src/commit.rs) | `#[repr(C)]` POD | `{origin*, origin_len, message*, message_len, timestamp:i64, has_timestamp:bool, immediate_renew:bool}`; a null string ptr means `None`. |
| `LoroTreeParentKind` | [container/tree.rs](loro-c-api/src/container/tree.rs) | `#[repr(C)]` enum | `{ROOT=0, DELETED=1, NODE=2, UNEXIST=3}` for `tree.parent()`. |

### G6.1 — Doc config & timestamps — [doc.rs](loro-c-api/src/doc.rs) (8 fns)

- [x] `loro_doc_config` → `*mut LoroConfigure`; `loro_configure_free`; `_record_timestamp`,
  `_set_record_timestamp`, `_merge_interval`, `_set_merge_interval` (setters take `*const` —
  interior mutability via `Arc<Atomic>`); doc-level shortcuts `loro_doc_set_record_timestamp(bool)`,
  `loro_doc_set_change_merge_interval(i64)`.

### G6.2 — Doc history & introspection — [doc.rs](loro-c-api/src/doc.rs) + [version.rs](loro-c-api/src/version.rs) (~24 fns)

- [x] Scalars: `loro_doc_len_ops`, `_len_changes`, `_get_pending_txn_len`,
  `_has_history_cache`(bool). Void→OK: `_free_history_cache`, `_free_diff_calculator`,
  `_compact_change_store`, `_set_hide_empty_root_containers(bool)`.
- [x] By container-id string: `_has_container`(bool), `_delete_root_container`.
- [x] JSON out: `_get_path_to_container` (`[{cid,index}]`, reuse the `index_to_json` shape in
  [event.rs](loro-c-api/src/event.rs)); `_get_changed_containers_in(LoroId,len)` (sorted
  `["cid:…"]` for deterministic output).
- [x] Out-param: `_cmp_with_frontiers` (`*i32`, −1/0/1; `cmp_with_frontiers` returns `Ordering`,
  not `Option`, so it never fails → always `LORO_OK`). Handle returns: `_minimize_frontiers`
  → `*mut LoroFrontiers` (null on Err), `_fork_at` → `*mut LoroDoc` (null on Err),
  `_try_get_{text,map,list,movable_list,tree,counter}` (nullable typed ptr, parse id string).
- [x] `_get_change(LoroId, out: **LoroChangeMetaOwned)` → `LoroStatus` (`NOT_FOUND` on `None`) +
  the `LoroChangeMetaOwned` type, its `_free`, and `_as_ref` accessor reuse.

### G6.3 — Doc method tail & commit options — [doc.rs](loro-c-api/src/doc.rs) + [commit.rs](loro-c-api/src/commit.rs) (9 fns)

- [ ] Doc: `loro_doc_attach`, `_detach` (void→OK; `is_detached`/`checkout`/`checkout_to_latest`
  already exist — do not duplicate); `_get_container(cid)` → `*mut LoroContainer` (type-erased,
  null on `None`); `_get_deep_value_with_id_json` (JSON); `_find_id_spans_between(from,to)` (JSON
  `{retreat,forward}` of `{peer:{start,end}}`).
- [ ] Commit: `LoroCommitOptions` POD + `loro_doc_commit_with(opts)`, `_set_next_commit_origin(str)`,
  `_set_next_commit_options(opts)`, `_clear_next_commit_options`. Keep the existing
  `set_next_commit_message`/`_timestamp`.

### G6.4 — Per-container uniform set & attribution — all six [container/](loro-c-api/src/container/) files (~36 fns)

- [ ] Uniform set ×6 (map/list/movable_list/text/tree/counter) via `loro::ContainerTrait`:
  `_is_deleted`(bool), `_is_attached`(bool), `_get_attached` → `*mut Self` (null on `None`),
  `_doc` → `*mut LoroDoc` (owned clone, null on `None`), `_subscribe(LoroSubscriber)` →
  `*mut LoroSubscription` (null when detached; reuse the shared subscription plumbing —
  cf. the existing `loro_doc_subscribe(doc, cid, …)` by-id path).
- [ ] Attribution (out-param `*mut u64` + `bool found`): map `_get_last_editor(key)`;
  movable_list `_get_creator_at`/`_get_last_mover_at`/`_get_last_editor_at(pos)`; text
  `_get_editor_at_unicode_pos(pos)`; tree `_get_last_move_id(LoroTreeID, out:*mut LoroId)`→bool.

### G6.5 — Tree extras & mergeable — [container/tree.rs](loro-c-api/src/container/tree.rs) + [container/map.rs](loro-c-api/src/container/map.rs) (12 fns)

- [ ] Tree: `LoroTreeParentKind` enum + `_parent(LoroTreeID, out_kind, out_node)`→bool;
  JSON-array bulk forms `_roots_json`, `_nodes_json`, `_children_json(parent*)` (TreeID
  `{peer,counter}`); `_get_value_with_meta_json` (JSON); `_disable_fractional_index`.
  (`get_value` is covered by the existing `loro_tree_to_json` — omit.)
- [ ] Map mergeable — upstream `ensure_mergeable_*` lives on **`LoroMap` only**, arg `key:&str`,
  returns the matching typed handle (null on Err):
  `_ensure_mergeable_{text,map,list,movable_list,tree,counter}`. (The plan's earlier
  "map/list/movable_list" was inaccurate — list/movable_list have no such method.) Distinct from
  the generic `*_insert_container(type)` path, so it is ported explicitly rather than collapsed.

### G6.6 — VersionVector algebra & UndoManager extras — [version.rs](loro-c-api/src/version.rs) + [undo.rs](loro-c-api/src/undo.rs) (10 fns)

- [ ] VV: `_merge(other)`, `_extend_to_include_vv(other)`, `_set_end(LoroId)` (void→OK);
  `_try_update_last(LoroId, out_updated:*mut bool)`; `_diff(other)` → JSON `{retreat,forward}`;
  `_get_missing_span(target)` → JSON `[{peer,counter_start,counter_end}]`;
  `_intersect_span(LoroIdSpan, out:*mut LoroCounterSpan)`→bool.
  (`to_hashmap` is covered by the existing `loro_version_vector_to_json` — omit.)
- [ ] Undo: `loro_undo_manager_peer`→u64; `_top_undo_value_json(out)`→bool,
  `_top_redo_value_json(out)`→bool. (`top_undo_meta`/`top_redo_meta` owned-meta handle deferred —
  its `cursors: Vec<CursorWithPos>` has no FFI form yet; see the omissions table.)

**Per-group recipe** (each sub-step, in order, on a feature branch — not `main`): add the Rust fns
→ **rebuild the staticlib by hand** (`cargo build` in `loro-c-api`; manual CMake mode won't rebuild
the `.a` on Rust edits) → `cmake --build build --target regenerate-header` and commit
[loro.h](include/loro/loro.h) (CI drift guard: `git diff --exit-code`) → add the C++ RAII wrappers
in [loro.hpp](include/loro/loro.hpp) → extend `tests/test_g6.cpp` + `test_g6_c()` in
[test_c_only.c](tests/test_c_only.c) (register in [tests/CMakeLists.txt](tests/CMakeLists.txt) like
the existing test targets) → build with the MSYS2 CLANG64 toolchain (gnullvm Rust host + clang +
Ninja) and run `ctest`.

**Effort:** ~99 small functions + 4 types across six groups. High count, low individual complexity;
one commit per group.

---

## Recommended ship order & rough sizing

| Order | Milestone | New C fns (approx) | Risk |
|---|---|---|---|
| 1 | **Step 0** — fix README/plan wording | 0 | none |
| 2 | **G1** — rich text | ~16 + enums/handle | medium (ExpandType semantics) |
| 3 | **G2** — cursors | ~7 + enum/handle | low (self-contained) |
| 4 | **G4** — diff/patch | ~3 (after G1 codec) | low |
| 5 | **G3** — JSON updates + export modes | ~12 + structs | medium (ExportMode union) |
| 6 | **G5** — value navigation ✅ landed | ~7 + handle/POD | low |
| 7 | **G6** — utility/attribution long tail (staged G6.1–G6.6) | ~99 | low, high volume |

Rationale for putting G4 before G3: G4 is tiny once G1 has factored out the shared delta/diff
JSON codec, and it unblocks time-travel demos; G3's `ExportMode` union is the only place with
real ABI-design risk, so it gets more bake time.

After each milestone: `regenerate-header`, commit `loro.h`, extend both the C++ and pure-C
test suites, and add an example where it demonstrates a headline capability (a rich-text
formatting example after G1; a cursor-tracking example after G2).

## Definition of done (parity)

Parity is reached when a diff of the committed [include/loro/loro.h](include/loro/loro.h)
against the upstream [`loro.udl`](https://github.com/loro-dev/loro-ffi/blob/main/src/loro.udl)
shows every interface method has a corresponding `loro_*` function (or a documented,
intentional omission — e.g. UniFFI-only scaffolding, or methods superseded by the JSON-value
design). Track residual intentional omissions in a short table appended here, so "parity"
stays a verifiable claim rather than a slogan.

### Intentional omissions (stub — confirm/extend as milestones land)

These upstream `loro.udl` interface methods are deliberately **not** mirrored 1:1. They are
"covered" for parity purposes by the listed local mechanism; the final `loro.h`-vs-`loro.udl`
diff-gate should treat them as expected absences, not gaps. Re-confirm each as the relevant
milestone lands.

| Upstream surface | Why omitted | Covered locally by |
|---|---|---|
| Typed container constructors: `get_or_create_{text,list,map,tree,movable_list,counter}_container`, `insert_*_container`, movable_list `set_*_container` (Map/List/MovableList) | Local API uses a generic type-discriminated path instead of one-fn-per-type | `loro_{map,list,movable_list}_insert_container(type, …)` / `_set_container` / `_get_container` (see [container/](loro-c-api/src/container/)). **Note:** `ensure_mergeable_*` is NOT covered by this — it is a distinct capability ported in G6. |
| `LoroValueLike`, `ContainerIdLike` | UniFFI trait-adapter scaffolding with no C ABI meaning | JSON marshalling for values ([value.rs](loro-c-api/src/value.rs)); `LoroContainerId` POD for container IDs |
| `DiffBatch` builder/inspect methods (`push`, `get_diff`) | A batch is produced by `diff()` and consumed by `apply_diff()`, not hand-assembled or iterated container-by-container across the C ABI | G4 ships `DiffBatch` as an opaque `LoroDiffBatch` handle ([diff.rs](loro-c-api/src/diff.rs)): `loro_doc_diff` → handle, `loro_doc_apply_diff`, `loro_diff_batch_to_json` (read-only inspection), `loro_diff_batch_free`. **Note:** JSON `apply_diff` was dropped — the upstream diff types aren't `Deserialize` and a diff can carry a live nested container JSON would lose; the opaque handle is lossless. Promote `push`/`get_diff` to real fns only if a caller must build or walk a batch by hand. |
| Callback interfaces (`Subscriber`, `LocalUpdateCallback`, `JsonPathSubscriber`, `Unsubscriber`, `OnPush`/`OnPop`, `PreCommitCallback`, `FirstCommitFromPeerCallback`, `ChangeAncestorsTraveler`, `LocalEphemeralListener`, `EphemeralSubscriber`) | UniFFI callback-interface types; not standalone C surface | The uniform `{invoke, user_data, free_user_data}` callback triple ([callbacks.rs](loro-c-api/src/callbacks.rs)) |
| `LoroDoc::check_state_correctness_slow` | Debug/test-only invariant checker | _(omit; not part of the public surface)_ — confirm before final sign-off |
| `LoroUnknown` (`id`) | Placeholder for unknown/forward-compat container types | _(omit unless a concrete need appears)_ |
| `FractionalIndex` constructors-only interface | No methods beyond construction | Existing `loro_fractional_index_*` constructors ([fractional_index.rs](loro-c-api/src/fractional_index.rs)) |
| `PosQueryResult.update` (the refreshed `Cursor?` returned by `get_cursor_pos`) | A caller-side optimization hint (swap in the refreshed cursor to avoid future history replay); not required to read a position. G2's `LoroPosQueryResult` POD carries only the `current` `AbsolutePosition` (`abs_pos` + `side`), per the plan. | `loro_doc_get_cursor_pos` ([cursor.rs](loro-c-api/src/cursor.rs)); recreate a cursor with `loro_*_get_cursor` when a refresh is wanted. Promote to a real field/out-param if a caller needs the perf optimization. |
| `LoroDoc::redact_json_updates` (+ the `VersionRange` POD it needed) | Not exposed on the public `loro` crate — redaction lives only in `loro-internal`'s `JsonSchema::redact`, and `loro-c` depends on `loro` alone. Dropped from G3 (this also removed the need for a `LoroVersionRange` POD). | _(deferred; promote back into G3 only if a caller needs redaction and adding the `loro-internal` dependency is accepted)_ |
| Runtime-selected `LoroExportMode` tagged union | G3 ships per-mode helper fns instead (no union ABI risk), per the plan's own recommendation ([GAPS_PLAN.md:170](pm/GAPS_PLAN.md#L170)). | `loro_doc_export_{shallow_snapshot,snapshot_at,state_only,updates_in_range}` plus the existing snapshot/updates wrappers ([doc.rs](loro-c-api/src/doc.rs)); add the union only if a caller must pick the mode at runtime. |
| `ImportStatus { success, pending }` returned by `import_json_updates`/`import_batch`/`import_with` (and binary `import`) | Not `Serialize`, and the existing binary `loro_doc_import` already discards it. | Imports return `LoroStatus` only ([doc.rs](loro-c-api/src/doc.rs)); surface the pending/success ranges as a JSON out-param if a caller needs dependency-gap detection. |
| Full structured `LoroValue` tagged union + `ValueOrContainer`'s `as_loro_text()`-style typed accessors | Large ABI surface; JSON marshalling already round-trips plain data. The only thing JSON loses — a live nested container — is recovered by the G5 navigation handle. | JSON value marshalling ([value.rs](loro-c-api/src/value.rs)) plus the opaque `LoroValueOrContainer` from G5 ([value_or_container.rs](loro-c-api/src/value_or_container.rs)): `is_container`/`container_type`/`get_container` (live handle) / `get_value_json`. Build the tagged union only if profiling shows JSON (de)serialization is hot ([CBINDGEN_PLAN.md:274](CBINDGEN_PLAN.md#L274)). |
| Full `VersionRange` range-algebra interface (`clear`/`get`/`insert`/`contains_ops_between`/`has_overlap_with`/`contains_id`/`contains_id_span`/`extends_to_include_id_span`/`is_empty`/`get_peers`/`get_all_ranges`) | No G6 consumer needs range algebra — ~11 fns + an opaque handle for zero callers; deferred per the plan's own gate. | _(deferred; promote into G6 only if a caller needs range algebra)_ |
| Container `values()` / list & movable_list `to_vec()` | Same contents the existing JSON serializers already emit at the JSON boundary | `loro_map_to_json` / `loro_list_to_json` / `loro_movable_list_to_json` ([container/](loro-c-api/src/container/)) |
| Tree `get_value` (plain) | Duplicate of the tree's existing JSON serialization | `loro_tree_to_json` ([container/tree.rs](loro-c-api/src/container/tree.rs)); G6.5 still adds `get_value_with_meta` (resolves node metadata — genuinely new) |
| `VersionVector::to_hashmap` | No such method upstream; the deref-to-map is already emitted as `{"peer":counter}` JSON | `loro_version_vector_to_json` ([version.rs](loro-c-api/src/version.rs)) |
| `UndoManager::top_undo_meta` / `top_redo_meta` (owned `UndoItemMeta` handle) | Its `cursors: Vec<CursorWithPos>` has no FFI representation yet; only `value` is expressible | G6.6 ships the value via `loro_undo_manager_top_{undo,redo}_value_json` ([undo.rs](loro-c-api/src/undo.rs)); promote an owned-meta handle only when a caller needs `cursors` |

If any row above turns out to have an actual caller need (e.g. `ensure_mergeable_*`, or
`VersionRange` range-algebra), promote it out of this table into the relevant milestone rather
than leaving it as a silent gap.
