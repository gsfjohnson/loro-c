//! Events & subscriptions (M3).
//!
//! Three subscription entry points mirror the `loro` crate:
//! - [`loro_doc_subscribe`] — changes to one container (by container-id string),
//! - [`loro_doc_subscribe_root`] — changes to the whole document,
//! - [`loro_doc_subscribe_local_update`] — the raw update bytes of each local commit.
//!
//! Each returns a `LoroSubscription*`. **Dropping the subscription unsubscribes**
//! ([`loro_subscription_free`]); [`loro_subscription_detach`] instead leaves the callback
//! firing until the document is dropped. The callback's `free_user_data` runs exactly once
//! when the subscription is released.
//!
//! ## DiffEvent marshalling (hybrid)
//!
//! The subscriber callback receives a `const LoroDiffEvent*` that is valid **only for the
//! duration of the call**. Its *envelope* is read with structured accessors
//! (`loro_diff_event_*` / `loro_container_diff_*`): trigger kind, origin, current target,
//! and the list of per-container diffs with each one's target id, kind and path. Each
//! container's actual delta payload is rendered as **JSON** via
//! [`loro_container_diff_to_json`] (see that function for the schema), consistent with the
//! project's "JSON for `LoroValue`" approach. Any `LoroBytes` produced by these accessors
//! is owned by the caller (free with `loro_bytes_free`), but the `LoroDiffEvent*` /
//! `LoroContainerDiff*` pointers themselves must NOT be stored beyond the callback.

use crate::callbacks::CCallback;
use crate::doc::LoroDoc;
use crate::error::{set_last_error, LoroStatus};
use crate::value::{str_from_raw, LoroBytes};
use loro::event::{ContainerDiff, Diff, DiffEvent, ListDiffItem};
use loro::{
    ContainerID, EventTriggerKind, FractionalIndex, Index, LoroValue, TextDelta, TreeExternalDiff,
    TreeID, TreeParentId, ValueOrContainer,
};
use serde_json::{json, Map as JsonMap, Value as JsonValue};
use std::collections::HashMap;
use std::os::raw::{c_char, c_void};
use std::sync::Arc;

/// How a diff event was triggered. Mirrors `loro::EventTriggerKind`.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroEventTriggerKind {
    /// A local transaction (commit).
    LORO_EVENT_TRIGGER_LOCAL = 0,
    /// Importing remote updates / a snapshot.
    LORO_EVENT_TRIGGER_IMPORT = 1,
    /// A `checkout` (time travel) to another version.
    LORO_EVENT_TRIGGER_CHECKOUT = 2,
}

/// The kind of a container diff — selects how to interpret
/// [`loro_container_diff_to_json`]'s payload. Mirrors the `loro::event::Diff` variants.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroDiffKind {
    LORO_DIFF_LIST = 0,
    LORO_DIFF_TEXT = 1,
    LORO_DIFF_MAP = 2,
    LORO_DIFF_TREE = 3,
    LORO_DIFF_COUNTER = 4,
    LORO_DIFF_UNKNOWN = 5,
}

