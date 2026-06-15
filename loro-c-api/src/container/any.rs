//! `LoroContainer` — a type-erased container handle used as the currency for nested
//! containers.
//!
//! A `LoroContainer*` wraps a `loro::Container` (the enum over all container kinds). It is
//! how a child container is passed into `*_insert_container` (as a *detached* container
//! created with [`loro_container_new`]) and how an attached child is handed back out. A
//! typed handle (`LoroMap*`, `LoroList*`, ...) is recovered with the matching
//! `loro_container_get_*` accessor.
//!
//! Like the typed handles, a `LoroContainer*` is a strong co-owner of the document state
//! once it is attached, and may be freed in any order. Free it with
//! [`loro_container_free`].

use crate::container::counter::LoroCounter;
use crate::container::list::LoroList;
use crate::container::map::LoroMap;
use crate::container::movable_list::LoroMovableList;
use crate::container::text::LoroText;
use crate::container::tree::LoroTree;
use crate::error::{set_last_error, LoroStatus};
use crate::value::LoroBytes;

/// Opaque, type-erased handle to any Loro container.
pub struct LoroContainer(loro::Container);

impl LoroContainer {
    pub(crate) fn from_inner(c: loro::Container) -> LoroContainer {
        LoroContainer(c)
    }

    pub(crate) fn inner(&self) -> &loro::Container {
        &self.0
    }
}

/// Consumes a `*mut LoroContainer`, returning the owned inner `loro::Container`. Records an
/// error and returns `None` on a null pointer. Used by `*_insert_container` to take
/// ownership of the detached child.
pub(crate) fn take(ptr: *mut LoroContainer) -> Option<loro::Container> {
    if ptr.is_null() {
        set_last_error("null container pointer passed to loro-c-api");
        return None;
    }
    Some(unsafe { Box::from_raw(ptr) }.0)
}

/// The kind of a [`LoroContainer`]. Mirrors `loro::ContainerType`.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroContainerType {
    LORO_CONTAINER_MAP = 0,
    LORO_CONTAINER_LIST = 1,
    LORO_CONTAINER_TEXT = 2,
    LORO_CONTAINER_MOVABLE_LIST = 3,
    LORO_CONTAINER_TREE = 4,
    LORO_CONTAINER_COUNTER = 5,
    /// An unknown / unsupported container kind. Cannot be created.
    LORO_CONTAINER_UNKNOWN = 6,
}

/// Creates a new *detached* container of the given kind. Attach it by passing it to a
/// `*_insert_container` / `*_push_container` / `*_set_container` function, which returns
/// the attached handle and consumes this one. Returns null for
/// `LORO_CONTAINER_UNKNOWN` or on a caught panic. Release an unused handle with
/// [`loro_container_free`].
#[no_mangle]
pub extern "C" fn loro_container_new(ty: LoroContainerType) -> *mut LoroContainer {
    ffi_guard!(std::ptr::null_mut(), {
        let container = match ty {
            LoroContainerType::LORO_CONTAINER_MAP => loro::Container::Map(loro::LoroMap::new()),
            LoroContainerType::LORO_CONTAINER_LIST => loro::Container::List(loro::LoroList::new()),
            LoroContainerType::LORO_CONTAINER_TEXT => loro::Container::Text(loro::LoroText::new()),
            LoroContainerType::LORO_CONTAINER_MOVABLE_LIST => {
                loro::Container::MovableList(loro::LoroMovableList::new())
            }
            LoroContainerType::LORO_CONTAINER_TREE => loro::Container::Tree(loro::LoroTree::new()),
            LoroContainerType::LORO_CONTAINER_COUNTER => {
                loro::Container::Counter(loro::LoroCounter::new())
            }
            LoroContainerType::LORO_CONTAINER_UNKNOWN => {
                set_last_error("cannot create a container of unknown type");
                return std::ptr::null_mut();
            }
        };
        Box::into_raw(Box::new(LoroContainer::from_inner(container)))
    })
}

/// Frees a container handle. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_container_free(container: *mut LoroContainer) {
    ffi_guard!((), {
        if !container.is_null() {
            unsafe {
                drop(Box::from_raw(container));
            }
        }
    });
}

/// Writes this container's id (a string such as `cid:root-name:Map`) into `*out`. `*out`
/// is only written on `LORO_OK`; free it with `loro_bytes_free`. Pass the written string
/// to `loro_doc_subscribe` to subscribe to this container's events. Returns
/// `LORO_ERR_INVALID_ARG` for a detached (not-yet-attached) container.
#[no_mangle]
pub extern "C" fn loro_container_id(
    container: *const LoroContainer,
    out: *mut LoroBytes,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let container = deref_or!(container, LoroStatus::LORO_ERR_INVALID_ARG);
        if out.is_null() {
            set_last_error("null out pointer passed to loro-c-api");
            return LoroStatus::LORO_ERR_INVALID_ARG;
        }
        let id = loro::ContainerTrait::id(container.inner());
        unsafe { out.write(LoroBytes::from_vec(id.to_string().into_bytes())) };
        LoroStatus::LORO_OK
    })
}

