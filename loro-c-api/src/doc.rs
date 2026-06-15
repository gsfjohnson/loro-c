//! `LoroDoc` lifecycle and document-level operations for M1: create / fork / free,
//! commit, peer id, snapshot & update export/import, whole-document JSON, and obtaining
//! the root `LoroText` container.

use crate::container::counter::LoroCounter;
use crate::container::list::LoroList;
use crate::container::map::LoroMap;
use crate::container::movable_list::LoroMovableList;
use crate::container::text::LoroText;
use crate::container::tree::LoroTree;
use crate::error::{record_encode_error, record_loro_error, set_last_error, LoroStatus};
use crate::value::LoroBytes;
use crate::version::{LoroChangeMetaOwned, LoroFrontiers, LoroId, LoroIdSpan, LoroVersionVector};
use loro::{ContainerID, ExportMode};
use std::cmp::Ordering;
use std::os::raw::c_char;

/// Opaque handle to a Loro document. Create with [`loro_doc_new`], release with
/// [`loro_doc_free`].
pub struct LoroDoc(loro::LoroDoc);

impl LoroDoc {
    pub(crate) fn inner(&self) -> &loro::LoroDoc {
        &self.0
    }
}

/// Interprets `(ptr, len)` as a UTF-8 string slice, recording an error and returning
/// `None` on a null pointer or invalid UTF-8.
fn str_from_raw<'a>(ptr: *const c_char, len: usize) -> Option<&'a str> {
    if ptr.is_null() && len != 0 {
        set_last_error("null string pointer passed to loro-c-api");
        return None;
    }
    let bytes = if len == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(ptr as *const u8, len) }
    };
    match std::str::from_utf8(bytes) {
        Ok(s) => Some(s),
        Err(_) => {
            set_last_error("argument is not valid UTF-8");
            None
        }
    }
}

/// Creates a new, empty document. Never returns null except on allocation failure /
/// caught panic. Release with [`loro_doc_free`].
#[no_mangle]
pub extern "C" fn loro_doc_new() -> *mut LoroDoc {
    ffi_guard!(std::ptr::null_mut(), {
        Box::into_raw(Box::new(LoroDoc(loro::LoroDoc::new())))
    })
}

/// Frees a document handle. Passing null is a no-op.
///
/// Container handles (e.g. `LoroText*`) obtained from this document are strong
/// co-owners of the underlying document state and remain valid after the `LoroDoc*`
/// is freed — they may be freed in any order. See `loro.h`/`loro.hpp` for details.
#[no_mangle]
pub extern "C" fn loro_doc_free(doc: *mut LoroDoc) {
    ffi_guard!((), {
        if !doc.is_null() {
            unsafe {
                drop(Box::from_raw(doc));
            }
        }
    });
}

/// Creates a deep fork of the document at its current version. Returns a new owned
/// handle, or null on error. Release with [`loro_doc_free`].
#[no_mangle]
pub extern "C" fn loro_doc_fork(doc: *const LoroDoc) -> *mut LoroDoc {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroDoc(doc.inner().fork())))
    })
}

/// Commits the pending operations into the oplog, making them exportable and triggering
/// any subscriptions.
#[no_mangle]
pub extern "C" fn loro_doc_commit(doc: *mut LoroDoc) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().commit();
        LoroStatus::LORO_OK
    })
}

/// Returns the document's current peer id. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_doc_peer_id(doc: *const LoroDoc) -> u64 {
    ffi_guard!(0u64, {
        let doc = deref_or!(doc, 0u64);
        doc.inner().peer_id()
    })
}

/// Sets the document's peer id. Fails if there are uncommitted local changes under the
/// previous peer id.
#[no_mangle]
pub extern "C" fn loro_doc_set_peer_id(doc: *mut LoroDoc, peer: u64) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        match doc.inner().set_peer_id(peer) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

// ---- G6.1: config & timestamps ----

/// Opaque handle to a document's [`loro::Configure`]. Obtain with [`loro_doc_config`],
/// release with [`loro_configure_free`].
///
/// `Configure` is cheap to clone and clones **share** the underlying configuration (it is
/// backed by atomics behind `Arc`). The handle returned by [`loro_doc_config`] therefore
/// reflects the document's live config: changes made through it affect the document, and
/// changes made to the document (e.g. via [`loro_doc_set_record_timestamp`]) are visible
/// through it.
pub struct LoroConfigure(loro::Configure);

