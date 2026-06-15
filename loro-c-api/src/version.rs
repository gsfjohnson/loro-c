//! Version vectors, frontiers, operation ids, change metadata, and the document-level
//! version / time-travel surface (M4).
//!
//! - [`LoroVersionVector`] — a `peer -> counter` map describing how much history a document
//!   has seen. Encode/decode for the wire; inclusion tests; conversion to [`LoroFrontiers`].
//! - [`LoroFrontiers`] — a set of operation ids that mark a document version (the leaves of
//!   the causal DAG). Encode/decode; build from / read back a list of [`LoroId`].
//! - [`LoroId`] — a single operation id `(peer, counter)`; mirrors `loro::ID`.
//! - [`LoroChangeMeta`] — callback-scoped metadata for one change, surfaced both here (via
//!   [`loro_doc_travel_change_ancestors`]) and by the pre-commit hook in `commit.rs`.
//!
//! All opaque handles created here are owned `Box` pointers freed by their `_free` function.

use crate::doc::LoroDoc;
use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::value::LoroBytes;
use loro::{ChangeMeta, ExportMode, Frontiers, IdSpan, VersionVector, ID};
use serde_json::{Map as JsonMap, Value as JsonValue};
use std::ops::ControlFlow;
use std::os::raw::c_void;

/// A single operation id: the creating peer and that peer's op counter. Mirrors `loro::ID`.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LoroId {
    pub peer: u64,
    pub counter: i32,
}

impl LoroId {
    pub(crate) fn to_loro(self) -> ID {
        ID::new(self.peer, self.counter)
    }

    pub(crate) fn from_loro(id: ID) -> LoroId {
        LoroId {
            peer: id.peer,
            counter: id.counter,
        }
    }
}

/// A half-open span of one peer's ops, `[counter_start, counter_end)`. Mirrors `loro::IdSpan`
/// (a `peer` plus a `loro::CounterSpan`). Passed by value to the id-span export functions.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LoroIdSpan {
    /// The peer whose ops this span covers.
    pub peer: u64,
    /// First op counter in the span (inclusive).
    pub counter_start: i32,
    /// One past the last op counter in the span (exclusive).
    pub counter_end: i32,
}

impl LoroIdSpan {
    pub(crate) fn to_loro(self) -> IdSpan {
        IdSpan::new(self.peer, self.counter_start, self.counter_end)
    }
}

// ---------------------------------------------------------------------------
// VersionVector
// ---------------------------------------------------------------------------

/// Opaque handle to a [`loro::VersionVector`]. Free with [`loro_version_vector_free`].
pub struct LoroVersionVector(VersionVector);

impl LoroVersionVector {
    pub(crate) fn inner(&self) -> &VersionVector {
        &self.0
    }
}

/// Creates a new, empty version vector. Release with [`loro_version_vector_free`].
#[no_mangle]
pub extern "C" fn loro_version_vector_new() -> *mut LoroVersionVector {
    ffi_guard!(std::ptr::null_mut(), {
        Box::into_raw(Box::new(LoroVersionVector(VersionVector::new())))
    })
}

/// Frees a version vector handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_version_vector_free(vv: *mut LoroVersionVector) {
    ffi_guard!((), {
        if !vv.is_null() {
            unsafe {
                drop(Box::from_raw(vv));
            }
        }
    });
}

/// Encodes the version vector into `*out`. `*out` is only written on `LORO_OK`; free it with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_version_vector_encode(
    vv: *const LoroVersionVector,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let vv = deref_or!(vv, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(vv.inner().encode())) };
        LoroStatus::LORO_OK
    })
}

