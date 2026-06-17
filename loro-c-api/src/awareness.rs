//! Presence / ephemeral state (M4): the legacy [`LoroAwareness`] and the recommended
//! [`LoroEphemeralStore`].
//!
//! Both are *out-of-document* state used to broadcast transient information (cursors,
//! selections, "user is typing", …) that should not be persisted in the CRDT history. They
//! are encoded to bytes, shipped over whatever transport the app uses, and applied on the
//! far side.
//!
//! - [`LoroAwareness`] mirrors `loro::awareness::Awareness` (one `LoroValue` of state per
//!   peer, with timeout-based expiry). It is **deprecated** upstream in favour of the
//!   ephemeral store, but kept here for parity.
//! - [`LoroEphemeralStore`] mirrors `loro::awareness::EphemeralStore` — a keyed
//!   last-write-wins map with `added`/`updated`/`removed` change events and local-update
//!   notifications for syncing.
//!
//! Values cross the boundary as JSON (consistent with the rest of the API), but both types also
//! expose a **typed, no-JSON** surface (`*_value` / `get_peer_*`) carrying values as opaque
//! `LoroValue*` handles so binary, integer-valued doubles, and the value-vs-container distinction
//! survive — this is what the loro-cpp-shaped C++ wrapper is built on. Subscriptions reuse the
//! shared `LoroSubscription` handle from `event.rs`.
#![allow(deprecated)]

use crate::error::{set_last_error, LoroStatus};
use crate::event::{LoroLocalUpdateCallback, LoroSubscription};
use crate::value::{str_from_raw, value_from_json, LoroBytes};
use loro::awareness::{Awareness, EphemeralEventTrigger, EphemeralStore, EphemeralStoreEvent};
use loro::LoroValue;
use serde_json::{Map as JsonMap, Value as JsonValue};
use std::os::raw::{c_char, c_void};
use std::sync::Arc;

// ===========================================================================
// Awareness (legacy)
// ===========================================================================

/// Opaque handle to a [`loro::awareness::Awareness`]. Free with [`loro_awareness_free`].
pub struct LoroAwareness(Awareness);

/// Creates an awareness instance for local `peer` with an inactivity `timeout` (in
/// milliseconds) after which other peers' state is considered outdated. Release with
/// [`loro_awareness_free`].
#[no_mangle]
pub extern "C" fn loro_awareness_new(peer: u64, timeout: i64) -> *mut LoroAwareness {
    ffi_guard!(std::ptr::null_mut(), {
        Box::into_raw(Box::new(LoroAwareness(Awareness::new(peer, timeout))))
    })
}

/// Frees an awareness handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_awareness_free(aw: *mut LoroAwareness) {
    ffi_guard!((), {
        if !aw.is_null() {
            unsafe {
                drop(Box::from_raw(aw));
            }
        }
    });
}

/// Returns the local peer id. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_awareness_peer(aw: *const LoroAwareness) -> u64 {
    ffi_guard!(0u64, {
        let aw = deref_or!(aw, 0u64);
        aw.0.peer()
    })
}

/// Sets the local peer's state to the JSON-encoded value `(json, json_len)`.
#[no_mangle]
pub extern "C" fn loro_awareness_set_local_state(
    aw: *mut LoroAwareness,
    json: *const c_char,
    json_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = match unsafe { aw.as_mut() } {
            Some(a) => a,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        let value = match value_from_json(json, json_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        aw.0.set_local_state(value);
        LoroStatus::LORO_OK
    })
}

/// Writes the local peer's state as JSON into `*out`. Returns `LORO_ERR_NOT_FOUND` if no
/// local state has been set. `*out` is only written on `LORO_OK`; free it with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_awareness_get_local_state(
    aw: *const LoroAwareness,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = deref_or!(aw, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match aw.0.get_local_state() {
            Some(v) => write_value_json(out, &v),
            None => {
                set_last_error("no local awareness state set");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Encodes the state of all known peers into `*out`. `*out` is only written on `LORO_OK`;
/// free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_awareness_encode_all(
    aw: *const LoroAwareness,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = deref_or!(aw, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(aw.0.encode_all())) };
        LoroStatus::LORO_OK
    })
}