impl LoroConfigure {
    pub(crate) fn inner(&self) -> &loro::Configure {
        &self.0
    }
}

/// Returns a handle to the document's live configuration. The handle shares state with the
/// document (see [`LoroConfigure`]). Returns null on a null handle. Release the returned
/// handle with [`loro_configure_free`].
#[no_mangle]
pub extern "C" fn loro_doc_config(doc: *const LoroDoc) -> *mut LoroConfigure {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroConfigure(doc.inner().config().clone())))
    })
}

/// Frees a configuration handle. Passing null is a no-op. Does not affect the document the
/// handle was obtained from.
#[no_mangle]
pub extern "C" fn loro_configure_free(config: *mut LoroConfigure) {
    ffi_guard!((), {
        if !config.is_null() {
            unsafe {
                drop(Box::from_raw(config));
            }
        }
    });
}

/// Returns whether commits record a wall-clock timestamp. Returns `false` on a null handle.
#[no_mangle]
pub extern "C" fn loro_configure_record_timestamp(config: *const LoroConfigure) -> bool {
    ffi_guard!(false, {
        let config = deref_or!(config, false);
        config.inner().record_timestamp()
    })
}

/// Sets whether commits record a wall-clock timestamp. Takes effect on the shared
/// configuration (interior mutability). No-op on a null handle.
#[no_mangle]
pub extern "C" fn loro_configure_set_record_timestamp(config: *const LoroConfigure, record: bool) {
    ffi_guard!((), {
        let config = deref_or!(config, ());
        config.inner().set_record_timestamp(record);
    });
}

/// Returns the change-merge interval in seconds. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_configure_merge_interval(config: *const LoroConfigure) -> i64 {
    ffi_guard!(0i64, {
        let config = deref_or!(config, 0i64);
        config.inner().merge_interval()
    })
}

/// Sets the change-merge interval in seconds. Takes effect on the shared configuration
/// (interior mutability). No-op on a null handle.
#[no_mangle]
pub extern "C" fn loro_configure_set_merge_interval(config: *const LoroConfigure, interval: i64) {
    ffi_guard!((), {
        let config = deref_or!(config, ());
        config.inner().set_merge_interval(interval);
    });
}

/// Convenience shortcut for `loro_doc_config` + `loro_configure_set_record_timestamp`: sets
/// whether commits record a wall-clock timestamp. No-op on a null handle.
#[no_mangle]
pub extern "C" fn loro_doc_set_record_timestamp(doc: *const LoroDoc, record: bool) {
    ffi_guard!((), {
        let doc = deref_or!(doc, ());
        doc.inner().set_record_timestamp(record);
    });
}

/// Convenience shortcut for `loro_doc_config` + `loro_configure_set_merge_interval`: sets the
/// change-merge interval in seconds. No-op on a null handle.
#[no_mangle]
pub extern "C" fn loro_doc_set_change_merge_interval(doc: *const LoroDoc, interval: i64) {
    ffi_guard!((), {
        let doc = deref_or!(doc, ());
        doc.inner().set_change_merge_interval(interval);
    });
}

/// Returns the root text container named `(id, id_len)` (UTF-8, not nul-terminated),
/// creating it if it does not yet exist. Returns null on error. Release the returned
/// handle with `loro_text_free`.
#[no_mangle]
pub extern "C" fn loro_doc_get_text(
    doc: *const LoroDoc,
    id: *const c_char,
    id_len: usize,
) -> *mut LoroText {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let name = match str_from_raw(id, id_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let text = doc.inner().get_text(name);
        Box::into_raw(Box::new(LoroText::from_inner(text)))
    })
}

/// Returns the root map container named `(id, id_len)`, creating it if it does not yet
/// exist. Returns null on error. Release the returned handle with `loro_map_free`.
#[no_mangle]
pub extern "C" fn loro_doc_get_map(
    doc: *const LoroDoc,
    id: *const c_char,
    id_len: usize,
) -> *mut LoroMap {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let name = match str_from_raw(id, id_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let map = doc.inner().get_map(name);
        Box::into_raw(Box::new(LoroMap::from_inner(map)))
    })
}