/// A C subscriber callback for [`loro_doc_subscribe`] / [`loro_doc_subscribe_root`].
///
/// `invoke` is called with a callback-scoped `const LoroDiffEvent*` and the opaque
/// `user_data`. `free_user_data` (may be null) is called once when the subscription is
/// released. The callback may be invoked from any thread that mutates the document, so it
/// must be reentrant / thread-safe.
#[repr(C)]
pub struct LoroSubscriber {
    pub invoke: extern "C" fn(event: *const LoroDiffEvent, user_data: *mut c_void),
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

/// A C callback for [`loro_doc_subscribe_local_update`].
///
/// `invoke` receives the update bytes `(data, len)` of a local commit and the opaque
/// `user_data`; returning `false` auto-unsubscribes. `free_user_data` (may be null) runs
/// once when the subscription is released.
#[repr(C)]
pub struct LoroLocalUpdateCallback {
    pub invoke: extern "C" fn(data: *const u8, len: usize, user_data: *mut c_void) -> bool,
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

/// Opaque subscription handle / unsubscriber. Free with [`loro_subscription_free`]
/// (unsubscribes) or [`loro_subscription_detach`] (keep firing until the doc drops).
pub struct LoroSubscription(loro::Subscription);

impl LoroSubscription {
    /// Boxes a `loro::Subscription` into an owned `*mut LoroSubscription`. Shared by the
    /// other modules (awareness / ephemeral / jsonpath / commit) so every subscription
    /// kind frees through the same [`loro_subscription_free`] / [`loro_subscription_detach`].
    pub(crate) fn into_raw(sub: loro::Subscription) -> *mut LoroSubscription {
        Box::into_raw(Box::new(LoroSubscription(sub)))
    }
}

/// Opaque, **callback-scoped** view of a diff event. Only valid for the duration of the
/// subscriber callback; never store it or use it afterwards.
///
/// This is an opaque token for C only. A `*const LoroDiffEvent` always actually points to
/// a borrowed `loro::event::DiffEvent`; the accessors reinterpret the pointer back to that
/// type (see [`event_ref`]). The wrapper's own layout is never relied upon — the field
/// exists only so cbindgen renders the type as an opaque `struct`.
#[allow(dead_code)]
pub struct LoroDiffEvent(DiffEvent<'static>);

/// Opaque, **callback-scoped** view of one container's diff, obtained from
/// [`loro_diff_event_get`]. Only valid for the duration of the subscriber callback. Like
/// [`LoroDiffEvent`], a `*const LoroContainerDiff` actually points to a borrowed
/// `loro::event::ContainerDiff` (see [`container_diff_ref`]).
#[allow(dead_code)]
pub struct LoroContainerDiff(ContainerDiff<'static>);

/// Reinterprets a callback-scoped `*const LoroDiffEvent` as the borrowed `DiffEvent` it
/// actually points to. Records an error and returns `None` on null. The lifetime is
/// unconstrained — soundness rests on the callback-scoped contract.
fn event_ref<'a>(ev: *const LoroDiffEvent) -> Option<&'a DiffEvent<'a>> {
    match unsafe { (ev as *const DiffEvent<'a>).as_ref() } {
        Some(e) => Some(e),
        None => {
            set_last_error("null diff event pointer passed to loro-c-api");
            None
        }
    }
}

/// Reinterprets a callback-scoped `*const LoroContainerDiff` as the borrowed
/// `ContainerDiff` it actually points to. Records an error and returns `None` on null.
fn container_diff_ref<'a>(cd: *const LoroContainerDiff) -> Option<&'a ContainerDiff<'a>> {
    match unsafe { (cd as *const ContainerDiff<'a>).as_ref() } {
        Some(c) => Some(c),
        None => {
            set_last_error("null container diff pointer passed to loro-c-api");
            None
        }
    }
}

// ---------------------------------------------------------------------------
// Subscribe / unsubscribe
// ---------------------------------------------------------------------------

/// Subscribes to changes of the container identified by the container-id string
/// `(cid, cid_len)` (e.g. as produced by `loro_text_id`). Returns a `LoroSubscription*`,
/// or null on a null doc / invalid id / caught panic. Free it with
/// [`loro_subscription_free`] (which unsubscribes).
#[no_mangle]
pub extern "C" fn loro_doc_subscribe(
    doc: *const LoroDoc,
    cid: *const c_char,
    cid_len: usize,
    callback: LoroSubscriber,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let cid_str = match str_from_raw(cid, cid_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let container_id = match ContainerID::try_from(cid_str) {
            Ok(id) => id,
            Err(_) => {
                set_last_error(format!("invalid container id: {cid_str}"));
                return std::ptr::null_mut();
            }
        };
        let owner = CCallback {
            invoke: callback.invoke,
            user_data: callback.user_data,
            free_user_data: callback.free_user_data,
        };
        let subscriber: loro::event::Subscriber = Arc::new(move |e: DiffEvent| {
            // Borrow the whole CCallback so it (and its Drop, which runs free_user_data) is
            // captured together with the closure, and so the closure stays Send + Sync.
            let owner = &owner;
            let ptr = (&e as *const DiffEvent) as *const LoroDiffEvent;
            (owner.invoke)(ptr, owner.user_data);
        });
        let sub = doc.inner().subscribe(&container_id, subscriber);
        Box::into_raw(Box::new(LoroSubscription(sub)))
    })
}

/// Subscribes to all changes of the whole document. Returns a `LoroSubscription*`, or null
/// on a null doc / caught panic. Free it with [`loro_subscription_free`].
#[no_mangle]
pub extern "C" fn loro_doc_subscribe_root(
    doc: *const LoroDoc,
    callback: LoroSubscriber,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let owner = CCallback {
            invoke: callback.invoke,
            user_data: callback.user_data,
            free_user_data: callback.free_user_data,
        };
        let subscriber: loro::event::Subscriber = Arc::new(move |e: DiffEvent| {
            // Borrow the whole CCallback so it (and its Drop, which runs free_user_data) is
            // captured together with the closure, and so the closure stays Send + Sync.
            let owner = &owner;
            let ptr = (&e as *const DiffEvent) as *const LoroDiffEvent;
            (owner.invoke)(ptr, owner.user_data);
        });
        let sub = doc.inner().subscribe_root(subscriber);
        Box::into_raw(Box::new(LoroSubscription(sub)))
    })
}

/// Subscribes to the raw update bytes produced by each local commit. The callback returns
/// `true` to stay subscribed (`false` auto-unsubscribes). Returns a `LoroSubscription*`,
/// or null on a null doc / caught panic. Free it with [`loro_subscription_free`].
#[no_mangle]
pub extern "C" fn loro_doc_subscribe_local_update(
    doc: *const LoroDoc,
    callback: LoroLocalUpdateCallback,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let owner = CCallback {
            invoke: callback.invoke,
            user_data: callback.user_data,
            free_user_data: callback.free_user_data,
        };
        let cb: loro::LocalUpdateCallback = Box::new(move |bytes: &Vec<u8>| -> bool {
            // Borrow the whole CCallback (see note above) to capture it wholesale.
            let owner = &owner;
            (owner.invoke)(bytes.as_ptr(), bytes.len(), owner.user_data)
        });
        let sub = doc.inner().subscribe_local_update(cb);
        Box::into_raw(Box::new(LoroSubscription(sub)))
    })
}

