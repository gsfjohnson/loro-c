//! Typed `LoroValue` C ABI (RESHAPE Phase 1).
//!
//! The JSON value bridge ([`crate::value`]) is lossy: `Binary` round-trips through JSON as
//! an array and comes back a `List`, and integer-valued doubles (`2.0`) collapse to `I64`.
//! This module exposes `loro::LoroValue` as an opaque, owned C handle ([`LoroValue`]) with
//! typed constructors and accessors, so values cross the FFI boundary **without JSON** —
//! preserving binary, doubles, and the value/container distinction. The loro-cpp-shaped C++
//! wrapper (`loro::LoroValue`) is built on this surface.
//!
//! A `LoroValue*` is an owned `Box` pointer; free it exactly once with [`loro_value_free`].
//! The compound builders ([`loro_value_list_push`] / [`loro_value_map_insert`]) *clone* the
//! child in — the caller still owns (and must free) the value it passed. Freeing a compound
//! `LoroValue` drops the whole subtree it owns.
//!
//! Container-valued `LoroValue`s (the `Container` variant) are out of scope for Phase 1:
//! only [`loro_value_as_container`] (cid string) is provided; there is no typed constructor
//! for them, and the C++ bridge does not yet reconstruct them.

use crate::error::{set_last_error, LoroStatus};
use crate::value::{str_from_raw, LoroBytes};
use loro::LoroValue as InnerValue;
use std::os::raw::c_char;

/// Opaque, owned typed value handle wrapping a `loro::LoroValue`. Free with
/// [`loro_value_free`].
//
// Plain newtype (NO `#[repr(transparent)]`): cbindgen emits it as an opaque `typedef`, like
// the other handle types. `repr(transparent)` would make a broken alias.
pub struct LoroValue(InnerValue);

impl LoroValue {
    pub(crate) fn inner(&self) -> &InnerValue {
        &self.0
    }
}

/// Boxes an owned `loro::LoroValue` into a `*mut LoroValue`. Shared with the container /
/// ephemeral modules that hand typed values back to C.
pub(crate) fn into_raw(v: InnerValue) -> *mut LoroValue {
    Box::into_raw(Box::new(LoroValue(v)))
}

/// The dynamic kind of a [`LoroValue`]. Mirrors the variants of `loro::LoroValue`.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroValueType {
    LORO_VALUE_NULL = 0,
    LORO_VALUE_BOOL = 1,
    LORO_VALUE_DOUBLE = 2,
    LORO_VALUE_I64 = 3,
    LORO_VALUE_BINARY = 4,
    LORO_VALUE_STRING = 5,
    LORO_VALUE_LIST = 6,
    LORO_VALUE_MAP = 7,
    LORO_VALUE_CONTAINER = 8,
}

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

/// Creates a `null` value. Free with [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_new_null() -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), { into_raw(InnerValue::Null) })
}

/// Creates a boolean value. Free with [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_new_bool(value: bool) -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), { into_raw(InnerValue::Bool(value)) })
}

/// Creates a double (f64) value. Free with [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_new_double(value: f64) -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), { into_raw(InnerValue::Double(value)) })
}

/// Creates a 64-bit signed integer value. Free with [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_new_i64(value: i64) -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), { into_raw(InnerValue::I64(value)) })
}

/// Creates a string value from UTF-8 `(data, len)`. Returns null on invalid UTF-8. Free
/// with [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_new_string(data: *const c_char, len: usize) -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let s = match str_from_raw(data, len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        into_raw(InnerValue::String(s.into()))
    })
}

/// Creates a binary value from `(data, len)`. A null `data` is allowed only when `len == 0`.
/// Free with [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_new_binary(data: *const u8, len: usize) -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let bytes = if len == 0 {
            Vec::new()
        } else if data.is_null() {
            set_last_error("null binary pointer passed to loro-c-api");
            return std::ptr::null_mut();
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }.to_vec()
        };
        into_raw(InnerValue::Binary(bytes.into()))
    })
}

/// Creates an empty list value. Append children with [`loro_value_list_push`]. Free with
/// [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_new_list() -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        into_raw(InnerValue::List(Vec::<InnerValue>::new().into()))
    })
}