/// Decodes a version vector previously produced by [`loro_version_vector_encode`]. Returns
/// null on a decode error. Release with [`loro_version_vector_free`].
#[no_mangle]
pub extern "C" fn loro_version_vector_decode(data: *const u8, len: usize) -> *mut LoroVersionVector {
    ffi_guard!(std::ptr::null_mut(), {
        if data.is_null() && len != 0 {
            set_last_error("null data pointer passed to loro-c-api");
            return std::ptr::null_mut();
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        match VersionVector::decode(bytes) {
            Ok(vv) => Box::into_raw(Box::new(LoroVersionVector(vv))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Writes the last (largest) counter seen for `peer` into `*out`. Returns
/// `LORO_ERR_NOT_FOUND` if the vector has no entry for `peer`.
#[no_mangle]
pub extern "C" fn loro_version_vector_get_last(
    vv: *const LoroVersionVector,
    peer: u64,
    out: *mut i32,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let vv = deref_or!(vv, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match vv.inner().get_last(peer) {
            Some(c) => {
                unsafe { out.write(c) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("no entry for peer in version vector");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Records `id` as the last op seen from its peer (extends the vector to include it).
#[no_mangle]
pub extern "C" fn loro_version_vector_set_last(
    vv: *mut LoroVersionVector,
    id: LoroId,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let vv = unsafe { vv.as_mut() };
        let vv = match vv {
            Some(v) => v,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        vv.0.set_last(id.to_loro());
        LoroStatus::LORO_OK
    })
}

/// Returns whether this vector includes `id` (i.e. it has seen that op). Returns `false` on
/// a null handle.
#[no_mangle]
pub extern "C" fn loro_version_vector_includes_id(
    vv: *const LoroVersionVector,
    id: LoroId,
) -> bool {
    ffi_guard!(false, {
        let vv = deref_or!(vv, false);
        vv.inner().includes_id(id.to_loro())
    })
}

/// Returns whether this vector includes everything in `other`. Returns `false` on a null
/// handle.
#[no_mangle]
pub extern "C" fn loro_version_vector_includes_vv(
    vv: *const LoroVersionVector,
    other: *const LoroVersionVector,
) -> bool {
    ffi_guard!(false, {
        let vv = deref_or!(vv, false);
        let other = deref_or!(other, false);
        vv.inner().includes_vv(other.inner())
    })
}

/// Compares two version vectors in the causal partial order, writing `-1` (a < b), `0`
/// (equal), or `1` (a > b) into `*out`. Returns `LORO_ERR_NOT_FOUND` if the two are
/// concurrent (incomparable); `*out` is only written on `LORO_OK`.
#[no_mangle]
pub extern "C" fn loro_version_vector_compare(
    a: *const LoroVersionVector,
    b: *const LoroVersionVector,
    out: *mut i32,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let a = deref_or!(a, LoroStatus::LORO_ERR_INVALID_ARG);
        let b = deref_or!(b, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match a.inner().partial_cmp(b.inner()) {
            Some(ord) => {
                unsafe { out.write(ord as i32) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("version vectors are concurrent (incomparable)");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Returns the frontiers corresponding to this version vector, or null on error. Release the
/// returned handle with [`loro_frontiers_free`].
#[no_mangle]
pub extern "C" fn loro_version_vector_to_frontiers(
    vv: *const LoroVersionVector,
) -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        let vv = deref_or!(vv, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroFrontiers(vv.inner().get_frontiers())))
    })
}

/// Writes the vector as a JSON object `{"<peer>": <counter>, ...}` into `*out`. `*out` is
/// only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_version_vector_to_json(
    vv: *const LoroVersionVector,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let vv = deref_or!(vv, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let mut obj = JsonMap::new();
        // VersionVector derefs to FxHashMap<PeerID, Counter>.
        for (peer, counter) in vv.inner().iter() {
            obj.insert(peer.to_string(), JsonValue::from(*counter));
        }
        match serde_json::to_vec(&JsonValue::Object(obj)) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize version vector: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

// ---------------------------------------------------------------------------
// Frontiers
// ---------------------------------------------------------------------------

/// Opaque handle to a [`loro::Frontiers`]. Free with [`loro_frontiers_free`].
pub struct LoroFrontiers(Frontiers);

impl LoroFrontiers {
    pub(crate) fn inner(&self) -> &Frontiers {
        &self.0
    }
}

/// Creates a new, empty frontiers. Release with [`loro_frontiers_free`].
#[no_mangle]
pub extern "C" fn loro_frontiers_new() -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        Box::into_raw(Box::new(LoroFrontiers(Frontiers::new())))
    })
}

/// Frees a frontiers handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_frontiers_free(frontiers: *mut LoroFrontiers) {
    ffi_guard!((), {
        if !frontiers.is_null() {
            unsafe {
                drop(Box::from_raw(frontiers));
            }
        }
    });
}

/// Builds a frontiers from the `count` ids in `ids`. Release with [`loro_frontiers_free`].
#[no_mangle]
pub extern "C" fn loro_frontiers_from_ids(
    ids: *const LoroId,
    count: usize,
) -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        if ids.is_null() && count != 0 {
            set_last_error("null ids pointer passed to loro-c-api");
            return std::ptr::null_mut();
        }
        let slice = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(ids, count) }
        };
        let f: Frontiers = slice.iter().map(|id| id.to_loro()).collect();
        Box::into_raw(Box::new(LoroFrontiers(f)))
    })
}

/// Encodes the frontiers into `*out`. `*out` is only written on `LORO_OK`; free it with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_frontiers_encode(
    frontiers: *const LoroFrontiers,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let f = deref_or!(frontiers, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(f.inner().encode())) };
        LoroStatus::LORO_OK
    })
}