/// Encodes the state of the `count` peers in `peers` into `*out`. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_awareness_encode(
    aw: *const LoroAwareness,
    peers: *const u64,
    count: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = deref_or!(aw, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        if peers.is_null() && count != 0 {
            set_last_error("null peers pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let slice = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(peers, count) }
        };
        unsafe { out.write(LoroBytes::from_vec(aw.0.encode(slice))) };
        LoroStatus::LORO_OK
    })
}

/// Applies encoded peer state `(data, len)` produced by another peer's `encode`/`encode_all`.
#[no_mangle]
pub extern "C" fn loro_awareness_apply(
    aw: *mut LoroAwareness,
    data: *const u8,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = match unsafe { aw.as_mut() } {
            Some(a) => a,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        if data.is_null() && len != 0 {
            set_last_error("null data pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        let _ = aw.0.apply(bytes);
        LoroStatus::LORO_OK
    })
}

/// Removes peers whose last update is older than the timeout.
#[no_mangle]
pub extern "C" fn loro_awareness_remove_outdated(aw: *mut LoroAwareness) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = match unsafe { aw.as_mut() } {
            Some(a) => a,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        aw.0.remove_outdated();
        LoroStatus::LORO_OK
    })
}

/// Writes all peers' state as a JSON object `{"<peer>": {"state": <value>, "counter": n,
/// "timestamp": n}, ...}` into `*out`. `*out` is only written on `LORO_OK`; free it with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_awareness_get_all_states(
    aw: *const LoroAwareness,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = deref_or!(aw, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let mut obj = JsonMap::new();
        for (peer, info) in aw.0.get_all_states().iter() {
            let mut entry = JsonMap::new();
            entry.insert(
                "state".to_string(),
                serde_json::to_value(&info.state).unwrap_or(JsonValue::Null),
            );
            entry.insert("counter".to_string(), JsonValue::from(info.counter));
            entry.insert("timestamp".to_string(), JsonValue::from(info.timestamp));
            obj.insert(peer.to_string(), JsonValue::Object(entry));
        }
        match serde_json::to_vec(&JsonValue::Object(obj)) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize awareness states: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

// ---------------------------------------------------------------------------
// Awareness — typed (no-JSON) surface for the loro-cpp-shaped C++ wrapper.
//
// The JSON functions above are lossy (binary → list, `2.0` → i64) and discard the structured
// results of `apply`/`remove_outdated`/`get_all_states`. These typed variants carry values as
// opaque `LoroValue*` handles (preserving binary/doubles/value-vs-container) and return peer-id
// lists as a raw little-endian `u64` buffer (lossless; JSON numbers would risk u64 precision
// loss). The legacy JSON functions are kept for the existing clean-API header.
// ---------------------------------------------------------------------------

/// Packs peer ids as a contiguous little-endian `u64` buffer (8 bytes each).
fn u64s_to_le_bytes(ids: &[u64]) -> Vec<u8> {
    let mut out = Vec::with_capacity(ids.len() * 8);
    for id in ids {
        out.extend_from_slice(&id.to_le_bytes());
    }
    out
}

/// Sets the local peer's state to a *clone* of the typed `value` (no JSON). Borrows `value`
/// (the caller still owns it). The typed counterpart to [`loro_awareness_set_local_state`].
#[no_mangle]
pub extern "C" fn loro_awareness_set_local_state_value(
    aw: *mut LoroAwareness,
    value: *const crate::value_typed::LoroValue,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = match unsafe { aw.as_mut() } {
            Some(a) => a,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        let value = deref_or!(value, LoroStatus::LORO_ERR_INVALID_ARG);
        aw.0.set_local_state(value.inner().clone());
        LoroStatus::LORO_OK
    })
}

