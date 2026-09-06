//! The C API, transcribed. Layouts are `repr(C)` mirrors of `tree_feller.h`; the
//! differential tests are what confirm they line up.
#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_void};

pub const TF_ERROR_MESSAGE_SIZE: usize = 512;

pub enum TFLanguage {}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TFPoint {
    pub row: u32,
    pub column: u32,
}

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

#[repr(C)]
pub struct TFFile {
    pub data: *const c_void,
    pub size: u32,
    pub mapped: bool,
}

#[repr(C)]
pub struct TFVisibleChild {
    pub symbol: u16,
    pub field_id: u16,
    pub extra: bool,
    pub value: *mut c_void,
}

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

#[repr(C)]
pub struct TFVisibleSink {
    pub payload: *mut c_void,
    pub on_node: Option<unsafe extern "C" fn(*mut c_void, *const TFVisibleNode) -> *mut c_void>,
    pub on_hidden: Option<unsafe extern "C" fn(*mut c_void, *const TFVisibleNode) -> *mut c_void>,
    pub named_only: bool,
}

extern "C" {
    pub fn tf_language_load(language: *const c_void, error: *mut *const c_char) -> *mut TFLanguage;
    pub fn tf_language_free(language: *mut TFLanguage);
    pub fn tf_language_symbol_name(language: *const TFLanguage, symbol: u16) -> *const c_char;
    pub fn tf_language_field_name(language: *const TFLanguage, field: u16) -> *const c_char;

    pub fn tf_file_open(file: *mut TFFile, path: *const c_char, error: *mut TFError) -> bool;
    pub fn tf_file_close(file: *mut TFFile);

    pub fn tf_parse_visible(
        language: *const TFLanguage,
        source: *const c_void,
        size: u32,
        sink: *const TFVisibleSink,
        root: *mut *mut c_void,
        error: *mut TFError,
    ) -> bool;
}