/// Decodes a frontiers previously produced by [`loro_frontiers_encode`]. Returns null on a
/// decode error. Release with [`loro_frontiers_free`].
#[no_mangle]
pub extern "C" fn loro_frontiers_decode(data: *const u8, len: usize) -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        if data.is_null() && len != 0 {
            set_last_error("null data pointer passed to loro-c-api");
            return std::ptr::null_mut();
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        match Frontiers::decode(bytes) {
            Ok(f) => Box::into_raw(Box::new(LoroFrontiers(f))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Returns the number of ids in the frontiers. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_frontiers_len(frontiers: *const LoroFrontiers) -> usize {
    ffi_guard!(0usize, {
        let f = deref_or!(frontiers, 0usize);
        f.inner().len()
    })
}

/// Returns whether the frontiers is empty (also returns `true` on a null handle).
#[no_mangle]
pub extern "C" fn loro_frontiers_is_empty(frontiers: *const LoroFrontiers) -> bool {
    ffi_guard!(true, {
        let f = deref_or!(frontiers, true);
        f.inner().is_empty()
    })
}

/// Returns whether the frontiers contains `id`. Returns `false` on a null handle.
#[no_mangle]
pub extern "C" fn loro_frontiers_contains(frontiers: *const LoroFrontiers, id: LoroId) -> bool {
    ffi_guard!(false, {
        let f = deref_or!(frontiers, false);
        f.inner().contains(&id.to_loro())
    })
}

/// Writes the id at `index` into `*out`. Returns `LORO_ERR_NOT_FOUND` if `index` is out of
/// range; `*out` is only written on `LORO_OK`.
#[no_mangle]
pub extern "C" fn loro_frontiers_get(
    frontiers: *const LoroFrontiers,
    index: usize,
    out: *mut LoroId,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let f = deref_or!(frontiers, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match f.inner().iter().nth(index) {
            Some(id) => {
                unsafe { out.write(LoroId::from_loro(id)) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("frontiers index out of range");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Adds `id` to the frontiers.
#[no_mangle]
pub extern "C" fn loro_frontiers_push(frontiers: *mut LoroFrontiers, id: LoroId) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let f = match unsafe { frontiers.as_mut() } {
            Some(f) => f,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        f.0.push(id.to_loro());
        LoroStatus::LORO_OK
    })
}

// ---------------------------------------------------------------------------
// LoroDoc version / time-travel surface
// ---------------------------------------------------------------------------

/// Returns the document's oplog version vector (everything in its history). Release with
/// [`loro_version_vector_free`].
#[no_mangle]
pub extern "C" fn loro_doc_oplog_vv(doc: *const LoroDoc) -> *mut LoroVersionVector {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroVersionVector(doc.inner().oplog_vv())))
    })
}

/// Returns the document's current state version vector. Release with
/// [`loro_version_vector_free`].
#[no_mangle]
pub extern "C" fn loro_doc_state_vv(doc: *const LoroDoc) -> *mut LoroVersionVector {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroVersionVector(doc.inner().state_vv())))
    })
}

/// Returns the document's oplog frontiers. Release with [`loro_frontiers_free`].
#[no_mangle]
pub extern "C" fn loro_doc_oplog_frontiers(doc: *const LoroDoc) -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroFrontiers(doc.inner().oplog_frontiers())))
    })
}

/// Returns the document's current state frontiers. Release with [`loro_frontiers_free`].
#[no_mangle]
pub extern "C" fn loro_doc_state_frontiers(doc: *const LoroDoc) -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroFrontiers(doc.inner().state_frontiers())))
    })
}