/// Releases a subscription, **unsubscribing** the callback and running its
/// `free_user_data`. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_subscription_free(sub: *mut LoroSubscription) {
    ffi_guard!((), {
        if !sub.is_null() {
            unsafe {
                drop(Box::from_raw(sub));
            }
        }
    });
}

/// Detaches a subscription: frees the handle but leaves the callback firing until the
/// document itself is dropped (at which point `free_user_data` runs). Passing null is a
/// no-op.
#[no_mangle]
pub extern "C" fn loro_subscription_detach(sub: *mut LoroSubscription) {
    ffi_guard!((), {
        if !sub.is_null() {
            let boxed = unsafe { Box::from_raw(sub) };
            boxed.0.detach();
        }
    });
}

// ---------------------------------------------------------------------------
// DiffEvent envelope accessors (callback-scoped)
// ---------------------------------------------------------------------------

/// Returns how the event was triggered. Returns `LORO_EVENT_TRIGGER_LOCAL` on a null
/// handle.
#[no_mangle]
pub extern "C" fn loro_diff_event_triggered_by(ev: *const LoroDiffEvent) -> LoroEventTriggerKind {
    ffi_guard!(LoroEventTriggerKind::LORO_EVENT_TRIGGER_LOCAL, {
        let event = match event_ref(ev) {
            Some(e) => e,
            None => return LoroEventTriggerKind::LORO_EVENT_TRIGGER_LOCAL,
        };
        match event.triggered_by {
            EventTriggerKind::Local => LoroEventTriggerKind::LORO_EVENT_TRIGGER_LOCAL,
            EventTriggerKind::Import => LoroEventTriggerKind::LORO_EVENT_TRIGGER_IMPORT,
            EventTriggerKind::Checkout => LoroEventTriggerKind::LORO_EVENT_TRIGGER_CHECKOUT,
        }
    })
}

