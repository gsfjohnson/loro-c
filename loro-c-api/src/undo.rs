//! `LoroUndoManager` (M4) — local undo/redo bound to a document's peer.
//!
//! An undo manager records the local peer's commits as checkpoints and lets you walk back
//! and forth through them. It is *local-only*: it reverts this peer's edits, not remote
//! ones (use time travel — `loro_doc_checkout` — for global rollback).
//!
//! Two optional listeners mirror `loro`'s `on_push` / `on_pop`:
//! - **on_push** fires when a new item is pushed onto the undo (or redo) stack. The callback
//!   may attach a JSON metadata value to the item via [`loro_undo_meta_set_value_json`]
//!   (e.g. to remember a cursor/selection); it also receives the change's [`LoroDiffEvent`]
//!   when the push came from a local edit (null otherwise).
//! - **on_pop** fires when an item is popped (during undo/redo); the callback reads back the
//!   metadata it stored via [`loro_undo_meta_get_value_json`].
//!
//! Free the manager with [`loro_undo_manager_free`]. Keep the document's peer id stable
//! while a manager is in use.

use crate::cursor::{from_side, LoroCursor, LoroSide};
use crate::doc::LoroDoc;
use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::event::LoroDiffEvent;
use crate::value::{str_from_raw, value_from_json, LoroBytes};
use crate::value_typed;
use loro::event::DiffEvent;
use loro::{CounterSpan, UndoItemMeta, UndoManager, UndoOrRedo};
use std::os::raw::{c_char, c_void};

/// Opaque handle to a [`loro::UndoManager`]. Free with [`loro_undo_manager_free`].
pub struct LoroUndoManager(UndoManager);

/// Whether a pushed/popped item belongs to the undo or the redo stack. Mirrors
/// `loro::UndoOrRedo`.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroUndoOrRedo {
    LORO_UNDO = 0,
    LORO_REDO = 1,
}

impl LoroUndoOrRedo {
    fn from_loro(u: UndoOrRedo) -> LoroUndoOrRedo {
        match u {
            UndoOrRedo::Undo => LoroUndoOrRedo::LORO_UNDO,
            UndoOrRedo::Redo => LoroUndoOrRedo::LORO_REDO,
        }
    }
}

/// A half-open span of op counters `[start, end)` for the change being pushed/popped.
/// Mirrors `loro::CounterSpan`.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LoroCounterSpan {
    pub start: i32,
    pub end: i32,
}

impl LoroCounterSpan {
    fn from_loro(s: CounterSpan) -> LoroCounterSpan {
        LoroCounterSpan {
            start: s.start,
            end: s.end,
        }
    }
}

/// Opaque, **callback-scoped** handle to an undo item's metadata, passed to the on_push /
/// on_pop callbacks. A `*mut LoroUndoMeta` always points to a real `loro::UndoItemMeta`.
#[allow(dead_code)]
pub struct LoroUndoMeta(UndoItemMeta);

/// The on_push listener. `invoke` is called with the stack kind, the change's counter span,
/// the originating [`LoroDiffEvent`] (`null` unless it was a local edit), a writable
/// [`LoroUndoMeta`] for attaching metadata, and the opaque `user_data`. `free_user_data`
/// (may be null) runs once when the listener is replaced or the manager is freed.
#[repr(C)]
pub struct LoroUndoOnPush {
    pub invoke: extern "C" fn(
        kind: LoroUndoOrRedo,
        span: LoroCounterSpan,
        event: *const LoroDiffEvent,
        meta: *mut LoroUndoMeta,
        user_data: *mut c_void,
    ),
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

unsafe impl Send for LoroUndoOnPush {}
unsafe impl Sync for LoroUndoOnPush {}
impl Drop for LoroUndoOnPush {
    fn drop(&mut self) {
        if let Some(free) = self.free_user_data {
            free(self.user_data);
        }
    }
}

/// The on_pop listener. `invoke` is called with the stack kind, the change's counter span, a
/// read-only [`LoroUndoMeta`] (carrying whatever on_push stored), and the opaque
/// `user_data`. `free_user_data` (may be null) runs once when the listener is replaced or the
/// manager is freed.
#[repr(C)]
pub struct LoroUndoOnPop {
    pub invoke: extern "C" fn(
        kind: LoroUndoOrRedo,
        span: LoroCounterSpan,
        meta: *const LoroUndoMeta,
        user_data: *mut c_void,
    ),
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

unsafe impl Send for LoroUndoOnPop {}
unsafe impl Sync for LoroUndoOnPop {}
impl Drop for LoroUndoOnPop {
    fn drop(&mut self) {
        if let Some(free) = self.free_user_data {
            free(self.user_data);
        }
    }
}

/// Creates an undo manager bound to `doc`'s current peer. Release with
/// [`loro_undo_manager_free`].
#[no_mangle]
pub extern "C" fn loro_undo_manager_new(doc: *const LoroDoc) -> *mut LoroUndoManager {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        Box::into_raw(Box::new(LoroUndoManager(UndoManager::new(doc.inner()))))
    })
}

