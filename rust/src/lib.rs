//! Streaming LR parsing over tree-sitter parse tables.
//!
//! A grammar's generated `parser.c` is read as a table artifact and driven by a
//! deterministic, non-incremental parser that reports nodes as it completes
//! them. No tree is built, and nothing is retained beyond the current nesting
//! depth -- so a data file of any size costs about as much as its deepest
//! nesting, not as much as its contents.
//!
//! A grammar is whatever its published crate exports as `LANGUAGE` -- the same
//! [`tree_sitter_language::LanguageFn`] you would hand to `tree_sitter::Parser`.
//! Add the grammar crate, and that is the whole binding:
//!
//! ```
//! # use tree_feller::{Child, Language, Node};
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! let language = Language::new(tree_sitter_c::LANGUAGE)?;
//! let nodes: usize = language.parse(b"int main(void) { return 0; }", |_node: Node<'_>, children: &mut Vec<Child<usize>>| {
//!     children.drain(..).map(|child| child.value).sum::<usize>() + 1
//! })?;
//! assert_eq!(nodes, 17);
//! # Ok(())
//! # }
//! ```
//!
//! The grammar must be ABI 15 and must not use an external scanner; both are
//! checked when it is loaded.

use std::ffi::{c_void, CStr, CString};
use std::fmt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

pub use tree_sitter_language::LanguageFn;

mod ffi;

/// A position in the source. As in tree-sitter, only `\n` advances `row`, and
/// `column` counts **bytes**.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Point {
    pub row: u32,
    pub column: u32,
}

impl From<ffi::TFPoint> for Point {
    fn from(point: ffi::TFPoint) -> Self {
        Self {
            row: point.row,
            column: point.column,
        }
    }
}

/// A node the parser has finished.
#[derive(Clone, Copy)]
pub struct Node<'a> {
    raw: &'a ffi::TFVisibleNode,
}

impl fmt::Debug for Node<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Node")
            .field("symbol", &self.symbol())
            .field("production_id", &self.production_id())
            .field("named", &self.is_named())
            .field("extra", &self.is_extra())
            .field("bytes", &self.byte_range())
            .field("children", &self.child_count())
            .finish()
    }
}

impl Node<'_> {
    /// The node's symbol id, with any alias its parent applied.
    pub fn symbol(&self) -> u16 {
        self.raw.symbol
    }
    /// Which production built it. Zero for tokens.
    pub fn production_id(&self) -> u16 {
        self.raw.production_id
    }
    pub fn is_named(&self) -> bool {
        self.raw.named
    }
    /// Whitespace or a comment: present in the tree, but not part of any rule.
    pub fn is_extra(&self) -> bool {
        self.raw.extra
    }
    pub fn byte_range(&self) -> std::ops::Range<u32> {
        self.raw.start_byte..self.raw.end_byte
    }
    pub fn start_point(&self) -> Point {
        self.raw.start_point.into()
    }
    pub fn end_point(&self) -> Point {
        self.raw.end_point.into()
    }
    pub fn child_count(&self) -> usize {
        self.raw.child_count as usize
    }
}

/// One child of a node, with whatever the visitor returned for it.
#[derive(Clone, Copy, Debug)]
pub struct Child<V> {
    pub symbol: u16,
    /// The field this child fills in its parent, or 0 for none.
    pub field_id: u16,
    pub extra: bool,
    pub value: V,
}

/// Called once per node, children before parents.
///
/// `children` is a buffer the parser reuses, so take what you need out of it --
/// by `drain`, or otherwise. Anything left behind is dropped. Capture whatever
/// state the visit needs; there is no separate trait to implement.
pub type Visit<'a, V> = dyn FnMut(Node<'_>, &mut Vec<Child<V>>) -> V + 'a;

/// Why a parse stopped.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParseError {
    /// Byte offset of the token that could not be used.
    pub byte: u32,
    pub point: Point,
    pub message: String,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}:{}: {}",
            self.point.row + 1,
            self.point.column,
            self.message
        )
    }
}

impl std::error::Error for ParseError {}

/// Why a language or file could not be used.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error(String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

