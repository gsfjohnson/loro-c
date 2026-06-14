//! Generic bridge for marshalling C callbacks across the FFI boundary.
//!
//! A C callback is supplied as a triple — an `invoke` function pointer, an opaque
//! `user_data` pointer, and an optional `free_user_data` destructor. [`CCallback`] owns
//! that triple: it stores the three parts and runs `free_user_data(user_data)` exactly
//! once when it is dropped (i.e. when the owning subscription / closure is released).
//!
//! `CCallback` is asserted `Send + Sync` so it can live inside loro's `Arc` / `Box`
//! subscriber closures, which loro may invoke from any thread that mutates the document.
//! The C side must therefore supply a callback (and `user_data`) that is safe to call
//! from an arbitrary thread; this contract is documented in `loro.h` / `loro.hpp`.
//!
//! This module carries no `#[no_mangle]` surface of its own. The concrete `#[repr(C)]`
//! callback structs (e.g. `LoroSubscriber`) live next to the API that consumes them;
//! each simply copies its triple into a `CCallback` to take ownership. The same machinery
//! is reused by the M4 callback-bearing APIs (awareness / ephemeral / undo / pre-commit).

use std::os::raw::c_void;

/// Owns a C callback triple and runs `free_user_data` on drop. `F` is the concrete
/// `extern "C"` invoke function-pointer type for the particular callback shape.
pub struct CCallback<F> {
    /// The C function to invoke. Its exact signature depends on the callback kind.
    pub invoke: F,
    /// Opaque user data forwarded to `invoke` and to `free_user_data`.
    pub user_data: *mut c_void,
    /// Optional destructor for `user_data`, run once when this `CCallback` is dropped.
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

// SAFETY (by contract): loro can fire subscribers from whichever thread mutates the
// document, so the C callback and its `user_data` must be safe to use from any thread.
// The C++ wrapper documents this requirement to the user.
unsafe impl<F> Send for CCallback<F> {}
unsafe impl<F> Sync for CCallback<F> {}

impl<F> Drop for CCallback<F> {
    fn drop(&mut self) {
        if let Some(free) = self.free_user_data {
            free(self.user_data);
        }
    }
}
