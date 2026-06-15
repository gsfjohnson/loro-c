//! Commit hooks and per-commit metadata (M4).
//!
//! - [`loro_doc_set_next_commit_message`] / [`loro_doc_set_next_commit_timestamp`] annotate
//!   the *next* commit before it happens.
//! - [`loro_doc_subscribe_pre_commit`] fires just before a commit is written to the oplog.
//!   The callback inspects the change's [`crate::version::LoroChangeMeta`] and may rewrite the
//!   commit message / timestamp via the payload setters. Returning `false` auto-unsubscribes.
//!   (Note: `import`, `export`, and `checkout` can trigger an implicit commit, so this hook
//!   runs for those too.)
//! - [`loro_doc_subscribe_first_commit_from_peer`] fires the first time a given peer id
//!   commits — handy for attaching author metadata. Returning `false` auto-unsubscribes.
//!
//! Both subscriptions return the shared `LoroSubscription` handle from `event.rs`.

use crate::doc::LoroDoc;
use crate::error::{set_last_error, LoroStatus};
use crate::event::LoroSubscription;
use crate::value::{str_from_raw, LoroBytes};
use crate::version::LoroChangeMeta;
use loro::{CommitOptions, FirstCommitFromPeerPayload, PreCommitCallbackPayload};
use std::os::raw::{c_char, c_void};

// ---------------------------------------------------------------------------
// Next-commit metadata
// ---------------------------------------------------------------------------

/// Sets the commit message `(msg, msg_len)` to attach to the next commit. The message is
/// persisted and replicates to peers.
#[no_mangle]
pub extern "C" fn loro_doc_set_next_commit_message(
    doc: *const LoroDoc,
    msg: *const c_char,
    msg_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let msg = match str_from_raw(msg, msg_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        doc.inner().set_next_commit_message(msg);
        LoroStatus::LORO_OK
    })
}

/// Sets the timestamp (seconds since the Unix epoch) for the next commit.
#[no_mangle]
pub extern "C" fn loro_doc_set_next_commit_timestamp(
    doc: *const LoroDoc,
    timestamp: i64,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().set_next_commit_timestamp(timestamp);
        LoroStatus::LORO_OK
    })
}

// ---------------------------------------------------------------------------
// Commit options (G6.3)
// ---------------------------------------------------------------------------

/// Options for [`loro_doc_commit_with`] / [`loro_doc_set_next_commit_options`].
///
/// A null `origin` / `message` pointer means "unset" (`None`); an empty-but-non-null string is a
/// real empty value. `has_timestamp == false` leaves the timestamp unset (the current time is used
/// at commit). `origin` does not persist; `message` does and replicates to peers.
#[repr(C)]
pub struct LoroCommitOptions {
    pub origin: *const c_char,
    pub origin_len: usize,
    pub message: *const c_char,
    pub message_len: usize,
    pub timestamp: i64,
    pub has_timestamp: bool,
    pub immediate_renew: bool,
}

/// Builds a `loro::CommitOptions` from the C POD. Returns `None` (after recording an error) if a
/// non-null string field is not valid UTF-8.
fn build_commit_options(opts: &LoroCommitOptions) -> Option<CommitOptions> {
    let mut co = CommitOptions::new().immediate_renew(opts.immediate_renew);
    if !opts.origin.is_null() {
        let origin = str_from_raw(opts.origin, opts.origin_len)?;
        co = co.origin(origin);
    }
    if !opts.message.is_null() {
        let message = str_from_raw(opts.message, opts.message_len)?;
        co = co.commit_msg(message);
    }
    if opts.has_timestamp {
        co = co.timestamp(opts.timestamp);
    }
    Some(co)
}

/// Commits the pending operations using `opts` (origin / message / timestamp / immediate_renew).
#[no_mangle]
pub extern "C" fn loro_doc_commit_with(doc: *const LoroDoc, opts: LoroCommitOptions) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let co = match build_commit_options(&opts) {
            Some(co) => co,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        doc.inner().commit_with(co);
        LoroStatus::LORO_OK
    })
}

/// Sets the origin `(origin, origin_len)` to attach to the next commit. The origin is reported to
/// event/commit subscribers but is not persisted.
#[no_mangle]
pub extern "C" fn loro_doc_set_next_commit_origin(
    doc: *const LoroDoc,
    origin: *const c_char,
    origin_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let origin = match str_from_raw(origin, origin_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        doc.inner().set_next_commit_origin(origin);
        LoroStatus::LORO_OK
    })
}

/// Sets the full options (origin / message / timestamp / immediate_renew) for the next commit.
#[no_mangle]
pub extern "C" fn loro_doc_set_next_commit_options(
    doc: *const LoroDoc,
    opts: LoroCommitOptions,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let co = match build_commit_options(&opts) {
            Some(co) => co,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        doc.inner().set_next_commit_options(co);
        LoroStatus::LORO_OK
    })
}

/// Clears any options previously set for the next commit (message / timestamp / origin / options).
#[no_mangle]
pub extern "C" fn loro_doc_clear_next_commit_options(doc: *const LoroDoc) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().clear_next_commit_options();
        LoroStatus::LORO_OK
    })
}

// ---------------------------------------------------------------------------
// Pre-commit hook
// ---------------------------------------------------------------------------

/// Opaque, **callback-scoped** view of a pre-commit payload. Only valid for the duration of
/// the pre-commit callback. A `*const LoroPreCommitPayload` always points to a borrowed
/// `loro::PreCommitCallbackPayload`.
#[allow(dead_code)]
pub struct LoroPreCommitPayload(PreCommitCallbackPayload);

