//! `LoroFractionalIndex` — a compact, comparable position key used to order list/tree
//! elements without renumbering neighbours (M4).
//!
//! A fractional index encodes a position *between* two others: given a `lower` and `upper`
//! index you can always mint a new one strictly between them. Indices compare bytewise, so
//! they can be sorted directly. They round-trip as raw bytes or as a hex string.
//!
//! Each handle is an owned `Box`; free it with [`loro_fractional_index_free`].

use crate::error::{set_last_error, LoroStatus};
use crate::value::{str_from_raw, LoroBytes};
use loro::FractionalIndex;

/// Opaque handle to a [`loro::FractionalIndex`].
pub struct LoroFractionalIndex(FractionalIndex);

impl LoroFractionalIndex {
    fn inner(&self) -> &FractionalIndex {
        &self.0
    }
}

/// Creates the default (smallest) fractional index. Release with
/// [`loro_fractional_index_free`].
#[no_mangle]
pub extern "C" fn loro_fractional_index_default() -> *mut LoroFractionalIndex {
    ffi_guard!(std::ptr::null_mut(), {
        Box::into_raw(Box::new(LoroFractionalIndex(FractionalIndex::default())))
    })
}

/// Frees a fractional index handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_fractional_index_free(fi: *mut LoroFractionalIndex) {
    ffi_guard!((), {
        if !fi.is_null() {
            unsafe {
                drop(Box::from_raw(fi));
            }
        }
    });
}

/// Builds a fractional index strictly between `lower` and `upper`. Either bound may be null
/// to mean "unbounded" (so a null/null pair yields the default index, a non-null `lower`
/// with null `upper` yields one after `lower`, etc.). Returns null if no index exists
/// between the two (e.g. they are equal). Release with [`loro_fractional_index_free`].
#[no_mangle]
pub extern "C" fn loro_fractional_index_between(
    lower: *const LoroFractionalIndex,
    upper: *const LoroFractionalIndex,
) -> *mut LoroFractionalIndex {
    ffi_guard!(std::ptr::null_mut(), {
        let lower = unsafe { lower.as_ref() }.map(|l| l.inner());
        let upper = unsafe { upper.as_ref() }.map(|u| u.inner());
        match FractionalIndex::new(lower, upper) {
            Some(fi) => Box::into_raw(Box::new(LoroFractionalIndex(fi))),
            None => {
                set_last_error("no fractional index exists between the given bounds");
                std::ptr::null_mut()
            }
        }
    })
}

/// Builds a fractional index from raw bytes (as produced by
/// [`loro_fractional_index_to_bytes`]). Release with [`loro_fractional_index_free`].
#[no_mangle]
pub extern "C" fn loro_fractional_index_from_bytes(
    data: *const u8,
    len: usize,
) -> *mut LoroFractionalIndex {
    ffi_guard!(std::ptr::null_mut(), {
        if data.is_null() && len != 0 {
            set_last_error("null data pointer passed to loro-c-api");
            return std::ptr::null_mut();
        }
        let bytes = if len == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }.to_vec()
        };
        Box::into_raw(Box::new(LoroFractionalIndex(FractionalIndex::from_bytes(
            bytes,
        ))))
    })
}

/// Builds a fractional index from a hex string `(str, str_len)` (as produced by
/// [`loro_fractional_index_to_string`]). Release with [`loro_fractional_index_free`].
#[no_mangle]
pub extern "C" fn loro_fractional_index_from_string(
    str: *const std::os::raw::c_char,
    str_len: usize,
) -> *mut LoroFractionalIndex {
    ffi_guard!(std::ptr::null_mut(), {
        let s = match str_from_raw(str, str_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        Box::into_raw(Box::new(LoroFractionalIndex(
            FractionalIndex::from_hex_string(s),
        )))
    })
}

/// Writes the raw bytes of the fractional index into `*out`. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_fractional_index_to_bytes(
    fi: *const LoroFractionalIndex,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let fi = deref_or!(fi, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(fi.inner().as_bytes().to_vec())) };
        LoroStatus::LORO_OK
    })
}

/// Writes the hex-string form of the fractional index into `*out`. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_fractional_index_to_string(
    fi: *const LoroFractionalIndex,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let fi = deref_or!(fi, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(fi.inner().to_string().into_bytes())) };
        LoroStatus::LORO_OK
    })
}

/// Compares two fractional indices, writing `-1` (a < b), `0` (equal), or `1` (a > b) into
/// `*out`. `*out` is only written on `LORO_OK`.
#[no_mangle]
pub extern "C" fn loro_fractional_index_compare(
    a: *const LoroFractionalIndex,
    b: *const LoroFractionalIndex,
    out: *mut i32,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let a = deref_or!(a, LoroStatus::LORO_ERR_INVALID_ARG);
        let b = deref_or!(b, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(a.inner().cmp(b.inner()) as i32) };
        LoroStatus::LORO_OK
    })
}
