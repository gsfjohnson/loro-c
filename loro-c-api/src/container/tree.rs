//! `LoroTree` container handle and its operations.
//!
//! A tree is a movable hierarchy of nodes. Each node is identified by a [`LoroTreeID`]
//! (a `(peer, counter)` pair mirroring `loro::TreeID`) and carries an associated metadata
//! map ([`loro_tree_get_meta`]). A null `const LoroTreeID*` parent means "root".
//!
//! Positional moves (`mov_to` / `mov_after` / `mov_before`) and `create_at` require the
//! tree's fractional index to be enabled first with
//! [`loro_tree_enable_fractional_index`].
//!
//! Like every container handle, a `LoroTree*` is a strong co-owner of the document state
//! (see `doc.rs`) and may be freed in any order. Free with [`loro_tree_free`].

use crate::callbacks::CCallback;
use crate::container::map::LoroMap;
use crate::doc::LoroDoc;
use crate::error::{record_loro_error, set_last_error, LoroStatus};
use crate::event::{LoroDiffEvent, LoroSubscriber, LoroSubscription};
use crate::value::{value_to_json_bytes, LoroBytes};
use crate::version::LoroId;
use std::sync::Arc;

/// Identifies a tree node. Mirrors `loro::TreeID` (`peer`: the creating peer id;
/// `counter`: that peer's op counter at creation).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LoroTreeID {
    pub peer: u64,
    pub counter: i32,
}

impl LoroTreeID {
    pub(crate) fn to_loro(self) -> loro::TreeID {
        loro::TreeID::new(self.peer, self.counter)
    }

    fn from_loro(id: loro::TreeID) -> LoroTreeID {
        LoroTreeID {
            peer: id.peer,
            counter: id.counter,
        }
    }
}

/// Interprets a `*const LoroTreeID` parent pointer as `Option<TreeID>`: null means the
/// root (no parent).
fn parent_opt(parent: *const LoroTreeID) -> Option<loro::TreeID> {
    unsafe { parent.as_ref() }.map(|p| p.to_loro())
}

/// Opaque handle to a Loro tree container.
pub struct LoroTree(loro::LoroTree);

impl LoroTree {
    pub(crate) fn from_inner(t: loro::LoroTree) -> LoroTree {
        LoroTree(t)
    }

    fn inner(&self) -> &loro::LoroTree {
        &self.0
    }
}

/// Frees a tree handle. Passing null is a no-op. Safe to call before or after the
/// originating `LoroDoc*` is freed.
#[no_mangle]
pub extern "C" fn loro_tree_free(tree: *mut LoroTree) {
    ffi_guard!((), {
        if !tree.is_null() {
            unsafe {
                drop(Box::from_raw(tree));
            }
        }
    });
}

/// Writes this container's id (a string such as `cid:root-name:Tree`) into `*out`. `*out`
/// is only written on `LORO_OK`; free it with `loro_bytes_free`. Pass the written string
/// to `loro_doc_subscribe` to subscribe to this container's events.
#[no_mangle]
pub extern "C" fn loro_tree_id(tree: *const LoroTree, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let id = loro::ContainerTrait::id(tree.inner());
        unsafe { out.write(LoroBytes::from_vec(id.to_string().into_bytes())) };
        LoroStatus::LORO_OK
    })
}

