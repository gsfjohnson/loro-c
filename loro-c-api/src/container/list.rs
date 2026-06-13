//! `LoroList` container handle and its operations.
//!
//! Values cross the boundary as JSON ([`loro_list_insert`] / [`loro_list_get`]). Nested
//! containers use the type-erased
//! [`LoroContainer`](crate::container::any::LoroContainer):
//! [`loro_list_insert_container`] / [`loro_list_push_container`] /
//! [`loro_list_get_container`].
//!
//! Like every container handle, a `LoroList*` is a strong co-owner of the document state
//! (see `doc.rs`) and may be freed in any order. Free with [`loro_list_free`].

use crate::container::any::LoroContainer;
use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::value::{value_from_json, value_to_json_bytes, LoroBytes};
use std::os::raw::c_char;

/// Opaque handle to a Loro list container.
pub struct LoroList(loro::LoroList);

impl LoroList {
    pub(crate) fn from_inner(l: loro::LoroList) -> LoroList {
        LoroList(l)
    }

    fn inner(&self) -> &loro::LoroList {
        &self.0
    }
}

/// Frees a list handle. Passing null is a no-op. Safe to call before or after the
/// originating `LoroDoc*` is freed.
#[no_mangle]
pub extern "C" fn loro_list_free(list: *mut LoroList) {
    ffi_guard!((), {
        if !list.is_null() {
            unsafe {
                drop(Box::from_raw(list));
            }
        }
    });
}

/// Inserts the JSON-encoded value `(value, value_len)` at index `pos`.
#[no_mangle]
pub extern "C" fn loro_list_insert(
    list: *mut LoroList,
    pos: usize,
    value: *const c_char,
    value_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let list = deref_or!(list, LoroStatus::LORO_ERR_INVALID_ARG);
        let value = match value_from_json(value, value_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match list.inner().insert(pos, value) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Appends the JSON-encoded value `(value, value_len)` to the end of the list.
#[no_mangle]
pub extern "C" fn loro_list_push(
    list: *mut LoroList,
    value: *const c_char,
    value_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let list = deref_or!(list, LoroStatus::LORO_ERR_INVALID_ARG);
        let value = match value_from_json(value, value_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match list.inner().push(value) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Writes the value at `index` as JSON bytes into `*out` (containers resolved to their
/// deep JSON value). Returns `LORO_ERR_NOT_FOUND` if `index` is out of range. `*out` is
/// only written on `LORO_OK`; free it with `loro_bytes_free`. For a child container handle
/// instead, use [`loro_list_get_container`].
#[no_mangle]
pub extern "C" fn loro_list_get(
    list: *const LoroList,
    index: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let list = deref_or!(list, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match list.inner().get(index) {
            Some(voc) => {
                let value = voc.get_deep_value();
                match value_to_json_bytes(&value) {
                    Some(bytes) => {
                        unsafe { out.write(LoroBytes::from_vec(bytes)) };
                        LoroStatus::LORO_OK
                    }
                    None => LoroStatus::LORO_ERR_ENCODE,
                }
            }
            None => {
                set_last_error("list index out of range");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Returns the child container stored at `index` as a type-erased `LoroContainer*`, or
/// null if the index is out of range or holds a plain value. Free the returned handle with
/// `loro_container_free`.
#[no_mangle]
pub extern "C" fn loro_list_get_container(
    list: *const LoroList,
    index: usize,
) -> *mut LoroContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let list = deref_or!(list, std::ptr::null_mut());
        match list.inner().get(index) {
            Some(loro::ValueOrContainer::Container(c)) => {
                Box::into_raw(Box::new(LoroContainer::from_inner(c)))
            }
            Some(_) => {
                set_last_error("list entry is a value, not a container");
                std::ptr::null_mut()
            }
            None => {
                set_last_error("list index out of range");
                std::ptr::null_mut()
            }
        }
    })
}

/// Inserts the detached `child` container at `index` and returns the attached handle.
/// Consumes `child` (do not free it). Returns null on error. Free the returned handle with
/// `loro_container_free`.
#[no_mangle]
pub extern "C" fn loro_list_insert_container(
    list: *mut LoroList,
    pos: usize,
    child: *mut LoroContainer,
) -> *mut LoroContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let list = deref_or!(list, std::ptr::null_mut());
        let child = match crate::container::any::take(child) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match attach_container!(child, |c| list.inner().insert_container(pos, c)) {
            Ok(container) => Box::into_raw(Box::new(LoroContainer::from_inner(container))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Appends the detached `child` container and returns the attached handle. Consumes
/// `child` (do not free it). Returns null on error. Free the returned handle with
/// `loro_container_free`.
#[no_mangle]
pub extern "C" fn loro_list_push_container(
    list: *mut LoroList,
    child: *mut LoroContainer,
) -> *mut LoroContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let list = deref_or!(list, std::ptr::null_mut());
        let child = match crate::container::any::take(child) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match attach_container!(child, |c| list.inner().push_container(c)) {
            Ok(container) => Box::into_raw(Box::new(LoroContainer::from_inner(container))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Deletes `len` elements starting at index `pos`.
#[no_mangle]
pub extern "C" fn loro_list_delete(list: *mut LoroList, pos: usize, len: usize) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let list = deref_or!(list, LoroStatus::LORO_ERR_INVALID_ARG);
        match list.inner().delete(pos, len) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Pops the last element. On success, `*out_present` is set to whether a value was popped;
/// if so, the value is written as JSON into `*out`. `out` must be non-null; `out_present`
/// may be null. Free a written `*out` with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_list_pop(
    list: *mut LoroList,
    out: *mut LoroBytes,
    out_present: *mut bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let list = deref_or!(list, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match list.inner().pop() {
            Ok(Some(value)) => {
                if !out_present.is_null() {
                    unsafe { out_present.write(true) };
                }
                match value_to_json_bytes(&value) {
                    Some(bytes) => {
                        unsafe { out.write(LoroBytes::from_vec(bytes)) };
                        LoroStatus::LORO_OK
                    }
                    None => LoroStatus::LORO_ERR_ENCODE,
                }
            }
            Ok(None) => {
                if !out_present.is_null() {
                    unsafe { out_present.write(false) };
                }
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Writes the whole list as a JSON array (deep values) into `*out`. `*out` is only written
/// on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_list_to_json(list: *const LoroList, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let list = deref_or!(list, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let value = list.inner().get_deep_value();
        match value_to_json_bytes(&value) {
            Some(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            None => LoroStatus::LORO_ERR_ENCODE,
        }
    })
}

/// Returns the number of elements in the list. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_list_len(list: *const LoroList) -> usize {
    ffi_guard!(0usize, {
        let list = deref_or!(list, 0usize);
        list.inner().len()
    })
}

/// Returns `true` if the list is empty (also returns `true`, with an error recorded, on a
/// null handle).
#[no_mangle]
pub extern "C" fn loro_list_is_empty(list: *const LoroList) -> bool {
    ffi_guard!(true, {
        let list = deref_or!(list, true);
        list.inner().is_empty()
    })
}

/// Removes all elements from the list.
#[no_mangle]
pub extern "C" fn loro_list_clear(list: *mut LoroList) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let list = deref_or!(list, LoroStatus::LORO_ERR_INVALID_ARG);
        match list.inner().clear() {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}