/// Frees an undo manager handle (running any listeners' `free_user_data`). Passing null is a
/// no-op.
#[no_mangle]
pub extern "C" fn loro_undo_manager_free(um: *mut LoroUndoManager) {
    ffi_guard!((), {
        if !um.is_null() {
            unsafe {
                drop(Box::from_raw(um));
            }
        }
    });
}

/// Helper: dereference a `*mut LoroUndoManager` to `&mut`, recording an error on null.
fn um_mut<'a>(um: *mut LoroUndoManager) -> Option<&'a mut LoroUndoManager> {
    match unsafe { um.as_mut() } {
        Some(u) => Some(u),
        None => {
            set_last_error("null pointer passed to loro-c-api");
            None
        }
    }
}

/// Undoes the last recorded change. Writes whether an undo actually happened into `*applied`
/// (may be null).
#[no_mangle]
pub extern "C" fn loro_undo_manager_undo(
    um: *mut LoroUndoManager,
    applied: *mut bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match um.0.undo() {
            Ok(did) => {
                if !applied.is_null() {
                    unsafe { applied.write(did) };
                }
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Redoes the last undone change. Writes whether a redo actually happened into `*applied`
/// (may be null).
#[no_mangle]
pub extern "C" fn loro_undo_manager_redo(
    um: *mut LoroUndoManager,
    applied: *mut bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match um.0.redo() {
            Ok(did) => {
                if !applied.is_null() {
                    unsafe { applied.write(did) };
                }
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Returns whether there is anything to undo. Returns `false` on a null handle.
#[no_mangle]
pub extern "C" fn loro_undo_manager_can_undo(um: *const LoroUndoManager) -> bool {
    ffi_guard!(false, {
        let um = deref_or!(um, false);
        um.0.can_undo()
    })
}

/// Returns whether there is anything to redo. Returns `false` on a null handle.
#[no_mangle]
pub extern "C" fn loro_undo_manager_can_redo(um: *const LoroUndoManager) -> bool {
    ffi_guard!(false, {
        let um = deref_or!(um, false);
        um.0.can_redo()
    })
}

/// Returns the number of items currently on the undo stack. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_undo_manager_undo_count(um: *const LoroUndoManager) -> usize {
    ffi_guard!(0usize, {
        let um = deref_or!(um, 0usize);
        um.0.undo_count()
    })
}

/// Returns the number of items currently on the redo stack. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_undo_manager_redo_count(um: *const LoroUndoManager) -> usize {
    ffi_guard!(0usize, {
        let um = deref_or!(um, 0usize);
        um.0.redo_count()
    })
}

/// Returns the peer id this undo manager is bound to. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_undo_manager_peer(um: *const LoroUndoManager) -> u64 {
    ffi_guard!(0u64, {
        let um = deref_or!(um, 0u64);
        um.0.peer()
    })
}

/// Writes the metadata value attached to the top item of the undo stack (whatever an on_push
/// listener stored via [`loro_undo_meta_set_value_json`]) as JSON into `*out`. Returns `true`
/// (and writes `*out`) when there is a top undo item; returns `false` (leaving `*out` untouched)
/// on a null handle, an empty undo stack, or a serialization error. Free `*out` with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_undo_manager_top_undo_value_json(
    um: *const LoroUndoManager,
    out: *mut LoroBytes,
) -> bool {
    ffi_guard!(false, {
        let um = deref_or!(um, false);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        top_value_json(um.0.top_undo_value(), out)
    })
}

/// Writes the metadata value attached to the top item of the redo stack as JSON into `*out`.
/// Returns `true` (and writes `*out`) when there is a top redo item; returns `false` (leaving
/// `*out` untouched) on a null handle, an empty redo stack, or a serialization error. Free
/// `*out` with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_undo_manager_top_redo_value_json(
    um: *const LoroUndoManager,
    out: *mut LoroBytes,
) -> bool {
    ffi_guard!(false, {
        let um = deref_or!(um, false);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        top_value_json(um.0.top_redo_value(), out)
    })
}

/// Shared body for `loro_undo_manager_top_{undo,redo}_value_json`: serializes an optional
/// [`loro::LoroValue`] to JSON and writes it into `*out` (assumed non-null). Returns whether a
/// value was present and successfully encoded.
fn top_value_json(value: Option<loro::LoroValue>, out: *mut LoroBytes) -> bool {
    let value = match value {
        Some(v) => v,
        None => return false,
    };
    match serde_json::to_vec(&value) {
        Ok(bytes) => {
            unsafe { out.write(LoroBytes::from_vec(bytes)) };
            true
        }
        Err(e) => {
            set_last_error(format!("failed to serialize undo value: {e}"));
            false
        }
    }
}

/// Returns the metadata value attached to the top item of the undo stack as an owned typed
/// `LoroValue*` (RESHAPE Phase 4), or null when the undo stack is empty or the handle is null.
/// Typed counterpart to [`loro_undo_manager_top_undo_value_json`]: it preserves binary,
/// integer-valued doubles, and the value/container distinction. A present-but-`Null` value is
/// returned as a non-null `LoroValue*` wrapping `Null`, so a null return unambiguously means
/// "no top undo item". Free the result with `loro_value_free`.
#[no_mangle]
pub extern "C" fn loro_undo_manager_top_undo_value(
    um: *const LoroUndoManager,
) -> *mut value_typed::LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let um = deref_or!(um, std::ptr::null_mut());
        match um.0.top_undo_value() {
            Some(v) => value_typed::into_raw(v),
            None => std::ptr::null_mut(),
        }
    })
}

