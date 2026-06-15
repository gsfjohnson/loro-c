//! Value navigation (G5): `get_by_path` / `get_by_str_path`.
//!
//! Most values cross this FFI boundary as JSON (see [`crate::value`]). JSON round-trips plain
//! data fine, but it cannot express the one thing this module exists for: "the thing at this
//! path is a **live nested container**" — a JSON dump flattens a container to a plain value and
//! loses the handle. [`loro_doc_get_by_path`] and [`loro_doc_get_by_str_path`] close that gap by
//! returning an opaque [`LoroValueOrContainer`] that can hand back a live `LoroContainer*`.
//!
//! This mirrors the [`crate::jsonpath`] `LoroJsonPathResults` pattern (each result is a value or
//! a live container) for the single-result `get_by_*` case, rather than porting the full
//! upstream tagged-union `LoroValue` (the JSON path already covers plain data).
//!
//! Like the other opaque handles, a `LoroValueOrContainer*` is an owned `Box` pointer freed with
//! [`loro_value_or_container_free`]; any `LoroContainer*` taken from it is independent and must
//! be freed separately with `loro_container_free`.

use crate::container::any::{container_type_of, LoroContainer, LoroContainerType};
use crate::container::tree::LoroTreeID;
use crate::doc::LoroDoc;
use crate::error::{set_last_error, LoroStatus};
use crate::value::{str_from_raw, value_to_json_bytes, LoroBytes};
use loro::{Index, ValueOrContainer};
use std::os::raw::c_char;

/// Which kind of path step a [`LoroPathComponent`] is. Mirrors the variants of `loro::Index`.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroPathComponentKind {
    /// Index into a map by string key (uses `key`/`key_len`).
    LORO_PATH_KEY = 0,
    /// Index into a list / movable-list by position (uses `seq`).
    LORO_PATH_SEQ = 1,
    /// Index into a tree by node id (uses `node`).
    LORO_PATH_NODE = 2,
}

/// One step of a path passed to [`loro_doc_get_by_path`]. Mirrors a `loro::Index`; `kind`
/// selects which of the remaining fields is meaningful. The `key` pointer is borrowed for the
/// duration of the call and is not owned by this struct.
#[repr(C)]
pub struct LoroPathComponent {
    /// Selects which field below carries the index.
    pub kind: LoroPathComponentKind,
    /// `LORO_PATH_KEY`: pointer to the UTF-8 map key (not nul-terminated, not owned).
    pub key: *const c_char,
    /// `LORO_PATH_KEY`: byte length of `key`.
    pub key_len: usize,
    /// `LORO_PATH_SEQ`: list / movable-list index.
    pub seq: usize,
    /// `LORO_PATH_NODE`: tree node id.
    pub node: LoroTreeID,
}

/// Opaque, owned result of [`loro_doc_get_by_path`] / [`loro_doc_get_by_str_path`]: either a
/// plain value (read as JSON with [`loro_value_or_container_get_value_json`]) or a live container
/// (recovered as a `LoroContainer*` with [`loro_value_or_container_get_container`]). Free with
/// [`loro_value_or_container_free`].
pub struct LoroValueOrContainer(ValueOrContainer);

/// Resolves the document path `(path, count)` to a value or live container, or returns null if a
/// component is invalid UTF-8 or the path does not resolve (see `loro_last_error_message`).
/// Release the returned handle with [`loro_value_or_container_free`].
#[no_mangle]
pub extern "C" fn loro_doc_get_by_path(
    doc: *const LoroDoc,
    path: *const LoroPathComponent,
    count: usize,
) -> *mut LoroValueOrContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        if path.is_null() && count != 0 {
            set_last_error("null path pointer passed to loro-c-api");
            return std::ptr::null_mut();
        }
        let components = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(path, count) }
        };
        let mut indices: Vec<Index> = Vec::with_capacity(count);
        for comp in components {
            let index = match comp.kind {
                LoroPathComponentKind::LORO_PATH_KEY => {
                    let key = match str_from_raw(comp.key, comp.key_len) {
                        Some(s) => s,
                        None => return std::ptr::null_mut(),
                    };
                    Index::Key(key.into())
                }
                LoroPathComponentKind::LORO_PATH_SEQ => Index::Seq(comp.seq),
                LoroPathComponentKind::LORO_PATH_NODE => Index::Node(comp.node.to_loro()),
            };
            indices.push(index);
        }
        match doc.inner().get_by_path(&indices) {
            Some(voc) => Box::into_raw(Box::new(LoroValueOrContainer(voc))),
            None => {
                set_last_error("path did not resolve to a value or container");
                std::ptr::null_mut()
            }
        }
    })
}

