//! Cursors (G2): stable positions that survive concurrent edits.
//!
//! A [`LoroCursor`] anchors to an element's id rather than a numeric index, so it keeps
//! pointing at the "same" place after remote peers insert or delete around it. Obtain one
//! from a text/list/movable-list container ([`loro_text_get_cursor`] and friends), encode it
//! for transport ([`loro_cursor_encode`] / [`loro_cursor_decode`]), and resolve it back to a
//! current absolute position with [`loro_doc_get_cursor_pos`].
//!
//! Like the other opaque handles, a `LoroCursor*` is an owned `Box` pointer freed with
//! [`loro_cursor_free`]. Unlike the container handles it does NOT co-own the document state;
//! it is a small standalone value.

use crate::container::list::LoroList;
use crate::container::movable_list::LoroMovableList;
use crate::container::text::LoroText;
use crate::doc::LoroDoc;
use crate::error::{set_last_error, LoroStatus};
use crate::value::LoroBytes;
use loro::cursor::{Cursor, Side};

/// Which side of an element a cursor (or resolved position) is anchored to. Mirrors
/// `loro::cursor::Side`. Used both to create a cursor and to report where a deleted anchor
/// resolved to. The numeric values match upstream (`Left = -1`, `Middle = 0`, `Right = 1`).
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroSide {
    /// Anchored to the left of the element.
    LORO_SIDE_LEFT = -1,
    /// Anchored at the element itself (the default).
    LORO_SIDE_MIDDLE = 0,
    /// Anchored to the right of the element.
    LORO_SIDE_RIGHT = 1,
}

fn to_side(s: LoroSide) -> Side {
    match s {
        LoroSide::LORO_SIDE_LEFT => Side::Left,
        LoroSide::LORO_SIDE_MIDDLE => Side::Middle,
        LoroSide::LORO_SIDE_RIGHT => Side::Right,
    }
}

fn from_side(s: Side) -> LoroSide {
    match s {
        Side::Left => LoroSide::LORO_SIDE_LEFT,
        Side::Middle => LoroSide::LORO_SIDE_MIDDLE,
        Side::Right => LoroSide::LORO_SIDE_RIGHT,
    }
}

/// The resolved absolute position of a cursor against a document. Plain-old-data, written
/// into the caller's `out` by [`loro_doc_get_cursor_pos`].
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct LoroPosQueryResult {
    /// Which side of `abs_pos` the cursor resolved to.
    pub side: LoroSide,
    /// The current absolute position (for text, a Unicode codepoint index).
    pub abs_pos: usize,
}

/// Opaque handle to a [`loro::cursor::Cursor`]. Free with [`loro_cursor_free`].
pub struct LoroCursor(Cursor);

impl LoroCursor {
    fn inner(&self) -> &Cursor {
        &self.0
    }
}

/// Frees a cursor handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_cursor_free(cursor: *mut LoroCursor) {
    ffi_guard!((), {
        if !cursor.is_null() {
            unsafe {
                drop(Box::from_raw(cursor));
            }
        }
    });
}

/// Encodes the cursor into a compact, transportable byte buffer written into `*out`. `*out`
/// is only written on `LORO_OK`; free it with `loro_bytes_free`. Round-trips through
/// [`loro_cursor_decode`].
#[no_mangle]
pub extern "C" fn loro_cursor_encode(cursor: *const LoroCursor, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let cursor = deref_or!(cursor, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(cursor.inner().encode())) };
        LoroStatus::LORO_OK
    })
}

/// Decodes a cursor previously produced by [`loro_cursor_encode`]. Returns null on a decode
/// error. Release the returned handle with [`loro_cursor_free`].
#[no_mangle]
pub extern "C" fn loro_cursor_decode(data: *const u8, len: usize) -> *mut LoroCursor {
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
        match Cursor::decode(bytes) {
            Ok(c) => Box::into_raw(Box::new(LoroCursor(c))),
            Err(e) => {
                set_last_error(format!("failed to decode cursor: {e}"));
                std::ptr::null_mut()
            }
        }
    })
}

/// Returns a cursor anchored at codepoint index `pos` (on `side`) of the text container, or
/// null if the position cannot be anchored (e.g. an empty container with `pos` out of range).
/// Release the returned handle with [`loro_cursor_free`].
#[no_mangle]
pub extern "C" fn loro_text_get_cursor(
    text: *const LoroText,
    pos: usize,
    side: LoroSide,
) -> *mut LoroCursor {
    ffi_guard!(std::ptr::null_mut(), {
        let text = deref_or!(text, std::ptr::null_mut());
        match text.inner().get_cursor(pos, to_side(side)) {
            Some(c) => Box::into_raw(Box::new(LoroCursor(c))),
            None => {
                set_last_error("could not create a cursor at the given position");
                std::ptr::null_mut()
            }
        }
    })
}

/// Returns a cursor anchored at index `pos` (on `side`) of the list container, or null if the
/// position cannot be anchored. Release the returned handle with [`loro_cursor_free`].
#[no_mangle]
pub extern "C" fn loro_list_get_cursor(
    list: *const LoroList,
    pos: usize,
    side: LoroSide,
) -> *mut LoroCursor {
    ffi_guard!(std::ptr::null_mut(), {
        let list = deref_or!(list, std::ptr::null_mut());
        match list.inner().get_cursor(pos, to_side(side)) {
            Some(c) => Box::into_raw(Box::new(LoroCursor(c))),
            None => {
                set_last_error("could not create a cursor at the given position");
                std::ptr::null_mut()
            }
        }
    })
}

/// Returns a cursor anchored at index `pos` (on `side`) of the movable-list container, or null
/// if the position cannot be anchored. Release the returned handle with [`loro_cursor_free`].
#[no_mangle]
pub extern "C" fn loro_movable_list_get_cursor(
    list: *const LoroMovableList,
    pos: usize,
    side: LoroSide,
) -> *mut LoroCursor {
    ffi_guard!(std::ptr::null_mut(), {
        let list = deref_or!(list, std::ptr::null_mut());
        match list.inner().get_cursor(pos, to_side(side)) {
            Some(c) => Box::into_raw(Box::new(LoroCursor(c))),
            None => {
                set_last_error("could not create a cursor at the given position");
                std::ptr::null_mut()
            }
        }
    })
}

/// Resolves `cursor` against the current state of `doc`, writing the absolute position and
/// resolved side into `*out`. `*out` is only written on `LORO_OK`. Returns
/// `LORO_ERR_NOT_FOUND` if the relative position cannot be located (the container was deleted,
/// the id is unknown, or the relevant history was cleared).
#[no_mangle]
pub extern "C" fn loro_doc_get_cursor_pos(
    doc: *const LoroDoc,
    cursor: *const LoroCursor,
    out: *mut LoroPosQueryResult,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let cursor = deref_or!(cursor, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match doc.inner().get_cursor_pos(cursor.inner()) {
            Ok(result) => {
                unsafe {
                    out.write(LoroPosQueryResult {
                        side: from_side(result.current.side),
                        abs_pos: result.current.pos,
                    })
                };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(e.to_string());
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}
