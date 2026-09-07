//! The raw C API. Layouts are `repr(C)` copies of the structs in
//! `tree_feller.h`, kept in sync by hand; the differential tests are what
//! confirm they still line up. [`crate::Language`] and [`crate::Options`] are
//! the documented, safe way to call into this.
#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_void};

/// Fixed byte size of `TFError::message` below.
pub const TF_ERROR_MESSAGE_SIZE: usize = 512;

/// Opaque handle to a loaded language, owned until passed to
/// `tf_language_free`.
pub enum TFLanguage {}

/// A source position. As in tree-sitter, only `\n` advances `row`, and
/// `column` counts bytes.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TFPoint {
    pub row: u32,
    pub column: u32,
}

/// Where and why a parse failed: a byte offset and point, and a
/// NUL-terminated message.
#[repr(C)]
pub struct TFError {
    pub byte: u32,
    pub point: TFPoint,
    pub message: [c_char; TF_ERROR_MESSAGE_SIZE],
}

impl Default for TFError {
    fn default() -> Self {
        Self {
            byte: 0,
            point: TFPoint::default(),
            message: [0; TF_ERROR_MESSAGE_SIZE],
        }
    }
}

/// A memory-mapped source file. `mapped` records whether `data` needs
/// unmapping on close; not meaningful to a caller.
#[repr(C)]
pub struct TFFile {
    pub data: *const c_void,
    pub size: u32,
    pub mapped: bool,
}

/// One child of a [`TFVisibleNode`]: its symbol, the field it fills in the
/// parent (0 for none), and whether it is an extra -- whitespace or a
/// comment, which never fills a field.
#[repr(C)]
pub struct TFVisibleChild {
    pub symbol: u16,
    pub field_id: u16,
    pub extra: bool,
    pub value: *mut c_void,
}

/// A node in the visibility-filtered stream: tree-sitter's public symbol,
/// with any alias its parent applied, plus its span and children. `children`
/// is valid only for the duration of the callback that receives it.
#[repr(C)]
pub struct TFVisibleNode {
    pub symbol: u16,
    pub production_id: u16,
    pub named: bool,
    pub extra: bool,
    pub start_byte: u32,
    pub end_byte: u32,
    pub start_point: TFPoint,
    pub end_point: TFPoint,
    pub child_count: u32,
    pub children: *const TFVisibleChild,
}

/// Callbacks for [`tf_parse_visible`]. `on_node` fires once per node,
/// children before parents; `on_discard` once per value nothing consumed,
/// after a failed parse. `on_hidden` and `named_only` change what is folded
/// or skipped -- see [`crate::Visit::hidden`] and [`crate::Options::named_only`]
/// for the semantics, since this layer only carries the bits across the FFI
/// boundary.
#[repr(C)]
pub struct TFVisibleSink {
    pub payload: *mut c_void,
    pub on_node: Option<unsafe extern "C" fn(*mut c_void, *const TFVisibleNode) -> *mut c_void>,
    pub on_discard: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub on_hidden: Option<unsafe extern "C" fn(*mut c_void, *const TFVisibleNode) -> *mut c_void>,
    pub named_only: bool,
}

extern "C" {
    /// Loads `language`, or returns NULL if it is not ABI 15. `error` may be
    /// NULL. The returned handle owns nothing from `language`, which must
    /// outlive it.
    pub fn tf_language_load(language: *const c_void, error: *mut *const c_char) -> *mut TFLanguage;
    /// Frees a handle from `tf_language_load`. Safe to call with NULL.
    pub fn tf_language_free(language: *mut TFLanguage);
    /// Name of a symbol id, or NULL if it is out of range.
    pub fn tf_language_symbol_name(language: *const TFLanguage, symbol: u16) -> *const c_char;
    /// Name of a field id, or NULL if it is out of range.
    pub fn tf_language_field_name(language: *const TFLanguage, field: u16) -> *const c_char;

    /// Maps `path` for parsing. Returns false and fills `error` on failure;
    /// `error` may be NULL. Safe to call `tf_file_close` on the result either
    /// way.
    pub fn tf_file_open(file: *mut TFFile, path: *const c_char, error: *mut TFError) -> bool;
    /// Unmaps a file opened by `tf_file_open`. Safe to call on one that failed
    /// to open or was already closed.
    pub fn tf_file_close(file: *mut TFFile);

    /// Parses `source`, reporting visible nodes to `sink` in the order a CST
    /// walk would. Returns false and fills `error` on the first parse error.
    pub fn tf_parse_visible(
        language: *const TFLanguage,
        source: *const c_void,
        size: usize,
        sink: *const TFVisibleSink,
        root: *mut *mut c_void,
        error: *mut TFError,
    ) -> bool;
}