/// Appends a *clone* of `child` to the list `value`. The caller still owns `child`. Returns
/// `LORO_ERR_INVALID_ARG` if `value` is not a list.
#[no_mangle]
pub extern "C" fn loro_value_list_push(
    value: *mut LoroValue,
    child: *const LoroValue,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let value = match unsafe { value.as_mut() } {
            Some(r) => r,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        let child = deref_or!(child, LoroStatus::LORO_ERR_INVALID_ARG);
        let mut items = match &value.0 {
            InnerValue::List(l) => (**l).clone(),
            _ => {
                set_last_error("value is not a list");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        items.push(child.0.clone());
        value.0 = InnerValue::List(items.into());
        LoroStatus::LORO_OK
    })
}

/// Creates an empty map value. Insert entries with [`loro_value_map_insert`]. Free with
/// [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_new_map() -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        into_raw(InnerValue::Map(Vec::<(String, InnerValue)>::new().into()))
    })
}

/// Inserts a *clone* of `child` under UTF-8 key `(key, key_len)` in the map `value`. The
/// caller still owns `child`. Returns `LORO_ERR_INVALID_ARG` if `value` is not a map and
/// `LORO_ERR_UTF8` on a bad key.
#[no_mangle]
pub extern "C" fn loro_value_map_insert(
    value: *mut LoroValue,
    key: *const c_char,
    key_len: usize,
    child: *const LoroValue,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let value = match unsafe { value.as_mut() } {
            Some(r) => r,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        let child = deref_or!(child, LoroStatus::LORO_ERR_INVALID_ARG);
        let mut entries = match &value.0 {
            InnerValue::Map(m) => (**m).clone(),
            _ => {
                set_last_error("value is not a map");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        entries.insert(key.to_string(), child.0.clone());
        value.0 = InnerValue::Map(entries.into());
        LoroStatus::LORO_OK
    })
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

/// Returns the dynamic kind of `value`. Returns `LORO_VALUE_NULL` on a null handle.
#[no_mangle]
pub extern "C" fn loro_value_get_type(value: *const LoroValue) -> LoroValueType {
    ffi_guard!(LoroValueType::LORO_VALUE_NULL, {
        let value = deref_or!(value, LoroValueType::LORO_VALUE_NULL);
        match &value.0 {
            InnerValue::Null => LoroValueType::LORO_VALUE_NULL,
            InnerValue::Bool(_) => LoroValueType::LORO_VALUE_BOOL,
            InnerValue::Double(_) => LoroValueType::LORO_VALUE_DOUBLE,
            InnerValue::I64(_) => LoroValueType::LORO_VALUE_I64,
            InnerValue::Binary(_) => LoroValueType::LORO_VALUE_BINARY,
            InnerValue::String(_) => LoroValueType::LORO_VALUE_STRING,
            InnerValue::List(_) => LoroValueType::LORO_VALUE_LIST,
            InnerValue::Map(_) => LoroValueType::LORO_VALUE_MAP,
            InnerValue::Container(_) => LoroValueType::LORO_VALUE_CONTAINER,
        }
    })
}

/// Writes the boolean payload into `*out` and returns true; returns false (leaving `*out`
/// untouched) if `value` is not a boolean or any pointer is null.
#[no_mangle]
pub extern "C" fn loro_value_as_bool(value: *const LoroValue, out: *mut bool) -> bool {
    ffi_guard!(false, {
        let value = deref_or!(value, false);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        match &value.0 {
            InnerValue::Bool(b) => {
                unsafe { out.write(*b) };
                true
            }
            _ => false,
        }
    })
}

/// Writes the double payload into `*out` and returns true; returns false otherwise. Does NOT
/// coerce an `I64` to a double — that distinction is the whole point of the typed ABI.
#[no_mangle]
pub extern "C" fn loro_value_as_double(value: *const LoroValue, out: *mut f64) -> bool {
    ffi_guard!(false, {
        let value = deref_or!(value, false);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        match &value.0 {
            InnerValue::Double(d) => {
                unsafe { out.write(*d) };
                true
            }
            _ => false,
        }
    })
}

/// Writes the i64 payload into `*out` and returns true; returns false otherwise.
#[no_mangle]
pub extern "C" fn loro_value_as_i64(value: *const LoroValue, out: *mut i64) -> bool {
    ffi_guard!(false, {
        let value = deref_or!(value, false);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        match &value.0 {
            InnerValue::I64(i) => {
                unsafe { out.write(*i) };
                true
            }
            _ => false,
        }
    })
}

/// Writes the string payload (UTF-8, not nul-terminated) into `*out`. Returns
/// `LORO_ERR_INVALID_ARG` if `value` is not a string. `*out` is only written on `LORO_OK`;
/// free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_value_as_string(value: *const LoroValue, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let value = deref_or!(value, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match &value.0 {
            InnerValue::String(s) => {
                unsafe { out.write(LoroBytes::from_vec(s.as_bytes().to_vec())) };
                LoroStatus::LORO_OK
            }
            _ => {
                set_last_error("value is not a string");
                LoroStatus::LORO_ERR_INVALID_ARG
            }
        }
    })
}

