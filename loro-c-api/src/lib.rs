//! `loro-c-api` — a pure C ABI wrapper around the [`loro`](https://crates.io/crates/loro)
//! CRDT library.
//!
//! The public surface is a set of `#[no_mangle] extern "C"` functions plus a handful of
//! `#[repr(C)]` types. Every Rust type is exposed as an opaque handle (a boxed pointer);
//! every fallible function returns a [`LoroStatus`] and stashes a detail message in a
//! thread-local last-error slot (see [`error`]).
//!
//! Each FFI entry point is wrapped in [`ffi_guard!`] so a Rust panic can never unwind
//! across the C ABI boundary (which is UB); instead it is converted into
//! `LORO_ERR_PANIC` / a null pointer.

#[macro_use]
mod macros;

pub mod awareness;
pub mod callbacks;
pub mod commit;
pub mod container;
pub mod doc;
pub mod error;
pub mod event;
pub mod fractional_index;
// The `loro` dependency unconditionally enables its `jsonpath` feature (see Cargo.toml),
// so this module is always available.
pub mod jsonpath;
pub mod undo;
pub mod value;
pub mod version;

use std::os::raw::c_char;

/// Returns the version of the underlying `loro` Rust crate as a static,
/// nul-terminated C string. The returned pointer is valid for the lifetime of the
/// program and must NOT be freed.
#[no_mangle]
pub extern "C" fn loro_version() -> *const c_char {
    // loro::LORO_VERSION is a &'static str without a trailing nul; expose a nul-terminated
    // static built once.
    static VERSION_C: std::sync::OnceLock<std::ffi::CString> = std::sync::OnceLock::new();
    VERSION_C
        .get_or_init(|| std::ffi::CString::new(loro::LORO_VERSION).unwrap_or_default())
        .as_ptr()
}
