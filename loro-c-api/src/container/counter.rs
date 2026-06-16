//! `LoroCounter` container handle and its operations.
//!
//! A counter holds an `f64` that peers increment/decrement; the increments merge
//! additively (a CRDT counter). Free with [`loro_counter_free`]. Like every container
//! handle, a `LoroCounter*` is a strong co-owner of the document state (see `doc.rs`) and
//! may be freed in any order.

use crate::callbacks::CCallback;
use crate::doc::LoroDoc;
use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::event::{LoroDiffEvent, LoroSubscriber, LoroSubscription};
use crate::value::LoroBytes;
use std::sync::Arc;

/// Opaque handle to a Loro counter container.
pub struct LoroCounter(loro::LoroCounter);

impl LoroCounter {
    pub(crate) fn from_inner(c: loro::LoroCounter) -> LoroCounter {
        LoroCounter(c)
    }

    fn inner(&self) -> &loro::LoroCounter {
        &self.0
    }
}

/// Frees a counter handle. Passing null is a no-op. Safe to call before or after the
/// originating `LoroDoc*` is freed.
#[no_mangle]
pub extern "C" fn loro_counter_free(counter: *mut LoroCounter) {
    ffi_guard!((), {
        if !counter.is_null() {
            unsafe {
                drop(Box::from_raw(counter));
            }
        }
    });
}

/// Writes this container's id (a string such as `cid:root-name:Counter`) into `*out`.
/// `*out` is only written on `LORO_OK`; free it with `loro_bytes_free`. Pass the written
/// string to `loro_doc_subscribe` to subscribe to this container's events.
#[no_mangle]
pub extern "C" fn loro_counter_id(counter: *const LoroCounter, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let counter = deref_or!(counter, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let id = loro::ContainerTrait::id(counter.inner());
        unsafe { out.write(LoroBytes::from_vec(id.to_string().into_bytes())) };
        LoroStatus::LORO_OK
    })
}

/// Increments the counter by `value` (may be negative).
#[no_mangle]
pub extern "C" fn loro_counter_increment(counter: *mut LoroCounter, value: f64) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let counter = deref_or!(counter, LoroStatus::LORO_ERR_INVALID_ARG);
        match counter.inner().increment(value) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Decrements the counter by `value` (may be negative).
#[no_mangle]
pub extern "C" fn loro_counter_decrement(counter: *mut LoroCounter, value: f64) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let counter = deref_or!(counter, LoroStatus::LORO_ERR_INVALID_ARG);
        match counter.inner().decrement(value) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Returns the counter's current value. Returns 0.0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_counter_get_value(counter: *const LoroCounter) -> f64 {
    ffi_guard!(0.0f64, {
        let counter = deref_or!(counter, 0.0f64);
        counter.inner().get_value()
    })
}

// ---------------------------------------------------------------------------
// G6.4 — uniform container introspection (via loro::ContainerTrait)
// ---------------------------------------------------------------------------

/// Returns whether this counter container has been deleted from its document.
#[no_mangle]
pub extern "C" fn loro_counter_is_deleted(counter: *const LoroCounter) -> bool {
    ffi_guard!(false, {
        let counter = deref_or!(counter, false);
        loro::ContainerTrait::is_deleted(counter.inner())
    })
}

/// Returns whether this counter container is attached to a document.
#[no_mangle]
pub extern "C" fn loro_counter_is_attached(counter: *const LoroCounter) -> bool {
    ffi_guard!(false, {
        let counter = deref_or!(counter, false);
        loro::ContainerTrait::is_attached(counter.inner())
    })
}

/// If this detached container has an attached counterpart in its document, returns a new
/// handle to it; otherwise returns null. Free the result with [`loro_counter_free`].
#[no_mangle]
pub extern "C" fn loro_counter_get_attached(counter: *const LoroCounter) -> *mut LoroCounter {
    ffi_guard!(std::ptr::null_mut(), {
        let counter = deref_or!(counter, std::ptr::null_mut());
        match loro::ContainerTrait::get_attached(counter.inner()) {
            Some(c) => Box::into_raw(Box::new(LoroCounter::from_inner(c))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Returns a new handle to the document this container belongs to, or null if it is
/// detached. Free the result with `loro_doc_free`.
#[no_mangle]
pub extern "C" fn loro_counter_doc(counter: *const LoroCounter) -> *mut LoroDoc {
    ffi_guard!(std::ptr::null_mut(), {
        let counter = deref_or!(counter, std::ptr::null_mut());
        match loro::ContainerTrait::doc(counter.inner()) {
            Some(d) => Box::into_raw(Box::new(LoroDoc::from_inner(d))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Subscribes to changes of this counter container. Returns a `LoroSubscription*`, or null
/// if the container is detached / on a null handle / caught panic. Free it with
/// `loro_subscription_free` (which unsubscribes).
#[no_mangle]
pub extern "C" fn loro_counter_subscribe(
    counter: *const LoroCounter,
    callback: LoroSubscriber,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let counter = deref_or!(counter, std::ptr::null_mut());
        // Resolve the doc first: a detached container returns null WITHOUT taking ownership
        // of the callback (the caller frees it), mirroring `loro_doc_subscribe`'s contract.
        let doc = match loro::ContainerTrait::doc(counter.inner()) {
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
        let sub = doc.subscribe(&loro::ContainerTrait::id(counter.inner()), subscriber);
        LoroSubscription::into_raw(sub)
    })
}