/// Returns the metadata value attached to the top item of the redo stack as an owned typed
/// `LoroValue*` (RESHAPE Phase 4), or null when the redo stack is empty or the handle is null.
/// Typed counterpart to [`loro_undo_manager_top_redo_value_json`]; see
/// [`loro_undo_manager_top_undo_value`] for the null-vs-`Null` distinction. Free the result
/// with `loro_value_free`.
#[no_mangle]
pub extern "C" fn loro_undo_manager_top_redo_value(
    um: *const LoroUndoManager,
) -> *mut value_typed::LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let um = deref_or!(um, std::ptr::null_mut());
        match um.0.top_redo_value() {
            Some(v) => value_typed::into_raw(v),
            None => std::ptr::null_mut(),
        }
    })
}

/// Records a checkpoint so subsequent edits become a new, separately-undoable item.
#[no_mangle]
pub extern "C" fn loro_undo_manager_record_new_checkpoint(
    um: *mut LoroUndoManager,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match um.0.record_new_checkpoint() {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Sets the merge interval in milliseconds: consecutive local edits closer together than
/// this are merged into one undo item (default 0 = no merge).
#[no_mangle]
pub extern "C" fn loro_undo_manager_set_merge_interval(
    um: *mut LoroUndoManager,
    interval_ms: i64,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        um.0.set_merge_interval(interval_ms);
        LoroStatus::LORO_OK
    })
}

/// Sets the maximum number of undo steps to retain (default 100).
#[no_mangle]
pub extern "C" fn loro_undo_manager_set_max_undo_steps(
    um: *mut LoroUndoManager,
    steps: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        um.0.set_max_undo_steps(steps);
        LoroStatus::LORO_OK
    })
}

/// Adds an origin prefix to exclude from undo recording: local commits whose origin starts
/// with `(prefix, prefix_len)` are not pushed onto the undo stack.
#[no_mangle]
pub extern "C" fn loro_undo_manager_add_exclude_origin_prefix(
    um: *mut LoroUndoManager,
    prefix: *const c_char,
    prefix_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        let prefix = match str_from_raw(prefix, prefix_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        um.0.add_exclude_origin_prefix(prefix);
        LoroStatus::LORO_OK
    })
}