/// Writes the binary payload into `*out`. Returns `LORO_ERR_INVALID_ARG` if `value` is not
/// binary. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_value_as_binary(value: *const LoroValue, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let value = deref_or!(value, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match &value.0 {
            InnerValue::Binary(b) => {
                unsafe { out.write(LoroBytes::from_vec((**b).clone())) };
                LoroStatus::LORO_OK
            }
            _ => {
                set_last_error("value is not binary");
                LoroStatus::LORO_ERR_INVALID_ARG
            }
        }
    })
}

/// Returns the number of elements in a list value, or 0 if `value` is not a list / is null.
#[no_mangle]
pub extern "C" fn loro_value_list_len(value: *const LoroValue) -> usize {
    ffi_guard!(0usize, {
        let value = deref_or!(value, 0usize);
        match &value.0 {
            InnerValue::List(l) => l.len(),
            _ => 0,
        }
    })
}

/// Returns a *clone* of the list element at `index` as a new owned `LoroValue*`, or null if
/// `value` is not a list or `index` is out of bounds. Free the result with
/// [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_list_get(value: *const LoroValue, index: usize) -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let value = deref_or!(value, std::ptr::null_mut());
        match &value.0 {
            InnerValue::List(l) => match l.get(index) {
                Some(v) => into_raw(v.clone()),
                None => {
                    set_last_error("list index out of bounds");
                    std::ptr::null_mut()
                }
            },
            _ => {
                set_last_error("value is not a list");
                std::ptr::null_mut()
            }
        }
    })
}

/// Returns the number of entries in a map value, or 0 if `value` is not a map / is null.
#[no_mangle]
pub extern "C" fn loro_value_map_len(value: *const LoroValue) -> usize {
    ffi_guard!(0usize, {
        let value = deref_or!(value, 0usize);
        match &value.0 {
            InnerValue::Map(m) => m.len(),
            _ => 0,
        }
    })
}

/// Writes the map's keys as a JSON array of strings into `*out` (keys are always strings, so
/// this is lossless). Returns `LORO_ERR_INVALID_ARG` if `value` is not a map. `*out` is only
/// written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_value_map_keys(value: *const LoroValue, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let value = deref_or!(value, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match &value.0 {
            InnerValue::Map(m) => {
                let keys: Vec<&String> = m.keys().collect();
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
            }
            _ => {
                set_last_error("value is not a map");
                LoroStatus::LORO_ERR_INVALID_ARG
            }
        }
    })
}

/// Returns a *clone* of the map entry at `(key, key_len)` as a new owned `LoroValue*`, or
/// null if `value` is not a map or the key is absent. Free the result with
/// [`loro_value_free`].
#[no_mangle]
pub extern "C" fn loro_value_map_get(
    value: *const LoroValue,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let value = deref_or!(value, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match &value.0 {
            InnerValue::Map(m) => match m.get(key) {
                Some(v) => into_raw(v.clone()),
                None => {
                    set_last_error("key not found in map value");
                    std::ptr::null_mut()
                }
            },
            _ => {
                set_last_error("value is not a map");
                std::ptr::null_mut()
            }
        }
    })
}

/// Writes the container id (a `cid:...` string) of a container-valued `LoroValue` into
/// `*out`. Returns `LORO_ERR_INVALID_ARG` if `value` is not a container. `*out` is only
/// written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_value_as_container(
    value: *const LoroValue,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let value = deref_or!(value, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match &value.0 {
            InnerValue::Container(cid) => {
                unsafe { out.write(LoroBytes::from_vec(cid.to_string().into_bytes())) };
                LoroStatus::LORO_OK
            }
            _ => {
                set_last_error("value is not a container");
                LoroStatus::LORO_ERR_INVALID_ARG
            }
        }
    })
}

/// Frees a `LoroValue` handle (recursively dropping any list/map subtree it owns). Passing
/// null is a no-op. Must be called at most once per handle.
#[no_mangle]
pub extern "C" fn loro_value_free(value: *mut LoroValue) {
    ffi_guard!((), {
        if !value.is_null() {
            unsafe {
                drop(Box::from_raw(value));
            }
        }
    });
}
