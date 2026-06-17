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

use crate::callbacks::CCallback;
use crate::container::any::LoroContainer;
use crate::container::counter::LoroCounter;
use crate::container::list::LoroList;
use crate::container::movable_list::LoroMovableList;
use crate::container::text::LoroText;
use crate::container::tree::LoroTree;
use crate::doc::LoroDoc;
use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::event::{LoroDiffEvent, LoroSubscriber, LoroSubscription};
use crate::value::{str_from_raw, value_from_json, value_to_json_bytes, LoroBytes};
use crate::value_or_container::LoroValueOrContainer;
use crate::value_typed::LoroValue;
use std::os::raw::c_char;
use std::sync::Arc;

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

/// Writes this container's id (a string such as `cid:root-name:Map`) into `*out`. `*out`
/// is only written on `LORO_OK`; free it with `loro_bytes_free`. Pass the written string
/// to `loro_doc_subscribe` to subscribe to this container's events.
#[no_mangle]
pub extern "C" fn loro_map_id(map: *const LoroMap, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let id = loro::ContainerTrait::id(map.inner());
        unsafe { out.write(LoroBytes::from_vec(id.to_string().into_bytes())) };
        LoroStatus::LORO_OK
    })
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

/// Inserts a *clone* of the typed `value` under UTF-8 key `(key, key_len)`. Unlike the
/// JSON [`loro_map_insert`], this preserves binary, integer-valued doubles, and the
/// value/container distinction. Borrows `value` (the caller still owns it).
#[no_mangle]
pub extern "C" fn loro_map_insert_value(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
    value: *const LoroValue,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        let value = deref_or!(value, LoroStatus::LORO_ERR_INVALID_ARG);
        match map.inner().insert(key, value.inner().clone()) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Returns the entry at `(key, key_len)` as an owned `LoroValueOrContainer*` (a plain value
/// or a live child container), or null if the key is absent. Free the result with
/// `loro_value_or_container_free`. This is the typed counterpart to [`loro_map_get`].
#[no_mangle]
pub extern "C" fn loro_map_get_value_or_container(
    map: *const LoroMap,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroValueOrContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match map.inner().get(key) {
            Some(voc) => Box::into_raw(Box::new(LoroValueOrContainer::from_inner(voc))),
            None => {
                set_last_error("key not found in map");
                std::ptr::null_mut()
            }
        }
    })
}

/// Returns the map's full recursive state as an owned typed `LoroValue*` (a `Map` value
/// with nested containers resolved to their deep values). Free with `loro_value_free`.
#[no_mangle]
pub extern "C" fn loro_map_get_deep_value(map: *const LoroMap) -> *mut LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        crate::value_typed::into_raw(map.inner().get_deep_value())
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

// ---------------------------------------------------------------------------
// G6.4 — uniform container introspection (via loro::ContainerTrait) + attribution
// ---------------------------------------------------------------------------

/// Returns whether this map container has been deleted from its document.
#[no_mangle]
pub extern "C" fn loro_map_is_deleted(map: *const LoroMap) -> bool {
    ffi_guard!(false, {
        let map = deref_or!(map, false);
        loro::ContainerTrait::is_deleted(map.inner())
    })
}

/// Returns whether this map container is attached to a document.
#[no_mangle]
pub extern "C" fn loro_map_is_attached(map: *const LoroMap) -> bool {
    ffi_guard!(false, {
        let map = deref_or!(map, false);
        loro::ContainerTrait::is_attached(map.inner())
    })
}