/// Maps a `loro::Container` to its [`LoroContainerType`]. Shared by [`loro_container_type`] and
/// the value-navigation accessors (see [`crate::value_or_container`]).
pub(crate) fn container_type_of(container: &loro::Container) -> LoroContainerType {
    match container {
        loro::Container::Map(_) => LoroContainerType::LORO_CONTAINER_MAP,
        loro::Container::List(_) => LoroContainerType::LORO_CONTAINER_LIST,
        loro::Container::Text(_) => LoroContainerType::LORO_CONTAINER_TEXT,
        loro::Container::MovableList(_) => LoroContainerType::LORO_CONTAINER_MOVABLE_LIST,
        loro::Container::Tree(_) => LoroContainerType::LORO_CONTAINER_TREE,
        loro::Container::Counter(_) => LoroContainerType::LORO_CONTAINER_COUNTER,
        loro::Container::Unknown(_) => LoroContainerType::LORO_CONTAINER_UNKNOWN,
    }
}

/// Returns the kind of the container. Returns `LORO_CONTAINER_UNKNOWN` on a null handle.
#[no_mangle]
pub extern "C" fn loro_container_type(container: *const LoroContainer) -> LoroContainerType {
    ffi_guard!(LoroContainerType::LORO_CONTAINER_UNKNOWN, {
        let container = deref_or!(container, LoroContainerType::LORO_CONTAINER_UNKNOWN);
        container_type_of(container.inner())
    })
}

/// Recovers a typed `LoroMap*` from a container, or null (with an error recorded) if the
/// container is not a map. The returned handle is independent; free it with
/// `loro_map_free`. The source `LoroContainer*` is unaffected.
#[no_mangle]
pub extern "C" fn loro_container_get_map(container: *const LoroContainer) -> *mut LoroMap {
    ffi_guard!(std::ptr::null_mut(), {
        let container = deref_or!(container, std::ptr::null_mut());
        match container.inner() {
            loro::Container::Map(m) => Box::into_raw(Box::new(LoroMap::from_inner(m.clone()))),
            _ => {
                set_last_error("container is not a map");
                std::ptr::null_mut()
            }
        }
    })
}

/// Recovers a typed `LoroList*` from a container, or null if it is not a list.
#[no_mangle]
pub extern "C" fn loro_container_get_list(container: *const LoroContainer) -> *mut LoroList {
    ffi_guard!(std::ptr::null_mut(), {
        let container = deref_or!(container, std::ptr::null_mut());
        match container.inner() {
            loro::Container::List(l) => Box::into_raw(Box::new(LoroList::from_inner(l.clone()))),
            _ => {
                set_last_error("container is not a list");
                std::ptr::null_mut()
            }
        }
    })
}

/// Recovers a typed `LoroText*` from a container, or null if it is not a text container.
#[no_mangle]
pub extern "C" fn loro_container_get_text(container: *const LoroContainer) -> *mut LoroText {
    ffi_guard!(std::ptr::null_mut(), {
        let container = deref_or!(container, std::ptr::null_mut());
        match container.inner() {
            loro::Container::Text(t) => Box::into_raw(Box::new(LoroText::from_inner(t.clone()))),
            _ => {
                set_last_error("container is not a text container");
                std::ptr::null_mut()
            }
        }
    })
}

/// Recovers a typed `LoroMovableList*` from a container, or null if it is not a movable
/// list.
#[no_mangle]
pub extern "C" fn loro_container_get_movable_list(
    container: *const LoroContainer,
) -> *mut LoroMovableList {
    ffi_guard!(std::ptr::null_mut(), {
        let container = deref_or!(container, std::ptr::null_mut());
        match container.inner() {
            loro::Container::MovableList(l) => {
                Box::into_raw(Box::new(LoroMovableList::from_inner(l.clone())))
            }
            _ => {
                set_last_error("container is not a movable list");
                std::ptr::null_mut()
            }
        }
    })
}

/// Recovers a typed `LoroTree*` from a container, or null if it is not a tree.
#[no_mangle]
pub extern "C" fn loro_container_get_tree(container: *const LoroContainer) -> *mut LoroTree {
    ffi_guard!(std::ptr::null_mut(), {
        let container = deref_or!(container, std::ptr::null_mut());
        match container.inner() {
            loro::Container::Tree(t) => Box::into_raw(Box::new(LoroTree::from_inner(t.clone()))),
            _ => {
                set_last_error("container is not a tree");
                std::ptr::null_mut()
            }
        }
    })
}

/// Recovers a typed `LoroCounter*` from a container, or null if it is not a counter.
#[no_mangle]
pub extern "C" fn loro_container_get_counter(container: *const LoroContainer) -> *mut LoroCounter {
    ffi_guard!(std::ptr::null_mut(), {
        let container = deref_or!(container, std::ptr::null_mut());
        match container.inner() {
            loro::Container::Counter(c) => {
                Box::into_raw(Box::new(LoroCounter::from_inner(c.clone())))
            }
            _ => {
                set_last_error("container is not a counter");
                std::ptr::null_mut()
            }
        }
    })
}