/// Creates a node under `parent` (null = root) and writes its id into `*out`. `*out` is
/// only written on `LORO_OK`.
#[no_mangle]
pub extern "C" fn loro_tree_create(
    tree: *mut LoroTree,
    parent: *const LoroTreeID,
    out: *mut LoroTreeID,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match tree.inner().create(parent_opt(parent)) {
            Ok(id) => {
                unsafe { out.write(LoroTreeID::from_loro(id)) };
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Creates a node under `parent` (null = root) at child position `index`, writing its id
/// into `*out`. Requires the fractional index to be enabled.
#[no_mangle]
pub extern "C" fn loro_tree_create_at(
    tree: *mut LoroTree,
    parent: *const LoroTreeID,
    index: usize,
    out: *mut LoroTreeID,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match tree.inner().create_at(parent_opt(parent), index) {
            Ok(id) => {
                unsafe { out.write(LoroTreeID::from_loro(id)) };
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Moves `target` to be a child of `parent` (null = root).
#[no_mangle]
pub extern "C" fn loro_tree_mov(
    tree: *mut LoroTree,
    target: LoroTreeID,
    parent: *const LoroTreeID,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        match tree.inner().mov(target.to_loro(), parent_opt(parent)) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Moves `target` to child position `index` under `parent` (null = root). Requires the
/// fractional index to be enabled.
#[no_mangle]
pub extern "C" fn loro_tree_mov_to(
    tree: *mut LoroTree,
    target: LoroTreeID,
    parent: *const LoroTreeID,
    index: usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        match tree.inner().mov_to(target.to_loro(), parent_opt(parent), index) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Moves `target` to be the sibling immediately after `after`. Requires the fractional
/// index to be enabled.
#[no_mangle]
pub extern "C" fn loro_tree_mov_after(
    tree: *mut LoroTree,
    target: LoroTreeID,
    after: LoroTreeID,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        match tree.inner().mov_after(target.to_loro(), after.to_loro()) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Moves `target` to be the sibling immediately before `before`. Requires the fractional
/// index to be enabled.
#[no_mangle]
pub extern "C" fn loro_tree_mov_before(
    tree: *mut LoroTree,
    target: LoroTreeID,
    before: LoroTreeID,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        match tree.inner().mov_before(target.to_loro(), before.to_loro()) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Deletes the node `target` (and its subtree).
#[no_mangle]
pub extern "C" fn loro_tree_delete(tree: *mut LoroTree, target: LoroTreeID) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        match tree.inner().delete(target.to_loro()) {
            Ok(()) => LoroStatus::LORO_OK,
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Returns the metadata map of `target` as a `LoroMap*`, or null on error. Free the
/// returned handle with `loro_map_free`.
#[no_mangle]
pub extern "C" fn loro_tree_get_meta(tree: *const LoroTree, target: LoroTreeID) -> *mut LoroMap {
    ffi_guard!(std::ptr::null_mut(), {
        let tree = deref_or!(tree, std::ptr::null_mut());
        match tree.inner().get_meta(target.to_loro()) {
            Ok(map) => Box::into_raw(Box::new(LoroMap::from_inner(map))),
            Err(e) => {
                record_loro_error(&e);
                std::ptr::null_mut()
            }
        }
    })
}

/// Returns `true` if `target` currently exists (is alive) in the tree.
#[no_mangle]
pub extern "C" fn loro_tree_contains(tree: *const LoroTree, target: LoroTreeID) -> bool {
    ffi_guard!(false, {
        let tree = deref_or!(tree, false);
        tree.inner().contains(target.to_loro())
    })
}

/// Writes whether `target` has been deleted into `*out`. Returns `LORO_ERR_NOT_FOUND` if
/// the node never existed.
#[no_mangle]
pub extern "C" fn loro_tree_is_node_deleted(
    tree: *const LoroTree,
    target: LoroTreeID,
    out: *mut bool,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match tree.inner().is_node_deleted(&target.to_loro()) {
            Ok(deleted) => {
                unsafe { out.write(deleted) };
                LoroStatus::LORO_OK
            }
            Err(e) => record_loro_error(&e),
        }
    })
}

/// Writes `target`'s fractional index (as a UTF-8 string) into `*out`. Returns
/// `LORO_ERR_NOT_FOUND` if the node has none (e.g. the fractional index is disabled).
/// Free a written `*out` with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_tree_fractional_index(
    tree: *const LoroTree,
    target: LoroTreeID,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match tree.inner().fractional_index(target.to_loro()) {
            Some(s) => {
                unsafe { out.write(LoroBytes::from_vec(s.into_bytes())) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("node has no fractional index");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Returns the number of root nodes. Returns 0 on a null handle.
#[no_mangle]
pub extern "C" fn loro_tree_roots_len(tree: *const LoroTree) -> usize {
    ffi_guard!(0usize, {
        let tree = deref_or!(tree, 0usize);
        tree.inner().roots().len()
    })
}

/// Writes the root node at `index` into `*out`. Returns `LORO_ERR_NOT_FOUND` if `index`
/// is out of range.
#[no_mangle]
pub extern "C" fn loro_tree_root_at(
    tree: *const LoroTree,
    index: usize,
    out: *mut LoroTreeID,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match tree.inner().roots().get(index) {
            Some(id) => {
                unsafe { out.write(LoroTreeID::from_loro(*id)) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("root index out of range");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Writes the number of children of `parent` (null = root) into `*out`. Returns
/// `LORO_ERR_NOT_FOUND` if `parent` does not exist.
#[no_mangle]
pub extern "C" fn loro_tree_children_len(
    tree: *const LoroTree,
    parent: *const LoroTreeID,
    out: *mut usize,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match tree.inner().children_num(parent_opt(parent)) {
            Some(n) => {
                unsafe { out.write(n) };
                LoroStatus::LORO_OK
            }
            None => {
                set_last_error("parent node not found");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Writes the child of `parent` (null = root) at `index` into `*out`. Returns
/// `LORO_ERR_NOT_FOUND` if `parent` does not exist or `index` is out of range.
#[no_mangle]
pub extern "C" fn loro_tree_child_at(
    tree: *const LoroTree,
    parent: *const LoroTreeID,
    index: usize,
    out: *mut LoroTreeID,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        match tree.inner().children(parent_opt(parent)) {
            Some(children) => match children.get(index) {
                Some(id) => {
                    unsafe { out.write(LoroTreeID::from_loro(*id)) };
                    LoroStatus::LORO_OK
                }
                None => {
                    set_last_error("child index out of range");
                    LoroStatus::LORO_ERR_NOT_FOUND
                }
            },
            None => {
                set_last_error("parent node not found");
                LoroStatus::LORO_ERR_NOT_FOUND
            }
        }
    })
}

/// Enables the tree's fractional index (required for positional moves), using `jitter`
/// bits of randomness to reduce interleaving between concurrent inserts.
#[no_mangle]
pub extern "C" fn loro_tree_enable_fractional_index(
    tree: *mut LoroTree,
    jitter: u8,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        tree.inner().enable_fractional_index(jitter);
        LoroStatus::LORO_OK
    })
}

/// Returns whether the tree's fractional index is enabled. Returns `false` on a null
/// handle.
#[no_mangle]
pub extern "C" fn loro_tree_is_fractional_index_enabled(tree: *const LoroTree) -> bool {
    ffi_guard!(false, {
        let tree = deref_or!(tree, false);
        tree.inner().is_fractional_index_enabled()
    })
}

/// Returns `true` if the tree has no nodes (also returns `true`, with an error recorded,
/// on a null handle).
#[no_mangle]
pub extern "C" fn loro_tree_is_empty(tree: *const LoroTree) -> bool {
    ffi_guard!(true, {
        let tree = deref_or!(tree, true);
        tree.inner().is_empty()
    })
}

/// Writes the whole tree as a JSON value into `*out`. `*out` is only written on `LORO_OK`;
/// free it with `loro_bytes_free`.
#[no_mangle]
pub extern "C" fn loro_tree_to_json(tree: *const LoroTree, out: *mut LoroBytes) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let tree = deref_or!(tree, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let value = tree.inner().get_value();
        match value_to_json_bytes(&value) {
            Some(bytes) => {
                unsafe { out.write(LoroBytes::from_vec(bytes)) };
                LoroStatus::LORO_OK
            }
            None => LoroStatus::LORO_ERR_ENCODE,
        }
    })
}

// ---------------------------------------------------------------------------
// G6.4 — uniform container introspection (via loro::ContainerTrait) + attribution
// ---------------------------------------------------------------------------

/// Returns whether this tree container has been deleted from its document.
#[no_mangle]
pub extern "C" fn loro_tree_is_deleted(tree: *const LoroTree) -> bool {
    ffi_guard!(false, {
        let tree = deref_or!(tree, false);
        loro::ContainerTrait::is_deleted(tree.inner())
    })
}

/// Returns whether this tree container is attached to a document.
#[no_mangle]
pub extern "C" fn loro_tree_is_attached(tree: *const LoroTree) -> bool {
    ffi_guard!(false, {
        let tree = deref_or!(tree, false);
        loro::ContainerTrait::is_attached(tree.inner())
    })
}

/// If this detached container has an attached counterpart in its document, returns a new
/// handle to it; otherwise returns null. Free the result with [`loro_tree_free`].
#[no_mangle]
pub extern "C" fn loro_tree_get_attached(tree: *const LoroTree) -> *mut LoroTree {
    ffi_guard!(std::ptr::null_mut(), {
        let tree = deref_or!(tree, std::ptr::null_mut());
        match loro::ContainerTrait::get_attached(tree.inner()) {
            Some(t) => Box::into_raw(Box::new(LoroTree::from_inner(t))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Returns a new handle to the document this container belongs to, or null if it is
/// detached. Free the result with `loro_doc_free`.
#[no_mangle]
pub extern "C" fn loro_tree_doc(tree: *const LoroTree) -> *mut LoroDoc {
    ffi_guard!(std::ptr::null_mut(), {
        let tree = deref_or!(tree, std::ptr::null_mut());
        match loro::ContainerTrait::doc(tree.inner()) {
            Some(d) => Box::into_raw(Box::new(LoroDoc::from_inner(d))),
            None => std::ptr::null_mut(),
        }
    })
}

/// Subscribes to changes of this tree container. Returns a `LoroSubscription*`, or null if
/// the container is detached / on a null handle / caught panic. Free it with
/// `loro_subscription_free` (which unsubscribes).
#[no_mangle]
pub extern "C" fn loro_tree_subscribe(
    tree: *const LoroTree,
    callback: LoroSubscriber,
) -> *mut LoroSubscription {
    ffi_guard!(std::ptr::null_mut(), {
        let tree = deref_or!(tree, std::ptr::null_mut());
        // Resolve the doc first: a detached container returns null WITHOUT taking ownership
        // of the callback (the caller frees it), mirroring `loro_doc_subscribe`'s contract.
        let doc = match loro::ContainerTrait::doc(tree.inner()) {
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
        let sub = doc.subscribe(&loro::ContainerTrait::id(tree.inner()), subscriber);
        LoroSubscription::into_raw(sub)
    })
}

/// Writes the op id of the last move of node `target` into `*out` and returns true; returns
/// false (leaving `*out` untouched) when the node has no recorded move (e.g. never moved or
/// unknown), or on a null handle / null `out` / caught panic.
#[no_mangle]
pub extern "C" fn loro_tree_get_last_move_id(
    tree: *const LoroTree,
    target: LoroTreeID,
    out: *mut LoroId,
) -> bool {
    ffi_guard!(false, {
        let tree = deref_or!(tree, false);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return false;
        }
        match tree.inner().get_last_move_id(&target.to_loro()) {
            Some(id) => {
                unsafe { out.write(LoroId::from_loro(id)) };
                true
            }
            None => false,
        }
    })
}