/// Returns the root list container named `(id, id_len)`, creating it if it does not yet
/// exist. Returns null on error. Release the returned handle with `loro_list_free`.
#[no_mangle]
pub extern "C" fn loro_doc_get_list(
    doc: *const LoroDoc,
    id: *const c_char,
    id_len: usize,
) -> *mut LoroList {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let name = match str_from_raw(id, id_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let list = doc.inner().get_list(name);
        Box::into_raw(Box::new(LoroList::from_inner(list)))
    })
}

/// Returns the root movable-list container named `(id, id_len)`, creating it if it does
/// not yet exist. Returns null on error. Release the returned handle with
/// `loro_movable_list_free`.
#[no_mangle]
pub extern "C" fn loro_doc_get_movable_list(
    doc: *const LoroDoc,
    id: *const c_char,
    id_len: usize,
) -> *mut LoroMovableList {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let name = match str_from_raw(id, id_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let list = doc.inner().get_movable_list(name);
        Box::into_raw(Box::new(LoroMovableList::from_inner(list)))
    })
}

/// Returns the root tree container named `(id, id_len)`, creating it if it does not yet
/// exist. Returns null on error. Release the returned handle with `loro_tree_free`.
#[no_mangle]
pub extern "C" fn loro_doc_get_tree(
    doc: *const LoroDoc,
    id: *const c_char,
    id_len: usize,
) -> *mut LoroTree {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let name = match str_from_raw(id, id_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let tree = doc.inner().get_tree(name);
        Box::into_raw(Box::new(LoroTree::from_inner(tree)))
    })
}

/// Returns the root counter container named `(id, id_len)`, creating it if it does not yet
/// exist. Returns null on error. Release the returned handle with `loro_counter_free`.
#[no_mangle]
pub extern "C" fn loro_doc_get_counter(
    doc: *const LoroDoc,
    id: *const c_char,
    id_len: usize,
) -> *mut LoroCounter {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let name = match str_from_raw(id, id_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let counter = doc.inner().get_counter(name);
        Box::into_raw(Box::new(LoroCounter::from_inner(counter)))
    })
}

/// Exports a full snapshot (complete history + current state) into `*out`.
/// `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_export_snapshot(doc: *const LoroDoc, out: *mut LoroBytes) -> LoroStatus {
    export_with(doc, out, ExportMode::snapshot())
}

/// Exports all updates (the complete operation history, no state snapshot) into `*out`.
/// `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_export_updates(doc: *const LoroDoc, out: *mut LoroBytes) -> LoroStatus {
    export_with(doc, out, ExportMode::all_updates())
}

fn export_with(doc: *const LoroDoc, out: *mut LoroBytes, mode: ExportMode<'_>) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match doc.inner().export(mode) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => record_encode_error(&e),
        }
    })
}