fn payload_ref<'a>(p: *const LoroPreCommitPayload) -> Option<&'a PreCommitCallbackPayload> {
    match unsafe { (p as *const PreCommitCallbackPayload).as_ref() } {
        Some(r) => Some(r),
        None => {
            set_last_error("null pre-commit payload pointer passed to loro-c-api");
            None
        }
    }
}

/// A C pre-commit callback. `invoke` receives a callback-scoped `const LoroPreCommitPayload*`
/// and the opaque `user_data`, and returns `true` to stay subscribed (`false`
/// auto-unsubscribes). `free_user_data` (may be null) runs once when the subscription is
/// released.
#[repr(C)]
pub struct LoroPreCommitCallback {
    pub invoke: extern "C" fn(payload: *const LoroPreCommitPayload, user_data: *mut c_void) -> bool,
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

unsafe impl Send for LoroPreCommitCallback {}
unsafe impl Sync for LoroPreCommitCallback {}
impl Drop for LoroPreCommitCallback {
    fn drop(&mut self) {
        if let Some(free) = self.free_user_data {
            free(self.user_data);
        }
    }
}

/// Returns a callback-scoped pointer to the change's metadata. Do NOT free or store it; read
/// it with the `loro_change_meta_*` accessors.
#[no_mangle]
pub extern "C" fn loro_pre_commit_payload_change_meta(
    payload: *const LoroPreCommitPayload,
) -> *const LoroChangeMeta {
    ffi_guard!(std::ptr::null(), {
        match payload_ref(payload) {
            Some(p) => (&p.change_meta as *const loro::ChangeMeta) as *const LoroChangeMeta,
            None => std::ptr::null(),
        }
    })
}

/// Writes the commit's origin string (UTF-8, possibly empty) into `*out`. `*out` is only
/// written on `LORO_OK`; free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_pre_commit_payload_origin(
    payload: *const LoroPreCommitPayload,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let p = match payload_ref(payload) {
            Some(p) => p,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        unsafe { out.write(LoroBytes::from_vec(p.origin.as_bytes().to_vec())) };
        LoroStatus::LORO_OK
    })
}

/// Rewrites the message for the commit being processed to `(msg, msg_len)`.
#[no_mangle]
pub extern "C" fn loro_pre_commit_payload_set_message(
    payload: *const LoroPreCommitPayload,
    msg: *const c_char,
    msg_len: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let p = match payload_ref(payload) {
            Some(p) => p,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        let msg = match str_from_raw(msg, msg_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        p.modifier.set_message(msg);
        LoroStatus::LORO_OK
    })
}

/// Rewrites the timestamp (seconds since the Unix epoch) for the commit being processed.
#[no_mangle]
pub extern "C" fn loro_pre_commit_payload_set_timestamp(
    payload: *const LoroPreCommitPayload,
    timestamp: i64,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let p = match payload_ref(payload) {
            Some(p) => p,
            None => return LoroStatus::LORO_ERR_INVALID_ARG,
        };
        p.modifier.set_timestamp(timestamp);
        LoroStatus::LORO_OK
    })
}

/// Subscribes to pre-commit events. Returns a `LoroSubscription*` (free with
/// `loro_subscription_free` to unsubscribe), or null on a null doc.
#[no_mangle]
pub extern "C" fn loro_doc_subscribe_pre_commit(
    doc: *const LoroDoc,
    callback: LoroPreCommitCallback,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let owner = callback;
        let cb: loro::PreCommitCallback =
            Box::new(move |payload: &PreCommitCallbackPayload| -> bool {
                let owner = &owner;
                let ptr =
                    (payload as *const PreCommitCallbackPayload) as *const LoroPreCommitPayload;
                (owner.invoke)(ptr, owner.user_data)
            });
        let sub = doc.inner().subscribe_pre_commit(cb);
        LoroSubscription::into_raw(sub)
    })
}

// ---------------------------------------------------------------------------
// First-commit-from-peer hook
// ---------------------------------------------------------------------------

/// A C first-commit-from-peer callback. `invoke` receives the committing `peer` id and the
/// opaque `user_data`, and returns `true` to stay subscribed (`false` auto-unsubscribes).
/// `free_user_data` (may be null) runs once when the subscription is released.
#[repr(C)]
pub struct LoroFirstCommitFromPeerCallback {
    pub invoke: extern "C" fn(peer: u64, user_data: *mut c_void) -> bool,
    pub user_data: *mut c_void,
    pub free_user_data: Option<extern "C" fn(*mut c_void)>,
}

unsafe impl Send for LoroFirstCommitFromPeerCallback {}
unsafe impl Sync for LoroFirstCommitFromPeerCallback {}
impl Drop for LoroFirstCommitFromPeerCallback {
    fn drop(&mut self) {
        if let Some(free) = self.free_user_data {
            free(self.user_data);
        }
    }
}

/// Subscribes to the first commit from each peer. Returns a `LoroSubscription*` (free with
/// `loro_subscription_free` to unsubscribe), or null on a null doc.
#[no_mangle]
pub extern "C" fn loro_doc_subscribe_first_commit_from_peer(
    doc: *const LoroDoc,
    callback: LoroFirstCommitFromPeerCallback,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let doc = deref_or!(doc, std::ptr::null_mut());
        let owner = callback;
        let cb: loro::FirstCommitFromPeerCallback =
            Box::new(move |payload: &FirstCommitFromPeerPayload| -> bool {
                let owner = &owner;
                (owner.invoke)(payload.peer, owner.user_data)
            });
        let sub = doc.inner().subscribe_first_commit_from_peer(cb);
        LoroSubscription::into_raw(sub)
    })
}
