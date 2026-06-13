//! `LoroText` container handle and its M1 operations.
//!
//! A `LoroText*` is a strong co-owner of the underlying document state (see the lifetime
//! note in `doc.rs`/`loro.h`): it stays valid after the originating `LoroDoc*` is freed,
//! and the document state is released only once the doc and every container handle have
//! been freed. Free with [`loro_text_free`].

use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::value::LoroBytes;
use std::os::raw::c_char;

/// Opaque handle to a Loro text container.
pub struct LoroText(loro::LoroText);

impl LoroText {
    pub(crate) fn from_inner(t: loro::LoroText) -> LoroText {
        LoroText(t)
    }

    fn inner(&self) -> &loro::LoroText {
        &self.0
    }
}

/// Interprets `(ptr, len)` as a UTF-8 string slice; records an error and returns `None`
/// on a null pointer or invalid UTF-8.
fn str_from_raw<'a>(ptr: *const c_char, len: usize) -> Option<&'a str> {
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

/// Frees a text handle. Passing null is a no-op. Safe to call before or after the
/// originating `LoroDoc*` is freed.
#[no_mangle]
pub extern "C" fn loro_text_free(text: *mut LoroText) {
    ffi_guard!((), {
        if !text.is_null() {
            unsafe {
                drop(Box::from_raw(text));
            }
        }
    });
}

/// Inserts the UTF-8 string `(s, len)` at Unicode codepoint index `pos`.
#[no_mangle]
pub extern "C" fn loro_text_insert(
    text: *mut LoroText,
    pos: usize,
    s: *const c_char,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let s = match str_from_raw(s, len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        match text.inner().insert(pos, s) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Inserts the UTF-8 string `(s, len)` at UTF-8 byte index `pos`.
#[no_mangle]
pub extern "C" fn loro_text_insert_utf8(
    text: *mut LoroText,
    pos: usize,
    s: *const c_char,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let s = match str_from_raw(s, len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        match text.inner().insert_utf8(pos, s) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Deletes `len` Unicode codepoints starting at codepoint index `pos`.
#[no_mangle]
pub extern "C" fn loro_text_delete(text: *mut LoroText, pos: usize, len: usize) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        match text.inner().delete(pos, len) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Deletes `len` UTF-8 bytes starting at byte index `pos`.
#[no_mangle]
pub extern "C" fn loro_text_delete_utf8(text: *mut LoroText, pos: usize, len: usize) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        match text.inner().delete_utf8(pos, len) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Returns the length of the text in Unicode codepoints. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_text_len_unicode(text: *const LoroText) -> usize {
    ffi_guard!(0usize, {
        let text = deref_or!(text, 0usize);
        text.inner().len_unicode()
    })
}

/// Returns the length of the text in UTF-8 bytes. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_text_len_utf8(text: *const LoroText) -> usize {
    ffi_guard!(0usize, {
        let text = deref_or!(text, 0usize);
        text.inner().len_utf8()
    })
}

/// Returns `true` if the text is empty (also returns `true`, with an error recorded, on
/// a null handle).
#[no_mangle]
pub extern "C" fn loro_text_is_empty(text: *const LoroText) -> bool {
    ffi_guard!(true, {
        let text = deref_or!(text, true);
        text.inner().is_empty()
    })
}

/// Writes the text's current content as UTF-8 bytes (NOT nul-terminated; use `len`) into
/// `*out`. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_text_to_string(text: *const LoroText, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let s = text.inner().to_string();
        unsafe { out.write(LoroBytes::from_vec(s.into_bytes())) };
        LoroStatus::LORO_OK
    })
}