/// Imports a snapshot or updates blob `(data, len)` produced by an export function,
/// merging it into the document.
#[no_mangle]
pub extern "C" fn loro_doc_import(
    doc: *mut LoroDoc,
    data: *const u8,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if data.is_null() && len != 0 {
            set_last_error("null data pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        match doc.inner().import(bytes) {
            Ok(_) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Serializes the entire document state (resolving nested containers) to JSON bytes into
/// `*out`. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_get_deep_value_json(
    doc: *const LoroDoc,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let value = doc.inner().get_deep_value();
        match serde_json::to_vec(&value) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize document to JSON: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

// ---------------------------------------------------------------------------
// G3 — JSON-update sync
//
// JSON updates are the peer-portable, human-readable interchange format used to interop with
// JS / other-language peers that don't speak the binary encoding. Exports marshal the loro
// `JsonSchema` / `JsonChange` types to JSON bytes via serde; imports parse a JSON string. As
// with the binary `loro_doc_import`, the returned `ImportStatus` (success / pending ranges) is
// not surfaced — fallible imports report only a `LoroStatus`.
// ---------------------------------------------------------------------------

/// Imports JSON-format updates `(json, len)` (as produced by [`loro_doc_export_json_updates`]
/// or another peer) into the document. `json` must be valid UTF-8 JSON in loro's update schema.
#[no_mangle]
pub extern "C" fn loro_doc_import_json_updates(
    doc: *mut LoroDoc,
    json: *const c_char,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let s = match str_from_raw(json, len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match doc.inner().import_json_updates(s) {
            Ok(_) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Shared body for the two JSON-update export variants (peer compression on/off).
fn export_json_updates_impl(
    doc: *const LoroDoc,
    start_vv: *const LoroVersionVector,
    end_vv: *const LoroVersionVector,
    out: *mut LoroBytes,
    compress: bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let start = deref_or!(start_vv, LoroStatus::LORO_ERR_INVALID_ARG);
        let end = deref_or!(end_vv, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let schema = if compress {
            doc.inner()
                .export_json_updates(start.inner(), end.inner())
        } else {
            doc.inner()
                .export_json_updates_without_peer_compression(start.inner(), end.inner())
        };
        match serde_json::to_vec(&schema) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize JSON updates: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

/// Exports the ops in the range `(start_vv, end_vv]` as JSON bytes into `*out`. `*out` is only
/// written on `LORO_OK`; free it with `loro_bytes_free`. Use an empty version vector for
/// `start_vv` to export the full history.
#[no_mangle]
pub extern "C" fn loro_doc_export_json_updates(
    doc: *const LoroDoc,
    start_vv: *const LoroVersionVector,
    end_vv: *const LoroVersionVector,
    out: *mut LoroBytes,
) -> LoroStatus {
    export_json_updates_impl(doc, start_vv, end_vv, out, true)
}

/// Like [`loro_doc_export_json_updates`] but without peer-id compression, so the JSON carries
/// full peer ids — easier for application code to read and process at the cost of size.
#[no_mangle]
pub extern "C" fn loro_doc_export_json_updates_without_peer_compression(
    doc: *const LoroDoc,
    start_vv: *const LoroVersionVector,
    end_vv: *const LoroVersionVector,
    out: *mut LoroBytes,
) -> LoroStatus {
    export_json_updates_impl(doc, start_vv, end_vv, out, false)
}

/// Exports the changes within the single id span `span` as a JSON array of changes into `*out`.
/// Deterministic output (suitable for hashing); can include pending uncommitted changes. `*out`
/// is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_export_json_in_id_span(
    doc: *const LoroDoc,
    span: LoroIdSpan,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let changes = doc.inner().export_json_in_id_span(span.to_loro());
        match serde_json::to_vec(&changes) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize id-span changes: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

// ---------------------------------------------------------------------------
// G3 — Export modes / shallow & batch import
//
// Per-mode helpers over `LoroDoc::export(ExportMode)` for the variants not covered by the thin
// `loro_doc_export_snapshot` / `_export_updates` wrappers. A runtime-selected `ExportMode`
// tagged union is intentionally deferred (see pm/GAPS_PLAN.md). All reuse the `export_with`
// helper above; the frontiers-taking modes deref their handle first.
// ---------------------------------------------------------------------------

/// Exports a shallow snapshot whose retained history starts at `frontiers` (older ops are
/// trimmed) into `*out`. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_export_shallow_snapshot(
    doc: *const LoroDoc,
    frontiers: *const LoroFrontiers,
    out: *mut LoroBytes,
) -> LoroStatus {
    let f = deref_or!(frontiers, LoroStatus::LORO_ERR_INVALID_ARG);
    export_with(doc, out, ExportMode::shallow_snapshot(f.inner()))
}

/// Exports a snapshot at the given `frontiers` — full history up to that version plus the state
/// there — into `*out`. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_export_snapshot_at(
    doc: *const LoroDoc,
    frontiers: *const LoroFrontiers,
    out: *mut LoroBytes,
) -> LoroStatus {
    let f = deref_or!(frontiers, LoroStatus::LORO_ERR_INVALID_ARG);
    export_with(doc, out, ExportMode::snapshot_at(f.inner()))
}

/// Exports a state-only snapshot (the state at `frontiers` with a minimal set of history) into
/// `*out`. Pass a null `frontiers` to use the latest version. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_export_state_only(
    doc: *const LoroDoc,
    frontiers: *const LoroFrontiers,
    out: *mut LoroBytes,
) -> LoroStatus {
    let f = unsafe { frontiers.as_ref() };
    export_with(doc, out, ExportMode::state_only(f.map(|f| f.inner())))
}

/// Exports only the ops in the given `count` id `spans` into `*out`. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_export_updates_in_range(
    doc: *const LoroDoc,
    spans: *const LoroIdSpan,
    count: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        if spans.is_null() && count != 0 {
            set_last_error("null spans pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let slice = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(spans, count) }
        };
        let id_spans: Vec<loro::IdSpan> = slice.iter().map(|s| s.to_loro()).collect();
        export_with(doc, out, ExportMode::updates_in_range(id_spans))
    })
}

/// Imports a batch of snapshot/update blobs at once. `datas[i]` / `lens[i]` describe the `i`th
/// blob, for `count` blobs; loro applies them in dependency order regardless of array order.
#[no_mangle]
pub extern "C" fn loro_doc_import_batch(
    doc: *mut LoroDoc,
    datas: *const *const u8,
    lens: *const usize,
    count: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if count != 0 && (datas.is_null() || lens.is_null()) {
            set_last_error("null datas/lens pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let ptrs = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(datas, count) }
        };
        let lengths = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(lens, count) }
        };
        let mut blobs: Vec<Vec<u8>> = Vec::with_capacity(count);
        for (i, &len) in lengths.iter().enumerate() {
            let ptr = ptrs[i];
            if ptr.is_null() && len != 0 {
                set_last_error("null blob pointer passed to loro_doc_import_batch");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
            let bytes = if len == 0 {
                Vec::new()
            } else {
                unsafe { std::slice::from_raw_parts(ptr, len) }.to_vec()
            };
            blobs.push(bytes);
        }
        match doc.inner().import_batch(&blobs) {
            Ok(_) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Imports a snapshot or updates blob `(data, len)` while attaching `(origin, origin_len)` as
/// the origin string on the resulting change event (handy for telemetry / event filtering).
#[no_mangle]
pub extern "C" fn loro_doc_import_with(
    doc: *mut LoroDoc,
    data: *const u8,
    len: usize,
    origin: *const c_char,
    origin_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if data.is_null() && len != 0 {
            set_last_error("null data pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let origin = match str_from_raw(origin, origin_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        match doc.inner().import_with(bytes, origin) {
            Ok(_) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

// ---------------------------------------------------------------------------
// G6.2 — Doc history & introspection
// ---------------------------------------------------------------------------

/// Parses a container-id string (e.g. `"cid:0@1:Text"`) into a [`ContainerID`], recording an
/// error and returning `None` on a null / invalid-UTF-8 pointer or an unparseable id.
fn cid_from_raw(cid: *const c_char, cid_len: usize) -> Option<ContainerID> {
    let s = str_from_raw(cid, cid_len)?;
    match ContainerID::try_from(s) {
        Ok(id) => Some(id),
        Err(_) => {
            set_last_error(format!("invalid container id: {s}"));
            None
        }
    }
}

/// Returns the total number of operations in the document's OpLog. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_doc_len_ops(doc: *const LoroDoc) -> usize {
    ffi_guard!(0usize, {
        let doc = deref_or!(doc, 0usize);
        doc.inner().len_ops()
    })
}

/// Returns the total number of changes in the document's OpLog. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_doc_len_changes(doc: *const LoroDoc) -> usize {
    ffi_guard!(0usize, {
        let doc = deref_or!(doc, 0usize);
        doc.inner().len_changes()
    })
}

/// Returns the number of operations in the pending (uncommitted) transaction. Returns 0 on a
/// null handle.
#[no_mangle]
pub extern "C" fn loro_doc_get_pending_txn_len(doc: *const LoroDoc) -> usize {
    ffi_guard!(0usize, {
        let doc = deref_or!(doc, 0usize);
        doc.inner().get_pending_txn_len()
    })
}

/// Returns whether the history cache (used to make checkout faster) is currently built.
/// Returns `false` on a null handle.
#[no_mangle]
pub extern "C" fn loro_doc_has_history_cache(doc: *const LoroDoc) -> bool {
    ffi_guard!(false, {
        let doc = deref_or!(doc, false);
        doc.inner().has_history_cache()
    })
}

/// Frees the history cache used for faster checkout. It is rebuilt automatically on demand.
#[no_mangle]
pub extern "C" fn loro_doc_free_history_cache(doc: *const LoroDoc) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().free_history_cache();
        LoroStatus::LORO_OK
    })
}

/// Frees the cached diff calculator used for checkout. It is rebuilt automatically on demand.
#[no_mangle]
pub extern "C" fn loro_doc_free_diff_calculator(doc: *const LoroDoc) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().free_diff_calculator();
        LoroStatus::LORO_OK
    })
}