/// Returns the local peer's state as an owned typed `LoroValue*` (no JSON), or null if no local
/// state has been set. Free the result with `loro_value_free`. The typed counterpart to
/// [`loro_awareness_get_local_state`].
#[no_mangle]
pub extern "C" fn loro_awareness_get_local_state_value(
    aw: *const LoroAwareness,
) -> *mut crate::value_typed::LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let aw = deref_or!(aw, std::ptr::null_mut());
        match aw.0.get_local_state() {
            Some(v) => crate::value_typed::into_raw(v),
            None => {
                set_last_error("no local awareness state set");
                std::ptr::null_mut()
            }
        }
    })
}

/// Applies encoded peer state `(data, len)` and reports which peers changed. Writes the updated
/// and added peer ids as little-endian `u64` buffers into `*out_updated` and `*out_added`
/// respectively; both are only written on `LORO_OK` and must be freed with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_awareness_apply_with_changes(
    aw: *mut LoroAwareness,
    data: *const u8,
    len: usize,
    out_updated: *mut LoroBytes,
    out_added: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = match unsafe { aw.as_mut() } {
            Some(a) => a,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        if out_updated.is_null() || out_added.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        if data.is_null() && len != 0 {
            set_last_error("null data pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        let (updated, added) = aw.0.apply(bytes);
        unsafe {
            out_updated.write(LoroBytes::from_vec(u64s_to_le_bytes(&updated)));
            out_added.write(LoroBytes::from_vec(u64s_to_le_bytes(&added)));
        }
        LoroStatus::LORO_OK
    })
}

/// Removes peers whose last update is older than the timeout and writes the removed peer ids as a
/// little-endian `u64` buffer into `*out`. `*out` is only written on `LORO_OK`; free it with
/// `loro_bytes_free`. The reporting counterpart to [`loro_awareness_remove_outdated`].
#[no_mangle]
pub extern "C" fn loro_awareness_remove_outdated_ids(
    aw: *mut LoroAwareness,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = match unsafe { aw.as_mut() } {
            Some(a) => a,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let removed = aw.0.remove_outdated();
        unsafe { out.write(LoroBytes::from_vec(u64s_to_le_bytes(&removed))) };
        LoroStatus::LORO_OK
    })
}

/// Writes the ids of all peers with known state as a little-endian `u64` buffer into `*out`.
/// `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`. Pair with
/// [`loro_awareness_get_peer_state_value`] / [`loro_awareness_get_peer_info`] to read each peer's
/// state typed (the typed counterpart to [`loro_awareness_get_all_states`]).
#[no_mangle]
pub extern "C" fn loro_awareness_peer_ids(
    aw: *const LoroAwareness,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let aw = deref_or!(aw, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let ids: Vec<u64> = aw.0.get_all_states().keys().copied().collect();
        unsafe { out.write(LoroBytes::from_vec(u64s_to_le_bytes(&ids))) };
        LoroStatus::LORO_OK
    })
}

/// Returns `peer`'s state as an owned typed `LoroValue*` (no JSON), or null if `peer` has no
/// known state. Free the result with `loro_value_free`.
#[no_mangle]
pub extern "C" fn loro_awareness_get_peer_state_value(
    aw: *const LoroAwareness,
    peer: u64,
) -> *mut crate::value_typed::LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let aw = deref_or!(aw, std::ptr::null_mut());
        match aw.0.get_all_states().get(&peer) {
            Some(info) => crate::value_typed::into_raw(info.state.clone()),
            None => {
                set_last_error("no awareness state for peer");
                std::ptr::null_mut()
            }
        }
    })
}

/// Writes `peer`'s `counter` and `timestamp` into `*out_counter` / `*out_timestamp`. Returns
/// `true` if `peer` has known state (both outputs written), `false` otherwise (outputs untouched).
#[no_mangle]
pub extern "C" fn loro_awareness_get_peer_info(
    aw: *const LoroAwareness,
    peer: u64,
    out_counter: *mut i32,
    out_timestamp: *mut i64,
) -> bool {
    ffi_guard!(false, {
        let aw = deref_or!(aw, false);
        if out_counter.is_null() || out_timestamp.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        match aw.0.get_all_states().get(&peer) {
            Some(info) => {
                unsafe {
                    out_counter.write(info.counter);
                    out_timestamp.write(info.timestamp);
                }
                true
            }
            None => false,
        }
    })
}

