//! Text-style configuration (G1): controls how a text mark expands when text is inserted
//! at its boundaries.
//!
//! Build a [`LoroStyleConfigMap`] (one entry per style key), then hand it to
//! [`loro_doc_config_text_style`]; or set a single fallback via
//! [`loro_doc_config_default_text_style`]. The expand type chosen here must match the type
//! used when calling `loro_text_mark` / `loro_text_unmark` for that key.

use crate::doc::LoroDoc;
use crate::error::{set_last_error, LoroStatus};
use crate::value::str_from_raw;
use loro::{ExpandType, StyleConfig, StyleConfigMap};
use std::os::raw::c_char;

/// How a text mark expands when text is inserted at its boundaries. Mirrors
/// `loro::ExpandType`.
#[repr(C)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoroExpandType {
    /// Inserting text just before the range extends the mark to cover it.
    LORO_EXPAND_BEFORE = 0,
    /// Inserting text just after the range extends the mark to cover it (the usual default).
    LORO_EXPAND_AFTER = 1,
    /// Inserting text at either boundary extends the mark.
    LORO_EXPAND_BOTH = 2,
    /// The mark never expands at its boundaries.
    LORO_EXPAND_NONE = 3,
}

fn to_expand_type(e: LoroExpandType) -> ExpandType {
    match e {
        LoroExpandType::LORO_EXPAND_BEFORE => ExpandType::Before,
        LoroExpandType::LORO_EXPAND_AFTER => ExpandType::After,
        LoroExpandType::LORO_EXPAND_BOTH => ExpandType::Both,
        LoroExpandType::LORO_EXPAND_NONE => ExpandType::None,
    }
}

/// Expand configuration for a single style key. Plain-old-data, passed by value.
#[repr(C)]
pub struct LoroStyleConfig {
    /// Expand behaviour for marks created with this key.
    pub expand: LoroExpandType,
}

/// Opaque, mutable builder mapping style keys to their expand behaviour. Create with
/// [`loro_style_config_map_new`], populate with [`loro_style_config_map_insert`], hand to
/// [`loro_doc_config_text_style`], then release with [`loro_style_config_map_free`].
pub struct LoroStyleConfigMap(StyleConfigMap);

impl LoroStyleConfigMap {
    fn inner(&self) -> &StyleConfigMap {
        &self.0
    }
}

/// Creates an empty style-config map. Release with [`loro_style_config_map_free`].
#[no_mangle]
pub extern "C" fn loro_style_config_map_new() -> *mut LoroStyleConfigMap {
    ffi_guard!(std::ptr::null_mut(), {
        Box::into_raw(Box::new(LoroStyleConfigMap(StyleConfigMap::new())))
    })
}

/// Inserts/overwrites the entry for style `key` (a UTF-8 string `(key, key_len)`, which
/// must not contain `:`).
#[no_mangle]
pub extern "C" fn loro_style_config_map_insert(
    map: *mut LoroStyleConfigMap,
    key: *const c_char,
    key_len: usize,
    config: LoroStyleConfig,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let map = match unsafe { map.as_mut() } {
            Some(m) => m,
            None => {
                set_last_error("null pointer passed to loro-c-api");
                return LoroStatus::LORO_ERR_INVALID_ARG;
            }
        };
        let key = match str_from_raw(key, key_len) {
            Some(s) => s,
            None => return LoroStatus::LORO_ERR_UTF8,
        };
        map.0.insert(
            key.into(),
            StyleConfig {
                expand: to_expand_type(config.expand),
            },
        );
        LoroStatus::LORO_OK
    })
}

/// Frees a style-config map. Passing null is a no-op.
#[no_mangle]
pub extern "C" fn loro_style_config_map_free(map: *mut LoroStyleConfigMap) {
    ffi_guard!((), {
        if !map.is_null() {
            unsafe {
                drop(Box::from_raw(map));
            }
        }
    });
}

/// Applies the style configuration in `map` to `doc`. The map is copied; the caller still
/// owns it and must free it.
#[no_mangle]
pub extern "C" fn loro_doc_config_text_style(
    doc: *const LoroDoc,
    map: *const LoroStyleConfigMap,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let map = deref_or!(map, LoroStatus::LORO_ERR_INVALID_ARG);
        doc.inner().config_text_style(map.inner().clone());
        LoroStatus::LORO_OK
    })
}

/// Sets the document's default text style (used for any key without an explicit entry).
/// Pass a null `config` to reset the default.
#[no_mangle]
pub extern "C" fn loro_doc_config_default_text_style(
    doc: *const LoroDoc,
    config: *const LoroStyleConfig,
) -> LoroStatus {
    ffi_guard!(LoroStatus::LORO_ERR_PANIC, {
        let doc = deref_or!(doc, LoroStatus::LORO_ERR_INVALID_ARG);
        let style = if config.is_null() {
            None
        } else {
            let c = unsafe { &*config };
            Some(StyleConfig {
                expand: to_expand_type(c.expand),
            })
        };
        doc.inner().config_default_text_style(style);
        LoroStatus::LORO_OK
    })
}
