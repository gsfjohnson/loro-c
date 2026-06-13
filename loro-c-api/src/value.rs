//! Owned byte buffers returned across the FFI boundary.
//!
//! Binary and string outputs are returned as a [`LoroBytes`] that the caller must release
//! with [`loro_bytes_free`]. The buffer is NOT guaranteed to be nul-terminated (string
//! outputs carry their byte length in `len`); the C++ wrapper turns it into a
//! `std::string` / `std::vector<uint8_t>` using `len`.

/// An owned, heap-allocated byte buffer handed to the caller.
///
/// Free it exactly once with [`loro_bytes_free`]. `data` may be null only when `len`
/// and `cap` are both `0` (an empty buffer); in that case `loro_bytes_free` is a no-op.
#[repr(C)]
pub struct LoroBytes {
    /// Pointer to the bytes. Owned by the buffer; freed by `loro_bytes_free`.
    pub data: *mut u8,
    /// Number of valid bytes in `data`.
    pub len: usize,
    /// Allocation capacity backing `data` (needed to reconstruct the Rust `Vec`).
    pub cap: usize,
}

impl LoroBytes {
    /// Builds a `LoroBytes` that takes ownership of `v`'s allocation.
    pub fn from_vec(mut v: Vec<u8>) -> LoroBytes {
        let data = v.as_mut_ptr();
        let len = v.len();
        let cap = v.capacity();
        std::mem::forget(v);
        LoroBytes { data, len, cap }
    }

    /// An empty buffer.
    pub fn empty() -> LoroBytes {
        LoroBytes {
            data: std::ptr::null_mut(),
            len: 0,
            cap: 0,
        }
    }
}

/// Frees a [`LoroBytes`] previously returned by this library. Passing an
/// all-zero/empty buffer is a no-op. Must be called at most once per buffer.
#[no_mangle]
pub extern "C" fn loro_bytes_free(bytes: LoroBytes) {
    ffi_guard!((), {
        if !bytes.data.is_null() {
            // Reconstruct the original Vec and let it drop.
            unsafe {
                drop(Vec::from_raw_parts(bytes.data, bytes.len, bytes.cap));
            }
        }
    });
}