// ===========================================================================
// EphemeralStore
// ===========================================================================

/// Opaque handle to a [`loro::awareness::EphemeralStore`]. Free with
/// [`loro_ephemeral_store_free`].
pub struct LoroEphemeralStore(EphemeralStore);

/// How an ephemeral-store event was triggered. Mirrors
/// `loro::awareness::EphemeralEventTrigger`.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroEphemeralEventTrigger {
    /// A local `set`/`delete`.
    LORO_EPHEMERAL_LOCAL = 0,
    /// An `apply` of remote data.
    LORO_EPHEMERAL_IMPORT = 1,
    /// A `remove_outdated` expiry sweep.
    LORO_EPHEMERAL_TIMEOUT = 2,
}

/// Opaque, **callback-scoped** view of an ephemeral-store change event. Only valid for the
/// duration of the subscriber callback; never store it. A `*const LoroEphemeralStoreEvent`
/// always points to a borrowed `loro::awareness::EphemeralStoreEvent`.
#[allow(dead_code)]
pub struct LoroEphemeralStoreEvent(EphemeralStoreEvent);

/// A C subscriber callback for [`loro_ephemeral_store_subscribe`].
///
/// `invoke` is called with a callback-scoped `const LoroEphemeralStoreEvent*` and the opaque
/// `user_data`. `free_user_data` (may be null) runs once when the subscription is released.
#[repr(C)]
pub struct LoroEphemeralSubscriber {
    pub invoke: extern "C" fn(event: *const LoroEphemeralStoreEvent, user_data: *mut c_void),
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

unsafe impl Send for LoroEphemeralSubscriber {}
unsafe impl Sync for LoroEphemeralSubscriber {}

impl Drop for LoroEphemeralSubscriber {
    fn drop(&mut self) {
        if let Some(free) = self.free_user_data {
            free(self.user_data);
        }
    }
}

fn ephemeral_event_ref<'a>(
    ev: *const LoroEphemeralStoreEvent,
) -> Option<&'a EphemeralStoreEvent> {
    match unsafe { (ev as *const EphemeralStoreEvent).as_ref() } {
        Some(e) => Some(e),
        None => {
            set_last_error("null ephemeral event pointer passed to loro-c-api");
            None
        }
    }
}

/// Creates an ephemeral store with an inactivity `timeout` in milliseconds. Release with
/// [`loro_ephemeral_store_free`].
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_new(timeout: i64) -> *mut LoroEphemeralStore {
    ffi_guard!(std::ptr::null_mut(), {
        Box::into_raw(Box::new(LoroEphemeralStore(EphemeralStore::new(timeout))))
    })
}

/// Frees an ephemeral store handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_free(store: *mut LoroEphemeralStore) {
    ffi_guard!((), {
        if !store.is_null() {
            unsafe {
                drop(Box::from_raw(store));
            }
        }
    });
}

/// Sets `key` to the JSON-encoded value `(json, json_len)`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_set(
    store: *const LoroEphemeralStore,
    key: *const c_char,
    key_len: usize,
    json: *const c_char,
    json_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        let value = match value_from_json(json, json_len) {
            Some(v) => v,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        store.0.set(key, value);
        LoroStatus::LORO_OK
    })
}