/// Writes the event's origin string (UTF-8, possibly empty) into `*out`. `*out` is only
/// written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_diff_event_origin(
    ev: *const LoroDiffEvent,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let event = match event_ref(ev) {
            Some(e) => e,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(event.origin.as_bytes().to_vec())) };
        LoroStatus::LORO_OK
    })
}

/// Writes the current target container id (as a string) into `*out`. Returns
/// `LORO_ERR_NOT_FOUND` if the event has no current target (e.g. a root subscription).
/// `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_diff_event_current_target(
    ev: *const LoroDiffEvent,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let event = match event_ref(ev) {
            Some(e) => e,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match &event.current_target {
            Some(cid) => {
                unsafe { out.write(LoroBytes::from_vec(cid.to_string().into_bytes())) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("event has no current target");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Returns the number of per-container diffs in the event. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_diff_event_count(ev: *const LoroDiffEvent) -> usize {
    ffi_guard!(0usize, {
        let event = match event_ref(ev) {
            Some(e) => e,
            None => return 0usize,
        };
        event.events.len()
    })
}

/// Returns a callback-scoped pointer to the container diff at `index`, or null (with an
/// error recorded) if `index` is out of range. Do NOT free or store the returned pointer.
#[no_mangle]
pub extern "C" fn loro_diff_event_get(
    ev: *const LoroDiffEvent,
    index: usize,
) -> *const LoroContainerDiff {
    ffi_guard!(std::ptr::null(), {
        let event = match event_ref(ev) {
            Some(e) => e,
            None => return std::ptr::null(),
        };
        match event.events.get(index) {
            Some(cd) => (cd as *const ContainerDiff) as *const LoroContainerDiff,
            None => {
                set_last_error("container diff index out of range");
                std::ptr::null()
            }
        }
    })
}

// ---------------------------------------------------------------------------
// ContainerDiff accessors (callback-scoped)
// ---------------------------------------------------------------------------

/// Writes the target container id (as a string) into `*out`. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_container_diff_target(
    cd: *const LoroContainerDiff,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let diff = match container_diff_ref(cd) {
            Some(c) => c,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(diff.target.to_string().into_bytes())) };
        LoroStatus::LORO_OK
    })
}

/// Returns whether this diff is from an unknown container type. Returns `false` on a null
/// handle.
#[no_mangle]
pub extern "C" fn loro_container_diff_is_unknown(cd: *const LoroContainerDiff) -> bool {
    ffi_guard!(false, {
        let diff = match container_diff_ref(cd) {
            Some(c) => c,
            None => return false,
        };
        diff.is_unknown
    })
}

/// Returns the kind of this container diff. Returns `LORO_DIFF_UNKNOWN` on a null handle.
#[no_mangle]
pub extern "C" fn loro_container_diff_kind(cd: *const LoroContainerDiff) -> LoroDiffKind {
    ffi_guard!(LoroDiffKind::LORO_DIFF_UNKNOWN, {
        let diff = match container_diff_ref(cd) {
            Some(c) => c,
            None => return LoroDiffKind::LORO_DIFF_UNKNOWN,
        };
        match &diff.diff {
            Diff::List(_) => LoroDiffKind::LORO_DIFF_LIST,
            Diff::Text(_) => LoroDiffKind::LORO_DIFF_TEXT,
            Diff::Map(_) => LoroDiffKind::LORO_DIFF_MAP,
            Diff::Tree(_) => LoroDiffKind::LORO_DIFF_TREE,
            Diff::Counter(_) => LoroDiffKind::LORO_DIFF_COUNTER,
            Diff::Unknown => LoroDiffKind::LORO_DIFF_UNKNOWN,
        }
    })
}