/// Resolves the string path `(path, path_len)` to a value or live container, or returns null if
/// the path is invalid UTF-8 or does not resolve. The path syntax follows the container kind,
/// e.g. `map/key`, `list/0`, `tree/{node_id}/prop` or `tree/0/prop`. Release the returned handle
/// with [`loro_value_or_container_free`].
#[no_mangle]
pub extern "C" fn loro_doc_get_by_str_path(
    doc: *const LoroDoc,
    path: *const c_char,
    path_len: usize,
) -> *mut LoroValueOrContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let path = match str_from_raw(path, path_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match doc.inner().get_by_str_path(path) {
            Some(voc) => Box::into_raw(Box::new(LoroValueOrContainer(voc))),
            None => {
                set_last_error("path did not resolve to a value or container");
                std::ptr::null_mut()
            }
        }
    })
}

/// Returns whether the result is a live container (vs. a plain value). Returns `false` on a null
/// handle.
#[no_mangle]
pub extern "C" fn loro_value_or_container_is_container(voc: *const LoroValueOrContainer) -> bool {
    ffi_guard!(false, {
        let voc = deref_or!(voc, false);
        matches!(voc.0, ValueOrContainer::Container(_))
    })
}

/// Returns the container kind of the result. Meaningful only when
/// [`loro_value_or_container_is_container`] is `true`; returns `LORO_CONTAINER_UNKNOWN` for a
/// plain value or a null handle.
#[no_mangle]
pub extern "C" fn loro_value_or_container_container_type(
    voc: *const LoroValueOrContainer,
) -> LoroContainerType {
    ffi_guard!(LoroContainerType::LORO_CONTAINER_UNKNOWN, {
        let voc = deref_or!(voc, LoroContainerType::LORO_CONTAINER_UNKNOWN);
        match &voc.0 {
            ValueOrContainer::Container(c) => container_type_of(c),
            ValueOrContainer::Value(_) => LoroContainerType::LORO_CONTAINER_UNKNOWN,
        }
    })
}

/// Recovers the result as a type-erased `LoroContainer*`, or null (with an error recorded) if it
/// is a plain value. The returned handle is independent; free it with `loro_container_free`. The
/// source `LoroValueOrContainer*` is unaffected.
#[no_mangle]
pub extern "C" fn loro_value_or_container_get_container(
    voc: *const LoroValueOrContainer,
) -> *mut LoroContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let voc = deref_or!(voc, std::ptr::null_mut());
        match &voc.0 {
            ValueOrContainer::Container(c) => {
                Box::into_raw(Box::new(LoroContainer::from_inner(c.clone())))
            }
            ValueOrContainer::Value(_) => {
                set_last_error("value-or-container holds a value, not a container");
                std::ptr::null_mut()
            }
        }
    })
}

/// Writes the result as JSON into `*out` (a container is rendered as its deep value, like
/// `loro_jsonpath_results_get_value_json`). `*out` is only written on `LORO_OK`; free it with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_value_or_container_get_value_json(
    voc: *const LoroValueOrContainer,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let voc = deref_or!(voc, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let value = voc.0.get_deep_value();
        match value_to_json_bytes(&value) {
            Some(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            None => LoroStatus::LORO_ERR_ENCODE,
        }
    })
}

/// Frees a value-or-container handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_value_or_container_free(voc: *mut LoroValueOrContainer) {
    ffi_guard!((), {
        if !voc.is_null() {
            unsafe {
                drop(Box::from_raw(voc));
            }
        }
    });
}
