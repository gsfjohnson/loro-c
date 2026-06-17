//! JSONPath queries over a document (M4; requires the `jsonpath` cargo feature).
//!
//! [`loro_doc_jsonpath`] runs a JSONPath expression against the whole document and returns a
//! [`LoroJsonPathResults`] collection. Each element is either a plain value (read as JSON via
//! [`loro_jsonpath_results_get_value_json`]) or a live container (recovered as a
//! `LoroContainer*` via [`loro_jsonpath_results_get_container`]). Free the collection with
//! [`loro_jsonpath_results_free`]; any container handle taken from it is independent and must
//! be freed separately.
//!
//! [`loro_doc_subscribe_jsonpath`] registers a lightweight notification (no payload, may
//! produce false positives) that fires when something matching the path *might* have changed,
//! so the caller can debounce and re-query.

use crate::container::any::LoroContainer;
use crate::doc::LoroDoc;
use crate::error::{set_last_error, LoroStatus};
use crate::event::LoroSubscription;
use crate::value::{str_from_raw, LoroBytes};
use crate::value_or_container::LoroValueOrContainer;
use loro::ValueOrContainer;
use std::os::raw::{c_char, c_void};
use std::sync::Arc;

/// Opaque, owned collection of JSONPath query results. Free with
/// [`loro_jsonpath_results_free`].
pub struct LoroJsonPathResults(Vec<ValueOrContainer>);

/// Runs the JSONPath expression `(path, path_len)` against the document. Returns a results
/// collection, or null on an invalid path / evaluation error (see `loro_last_error_message`).
/// Release with [`loro_jsonpath_results_free`].
#[no_mangle]
pub extern "C" fn loro_doc_jsonpath(
    doc: *const LoroDoc,
    path: *const c_char,
    path_len: usize,
) -> *mut LoroJsonPathResults {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let path = match str_from_raw(path, path_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match doc.inner().jsonpath(path) {
            Ok(results) => Box::into_raw(Box::new(LoroJsonPathResults(results))),
            Err(e) => {
                set_last_error(e.to_string());
                std::ptr::null_mut()
            }
        }
    })
}

/// Frees a results collection. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_jsonpath_results_free(results: *mut LoroJsonPathResults) {
    ffi_guard!((), {
        if !results.is_null() {
            unsafe {
                drop(Box::from_raw(results));
            }
        }
    });
}

/// Returns the number of results. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_jsonpath_results_len(results: *const LoroJsonPathResults) -> usize {
    ffi_guard!(0usize, {
        let results = deref_or!(results, 0usize);
        results.0.len()
    })
}

/// Writes whether the result at `index` is a container (vs. a plain value) into `*out`.
/// Returns `LORO_ERR_NOT_FOUND` if `index` is out of range; `*out` is only written on
/// `LORO_OK`.
#[no_mangle]
pub extern "C" fn loro_jsonpath_results_is_container(
    results: *const LoroJsonPathResults,
    index: usize,
    out: *mut bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let results = deref_or!(results, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match results.0.get(index) {
            Some(voc) => {
                unsafe { out.write(matches!(voc, ValueOrContainer::Container(_))) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("jsonpath result index out of range");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Writes the result at `index` as JSON into `*out` (a container is rendered as its deep
/// value). Returns `LORO_ERR_NOT_FOUND` if `index` is out of range. `*out` is only written
/// on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_jsonpath_results_get_value_json(
    results: *const LoroJsonPathResults,
    index: usize,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let results = deref_or!(results, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match results.0.get(index) {
            Some(voc) => {
                let value = voc.get_deep_value();
                match serde_json::to_vec(&value) {
                    Ok(bytes) => {
                        unsafe { out.write(LoroBytes::from_vec(bytes)) };
                        LoroStatus::LORO_OK
                    }
                    Err(e) => {
                        set_last_error(format!("failed to serialize jsonpath result: {e}"));
                        LoroStatus::LORO_ERR_ENCODE
                    }
                }
            }
            None => {
                set_last_error("jsonpath result index out of range");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Recovers the result at `index` as an owned, typed [`LoroValueOrContainer`] — the same
/// value/container bridge returned by `loro_doc_get_by_path`. Unlike
/// [`loro_jsonpath_results_get_value_json`] this preserves the exact value type across the
/// boundary (binary stays binary, integer-valued doubles like `2.0` stay doubles), so callers
/// that need faithful values should use this and read it with `loro_value_or_container_get_value`.
/// Returns null if `index` is out of range. Release the returned handle with
/// `loro_value_or_container_free`.
#[no_mangle]
pub extern "C" fn loro_jsonpath_results_get_value_or_container(
    results: *const LoroJsonPathResults,
    index: usize,
) -> *mut LoroValueOrContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let results = deref_or!(results, std::ptr::null_mut());
        match results.0.get(index) {
            Some(voc) => Box::into_raw(Box::new(LoroValueOrContainer::from_inner(voc.clone()))),
            None => {
                set_last_error("jsonpath result index out of range");
                std::ptr::null_mut()
            }
        }
    })
}

/// Recovers the result at `index` as a type-erased `LoroContainer*`, or null if `index` is
/// out of range or the result is a plain value (check with
/// [`loro_jsonpath_results_is_container`] first). The returned handle is independent; free it
/// with `loro_container_free`.
#[no_mangle]
pub extern "C" fn loro_jsonpath_results_get_container(
    results: *const LoroJsonPathResults,
    index: usize,
) -> *mut LoroContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let results = deref_or!(results, std::ptr::null_mut());
        match results.0.get(index) {
            Some(ValueOrContainer::Container(c)) => {
                Box::into_raw(Box::new(LoroContainer::from_inner(c.clone())))
            }
            Some(_) => {
                set_last_error("jsonpath result is a value, not a container");
                std::ptr::null_mut()
            }
            None => {
                set_last_error("jsonpath result index out of range");
                std::ptr::null_mut()
            }
        }
    })
}

/// A C subscriber callback for [`loro_doc_subscribe_jsonpath`]. `invoke` is a payload-free
/// notification; `free_user_data` (may be null) runs once when the subscription is released.
#[repr(C)]
pub struct LoroJsonPathSubscriber {
    pub invoke: extern "C" fn(user_data: *mut c_void),
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

unsafe impl Send for LoroJsonPathSubscriber {}
unsafe impl Sync for LoroJsonPathSubscriber {}
impl Drop for LoroJsonPathSubscriber {
    fn drop(&mut self) {
        if let Some(free) = self.free_user_data {
            free(self.user_data);
        }
    }
}

/// Subscribes to updates that might affect the JSONPath `(path, path_len)`. The callback is a
/// lightweight notification (no result payload, may fire false positives). Returns a
/// `LoroSubscription*` (free with `loro_subscription_free` to unsubscribe), or null on an
/// invalid path.
#[no_mangle]
pub extern "C" fn loro_doc_subscribe_jsonpath(
    doc: *const LoroDoc,
    path: *const c_char,
    path_len: usize,
    callback: LoroJsonPathSubscriber,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let path = match str_from_raw(path, path_len) {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        let owner = callback;
        let cb: loro::SubscribeJsonPathCallback = Arc::new(move || {
            let owner = &owner;
            (owner.invoke)(owner.user_data);
        });
        match doc.inner().subscribe_jsonpath(path, cb) {
            Ok(sub) => LoroSubscription::into_raw(sub),
            Err(e) => {
                crate::error::record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}