/// Writes the diff's path (from the root to this container) into `*out` as a JSON array of
/// `{"cid": <string>, "index": {"key"|"seq"|"node": ...}}`. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_container_diff_path_json(
    cd: *const LoroContainerDiff,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let diff = match container_diff_ref(cd) {
            Some(c) => c,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let arr: Vec<JsonValue> = diff
            .path
            .iter()
            .map(|(cid, index)| json!({ "cid": cid.to_string(), "index": index_to_json(index) }))
            .collect();
        write_json(out, &JsonValue::Array(arr))
    })
}

/// Writes this container's delta payload into `*out` as JSON. `*out` is only written on
/// `LORO_OK`; free it with `loro_bytes_free`. The shape depends on the diff kind
/// ([`loro_container_diff_kind`]):
/// - **Text**: `[{"retain":n,"attributes":{...}?},{"insert":"..","attributes":{...}?},{"delete":n}]`
/// - **List**: `[{"retain":n},{"insert":[<value>...],"is_move":bool},{"delete":n}]`
/// - **Map**: `{"<key>": <value> | null}` (null = key deleted)
/// - **Tree**: `[{"target":{...},"action":"create"|"move"|"delete",...}]`
/// - **Counter**: `<number>`  •  **Unknown**: `null`
///
/// Inserted values/containers are rendered as their deep value (JSON); a live container
/// handle is not surfaced here.
#[no_mangle]
pub extern "C" fn loro_container_diff_to_json(
    cd: *const LoroContainerDiff,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let diff = match container_diff_ref(cd) {
            Some(c) => c,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let value = diff_to_json(&diff.diff);
        write_json(out, &value)
    })
}

// ---------------------------------------------------------------------------
// JSON serialization helpers (private)
// ---------------------------------------------------------------------------

/// Serializes `value` into `*out`, mapping a serde failure to `LORO_ERR_ENCODE`.
fn write_json(out: *mut LoroBytes, value: &JsonValue) -> LoroStatus {
    match serde_json::to_vec(value) {
        Ok(bytes) => {
            unsafe { out.write(LoroBytes::from_vec(bytes)) };
            LoroStatus::LORO_OK
        }
        Err(e) => {
            set_last_error(format!("failed to serialize diff to JSON: {e}"));
            LoroStatus::LORO_ERR_ENCODE
        }
    }
}

fn index_to_json(index: &Index) -> JsonValue {
    match index {
        Index::Key(k) => json!({ "key": k.to_string() }),
        Index::Seq(i) => json!({ "seq": i }),
        Index::Node(id) => json!({ "node": tree_id_to_json(id) }),
    }
}

fn diff_to_json(diff: &Diff) -> JsonValue {
    match diff {
        Diff::Text(deltas) => JsonValue::Array(deltas.iter().map(text_delta_to_json).collect()),
        Diff::List(items) => JsonValue::Array(items.iter().map(list_item_to_json).collect()),
        Diff::Map(map_delta) => {
            let mut obj = JsonMap::new();
            for (k, v) in map_delta.updated.iter() {
                let jv = match v {
                    Some(voc) => value_or_container_to_json(voc),
                    None => JsonValue::Null,
                };
                obj.insert(k.to_string(), jv);
            }
            JsonValue::Object(obj)
        }
        Diff::Tree(tree_diff) => {
            JsonValue::Array(tree_diff.diff.iter().map(tree_diff_item_to_json).collect())
        }
        Diff::Counter(c) => json!(c),
        Diff::Unknown => JsonValue::Null,
    }
}