/// Encodes all ops and the history cache into the document's kv store, freeing the memory used
/// by parsed ops.
#[no_mangle]
pub extern "C" fn loro_doc_compact_change_store(doc: *const LoroDoc) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().compact_change_store();
        LoroStatus::LORO_OK
    })
}

/// Sets whether empty root containers are hidden from `get_deep_value` results and snapshots.
#[no_mangle]
pub extern "C" fn loro_doc_set_hide_empty_root_containers(
    doc: *const LoroDoc,
    hide: bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().set_hide_empty_root_containers(hide);
        LoroStatus::LORO_OK
    })
}

/// Returns whether the document contains the given container (by its history or current state).
/// `cid` is a container-id string. Returns `false` on a null handle or unparseable id.
#[no_mangle]
pub extern "C" fn loro_doc_has_container(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
) -> bool {
    ffi_guard!(false, {
        let doc = deref_or!(doc, false);
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return false,
        };
        doc.inner().has_container(&cid)
    })
}

/// Deletes all content from a root container and hides it from the document. Only affects root
/// containers (those without a parent). `cid` is a container-id string.
#[no_mangle]
pub extern "C" fn loro_doc_delete_root_container(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        doc.inner().delete_root_container(cid);
        LoroStatus::LORO_OK
    })
}

