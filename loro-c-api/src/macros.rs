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

/// Dispatches an `insert_container`/`push_container`/`set_container` call over a
/// `loro::Container` enum, performing the type-erased attach generically.
///
/// `$child` is the detached child container; `$c` is bound to the typed inner handle for
/// `$op`, whose result is rewrapped into a `loro::Container`. Evaluates to
/// `loro::LoroResult<loro::Container>`. Example:
/// `attach_container!(child, |c| map.inner().insert_container(key, c))`.
#[macro_export]
macro_rules! attach_container {
    ($child:expr, |$c:ident| $op:expr) => {
        match $child {
            loro::Container::Map($c) => $op.map(loro::Container::Map),
            loro::Container::List($c) => $op.map(loro::Container::List),
            loro::Container::Text($c) => $op.map(loro::Container::Text),
            loro::Container::MovableList($c) => $op.map(loro::Container::MovableList),
            loro::Container::Tree($c) => $op.map(loro::Container::Tree),
            loro::Container::Counter($c) => $op.map(loro::Container::Counter),
            loro::Container::Unknown(_) => Err(loro::LoroError::ArgErr(
                "cannot attach a container of unknown type".into(),
            )),
        }
    };
}
