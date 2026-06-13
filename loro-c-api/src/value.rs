//! Owned byte buffers returned across the FFI boundary, plus the shared input/output
//! conversion helpers used by the container modules.
//!
//! Binary and string outputs are returned as a [`LoroBytes`] that the caller must release
//! with [`loro_bytes_free`]. The buffer is NOT guaranteed to be nul-terminated (string
//! outputs carry their byte length in `len`); the C++ wrapper turns it into a
//! `std::string` / `std::vector<uint8_t>` using `len`.
//!
//! Container values cross the boundary as JSON: inputs via [`value_from_json`], outputs
//! via [`value_to_json_bytes`]. `loro::LoroValue` round-trips cleanly through `serde_json`
//! (its human-readable serde representation is plain JSON).

use crate::error::set_last_error;
use std::os::raw::c_char;

/// An owned, heap-allocated byte buffer handed to the caller.
///
/// Free it exactly once with [`loro_bytes_free`]. `data` may be null only when `len`
/// and `cap` are both `0` (an empty buffer); in that case `loro_bytes_free` is a no-op.
#[repr(C)]
pub struct LoroBytes {
    /// Pointer to the bytes. Owned by the buffer; freed by `loro_bytes_free`.
    pub data: *mut u8,
    /// Number of valid bytes in `data`.
    pub len: usize,
    /// Allocation capacity backing `data` (needed to reconstruct the Rust `Vec`).
    pub cap: usize,
}

impl LoroBytes {
    /// Builds a `LoroBytes` that takes ownership of `v`'s allocation.
    pub fn from_vec(mut v: Vec<u8>) -> LoroBytes {
        let data = v.as_mut_ptr();
        let len = v.len();
        let cap = v.capacity();
        std::mem::forget(v);
        LoroBytes { data, len, cap }
    }

    /// An empty buffer.
    pub fn empty() -> LoroBytes {
        LoroBytes {
            data: std::ptr::null_mut(),
            len: 0,
            cap: 0,
        }
    }
}

/// Frees a [`LoroBytes`] previously returned by this library. Passing an
/// all-zero/empty buffer is a no-op. Must be called at most once per buffer.
#[no_mangle]
pub extern "C" fn loro_bytes_free(bytes: LoroBytes) {
    ffi_guard!((), {
        if !bytes.data.is_null() {
            // Reconstruct the original Vec and let it drop.
            unsafe {
                drop(Vec::from_raw_parts(bytes.data, bytes.len, bytes.cap));
            }
        }
    });
}

/// Interprets `(ptr, len)` as a UTF-8 string slice; records an error and returns `None`
/// on a null pointer or invalid UTF-8. Shared by the container modules.
pub(crate) fn str_from_raw<'a>(ptr: *const c_char, len: usize) -> Option<&'a str> {
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

/// Parses `(ptr, len)` as a JSON-encoded [`loro::LoroValue`]; records an error and returns
/// `None` on a null pointer or malformed JSON.
pub(crate) fn value_from_json(ptr: *const c_char, len: usize) -> Option<loro::LoroValue> {
    if ptr.is_null() && len != 0 {
        set_last_error("null value pointer passed to loro-c-api");
        return None;
    }
    let bytes = if len == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(ptr as *const u8, len) }
    };
    match serde_json::from_slice::<loro::LoroValue>(bytes) {
        Ok(v) => Some(v),
        Err(e) => {
            set_last_error(format!("failed to parse value as JSON: {e}"));
            None
        }
    }
}

/// Serializes a [`loro::LoroValue`] to JSON bytes; records an error and returns `None`
/// on a serialization failure.
pub(crate) fn value_to_json_bytes(value: &loro::LoroValue) -> Option<Vec<u8>> {
    match serde_json::to_vec(value) {
        Ok(bytes) => Some(bytes),
        Err(e) => {
            set_last_error(format!("failed to serialize value to JSON: {e}"));
            None
        }
    }
}