/// Writes the path from the document root to the given container into `*out` as a JSON array of
/// `{"cid": string, "index": {...}}` steps. `*out` is only written on `LORO_OK`; free it with
/// `loro_bytes_free`. Returns `LORO_ERR_NOT_FOUND` if the container does not resolve.
#[no_mangle]
pub extern "C" fn loro_doc_get_path_to_container(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        let path = match doc.inner().get_path_to_container(&cid) {
            Some(p) => p,
            None => {
                set_last_error("no path to container (container not found)");
                return LoroStatus::LORO_ERR_NOT_FOUND;
            }
        };
        let arr: Vec<serde_json::Value> = path
            .iter()
            .map(|(cid, index)| {
                serde_json::json!({
                    "cid": cid.to_string(),
                    "index": crate::event::index_to_json(index),
                })
            })
            .collect();
        match serde_json::to_vec(&arr) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize path to JSON: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

/// Writes the container ids modified in the change range `[id, id+len)` into `*out` as a sorted
/// JSON array of container-id strings (sorted for deterministic output). This implicitly commits
/// the current transaction. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_get_changed_containers_in(
    doc: *const LoroDoc,
    id: LoroId,
    len: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let set = doc.inner().get_changed_containers_in(id.to_loro(), len);
        let mut cids: Vec<String> = set.iter().map(|c| c.to_string()).collect();
        cids.sort();
        match serde_json::to_vec(&cids) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize containers to JSON: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

/// Compares `frontiers` with the document's current OpLog version, writing `-1` (the doc is
/// behind / `frontiers` is not fully contained), `0` (equal), or `1` (the doc is ahead) into
/// `*out`. `*out` is only written on `LORO_OK`.
#[no_mangle]
pub extern "C" fn loro_doc_cmp_with_frontiers(
    doc: *const LoroDoc,
    frontiers: *const LoroFrontiers,
    out: *mut i32,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let frontiers = deref_or!(frontiers, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let ord = match doc.inner().cmp_with_frontiers(frontiers.inner()) {
            Ordering::Less => -1,
            Ordering::Equal => 0,
            Ordering::Greater => 1,
        };
        unsafe { out.write(ord) };
        LoroStatus::LORO_OK
    })
}

/// Returns a minimized equivalent of `frontiers` (the smallest set marking the same version), as
/// a new owned [`LoroFrontiers`]. Returns null if a frontier id is not included in this
/// document's history. Release with `loro_frontiers_free`.
#[no_mangle]
pub extern "C" fn loro_doc_minimize_frontiers(
    doc: *const LoroDoc,
    frontiers: *const LoroFrontiers,
) -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let frontiers = deref_or!(frontiers, std::ptr::null_mut());
        match doc.inner().minimize_frontiers(frontiers.inner()) {
            Ok(f) => Box::into_raw(Box::new(LoroFrontiers::from_inner(f))),
            Err(id) => {
                set_last_error(format!("frontier id not included in document: {id}"));
                std::ptr::null_mut()
            }
        }
    })
}