/// Writes the value at `key` as JSON into `*out`. Returns `LORO_ERR_NOT_FOUND` if `key` is
/// absent (or expired). `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_get(
    store: *const LoroEphemeralStore,
    key: *const c_char,
    key_len: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        match store.0.get(key) {
            Some(v) => write_value_json(out, &v),
            None => {
                set_last_error("no ephemeral value for key");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Sets `key` to a *clone* of the typed `value` (no JSON). Borrows `value` (the caller still
/// owns it). The typed counterpart to [`loro_ephemeral_store_set`].
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_set_value(
    store: *const LoroEphemeralStore,
    key: *const c_char,
    key_len: usize,
    value: *const crate::value_typed::LoroValue,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        let value = deref_or!(value, LoroStatus::LORO_ERR_INVALID_ARG);
        store.0.set(key, value.inner().clone());
        LoroStatus::LORO_OK
    })
}

/// Returns the value at `key` as an owned typed `LoroValue*` (no JSON), or null if `key` is
/// absent (or expired). Free the result with `loro_value_free`. The typed counterpart to
/// [`loro_ephemeral_store_get`].
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_get_value(
    store: *const LoroEphemeralStore,
    key: *const c_char,
    key_len: usize,
) -> *mut crate::value_typed::LoroValue {
    ffi_guard!(std::ptr::null_mut(), {
        let store = deref_or!(store, std::ptr::null_mut());
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match store.0.get(key) {
            Some(v) => crate::value_typed::into_raw(v),
            None => {
                set_last_error("no ephemeral value for key");
                std::ptr::null_mut()
            }
        }
    })
}

/// Deletes `key` from the store.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_delete(
    store: *const LoroEphemeralStore,
    key: *const c_char,
    key_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        store.0.delete(key);
        LoroStatus::LORO_OK
    })
}

/// Encodes the latest value of `key` into `*out`. `*out` is only written on `LORO_OK`; free
/// it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_encode(
    store: *const LoroEphemeralStore,
    key: *const c_char,
    key_len: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        unsafe { out.write(LoroBytes::from_vec(store.0.encode(key))) };
        LoroStatus::LORO_OK
    })
}

/// Encodes all non-expired entries into `*out`. `*out` is only written on `LORO_OK`; free it
/// with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_encode_all(
    store: *const LoroEphemeralStore,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(store.0.encode_all())) };
        LoroStatus::LORO_OK
    })
}

/// Applies encoded ephemeral data `(data, len)` from another peer. Returns
/// `LORO_ERR_DECODE` on malformed input.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_apply(
    store: *const LoroEphemeralStore,
    data: *const u8,
    len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        if data.is_null() && len != 0 {
            set_last_error("null data pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        match store.0.apply(bytes) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => {
                set_last_error(e.to_string());
                LoroStatus::LORO_ERR_DECODE
            }
        }
    })
}

/// Removes entries whose last update is older than the timeout (emitting a `Timeout` event
/// to subscribers if any are removed).
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_remove_outdated(
    store: *const LoroEphemeralStore,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        store.0.remove_outdated();
        LoroStatus::LORO_OK
    })
}

/// Writes the store's keys as a JSON array of strings into `*out`. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_keys(
    store: *const LoroEphemeralStore,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let keys: Vec<JsonValue> = store.0.keys().into_iter().map(JsonValue::from).collect();
        match serde_json::to_vec(&JsonValue::Array(keys)) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize ephemeral keys: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

/// Writes all entries as a JSON object `{"<key>": <value>, ...}` into `*out`. `*out` is only
/// written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_get_all_states(
    store: *const LoroEphemeralStore,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let store = deref_or!(store, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let mut obj = JsonMap::new();
        for (k, v) in store.0.get_all_states().into_iter() {
            obj.insert(k, serde_json::to_value(&v).unwrap_or(JsonValue::Null));
        }
        match serde_json::to_vec(&JsonValue::Object(obj)) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize ephemeral states: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

/// Subscribes to change events (added/updated/removed keys). Returns a `LoroSubscription*`
/// (free with `loro_subscription_free` to unsubscribe), or null on error.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_subscribe(
    store: *const LoroEphemeralStore,
    callback: LoroEphemeralSubscriber,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let store = deref_or!(store, std::ptr::null_mut());
        let owner = callback;
        let subscriber: loro::awareness::EphemeralSubscriber =
            Box::new(move |e: &EphemeralStoreEvent| -> bool {
                let owner = &owner;
                let ptr = (e as *const EphemeralStoreEvent) as *const LoroEphemeralStoreEvent;
                (owner.invoke)(ptr, owner.user_data);
                true
            });
        let sub = store.0.subscribe(subscriber);
        LoroSubscription::into_raw(sub)
    })
}

