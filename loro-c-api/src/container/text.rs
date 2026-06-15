//! `LoroText` container handle and its M1 operations.
//!
//! A `LoroText*` is a strong co-owner of the underlying document state (see the lifetime
//! note in `doc.rs`/`loro.h`): it stays valid after the originating `LoroDoc*` is freed,
//! and the document state is released only once the doc and every container handle have
//! been freed. Free with [`loro_text_free`].

use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::value::{value_from_json, value_to_json_bytes, LoroBytes};
use loro::{TextDelta, UpdateOptions};
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

/// Writes this container's id (a string such as `cid:root-name:Text`) into `*out`. `*out`
/// is only written on `LORO_OK`; free it with `loro_bytes_free`. Pass the written string
/// to `loro_doc_subscribe` to subscribe to this container's events.
#[no_mangle]
pub extern "C" fn loro_text_id(text: *const LoroText, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let id = loro::ContainerTrait::id(text.inner());
        unsafe { out.write(LoroBytes::from_vec(id.to_string().into_bytes())) };
        LoroStatus::LORO_OK
    })
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

// ---------------------------------------------------------------------------
// G1: rich text — marks, deltas, in-place update, splice/slice, UTF-16, etc.
// ---------------------------------------------------------------------------

/// Indexing coordinate system for text positions. Mirrors the common subset of
/// `loro::cursor::PosType`.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroPosType {
    /// Index counts UTF-8 bytes.
    LORO_POS_BYTES = 0,
    /// Index counts Unicode code points.
    LORO_POS_UNICODE = 1,
    /// Index counts UTF-16 code units.
    LORO_POS_UTF16 = 2,
}

fn to_pos_type(p: LoroPosType) -> loro::cursor::PosType {
    match p {
        LoroPosType::LORO_POS_BYTES => loro::cursor::PosType::Bytes,
        LoroPosType::LORO_POS_UNICODE => loro::cursor::PosType::Unicode,
        LoroPosType::LORO_POS_UTF16 => loro::cursor::PosType::Utf16,
    }
}

