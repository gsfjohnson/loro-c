//! `LoroMap` container handle and its operations.
//!
//! Values cross the boundary as JSON: [`loro_map_insert`] takes a JSON-encoded value and
//! [`loro_map_get`] returns one. Nested containers are handled with the type-erased
//! [`LoroContainer`](crate::container::any::LoroContainer):
//! [`loro_map_insert_container`] / [`loro_map_get_container`].
//!
//! Like every container handle, a `LoroMap*` is a strong co-owner of the document state
//! (see `doc.rs`): it stays valid after the originating `LoroDoc*` is freed, and handles
//! may be freed in any order. Free with [`loro_map_free`].

use crate::container::any::LoroContainer;
use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::value::{str_from_raw, value_from_json, value_to_json_bytes, LoroBytes};
use std::os::raw::c_char;

/// Opaque handle to a Loro map container.
pub struct LoroMap(loro::LoroMap);

impl LoroMap {
    pub(crate) fn from_inner(m: loro::LoroMap) -> LoroMap {
        LoroMap(m)
    }

    fn inner(&self) -> &loro::LoroMap {
        &self.0
    }
}

/// Frees a map handle. Passing null is a no-op. Safe to call before or after the
/// originating `LoroDoc*` is freed.
#[no_mangle]
pub extern "C" fn loro_map_free(map: *mut LoroMap) {
    ffi_guard!((), {
        if !map.is_null() {
            unsafe {
                drop(Box::from_raw(map));
            }
        }
    });
}

/// Inserts the JSON-encoded value `(value, value_len)` under UTF-8 key `(key, key_len)`.
#[no_mangle]
pub extern "C" fn loro_map_insert(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
    value: *const c_char,
    value_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        let value = match value_from_json(value, value_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match map.inner().insert(key, value) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Writes the value at `(key, key_len)` as JSON bytes into `*out` (containers are
/// resolved to their deep JSON value). Returns `LORO_ERR_NOT_FOUND` if the key is absent.
/// `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`. For a child
/// container handle instead, use [`loro_map_get_container`].
#[no_mangle]
pub extern "C" fn loro_map_get(
    map: *const LoroMap,
    key: *const c_char,
    key_len: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match map.inner().get(key) {
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
                set_last_error("key not found in map");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Returns the child container stored at `(key, key_len)` as a type-erased
/// `LoroContainer*`, or null if the key is absent or holds a plain value. Free the
/// returned handle with `loro_container_free`.
#[no_mangle]
pub extern "C" fn loro_map_get_container(
    map: *const LoroMap,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match map.inner().get(key) {
            Some(loro::ValueOrContainer::Container(c)) => {
                Box::into_raw(Box::new(LoroContainer::from_inner(c)))
            }
            Some(_) => {
                set_last_error("map entry is a value, not a container");
                std::ptr::null_mut()
            }
            None => {
                set_last_error("key not found in map");
                std::ptr::null_mut()
            }
        }
    })
}

/// Attaches the detached `child` container under `(key, key_len)` and returns the attached
/// handle. Consumes `child` (do not free it). Returns null on error. Free the returned
/// handle with `loro_container_free`.
#[no_mangle]
pub extern "C" fn loro_map_insert_container(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
    child: *mut LoroContainer,
) -> *mut LoroContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let child = match crate::container::any::take(child) {
            Some(c) => c,
            None => return std::ptr::null_mut(),
        };
        match attach_container!(child, |c| map.inner().insert_container(key, c)) {
            Ok(container) => Box::into_raw(Box::new(LoroContainer::from_inner(container))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Deletes the entry at `(key, key_len)`. Succeeds even if the key is absent.
#[no_mangle]
pub extern "C" fn loro_map_delete(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        match map.inner().delete(key) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Writes the map's keys as a JSON array of strings into `*out`. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_map_keys(map: *const LoroMap, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let keys: Vec<String> = map.inner().keys().map(|k| k.to_string()).collect();
        match serde_json::to_vec(&keys) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize map keys to JSON: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

/// Writes the whole map (keys + deep values) as a JSON object into `*out`. `*out` is only
/// written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_map_to_json(map: *const LoroMap, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let value = map.inner().get_deep_value();
        match value_to_json_bytes(&value) {
            Some(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            None => LoroStatus::LORO_ERR_ENCODE,
        }
    })
}

/// Returns the number of entries in the map. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_map_len(map: *const LoroMap) -> usize {
    ffi_guard!(0usize, {
        let map = deref_or!(map, 0usize);
        map.inner().len()
    })
}

/// Returns `true` if the map is empty (also returns `true`, with an error recorded, on a
/// null handle).
#[no_mangle]
pub extern "C" fn loro_map_is_empty(map: *const LoroMap) -> bool {
    ffi_guard!(true, {
        let map = deref_or!(map, true);
        map.inner().is_empty()
    })
}

/// Removes all entries from the map.
#[no_mangle]
pub extern "C" fn loro_map_clear(map: *mut LoroMap) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        match map.inner().clear() {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}