fn message_of(error: &ffi::TFError) -> String {
    unsafe { CStr::from_ptr(error.message.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

// ---------------------------------------------------------------------------

/// Values live here while the parser holds them, because it can only carry a
/// pointer per node. Slots are reused as parents consume their children, so this
/// stays the size of the live set rather than of the file.
struct Slab<V> {
    items: Vec<Option<V>>,
    free: Vec<u32>,
}

impl<V> Slab<V> {
    fn new() -> Self {
        Self {
            items: Vec::new(),
            free: Vec::new(),
        }
    }

    /// Returns a handle that is never null, so a null value means "no value".
    fn insert(&mut self, value: V) -> *mut c_void {
        let index = match self.free.pop() {
            Some(index) => {
                self.items[index as usize] = Some(value);
                index
            }
            None => {
                self.items.push(Some(value));
                (self.items.len() - 1) as u32
            }
        };
        (index as usize + 1) as *mut c_void
    }

    fn take(&mut self, handle: *mut c_void) -> Option<V> {
        let index = (handle as usize).checked_sub(1)?;
        let value = self.items.get_mut(index)?.take();
        if value.is_some() {
            self.free.push(index as u32);
        }
        value
    }
}

struct State<V, F> {
    visit: F,
    values: Slab<V>,
    children: Vec<Child<V>>,
    panic: Option<Box<dyn std::any::Any + Send>>,
}

unsafe extern "C" fn on_node<V, F>(
    payload: *mut c_void,
    node: *const ffi::TFVisibleNode,
) -> *mut c_void
where
    F: FnMut(Node<'_>, &mut Vec<Child<V>>) -> V,
{
    let state = &mut *(payload as *mut State<V, F>);
    // A visitor that panicked has already unwound as far as it may: crossing back
    // into C would be undefined. Stop doing work and re-raise once C is done.
    if state.panic.is_some() {
        return std::ptr::null_mut();
    }
    let raw = &*node;

    let result = catch_unwind(AssertUnwindSafe(|| {
        state.children.clear();
        if raw.child_count > 0 {
            let slice = std::slice::from_raw_parts(raw.children, raw.child_count as usize);
            state.children.reserve(slice.len());
            for child in slice {
                // A child with no value cannot happen: every visible node was
                // reported before its parent. If it ever does, it is a bug here,
                // not something to paper over.
                let value = state
                    .values
                    .take(child.value)
                    .expect("child reported without a value");
                state.children.push(Child {
                    symbol: child.symbol,
                    field_id: child.field_id,
                    extra: child.extra,
                    value,
                });
            }
        }
        let value = (state.visit)(Node { raw }, &mut state.children);
        state.children.clear();
        state.values.insert(value)
    }));

    match result {
        Ok(handle) => handle,
        Err(payload) => {
            state.panic = Some(payload);
            std::ptr::null_mut()
        }
    }
}

// ---------------------------------------------------------------------------

/// A grammar's parse tables, prepared for the driver.
pub struct Language {
    raw: *mut ffi::TFLanguage,
}

// The tables are read-only once loaded.
unsafe impl Send for Language {}
unsafe impl Sync for Language {}

impl Language {
    /// Read the tables of a grammar, given whatever its crate exports as
    /// `LANGUAGE`.
    ///
    /// This is the `tree_sitter_language::LanguageFn` the Tree-sitter CLI
    /// generates for every grammar, so any published grammar crate works
    /// unchanged and no `extern "C"` block is needed:
    ///
    /// ```
    /// # use tree_feller::Language;
    /// let language = Language::new(tree_sitter_c::LANGUAGE)?;
    /// # Ok::<_, tree_feller::Error>(())
    /// ```
    ///
    /// Fails if the grammar is not ABI 15, or uses an external scanner.
    pub fn new(language: LanguageFn) -> Result<Self, Error> {
        // Safe for the same reason `tree_sitter::Language::new` is: constructing
        // a `LanguageFn` is the unsafe step, and its contract is that the
        // function came from the Tree-sitter CLI. Everything the tables claim
        // about themselves is then checked by `tf_language_load`.
        unsafe { Self::from_raw(language.into_raw()().cast()) }
    }

    /// Wrap the `TSLanguage` a generated `tree_sitter_<name>()` returns.
    ///
    /// Prefer [`Language::new`]. This is for a grammar reached some other way --
    /// `dlopen`, or a table built by hand.
    ///
    /// # Safety
    ///
    /// `language` must be a valid `TSLanguage`, and must outlive the `Language`.
    /// In practice it is a pointer to static data in a generated `parser.c`.
    pub unsafe fn from_raw(language: *const c_void) -> Result<Self, Error> {
        let mut message: *const std::ffi::c_char = std::ptr::null();
        let raw = ffi::tf_language_load(language, &mut message);
        if raw.is_null() {
            let text = if message.is_null() {
                "could not load language".to_owned()
            } else {
                CStr::from_ptr(message).to_string_lossy().into_owned()
            };
            return Err(Error(text));
        }
        Ok(Self { raw })
    }

    pub fn symbol_name(&self, symbol: u16) -> Option<&str> {
        let name = unsafe { ffi::tf_language_symbol_name(self.raw, symbol) };
        if name.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(name) }.to_str().ok()
    }

    pub fn field_name(&self, field: u16) -> Option<&str> {
        let name = unsafe { ffi::tf_language_field_name(self.raw, field) };
        if name.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(name) }.to_str().ok()
    }

    /// Parse `source`, reporting each node to `visitor`, and return whatever it
    /// returned for the root.
    pub fn parse<V, F>(&self, source: &[u8], visit: F) -> Result<V, ParseError>
    where
        F: FnMut(Node<'_>, &mut Vec<Child<V>>) -> V,
    {
        assert!(
            source.len() <= u32::MAX as usize,
            "source is larger than 4 GiB"
        );

        let mut state = State::<V, F> {
            visit,
            values: Slab::new(),
            children: Vec::new(),
            panic: None,
        };
        let sink = ffi::TFVisibleSink {
            payload: &mut state as *mut _ as *mut c_void,
            on_node: Some(on_node::<V, F>),
            // Folding hidden runs changes the shape a visitor sees, so it is not
            // something to turn on behind its back.
            on_hidden: None,
            named_only: false,
        };

        let mut error = ffi::TFError::default();
        let mut root: *mut c_void = std::ptr::null_mut();
        let ok = unsafe {
            ffi::tf_parse_visible(
                self.raw,
                source.as_ptr() as *const c_void,
                source.len() as u32,
                &sink,
                &mut root,
                &mut error,
            )
        };

        if let Some(panic) = state.panic.take() {
            std::panic::resume_unwind(panic);
        }
        if !ok {
            return Err(ParseError {
                byte: error.byte,
                point: error.point.into(),
                message: message_of(&error),
            });
        }
        state.values.take(root).ok_or_else(|| ParseError {
            byte: 0,
            point: Point::default(),
            message: "the grammar's root rule is not a visible node".to_owned(),
        })
    }

    /// Parse a file, mapped rather than read: it costs address space, not
    /// committed memory. Fails above 4 GiB, the limit of a byte offset.
    pub fn parse_file<V, F>(
        &self,
        path: impl AsRef<Path>,
        visit: F,
    ) -> Result<V, Box<dyn std::error::Error>>
    where
        F: FnMut(Node<'_>, &mut Vec<Child<V>>) -> V,
    {
        let path = path.as_ref();
        let text = CString::new(path.to_string_lossy().as_bytes())
            .map_err(|_| Error("path contains a null byte".to_owned()))?;

        let mut file = ffi::TFFile {
            data: std::ptr::null(),
            size: 0,
            mapped: false,
        };
        let mut error = ffi::TFError::default();
        if !unsafe { ffi::tf_file_open(&mut file, text.as_ptr(), &mut error) } {
            return Err(Box::new(Error(message_of(&error))));
        }
        let source =
            unsafe { std::slice::from_raw_parts(file.data as *const u8, file.size as usize) };
        let result = self.parse(source, visit);
        unsafe { ffi::tf_file_close(&mut file) };
        Ok(result?)
    }
}

impl Drop for Language {
    fn drop(&mut self) {
        unsafe { ffi::tf_language_free(self.raw) };
    }
}

impl TryFrom<LanguageFn> for Language {
    type Error = Error;

    fn try_from(language: LanguageFn) -> Result<Self, Error> {
        Self::new(language)
    }
}