/// Marks the range `[from, to)` (Unicode codepoint indices) with `key` = `value`, where
/// `value` is a JSON-encoded value (e.g. `true` for bold, or `"https://…"` for a link).
/// Use the doc-level style config to control how the mark expands at its boundaries.
#[no_mangle]
pub extern "C" fn loro_text_mark(
    text: *mut LoroText,
    from: usize,
    to: usize,
    key: *const c_char,
    key_len: usize,
    value: *const c_char,
    value_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        let value = match value_from_json(value, value_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match text.inner().mark(from..to, key, value) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Like [`loro_text_mark`] but `from`/`to` are UTF-8 byte indices.
#[no_mangle]
pub extern "C" fn loro_text_mark_utf8(
    text: *mut LoroText,
    from: usize,
    to: usize,
    key: *const c_char,
    key_len: usize,
    value: *const c_char,
    value_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        let value = match value_from_json(value, value_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match text.inner().mark_utf8(from..to, key, value) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Like [`loro_text_mark`] but `from`/`to` are UTF-16 code unit indices.
#[no_mangle]
pub extern "C" fn loro_text_mark_utf16(
    text: *mut LoroText,
    from: usize,
    to: usize,
    key: *const c_char,
    key_len: usize,
    value: *const c_char,
    value_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        let value = match value_from_json(value, value_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match text.inner().mark_utf16(from..to, key, value) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Removes the mark `key` over the range `[from, to)` (Unicode codepoint indices). Use the
/// same expand type that was used when marking. Cannot delete unmergeable annotations.
#[no_mangle]
pub extern "C" fn loro_text_unmark(
    text: *mut LoroText,
    from: usize,
    to: usize,
    key: *const c_char,
    key_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        match text.inner().unmark(from..to, key) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Like [`loro_text_unmark`] but `from`/`to` are UTF-16 code unit indices.
#[no_mangle]
pub extern "C" fn loro_text_unmark_utf16(
    text: *mut LoroText,
    from: usize,
    to: usize,
    key: *const c_char,
    key_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        match text.inner().unmark_utf16(from..to, key) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Writes the rich-text value as a JSON delta-with-attributes array (e.g.
/// `[{"insert":"Hello","attributes":{"bold":true}},{"insert":" world"}]`) into `*out`.
/// `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_text_get_richtext_value(
    text: *const LoroText,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let v = text.inner().get_richtext_value();
        let bytes = match value_to_json_bytes(&v) {
            Some(b) => b,
            None => return LoroStatus::LORO_ERR_ENCODE,
        };
        unsafe { out.write(LoroBytes::from_vec(bytes)) };
        LoroStatus::LORO_OK
    })
}

/// Writes the text in [Quill Delta](https://quilljs.com/docs/delta/) format as a JSON array
/// into `*out`. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_text_to_delta(text: *const LoroText, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let delta = text.inner().to_delta();
        let bytes = match serde_json::to_vec(&delta) {
            Ok(b) => b,
            Err(e) => {
                set_last_error(format!("failed to serialize delta to JSON: {e}"));
                return LoroStatus::LORO_ERR_ENCODE;
            }
        };
        unsafe { out.write(LoroBytes::from_vec(bytes)) };
        LoroStatus::LORO_OK
    })
}

/// Applies a [Quill Delta](https://quilljs.com/docs/delta/), supplied as a JSON array
/// `(delta, len)`, to the text container.
#[no_mangle]
pub extern "C" fn loro_text_apply_delta(
    text: *mut LoroText,
    delta: *const c_char,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if delta.is_null() && len != 0 {
            set_last_error("null delta pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(delta as *const u8, len) }
        };
        let parsed: Vec<TextDelta> = match serde_json::from_slice(bytes) {
            Ok(v) => v,
            Err(e) => {
                set_last_error(format!("failed to parse delta as JSON: {e}"));
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        match text.inner().apply_delta(&parsed) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Replaces the whole content of the text with `(s, len)` by computing and applying a diff.
/// `timeout_ms` bounds the diff computation; pass a negative value for no timeout.
/// `use_refined_diff` selects the slower-but-more-precise diff algorithm. Returns
/// `LORO_ERR_OTHER` if the computation times out.
#[no_mangle]
pub extern "C" fn loro_text_update(
    text: *mut LoroText,
    s: *const c_char,
    len: usize,
    timeout_ms: f64,
    use_refined_diff: bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let s = match str_from_raw(s, len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        let options = UpdateOptions {
            timeout_ms: if timeout_ms < 0.0 { None } else { Some(timeout_ms) },
            use_refined_diff,
        };
        match text.inner().update(s, options) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(_) => {
                set_last_error("text update timed out");
                LoroStatus::LORO_ERR_OTHER
            }
        }
    })
}

/// Like [`loro_text_update`] but uses a faster, line-based (less precise) diff.
#[no_mangle]
pub extern "C" fn loro_text_update_by_line(
    text: *mut LoroText,
    s: *const c_char,
    len: usize,
    timeout_ms: f64,
    use_refined_diff: bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let s = match str_from_raw(s, len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        let options = UpdateOptions {
            timeout_ms: if timeout_ms < 0.0 { None } else { Some(timeout_ms) },
            use_refined_diff,
        };
        match text.inner().update_by_line(s, options) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(_) => {
                set_last_error("text update timed out");
                LoroStatus::LORO_ERR_OTHER
            }
        }
    })
}

/// Deletes `len` Unicode codepoints at `pos`, then inserts `(s, s_len)` there. Writes the
/// removed text (UTF-8) into `*out`. `*out` is only written on `LORO_OK`; free it with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_text_splice(
    text: *mut LoroText,
    pos: usize,
    len: usize,
    s: *const c_char,
    s_len: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let s = match str_from_raw(s, s_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        match text.inner().splice(pos, len, s) {
            Ok(removed) => {
                unsafe { out.write(LoroBytes::from_vec(removed.into_bytes())) };
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Writes the substring over the Unicode codepoint range `[start, end)` (UTF-8) into
/// `*out`. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_text_slice(
    text: *const LoroText,
    start: usize,
    end: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match text.inner().slice(start, end) {
            Ok(s) => {
                unsafe { out.write(LoroBytes::from_vec(s.into_bytes())) };
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Writes the single Unicode character at codepoint index `pos` (encoded as 1–4 UTF-8
/// bytes) into `*out`. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_text_char_at(
    text: *const LoroText,
    pos: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match text.inner().char_at(pos) {
            Ok(c) => {
                let mut buf = [0u8; 4];
                let encoded = c.encode_utf8(&mut buf);
                unsafe { out.write(LoroBytes::from_vec(encoded.as_bytes().to_vec())) };
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Inserts the UTF-8 string `(s, len)` at UTF-16 code unit index `pos`.
#[no_mangle]
pub extern "C" fn loro_text_insert_utf16(
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
        match text.inner().insert_utf16(pos, s) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Deletes `len` UTF-16 code units starting at UTF-16 code unit index `pos`.
#[no_mangle]
pub extern "C" fn loro_text_delete_utf16(
    text: *mut LoroText,
    pos: usize,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        match text.inner().delete_utf16(pos, len) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Returns the length of the text in UTF-16 code units. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_text_len_utf16(text: *const LoroText) -> usize {
    ffi_guard!(0usize, {
        let text = deref_or!(text, 0usize);
        text.inner().len_utf16()
    })
}

/// Converts `index` from the `from` coordinate system to the `to` coordinate system,
/// writing the result into `*out`. `*out` is only written on `LORO_OK`. Returns
/// `LORO_ERR_INVALID_ARG` if the position is out of bounds or the conversion is
/// unsupported.
#[no_mangle]
pub extern "C" fn loro_text_convert_pos(
    text: *const LoroText,
    index: usize,
    from: LoroPosType,
    to: LoroPosType,
    out: *mut usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match text
            .inner()
            .convert_pos(index, to_pos_type(from), to_pos_type(to))
        {
            Some(p) => {
                unsafe { out.write(p) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("position is out of bounds or conversion is unsupported");
                LoroStatus::LORO_ERR_INVALID_ARG
            }
        }
    })
}

/// Appends the UTF-8 string `(s, len)` to the end of the text container.
#[no_mangle]
pub extern "C" fn loro_text_push_str(
    text: *mut LoroText,
    s: *const c_char,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let text = deref_or!(text, LoroStatus::LORO_ERR_INVALID_ARG);
        let s = match str_from_raw(s, len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        match text.inner().push_str(s) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}