fn text_delta_to_json(delta: &TextDelta) -> JsonValue {
    match delta {
        TextDelta::Retain { retain, attributes } => {
            let mut o = JsonMap::new();
            o.insert("retain".to_string(), json!(retain));
            if let Some(attrs) = attributes {
                o.insert("attributes".to_string(), attributes_to_json(attrs));
            }
            JsonValue::Object(o)
        }
        TextDelta::Insert { insert, attributes } => {
            let mut o = JsonMap::new();
            o.insert("insert".to_string(), json!(insert));
            if let Some(attrs) = attributes {
                o.insert("attributes".to_string(), attributes_to_json(attrs));
            }
            JsonValue::Object(o)
        }
        TextDelta::Delete { delete } => json!({ "delete": delete }),
    }
}

fn attributes_to_json<S: std::hash::BuildHasher>(attrs: &HashMap<String, LoroValue, S>) -> JsonValue {
    let mut o = JsonMap::new();
    for (k, v) in attrs.iter() {
        o.insert(k.clone(), serde_json::to_value(v).unwrap_or(JsonValue::Null));
    }
    JsonValue::Object(o)
}

fn list_item_to_json(item: &ListDiffItem) -> JsonValue {
    match item {
        ListDiffItem::Insert { insert, is_move } => json!({
            "insert": insert.iter().map(value_or_container_to_json).collect::<Vec<_>>(),
            "is_move": is_move,
        }),
        ListDiffItem::Delete { delete } => json!({ "delete": delete }),
        ListDiffItem::Retain { retain } => json!({ "retain": retain }),
    }
}

fn value_or_container_to_json(voc: &ValueOrContainer) -> JsonValue {
    serde_json::to_value(voc.get_deep_value()).unwrap_or(JsonValue::Null)
}

fn tree_id_to_json(id: &TreeID) -> JsonValue {
    json!({ "peer": id.peer, "counter": id.counter })
}

fn tree_parent_to_json(parent: &TreeParentId) -> JsonValue {
    match parent {
        TreeParentId::Node(id) => json!({ "node": tree_id_to_json(id) }),
        TreeParentId::Root => json!("root"),
        TreeParentId::Deleted => json!("deleted"),
        TreeParentId::Unexist => json!("unexist"),
    }
}

fn fractional_index_to_json(position: &FractionalIndex) -> JsonValue {
    json!(position.to_string())
}

fn tree_diff_item_to_json(item: &loro::TreeDiffItem) -> JsonValue {
    let mut o = JsonMap::new();
    o.insert("target".to_string(), tree_id_to_json(&item.target));
    match &item.action {
        TreeExternalDiff::Create {
            parent,
            index,
            position,
        } => {
            o.insert("action".to_string(), json!("create"));
            o.insert("parent".to_string(), tree_parent_to_json(parent));
            o.insert("index".to_string(), json!(index));
            o.insert(
                "fractional_index".to_string(),
                fractional_index_to_json(position),
            );
        }
        TreeExternalDiff::Move {
            parent,
            index,
            position,
            old_parent,
            old_index,
        } => {
            o.insert("action".to_string(), json!("move"));
            o.insert("parent".to_string(), tree_parent_to_json(parent));
            o.insert("index".to_string(), json!(index));
            o.insert(
                "fractional_index".to_string(),
                fractional_index_to_json(position),
            );
            o.insert("old_parent".to_string(), tree_parent_to_json(old_parent));
            o.insert("old_index".to_string(), json!(old_index));
        }
        TreeExternalDiff::Delete {
            old_parent,
            old_index,
        } => {
            o.insert("action".to_string(), json!("delete"));
            o.insert("old_parent".to_string(), tree_parent_to_json(old_parent));
            o.insert("old_index".to_string(), json!(old_index));
        }
    }
    JsonValue::Object(o)
}