/// Subscribes to local updates: the callback receives the encoded bytes to broadcast each
/// time local ephemeral state changes, returning `true` to stay subscribed (`false`
/// auto-unsubscribes). Returns a `LoroSubscription*`, or null on error.
#[no_mangle]
pub extern "C" fn loro_ephemeral_store_subscribe_local_updates(
    store: *const LoroEphemeralStore,
    callback: LoroLocalUpdateCallback,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let store = deref_or!(store, std::ptr::null_mut());
        let owner = crate::callbacks::CCallback {
            invoke: callback.invoke,
            user_data: callback.user_data,
            free_user_data: callback.free_user_data,
        };
        let cb: loro::awareness::LocalEphemeralCallback =
            Box::new(move |bytes: &Vec<u8>| -> bool {
                let owner = &owner;
                (owner.invoke)(bytes.as_ptr(), bytes.len(), owner.user_data)
            });
        let sub = store.0.subscribe_local_updates(cb);
        LoroSubscription::into_raw(sub)
    })
}

// ---------------------------------------------------------------------------
// EphemeralStoreEvent accessors (callback-scoped)
// ---------------------------------------------------------------------------

/// Returns how the event was triggered. Returns `LORO_EPHEMERAL_LOCAL` on a null handle.
#[no_mangle]
pub extern "C" fn loro_ephemeral_event_by(
    ev: *const LoroEphemeralStoreEvent,
) -> LoroEphemeralEventTrigger {
    ffi_guard!(LoroEphemeralEventTrigger::LORO_EPHEMERAL_LOCAL, {
        let event = match ephemeral_event_ref(ev) {
            Some(e) => e,
            None => return LoroEphemeralEventTrigger::LORO_EPHEMERAL_LOCAL,
        };
        match event.by {
            EphemeralEventTrigger::Local => LoroEphemeralEventTrigger::LORO_EPHEMERAL_LOCAL,
            EphemeralEventTrigger::Import => LoroEphemeralEventTrigger::LORO_EPHEMERAL_IMPORT,
            EphemeralEventTrigger::Timeout => LoroEphemeralEventTrigger::LORO_EPHEMERAL_TIMEOUT,
        }
    })
}

/// Writes the event's added keys as a JSON array of strings into `*out`. Free with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_event_added(
    ev: *const LoroEphemeralStoreEvent,
    out: *mut LoroBytes,
) -> LoroStatus {
    write_keys(ev, out, |e| &e.added)
}

/// Writes the event's updated keys as a JSON array of strings into `*out`. Free with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_event_updated(
    ev: *const LoroEphemeralStoreEvent,
    out: *mut LoroBytes,
) -> LoroStatus {
    write_keys(ev, out, |e| &e.updated)
}

/// Writes the event's removed keys as a JSON array of strings into `*out`. Free with
/// `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_ephemeral_event_removed(
    ev: *const LoroEphemeralStoreEvent,
    out: *mut LoroBytes,
) -> LoroStatus {
    write_keys(ev, out, |e| &e.removed)
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

fn write_keys(
    ev: *const LoroEphemeralStoreEvent,
    out: *mut LoroBytes,
    pick: impl Fn(&EphemeralStoreEvent) -> &Arc<Vec<String>>,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let event = match ephemeral_event_ref(ev) {
            Some(e) => e,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let arr: Vec<JsonValue> = pick(event)
            .iter()
            .map(|s| JsonValue::from(s.clone()))
            .collect();
        match serde_json::to_vec(&JsonValue::Array(arr)) {
            Ok(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            Err(e) => {
                set_last_error(format!("failed to serialize ephemeral keys: {e}"));
                LoroStatus::LORO_ERR_ENCODE
            }
        }
    })
}

/// Serializes a `LoroValue` to JSON bytes into `*out`.
fn write_value_json(out: *mut LoroBytes, value: &LoroValue) -> LoroStatus {
    match serde_json::to_vec(value) {
        Ok(bytes) => {
            unsafe { out.write(LoroBytes::from_vec(bytes)) };
            LoroStatus::LORO_OK
        }
        Err(e) => {
            set_last_error(format!("failed to serialize value to JSON: {e}"));
            LoroStatus::LORO_ERR_ENCODE
        }
    }
}
