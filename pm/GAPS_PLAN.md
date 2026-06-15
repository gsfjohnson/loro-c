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

## G4 — Diff / patch API

Programmatic diffing and time-travel beyond read-only events.

**Rust — [doc.rs](loro-c-api/src/doc.rs):**
- `loro_doc_diff(doc, from_frontiers, to_frontiers, out)` → `LoroBytes` JSON `DiffBatch`
  (reuse the diff→JSON marshalling already built for events in
  [event.rs](loro-c-api/src/event.rs)).
- `loro_doc_apply_diff(doc, diff_json, len)` → status (parse JSON → `DiffBatch`).
- `loro_doc_revert_to(doc, frontiers)` → status.

**Design note:** the event module already serializes `Diff`/`TextDelta`/`ListDiffItem`/
`MapDelta`/`TreeDiff` to JSON for subscribers. Factor that into a shared
`diff_to_json` / `diff_from_json` in [event.rs](loro-c-api/src/event.rs) so G4's
`apply_diff` and G1's `apply_delta` share one codec rather than duplicating it.

**C++ wrapper:** `Doc::diff(a,b)`, `Doc::apply_diff(diff)`, `Doc::revert_to(frontiers)`.

**Tests:** edit doc, capture frontiers F0; edit more; `diff(F0, now)` then `apply_diff` onto a
clone of the F0 state reproduces `now`; `revert_to(F0)` returns content to the F0 snapshot.

**Effort:** ~3 C functions, mostly reusing the existing diff codec. Small once G1 lands the
shared delta codec.

---

## G5 — Structured values *(decision point, not necessarily build)*

The deepest divergence: upstream exposes a typed `LoroValue` enum + `ValueOrContainer` with
`as_loro_text()`-style accessors; we marshal everything as JSON strings. JSON round-trips
plain data fine, so this is **lower priority than G1–G4**. The one thing JSON genuinely
*cannot* express is "this value is a live nested container" — a JSON dump flattens it and
loses the handle.

**Recommendation:** don't port the full tagged-union `LoroValue` (large ABI surface, the JSON
path already works). Instead close just the navigation gap:

- `loro_doc_get_by_path(doc, indices, count, out_value_or_container)` and
  `loro_doc_get_by_str_path(doc, path, len, …)` returning a small
  `LoroValueOrContainer { bool is_container; LoroContainerType type; }` plus, when it's a
  container, a `loro_*_get_container`-style live handle (the M1 plan already extracts
  containers from values this way — see [value.rs](loro-c-api/src/value.rs)).