/// Reports whether the document is shallow (its history was trimmed by a shallow snapshot, so
/// it only retains ops since [`loro_doc_shallow_since_vv`]). Returns `false` on a null handle.
#[no_mangle]
pub extern "C" fn loro_doc_is_shallow(doc: *const LoroDoc) -> bool {
    ffi_guard!(false, {
        let doc = deref_or!(doc, false);
        doc.inner().is_shallow()
    })
}

/// Returns the start version of a shallow document's retained history — the ops *before* this
/// version are not present. Empty when the document is not shallow. Release with
/// [`loro_version_vector_free`].
#[no_mangle]
pub extern "C" fn loro_doc_shallow_since_vv(doc: *const LoroDoc) -> *mut LoroVersionVector {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroVersionVector(doc.inner().shallow_since_vv().to_vv())))
    })
}

/// Converts `frontiers` to a version vector against this document, or null if the frontiers
/// are not contained in the document's history. Release with [`loro_version_vector_free`].
#[no_mangle]
pub extern "C" fn loro_doc_frontiers_to_vv(
    doc: *const LoroDoc,
    frontiers: *const LoroFrontiers,
) -> *mut LoroVersionVector {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let f = deref_or!(frontiers, std::ptr::null_mut());
        match doc.inner().frontiers_to_vv(f.inner()) {
            Some(vv) => Box::into_raw(Box::new(LoroVersionVector(vv))),
            None => {
                set_last_error("frontiers not contained in the document history");
                std::ptr::null_mut()
            }
        }
    })
}

/// Converts a version vector to frontiers against this document. Release with
/// [`loro_frontiers_free`].
#[no_mangle]
pub extern "C" fn loro_doc_vv_to_frontiers(
    doc: *const LoroDoc,
    vv: *const LoroVersionVector,
) -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let vv = deref_or!(vv, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroFrontiers(doc.inner().vv_to_frontiers(vv.inner()))))
    })
}

/// Time-travels the document state to `frontiers` (detaching it from the latest version).
#[no_mangle]
pub extern "C" fn loro_doc_checkout(
    doc: *mut LoroDoc,
    frontiers: *const LoroFrontiers,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let f = deref_or!(frontiers, LoroStatus::LORO_ERR_INVALID_ARG);
        match doc.inner().checkout(f.inner()) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Re-attaches the document state to the latest version after a [`loro_doc_checkout`].
#[no_mangle]
pub extern "C" fn loro_doc_checkout_to_latest(doc: *mut LoroDoc) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().checkout_to_latest();
        LoroStatus::LORO_OK
    })
}

/// Returns whether the document is detached (checked out to a non-latest version). Returns
/// `false` on a null handle.
#[no_mangle]
pub extern "C" fn loro_doc_is_detached(doc: *const LoroDoc) -> bool {
    ffi_guard!(false, {
        let doc = deref_or!(doc, false);
        doc.inner().is_detached()
    })
}

/// Exports only the updates `from` the given version vector up to the latest, into `*out`
/// (the delta needed to bring a peer at `from` up to date). `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_doc_export_updates_from(
    doc: *const LoroDoc,
    from: *const LoroVersionVector,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let from = deref_or!(from, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match doc.inner().export(ExportMode::updates(from.inner())) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => crate::error::record_encode_error(&e),
        }
    })
}

// ---------------------------------------------------------------------------
// ChangeMeta (callback-scoped) + travel_change_ancestors
// ---------------------------------------------------------------------------

/// Opaque, **callback-scoped** view of one change's metadata. Only valid for the duration of
/// the callback that receives it (a change-ancestor traveler or a pre-commit hook); never
/// store the pointer. Internally a `*const LoroChangeMeta` always points to a borrowed
/// `loro::ChangeMeta` (see [`change_meta_ref`]).
#[allow(dead_code)]
pub struct LoroChangeMeta(ChangeMeta);

/// Reinterprets a callback-scoped `*const LoroChangeMeta` as the borrowed `ChangeMeta` it
/// actually points to. Records an error and returns `None` on null.
pub(crate) fn change_meta_ref<'a>(cm: *const LoroChangeMeta) -> Option<&'a ChangeMeta> {
    match unsafe { (cm as *const ChangeMeta).as_ref() } {
        Some(c) => Some(c),
        None => {
            set_last_error("null change meta pointer passed to loro-c-api");
            None
        }
    }
}