/// Clears both the undo and redo stacks.
#[no_mangle]
pub extern "C" fn loro_undo_manager_clear(um: *const LoroUndoManager) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = deref_or!(um, LoroStatus::LORO_ERR_INVALID_ARG);
        um.0.clear();
        LoroStatus::LORO_OK
    })
}

/// Starts a group: subsequent edits merge into a single undo item until
/// [`loro_undo_manager_group_end`].
#[no_mangle]
pub extern "C" fn loro_undo_manager_group_start(um: *mut LoroUndoManager) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match um.0.group_start() {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Ends the current group started with [`loro_undo_manager_group_start`].
#[no_mangle]
pub extern "C" fn loro_undo_manager_group_end(um: *mut LoroUndoManager) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        um.0.group_end();
        LoroStatus::LORO_OK
    })
}

/// Installs (or replaces) the on_push listener. The previous listener's `free_user_data`
/// runs when it is replaced.
#[no_mangle]
pub extern "C" fn loro_undo_manager_set_on_push(
    um: *mut LoroUndoManager,
    callback: LoroUndoOnPush,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        let owner = callback;
        um.0.set_on_push(Some(Box::new(
            move |kind: UndoOrRedo, span: CounterSpan, ev: Option<DiffEvent>| -> UndoItemMeta {
                let owner = &owner;
                let mut meta = UndoItemMeta::new();
                let ev_ptr = match &ev {
                    Some(e) => (e as *const DiffEvent) as *const LoroDiffEvent,
                    None => std::ptr::null(),
                };
                let meta_ptr = (&mut meta as *mut UndoItemMeta) as *mut LoroUndoMeta;
                (owner.invoke)(
                    LoroUndoOrRedo::from_loro(kind),
                    LoroCounterSpan::from_loro(span),
                    ev_ptr,
                    meta_ptr,
                    owner.user_data,
                );
                meta
            },
        )));
        LoroStatus::LORO_OK
    })
}

/// Installs (or replaces) the on_pop listener. The previous listener's `free_user_data`
/// runs when it is replaced.
#[no_mangle]
pub extern "C" fn loro_undo_manager_set_on_pop(
    um: *mut LoroUndoManager,
    callback: LoroUndoOnPop,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let um = match um_mut(um) {
            Some(u) => u,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        let owner = callback;
        um.0.set_on_pop(Some(Box::new(
            move |kind: UndoOrRedo, span: CounterSpan, meta: UndoItemMeta| {
                let owner = &owner;
                let meta_ptr = (&meta as *const UndoItemMeta) as *const LoroUndoMeta;
                (owner.invoke)(
                    LoroUndoOrRedo::from_loro(kind),
                    LoroCounterSpan::from_loro(span),
                    meta_ptr,
                    owner.user_data,
                );
            },
        )));
        LoroStatus::LORO_OK
    })
}

// ---------------------------------------------------------------------------
// LoroUndoMeta accessors (callback-scoped)
// ---------------------------------------------------------------------------