/// Forks the document at `frontiers`: the new document contains only the history up to that
/// version. Returns a new owned [`LoroDoc`], or null on error (e.g. an unknown frontier).
/// Release with `loro_doc_free`.
#[no_mangle]
pub extern "C" fn loro_doc_fork_at(
    doc: *const LoroDoc,
    frontiers: *const LoroFrontiers,
) -> *mut LoroDoc {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let frontiers = deref_or!(frontiers, std::ptr::null_mut());
        match doc.inner().fork_at(frontiers.inner()) {
            Ok(d) => Box::into_raw(Box::new(LoroDoc(d))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Looks up an existing text container by container-id string, returning a live handle or null
/// if it does not exist (or `cid` is unparseable). Release with `loro_text_free`.
#[no_mangle]
pub extern "C" fn loro_doc_try_get_text(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
) -> *mut LoroText {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match doc.inner().try_get_text(cid) {
            Some(t) => Box::into_raw(Box::new(LoroText::from_inner(t))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Looks up an existing map container by container-id string, returning a live handle or null
/// if it does not exist (or `cid` is unparseable). Release with `loro_map_free`.
#[no_mangle]
pub extern "C" fn loro_doc_try_get_map(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
) -> *mut LoroMap {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match doc.inner().try_get_map(cid) {
            Some(m) => Box::into_raw(Box::new(LoroMap::from_inner(m))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Looks up an existing list container by container-id string, returning a live handle or null
/// if it does not exist (or `cid` is unparseable). Release with `loro_list_free`.
#[no_mangle]
pub extern "C" fn loro_doc_try_get_list(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
) -> *mut LoroList {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match doc.inner().try_get_list(cid) {
            Some(l) => Box::into_raw(Box::new(LoroList::from_inner(l))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Looks up an existing movable-list container by container-id string, returning a live handle
/// or null if it does not exist (or `cid` is unparseable). Release with `loro_movable_list_free`.
#[no_mangle]
pub extern "C" fn loro_doc_try_get_movable_list(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
) -> *mut LoroMovableList {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match doc.inner().try_get_movable_list(cid) {
            Some(l) => Box::into_raw(Box::new(LoroMovableList::from_inner(l))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Looks up an existing tree container by container-id string, returning a live handle or null
/// if it does not exist (or `cid` is unparseable). Release with `loro_tree_free`.
#[no_mangle]
pub extern "C" fn loro_doc_try_get_tree(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
) -> *mut LoroTree {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match doc.inner().try_get_tree(cid) {
            Some(t) => Box::into_raw(Box::new(LoroTree::from_inner(t))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Looks up an existing counter container by container-id string, returning a live handle or
/// null if it does not exist (or `cid` is unparseable). Release with `loro_counter_free`.
#[no_mangle]
pub extern "C" fn loro_doc_try_get_counter(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
) -> *mut LoroCounter {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let cid = match cid_from_raw(cid, cid_len) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match doc.inner().try_get_counter(cid) {
            Some(c) => Box::into_raw(Box::new(LoroCounter::from_inner(c))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Looks up the change containing operation `id` and, on success, writes a new owned
/// [`LoroChangeMetaOwned`] handle into `*out`. Read it via `loro_change_meta_owned_as_ref` +
/// the `loro_change_meta_*` accessors; release it with `loro_change_meta_owned_free`. Returns
/// `LORO_ERR_NOT_FOUND` if no change contains `id`. `*out` is only written on `LORO_OK`.
#[no_mangle]
pub extern "C" fn loro_doc_get_change(
    doc: *const LoroDoc,
    id: LoroId,
    out: *mut *mut LoroChangeMetaOwned,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match doc.inner().get_change(id.to_loro()) {
            Some(meta) => {
                let boxed = Box::into_raw(Box::new(LoroChangeMetaOwned(meta)));
                unsafe { out.write(boxed) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("no change at the given id");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}
