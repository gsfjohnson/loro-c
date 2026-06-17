//! `LoroImportStatus` (RESHAPE Phase 4) — the success / pending span maps returned by an
//! import.
//!
//! `loro`'s import functions return an [`loro::ImportStatus`] describing, per peer, which op
//! counter spans were applied (`success`) and which are still waiting on missing dependencies
//! (`pending`). The plain `loro_doc_import*` functions discard this and report only a
//! [`LoroStatus`]; the `loro_doc_import_*_status` variants (see [`crate::doc`]) instead hand
//! back an owned [`LoroImportStatus`] handle built here. The loro-cpp-shaped C++ wrapper
//! (`loro::ImportStatus { unordered_map<uint64_t, CounterSpan> success; optional<...> pending; }`)
//! reads it through the indexed accessors below.
//!
//! Free the handle with [`loro_import_status_free`].

use crate::error::set_last_error;
use loro::{ImportStatus, VersionRange};

/// One entry of an import status span map: the op counters `[start, end)` applied for `peer`.
/// Mirrors a `(PeerID, CounterSpan)` pair.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LoroPeerCounterSpan {
    pub peer: u64,
    pub start: i32,
    pub end: i32,
}

/// Opaque, owned handle to an import's status (the `success` / `pending` span maps). Built by
/// the `loro_doc_import_*_status` functions; free with [`loro_import_status_free`].
pub struct LoroImportStatus {
    success: Vec<LoroPeerCounterSpan>,
    pending: Option<Vec<LoroPeerCounterSpan>>,
}

/// Flattens a [`VersionRange`] into a vector of `(peer, start, end)` entries (order is the
/// underlying hash-map order; the C++ side rebuilds an unordered_map, so order is irrelevant).
fn range_to_vec(range: &VersionRange) -> Vec<LoroPeerCounterSpan> {
    range
        .iter()
        .map(|(peer, (start, end))| LoroPeerCounterSpan {
            peer: *peer,
            start: *start,
            end: *end,
        })
        .collect()
}

/// Boxes a `loro::ImportStatus` into an owned `*mut LoroImportStatus`. Shared with
/// [`crate::doc`]'s `loro_doc_import_*_status` functions.
pub(crate) fn into_raw(status: ImportStatus) -> *mut LoroImportStatus {
    Box::into_raw(Box::new(LoroImportStatus {
        success: range_to_vec(&status.success),
        pending: status.pending.as_ref().map(range_to_vec),
    }))
}

/// Returns the number of peers in the `success` map. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_import_status_success_len(status: *const LoroImportStatus) -> usize {
    ffi_guard!(0usize, {
        let status = deref_or!(status, 0usize);
        status.success.len()
    })
}

/// Writes the `success` entry at `index` into `*out`. Returns `true` on success; returns
/// `false` (leaving `*out` untouched) on a null handle, a null `out`, or an out-of-range index.
#[no_mangle]
pub extern "C" fn loro_import_status_success_at(
    status: *const LoroImportStatus,
    index: usize,
    out: *mut LoroPeerCounterSpan,
) -> bool {
    ffi_guard!(false, {
        let status = deref_or!(status, false);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        match status.success.get(index) {
            Some(entry) => {
                unsafe { out.write(*entry) };
                true
            }
            None => {
                set_last_error("import status success index out of range");
                false
            }
        }
    })
}

/// Returns whether this import has a `pending` map (some ops are waiting on missing
/// dependencies). Returns `false` on a null handle.
#[no_mangle]
pub extern "C" fn loro_import_status_has_pending(status: *const LoroImportStatus) -> bool {
    ffi_guard!(false, {
        let status = deref_or!(status, false);
        status.pending.is_some()
    })
}

/// Returns the number of peers in the `pending` map, or 0 if there is no pending map or the
/// handle is null.
#[no_mangle]
pub extern "C" fn loro_import_status_pending_len(status: *const LoroImportStatus) -> usize {
    ffi_guard!(0usize, {
        let status = deref_or!(status, 0usize);
        status.pending.as_ref().map_or(0, |p| p.len())
    })
}

/// Writes the `pending` entry at `index` into `*out`. Returns `true` on success; returns
/// `false` (leaving `*out` untouched) on a null handle, a null `out`, no pending map, or an
/// out-of-range index.
#[no_mangle]
pub extern "C" fn loro_import_status_pending_at(
    status: *const LoroImportStatus,
    index: usize,
    out: *mut LoroPeerCounterSpan,
) -> bool {
    ffi_guard!(false, {
        let status = deref_or!(status, false);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        match status.pending.as_ref().and_then(|p| p.get(index)) {
            Some(entry) => {
                unsafe { out.write(*entry) };
                true
            }
            None => {
                set_last_error("import status pending index out of range");
                false
            }
        }
    })
}

/// Frees an import status handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_import_status_free(status: *mut LoroImportStatus) {
    ffi_guard!((), {
        if !status.is_null() {
            unsafe {
                drop(Box::from_raw(status));
            }
        }
    });
}
