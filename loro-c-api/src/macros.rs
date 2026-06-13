//! Internal helper macros shared across the FFI modules. Not part of the public C ABI.

/// Wraps an FFI body in `catch_unwind` so a Rust panic is converted into `$fallback`
/// instead of unwinding across the C ABI boundary (which is undefined behaviour).
///
/// Usage: `ffi_guard!(LoroStatus::LORO_ERR_PANIC, { ...body returning LoroStatus... })`
/// or for pointer-returning functions: `ffi_guard!(std::ptr::null_mut(), { ... })`.
#[macro_export]
macro_rules! ffi_guard {
    ($fallback:expr, $body:block) => {{
        match std::panic::catch_unwind(std::panic::AssertUnwindSafe(move || $body)) {
            Ok(v) => v,
            Err(_) => {
                $crate::error::set_last_error("panic caught at the loro-c-api FFI boundary");
                $fallback
            }
        }
    }};
}

/// Dereferences a `*const T` / `*mut T` handle, returning `$fallback` (after recording a
/// last-error) if the pointer is null. Produces a shared reference `&T`.
#[macro_export]
macro_rules! deref_or {
    ($ptr:expr, $fallback:expr) => {{
        match unsafe { $ptr.as_ref() } {
            Some(r) => r,
            None => {
                $crate::error::set_last_error("null pointer passed to loro-c-api");
                return $fallback;
            }
        }
    }};
}