/// Returns the change's first-op id. Returns `{0, 0}` on a null handle.
#[no_mangle]
pub extern "C" fn loro_change_meta_id(cm: *const LoroChangeMeta) -> LoroId {
    ffi_guard!(LoroId { peer: 0, counter: 0 }, {
        match change_meta_ref(cm) {
            Some(c) => LoroId::from_loro(c.id),
            None => LoroId { peer: 0, counter: 0 },
        }
    })
}

/// Returns the change's Lamport timestamp. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_change_meta_lamport(cm: *const LoroChangeMeta) -> u32 {
    ffi_guard!(0u32, {
        match change_meta_ref(cm) {
            Some(c) => c.lamport,
            None => 0,
        }
    })
}

/// Returns the change's wall-clock timestamp (seconds since the Unix epoch; 0 if unset).
/// Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_change_meta_timestamp(cm: *const LoroChangeMeta) -> i64 {
    ffi_guard!(0i64, {
        match change_meta_ref(cm) {
            Some(c) => c.timestamp,
            None => 0,
        }
    })
}

/// Returns the number of ops in the change. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_change_meta_len(cm: *const LoroChangeMeta) -> usize {
    ffi_guard!(0usize, {
        match change_meta_ref(cm) {
            Some(c) => c.len,
            None => 0,
        }
    })
}

/// Writes the change's commit message (UTF-8, possibly empty) into `*out`. `*out` is only
/// written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_change_meta_message(
    cm: *const LoroChangeMeta,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let c = match change_meta_ref(cm) {
            Some(c) => c,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(c.message().as_bytes().to_vec())) };
        LoroStatus::LORO_OK
    })
}

/// Returns an owned copy of the change's dependency frontiers, or null on a null handle.
/// Release with [`loro_frontiers_free`].
#[no_mangle]
pub extern "C" fn loro_change_meta_deps(cm: *const LoroChangeMeta) -> *mut LoroFrontiers {
    ffi_guard!(std::ptr::null_mut(), {
        match change_meta_ref(cm) {
            Some(c) => Box::into_raw(Box::new(LoroFrontiers(c.deps.clone()))),
            None => std::ptr::null_mut(),
        }
    })
}

/// A traveler callback for [`loro_doc_travel_change_ancestors`].
///
/// `invoke` is called for each ancestor change (latest to oldest) with a callback-scoped
/// `const LoroChangeMeta*` and the opaque `user_data`; it returns `true` to continue or
/// `false` to stop the traversal early. `free_user_data` (may be null) runs once when the
/// traversal finishes.
#[repr(C)]
pub struct LoroChangeAncestorsTraveler {
    pub invoke: extern "C" fn(meta: *const LoroChangeMeta, user_data: *mut c_void) -> bool,
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

/// Traverses the ancestors of the changes containing `ids` (including those changes
/// themselves), in causal order from latest to oldest, calling `traveler.invoke` for each.
/// Returns `LORO_ERR_NOT_FOUND` if an id is missing from the document history.
#[no_mangle]
pub extern "C" fn loro_doc_travel_change_ancestors(
    doc: *const LoroDoc,
    ids: *const LoroId,
    count: usize,
    traveler: LoroChangeAncestorsTraveler,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        if ids.is_null() && count != 0 {
            set_last_error("null ids pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let slice = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(ids, count) }
        };
        let loro_ids: Vec<ID> = slice.iter().map(|id| id.to_loro()).collect();

        // Run free_user_data exactly once when the traversal returns (mirrors the callback
        // ownership model used by subscriptions).
        struct Guard(LoroChangeAncestorsTraveler);
        impl Drop for Guard {
            fn drop(&mut self) {
                if let Some(free) = self.0.free_user_data {
                    free(self.0.user_data);
                }
            }
        }
        let guard = Guard(traveler);

        let mut f = |meta: ChangeMeta| -> ControlFlow<()> {
            let ptr = (&meta as *const ChangeMeta) as *const LoroChangeMeta;
            if (guard.0.invoke)(ptr, guard.0.user_data) {
                ControlFlow::Continue(())
            } else {
                ControlFlow::Break(())
            }
        };
        match doc.inner().travel_change_ancestors(&loro_ids, &mut f) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => {
                set_last_error(e.to_string());
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}
