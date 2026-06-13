//! `LoroDoc` lifecycle and document-level operations for M1: create / fork / free,
//! commit, peer id, snapshot & update export/import, whole-document JSON, and obtaining
//! the root `LoroText` container.

use crate::container::text::LoroText;
use crate::error::{record_encode_error, record_loro_error, set_last_error, LoroStatus};
use crate::value::LoroBytes;
use loro::ExportMode;
use std::os::raw::c_char;

/// Opaque handle to a Loro document. Create with [`loro_doc_new`], release with
/// [`loro_doc_free`].
pub struct LoroDoc(loro::LoroDoc);

impl LoroDoc {
    fn inner(&self) -> &loro::LoroDoc {
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
