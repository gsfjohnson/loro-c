//! Error model for the C ABI.
//!
//! Every fallible function returns a [`LoroStatus`]. When it returns anything other than
//! `LORO_OK`, a human-readable detail message is stored in a thread-local slot that the
//! caller can read with [`loro_last_error_message`]. The message is owned by the thread
//! and remains valid until the next FFI call that records an error on the same thread.

use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::c_char;

/// Status / error code returned by fallible `loro-c-api` functions.
///
/// `LORO_OK` is guaranteed to be `0`, so callers may treat any non-zero value as failure.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroStatus {
    /// Success.
    LORO_OK = 0,
    /// A pointer argument was null, an index was out of bounds, or an argument was
    /// otherwise invalid.
    LORO_ERR_INVALID_ARG = 1,
    /// A requested container, version, or id could not be found.
    LORO_ERR_NOT_FOUND = 2,
    /// Failed to decode/import binary data (corruption, version mismatch, ...).
    LORO_ERR_DECODE = 3,
    /// Failed to encode/export.
    LORO_ERR_ENCODE = 4,
    /// A string argument was not valid UTF-8.
    LORO_ERR_UTF8 = 5,
    /// A Rust panic was caught at the FFI boundary.
    LORO_ERR_PANIC = 6,
    /// Any other error reported by the underlying loro library.
    LORO_ERR_OTHER = 7,
}

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

/// Records `msg` as the current thread's last-error message. Internal helper.
pub fn set_last_error(msg: impl Into<String>) {
    let msg = msg.into();
    // Replace interior nul bytes so CString::new cannot fail.
    let cleaned: String = msg.replace('\0', " ");
    let cstr = CString::new(cleaned).unwrap_or_default();
    LAST_ERROR.with(|slot| *slot.borrow_mut() = Some(cstr));
}

/// Clears the current thread's last-error message. Internal helper.
pub fn clear_last_error() {
    LAST_ERROR.with(|slot| *slot.borrow_mut() = None);
}

/// Maps a [`loro::LoroError`] to a [`LoroStatus`] and records its message.
pub fn record_loro_error(err: &loro::LoroError) -> LoroStatus {
    use loro::LoroError as E;
    let status = match err {
        E::DecodeVersionVectorError
        | E::DecodeError(_)
        | E::DecodeDataCorruptionError
        | E::DecodeChecksumMismatchError
        | E::IncompatibleFutureEncodingError(_) => LoroStatus::LORO_ERR_DECODE,
        E::NotFoundError(_) | E::FrontiersNotFound(_) => LoroStatus::LORO_ERR_NOT_FOUND,
        E::ArgErr(_) | E::OutOfBound { .. } => LoroStatus::LORO_ERR_INVALID_ARG,
        _ => LoroStatus::LORO_ERR_OTHER,
    };
    set_last_error(err.to_string());
    status
}

/// Maps a [`loro::LoroEncodeError`] to a [`LoroStatus`] and records its message.
pub fn record_encode_error(err: &loro::LoroEncodeError) -> LoroStatus {
    set_last_error(err.to_string());
    LoroStatus::LORO_ERR_ENCODE
}

/// Returns a pointer to the current thread's last-error message as a nul-terminated C
/// string, or `NULL` if no error has been recorded on this thread.
///
/// The returned pointer is owned by the library and is valid only until the next
/// `loro-c-api` call that records an error on the same thread. Do NOT free it. Copy the
/// string if you need to keep it.
#[no_mangle]
pub extern "C" fn loro_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| match &*slot.borrow() {
        Some(cstr) => cstr.as_ptr(),
        None => std::ptr::null(),
    })
}
