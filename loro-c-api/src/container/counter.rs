//! `LoroCounter` container handle and its operations.
//!
//! A counter holds an `f64` that peers increment/decrement; the increments merge
//! additively (a CRDT counter). Free with [`loro_counter_free`]. Like every container
//! handle, a `LoroCounter*` is a strong co-owner of the document state (see `doc.rs`) and
//! may be freed in any order.

use crate::error::{record_loro_error, LoroStatus};

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