/// Sets the undo item's metadata value to the JSON-encoded value `(json, json_len)`. Call
/// this from an on_push listener. Returns `LORO_ERR_INVALID_ARG` on a null handle.
#[no_mangle]
pub extern "C" fn loro_undo_meta_set_value_json(
    meta: *mut LoroUndoMeta,
    json: *const c_char,
    json_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let meta = match unsafe { (meta as *mut UndoItemMeta).as_mut() } {
            Some(m) => m,
            None => {
                set_last_error("null undo meta pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        let value = match value_from_json(json, json_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        meta.set_value(value);
        LoroStatus::LORO_OK
    })
}

/// Writes the undo item's metadata value as JSON into `*out`. Call this from an on_pop
/// listener. `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_undo_meta_get_value_json(
    meta: *const LoroUndoMeta,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let meta = match unsafe { (meta as *const UndoItemMeta).as_ref() } {
            Some(m) => m,
            None => {
                set_last_error("null undo meta pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match serde_json::to_vec(&meta.value) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize undo meta value: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

/// Sets the undo item's metadata value from an owned typed `LoroValue*` (RESHAPE Phase 4).
/// Call this from an on_push listener. The value is cloned in — the caller still owns (and
/// must free) `value`. Typed counterpart to [`loro_undo_meta_set_value_json`]: it preserves
/// binary, integer-valued doubles, and the value/container distinction. Returns
/// `LORO_ERR_INVALID_ARG` on a null handle or value.
#[no_mangle]
pub extern "C" fn loro_undo_meta_set_value(
    meta: *mut LoroUndoMeta,
    value: *const value_typed::LoroValue,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let meta = match unsafe { (meta as *mut UndoItemMeta).as_mut() } {
            Some(m) => m,
            None => {
                set_last_error("null undo meta pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        let value = deref_or!(value, LoroStatus::LORO_ERR_INVALID_ARG);
        meta.set_value(value.inner().clone());
        LoroStatus::LORO_OK
    })
}

/// Returns the undo item's metadata value as an owned typed `LoroValue*` (RESHAPE Phase 4),
/// or null on a null handle. Call this from an on_pop listener. Typed counterpart to
/// [`loro_undo_meta_get_value_json`]; free the result with `loro_value_free`.
#[no_mangle]
pub extern "C" fn loro_undo_meta_get_value(
    meta: *const LoroUndoMeta,
) -> *mut value_typed::LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let meta = match unsafe { (meta as *const UndoItemMeta).as_ref() } {
            Some(m) => m,
            None => {
                set_last_error("null undo meta pointer passed to loro-c-api");
                return std::ptr::null_mut();
            }
        };
        value_typed::into_raw(meta.value.clone())
    })
}

// ---------------------------------------------------------------------------
// LoroUndoMeta cursors (callback-scoped) — loro-cpp parity for `UndoItemMeta.cursors`
// ---------------------------------------------------------------------------

/// Appends `cursor` to the undo item's cursor list. Call this from an on_push listener to
/// capture a cursor whose position should be restored when the item is later popped — loro
/// transforms the stored cursors by any intervening (remote) ops, so on_pop sees the replayed
/// position. The cursor is cloned in; the caller still owns (and must free) `cursor`. Returns
/// `LORO_ERR_INVALID_ARG` on a null handle or cursor.
#[no_mangle]
pub extern "C" fn loro_undo_meta_add_cursor(
    meta: *mut LoroUndoMeta,
    cursor: *const LoroCursor,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let meta = match unsafe { (meta as *mut UndoItemMeta).as_mut() } {
            Some(m) => m,
            None => {
                set_last_error("null undo meta pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        let cursor = deref_or!(cursor, LoroStatus::LORO_ERR_INVALID_ARG);
        meta.add_cursor(cursor.inner());
        LoroStatus::LORO_OK
    })
}

/// Returns the number of cursors attached to the undo item (whatever on_push stored). Call this
/// from an on_pop listener. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_undo_meta_cursors_len(meta: *const LoroUndoMeta) -> usize {
    ffi_guard!(0usize, {
        let meta = match unsafe { (meta as *const UndoItemMeta).as_ref() } {
            Some(m) => m,
            None => {
                set_last_error("null undo meta pointer passed to loro-c-api");
                return 0;
            }
        };
        meta.cursors.len()
    })
}

/// Returns the `index`-th attached cursor as an owned `LoroCursor*` (free with
/// `loro_cursor_free`), writing its transformed absolute position into `*out_pos` and the side
/// into `*out_side` (each written only on success, when the pointer is non-null). Call this from
/// an on_pop listener after reading [`loro_undo_meta_cursors_len`]. Returns null on a null
/// handle, an out-of-range index, or a caught panic.
#[no_mangle]
pub extern "C" fn loro_undo_meta_get_cursor(
    meta: *const LoroUndoMeta,
    index: usize,
    out_pos: *mut usize,
    out_side: *mut LoroSide,
) -> *mut LoroCursor {
    ffi_guard!(std::ptr::null_mut(), {
        let meta = match unsafe { (meta as *const UndoItemMeta).as_ref() } {
            Some(m) => m,
            None => {
                set_last_error("null undo meta pointer passed to loro-c-api");
                return std::ptr::null_mut();
            }
        };
        let cwp = match meta.cursors.get(index) {
            Some(c) => c,
            None => {
                set_last_error("undo meta cursor index out of range");
                return std::ptr::null_mut();
            }
        };
        if !out_pos.is_null() {
            unsafe { out_pos.write(cwp.pos.pos) };
        }
        if !out_side.is_null() {
            unsafe { out_side.write(from_side(cwp.pos.side)) };
        }
        Box::into_raw(Box::new(LoroCursor::from_inner(cwp.cursor.clone())))
    })
}
