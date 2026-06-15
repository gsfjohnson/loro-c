//! Diff / patch (G4): programmatic diffing and time-travel beyond read-only events.
//!
//! [`loro_doc_diff`] computes the [`LoroDiffBatch`] that transforms the document state at one
//! version into the state at another; [`loro_doc_apply_diff`] replays such a batch onto a
//! document; and [`loro_doc_revert_to`] rewinds a document to an earlier version (recording the
//! inverse as new ops, so history is preserved).
//!
//! A diff crosses the ABI as an **opaque `LoroDiffBatch*` handle** (an owned `Box`, freed with
//! [`loro_diff_batch_free`]) rather than as JSON. Upstream `Diff`/`DiffBatch` are not
//! `Deserialize`, and a diff can carry a *live* nested container that JSON would flatten and lose,
//! so a JSON round-trip would be lossy in the apply direction. For read-only inspection,
//! [`loro_diff_batch_to_json`] renders a batch as JSON via the shared event-diff codec.

use crate::doc::LoroDoc;
use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::event::{diff_batch_to_json, write_json};
use crate::value::LoroBytes;
use crate::version::LoroFrontiers;
use loro::event::DiffBatch;

/// Opaque handle to a [`loro::event::DiffBatch`] — a collection of per-container diffs produced
/// by [`loro_doc_diff`]. Free with [`loro_diff_batch_free`].
pub struct LoroDiffBatch(DiffBatch);

impl LoroDiffBatch {
    pub(crate) fn inner(&self) -> &DiffBatch {
        &self.0
    }
}

/// Frees a diff-batch handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_diff_batch_free(batch: *mut LoroDiffBatch) {
    ffi_guard!((), {
        if !batch.is_null() {
            unsafe {
                drop(Box::from_raw(batch));
            }
        }
    });
}

/// Computes the diff that turns the state at frontiers `from` into the state at frontiers `to`,
/// writing a new `LoroDiffBatch*` into `*out`. `*out` is only written on `LORO_OK`; release it
/// with [`loro_diff_batch_free`]. Returns `LORO_ERR_NOT_FOUND` if either frontiers references a
/// version the document does not contain.
#[no_mangle]
pub extern "C" fn loro_doc_diff(
    doc: *const LoroDoc,
    from: *const LoroFrontiers,
    to: *const LoroFrontiers,
    out: *mut *mut LoroDiffBatch,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let from = deref_or!(from, LoroStatus::LORO_ERR_INVALID_ARG);
        let to = deref_or!(to, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match doc.inner().diff(from.inner(), to.inner()) {
            Ok(batch) => {
                unsafe { out.write(Box::into_raw(Box::new(LoroDiffBatch(batch)))) };
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Applies `batch` to `doc`, mutating its state (and recording the change as new ops). The batch
/// is **not** consumed — it is cloned internally, so the caller still owns it and must release it
/// with [`loro_diff_batch_free`].
#[no_mangle]
pub extern "C" fn loro_doc_apply_diff(
    doc: *mut LoroDoc,
    batch: *const LoroDiffBatch,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let batch = deref_or!(batch, LoroStatus::LORO_ERR_INVALID_ARG);
        match doc.inner().apply_diff(batch.inner().clone()) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Reverts `doc`'s state back to `frontiers`. Unlike [`loro_doc_checkout`], this does not detach
/// the document: it records the inverse operations as a new change, so the rewind is itself part
/// of history. Returns `LORO_ERR_NOT_FOUND` if `frontiers` references an unknown version.
#[no_mangle]
pub extern "C" fn loro_doc_revert_to(
    doc: *mut LoroDoc,
    frontiers: *const LoroFrontiers,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let frontiers = deref_or!(frontiers, LoroStatus::LORO_ERR_INVALID_ARG);
        match doc.inner().revert_to(frontiers.inner()) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Renders `batch` as JSON into `*out` for inspection/debugging. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`. The result is a JSON object keyed by container-id
/// string (e.g. `"cid:root-text:Text"`), each mapping to that container's per-kind diff payload
/// using the same schema as [`loro_container_diff_to_json`]. Inserted values are rendered as
/// their deep value; a live container handle is not surfaced.
#[no_mangle]
pub extern "C" fn loro_diff_batch_to_json(
    batch: *const LoroDiffBatch,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let batch = deref_or!(batch, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let value = diff_batch_to_json(batch.inner());
        write_json(out, &value)
    })
}