Revisit a full structured `LoroValue` only if profiling shows JSON (de)serialization is hot,
exactly as flagged in [CBINDGEN_PLAN.md:274](CBINDGEN_PLAN.md#L274). Pull `get_by_path`
forward into G6 if a structured value isn't built.

---

## G6 — Doc utilities, attribution & long tail

The breadth that takes `LoroDoc` from ~40 to ~80 methods and fills in per-container/version
accessors. Independent, low-risk, batchable; do in any order after G1–G4.

- **Configure** — [doc.rs](loro-c-api/src/doc.rs): `loro_doc_config` → `LoroConfigure*`
  (opaque) with `record_timestamp`/`set_record_timestamp`, `merge_interval`/
  `set_merge_interval`; plus doc-level `set_record_timestamp`, `set_change_merge_interval`.
- **History / introspection:** `get_change`(→`LoroChangeMeta`), `len_ops`, `len_changes`,
  `get_pending_txn_len`, `has_container`, `get_path_to_container`,
  `get_changed_containers_in`, `cmp_with_frontiers`, `minimize_frontiers`, `fork_at`,
  `try_get_{text,map,list,movable_list,tree,counter}`, history-cache controls
  (`has_history_cache`/`free_history_cache`/`free_diff_calculator`/`compact_change_store`),
  `set_hide_empty_root_containers`, `delete_root_container`.
- **Doc method tail** (was unenumerated — these have no local equivalent today):
  `attach`/`detach` (we only ship `checkout`/`checkout_to_latest`/`is_detached`),
  `get_container` (generic by `ContainerID`, complementing the existing typed `get_*`),
  `get_deep_value_with_id`, `find_id_spans_between`.
- **Commit-options surface** — [commit.rs](loro-c-api/src/commit.rs): `commit_with`,
  `set_next_commit_origin`, `set_next_commit_options`, `clear_next_commit_options` (we only
  ship `set_next_commit_message`/`set_next_commit_timestamp`). Marshal `CommitOptions` as a
  small POD struct or opaque builder, consistent with the M5 commit-hook plumbing.
- **Attribution getters:** map `get_last_editor`; movable_list `get_creator_at`/
  `get_last_mover_at`/`get_last_editor_at`; text `get_editor_at_unicode_pos`; tree
  `get_last_move_id`/`parent`/`nodes`/`get_value_with_meta`/`disable_fractional_index`, plus
  the vec-returning `roots`/`children`/`get_value` forms (we only ship the
  `roots_len`/`root_at` and `children_len`/`child_at` index accessors today).
- **Per-container uniform set** (each of the six): `is_deleted`, `is_attached`,
  `get_attached`, `doc`, `subscribe`(reuse the callback triple), `values`/`to_vec`.
- **Mergeable containers** — map/list/movable_list `ensure_mergeable_{text,list,map,tree,`
  `movable_list,counter}`. Distinct capability from the generic `*_insert_container(type)`
  path (which supersedes the typed `get_or_create_*`/`insert_*`/`set_*_container` family — see
  the intentional-omissions table), so it must be ported explicitly rather than collapsed.
- **VersionVector algebra** — [version.rs](loro-c-api/src/version.rs): `merge`, `diff`,
  `get_missing_span`, `intersect_span`, `extend_to_include_vv`, `try_update_last`, `set_end`,
  `to_hashmap`.
- **VersionRange interface** — [version.rs](loro-c-api/src/version.rs): the upstream
  `VersionRange` is a full interface (`clear`, `get`, `insert`, `contains_ops_between`,
  `has_overlap_with`, `contains_id`, `contains_id_span`, `extends_to_include_id_span`,
  `is_empty`, `get_peers`, `get_all_ranges`), not just the POD struct G3 needs for
  `redact_json_updates`. Low priority — gate behind whether any caller needs range algebra;
  if not, move it to the intentional-omissions table instead of building it.
- **UndoManager extras** — [undo.rs](loro-c-api/src/undo.rs): `peer`, `top_undo_meta`/
  `top_redo_meta`, `top_undo_value`/`top_redo_value`.

**Effort:** ~60 small functions (up from the original ~40 once the previously-unenumerated
tail above is counted). High count, low individual complexity. Split across PRs by sub-bullet.

---

## Recommended ship order & rough sizing

| Order | Milestone | New C fns (approx) | Risk |
|---|---|---|---|
| 1 | **Step 0** — fix README/plan wording | 0 | none |
| 2 | **G1** — rich text | ~16 + enums/handle | medium (ExpandType semantics) |
| 3 | **G2** — cursors | ~7 + enum/handle | low (self-contained) |
| 4 | **G4** — diff/patch | ~3 (after G1 codec) | low |
| 5 | **G3** — JSON updates + export modes | ~12 + structs | medium (ExportMode union) |
| 6 | **G5** — value navigation (decision) | ~2 or defer | low |
| 7 | **G6** — utility/attribution long tail | ~60 | low, high volume |

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
| `DiffBatch` interface (`push`, `get_diff`) | UniFFI object wrapper around a diff collection | G4 marshals diffs as JSON (`diff`/`apply_diff`), per the shared codec decision |
| Callback interfaces (`Subscriber`, `LocalUpdateCallback`, `JsonPathSubscriber`, `Unsubscriber`, `OnPush`/`OnPop`, `PreCommitCallback`, `FirstCommitFromPeerCallback`, `ChangeAncestorsTraveler`, `LocalEphemeralListener`, `EphemeralSubscriber`) | UniFFI callback-interface types; not standalone C surface | The uniform `{invoke, user_data, free_user_data}` callback triple ([callbacks.rs](loro-c-api/src/callbacks.rs)) |
| `LoroDoc::check_state_correctness_slow` | Debug/test-only invariant checker | _(omit; not part of the public surface)_ — confirm before final sign-off |
| `LoroUnknown` (`id`) | Placeholder for unknown/forward-compat container types | _(omit unless a concrete need appears)_ |
| `FractionalIndex` constructors-only interface | No methods beyond construction | Existing `loro_fractional_index_*` constructors ([fractional_index.rs](loro-c-api/src/fractional_index.rs)) |

If any row above turns out to have an actual caller need (e.g. `ensure_mergeable_*`, or
`VersionRange` range-algebra), promote it out of this table into the relevant milestone rather
than leaving it as a silent gap.