/// If this detached container has an attached counterpart in its document, returns a new
/// handle to it; otherwise returns null. Free the result with [`loro_map_free`].
#[no_mangle]
pub extern "C" fn loro_map_get_attached(map: *const LoroMap) -> *mut LoroMap {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        match loro::ContainerTrait::get_attached(map.inner()) {
            Some(m) => Box::into_raw(Box::new(LoroMap::from_inner(m))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Returns a new handle to the document this container belongs to, or null if it is
/// detached. Free the result with `loro_doc_free`.
#[no_mangle]
pub extern "C" fn loro_map_doc(map: *const LoroMap) -> *mut LoroDoc {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        match loro::ContainerTrait::doc(map.inner()) {
            Some(d) => Box::into_raw(Box::new(LoroDoc::from_inner(d))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Subscribes to changes of this map container. Returns a `LoroSubscription*`, or null if
/// the container is detached / on a null handle / caught panic. Free it with
/// `loro_subscription_free` (which unsubscribes).
#[no_mangle]
pub extern "C" fn loro_map_subscribe(
    map: *const LoroMap,
    callback: LoroSubscriber,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        // Resolve the doc first: a detached container returns null WITHOUT taking ownership
        // of the callback (the caller frees it), mirroring `loro_doc_subscribe`'s contract.
        let doc = match loro::ContainerTrait::doc(map.inner()) {
            Some(d) => d,
            None => return std::ptr::null_mut(),
        };
        let owner = CCallback {
            invoke: callback.invoke,
            user_data: callback.user_data,
            free_user_data: callback.free_user_data,
        };
        let subscriber: loro::event::Subscriber = Arc::new(move |e: loro::event::DiffEvent| {
            let owner = &owner;
            let ptr = (&e as *const loro::event::DiffEvent) as *const LoroDiffEvent;
            (owner.invoke)(ptr, owner.user_data);
        });
        let sub = doc.subscribe(&loro::ContainerTrait::id(map.inner()), subscriber);
        LoroSubscription::into_raw(sub)
    })
}

/// Writes the peer id of the last editor of `key` into `*out` and returns true; returns
/// false (leaving `*out` untouched) when the key has no recorded editor, or on a null
/// handle / null `out` / invalid UTF-8 key / caught panic.
#[no_mangle]
pub extern "C" fn loro_map_get_last_editor(
    map: *const LoroMap,
    key: *const c_char,
    key_len: usize,
    out: *mut u64,
) -> bool {
    ffi_guard!(false, {
        let map = deref_or!(map, false);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return false,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        match map.inner().get_last_editor(key) {
            Some(peer) => {
                unsafe { out.write(peer) };
                true
            }
            None => false,
        }
    })
}

// ---------------------------------------------------------------------------
// G6.5 — mergeable child containers
// ---------------------------------------------------------------------------
//
// `ensure_mergeable_*` gets (or creates) a child container at `key` whose concurrent
// creations at the same key *merge* into one container, rather than conflicting as the
// generic `loro_map_insert_container` path does. Each returns a typed handle (null on error,
// e.g. the key already holds a non-mergeable value); free it with the matching `loro_*_free`.

/// Gets or creates a mergeable text container at `(key, key_len)`. Returns null on error
/// (bad UTF-8 key, or the key holds a non-mergeable value). Free with `loro_text_free`.
#[no_mangle]
pub extern "C" fn loro_map_ensure_mergeable_text(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroText {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match map.inner().ensure_mergeable_text(key) {
            Ok(c) => Box::into_raw(Box::new(LoroText::from_inner(c))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Gets or creates a mergeable map container at `(key, key_len)`. Returns null on error
/// (bad UTF-8 key, or the key holds a non-mergeable value). Free with `loro_map_free`.
#[no_mangle]
pub extern "C" fn loro_map_ensure_mergeable_map(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroMap {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match map.inner().ensure_mergeable_map(key) {
            Ok(c) => Box::into_raw(Box::new(LoroMap::from_inner(c))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Gets or creates a mergeable list container at `(key, key_len)`. Returns null on error
/// (bad UTF-8 key, or the key holds a non-mergeable value). Free with `loro_list_free`.
#[no_mangle]
pub extern "C" fn loro_map_ensure_mergeable_list(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroList {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match map.inner().ensure_mergeable_list(key) {
            Ok(c) => Box::into_raw(Box::new(LoroList::from_inner(c))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Gets or creates a mergeable movable-list container at `(key, key_len)`. Returns null on
/// error (bad UTF-8 key, or the key holds a non-mergeable value). Free with
/// `loro_movable_list_free`.
#[no_mangle]
pub extern "C" fn loro_map_ensure_mergeable_movable_list(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroMovableList {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match map.inner().ensure_mergeable_movable_list(key) {
            Ok(c) => Box::into_raw(Box::new(LoroMovableList::from_inner(c))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Gets or creates a mergeable tree container at `(key, key_len)`. Returns null on error
/// (bad UTF-8 key, or the key holds a non-mergeable value). Free with `loro_tree_free`.
#[no_mangle]
pub extern "C" fn loro_map_ensure_mergeable_tree(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroTree {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match map.inner().ensure_mergeable_tree(key) {
            Ok(c) => Box::into_raw(Box::new(LoroTree::from_inner(c))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Gets or creates a mergeable counter container at `(key, key_len)`. Returns null on error
/// (bad UTF-8 key, or the key holds a non-mergeable value). Free with `loro_counter_free`.
#[no_mangle]
pub extern "C" fn loro_map_ensure_mergeable_counter(
    map: *mut LoroMap,
    key: *const c_char,
    key_len: usize,
) -> *mut LoroCounter {
    ffi_guard!(std::ptr::null_mut(), {
        let map = deref_or!(map, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match map.inner().ensure_mergeable_counter(key) {
            Ok(c) => Box::into_raw(Box::new(LoroCounter::from_inner(c))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}
