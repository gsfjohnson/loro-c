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
use crate::version::{LoroFrontiers, LoroIdSpan, LoroVersionVector};
use loro::ExportMode;
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
