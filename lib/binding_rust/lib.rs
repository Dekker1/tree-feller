//! Streaming LR parsing over tree-sitter parse tables.
//!
//! Drives a grammar's generated lexer and parse tables once, reporting nodes as
//! they finish. It builds no tree and retains only live parser state.
//!
//! Pass the [`tree_sitter_language::LanguageFn`] a grammar crate exports as
//! `LANGUAGE`:
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
//! Implement [`Visit::hidden`] to fold long repetitions as they finish. Use
//! [`Options::named_only`] to omit unfielded punctuation.
//!
//! ```
//! # use tree_feller::{Child, Language, Node, Options, Visit};
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! struct Count(usize);
//! impl Visit<usize> for Count {
//!     fn node(&mut self, _n: Node<'_>, kids: &mut Vec<Child<usize>>) -> usize {
//!         self.0 += 1;
//!         kids.drain(..).map(|c| c.value).sum::<usize>() + 1
//!     }
//!     fn hidden(&mut self, _n: Node<'_>, kids: &mut Vec<Child<usize>>) -> Option<usize> {
//!         Some(kids.drain(..).map(|c| c.value).sum())
//!     }
//! }
//! let language = Language::new(tree_sitter_c::LANGUAGE)?;
//! let n = language.parse_with(b"int a[] = {1, 2, 3};", Options::default().named_only(true), Count(0))?;
//! assert!(n > 0);
//! # Ok(())
//! # }
//! ```
//!
//! Grammars must use ABI 15 and no external scanner; loading checks both.

use std::ffi::{c_void, CStr, CString};
use std::fmt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

/// A grammar crate's `LANGUAGE` entry point.
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
    /// Whether the node is named, as `ts_node_is_named` would report.
    pub fn is_named(&self) -> bool {
        self.raw.named
    }
    /// Whitespace or a comment: present in the tree, but not part of any rule.
    pub fn is_extra(&self) -> bool {
        self.raw.extra
    }
    /// Byte offsets of the node in the source.
    pub fn byte_range(&self) -> std::ops::Range<u32> {
        self.raw.start_byte..self.raw.end_byte
    }
    /// Where the node starts in the source.
    pub fn start_point(&self) -> Point {
        self.raw.start_point.into()
    }
    /// Where the node ends in the source.
    pub fn end_point(&self) -> Point {
        self.raw.end_point.into()
    }
    /// How many children `Visit::node` or `Visit::hidden` will be handed for
    /// this node.
    pub fn child_count(&self) -> usize {
        self.raw.child_count as usize
    }
}

/// A child and its visitor-produced value.
#[derive(Clone, Copy, Debug)]
pub struct Child<V> {
    /// The child's symbol id, with any alias its parent applied.
    pub symbol: u16,
    /// The field this child fills in its parent, or 0 for none.
    pub field_id: u16,
    /// Whitespace or a comment: present in the tree, but not part of any rule.
    pub extra: bool,
    /// What the visitor returned when it completed this child.
    pub value: V,
}

/// Called once per node, children before parents.
///
/// Closures implement this trait. Implement it directly to fold hidden runs.
///
/// The parser reuses `children`; drain what you need. Remaining values are dropped.
pub trait Visit<V> {
    /// A node has been completed, after all of its children.
    fn node(&mut self, node: Node<'_>, children: &mut Vec<Child<V>>) -> V;

    /// A hidden rule has completed with more than one visible child.
    ///
    /// Returning `Some` folds the run into one value, keeping long lists from
    /// retaining every member until their visible parent finishes.
    ///
    /// The parent sees one child instead of the run, unlike a CST walk.
    ///
    /// `None` declines that symbol permanently, avoiding quadratic repeated
    /// offers. The default declines all folds and reproduces a CST walk.
    fn hidden(&mut self, _node: Node<'_>, _children: &mut Vec<Child<V>>) -> Option<V> {
        None
    }
}

impl<V, F: FnMut(Node<'_>, &mut Vec<Child<V>>) -> V> Visit<V> for F {
    fn node(&mut self, node: Node<'_>, children: &mut Vec<Child<V>>) -> V {
        self(node, children)
    }
}

/// What a parse reports. The default is what a tree-sitter CST walk gives.
#[derive(Clone, Copy, Debug, Default)]
pub struct Options {
    /// Skip anonymous *leaves* that fill no field: the punctuation, which on a
    /// list-heavy file is about half the nodes. Anonymous tokens that do fill a
    /// field, such as an `operator`, are still reported.
    pub named_only: bool,
}

impl Options {
    /// Sets [`Options::named_only`].
    pub fn named_only(mut self, yes: bool) -> Self {
        self.named_only = yes;
        self
    }
}

/// Why a parse stopped.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParseError {
    /// Byte offset of the token that could not be used.
    pub byte: u32,
    /// Where `byte` falls, as a row and column.
    pub point: Point,
    /// Human-readable details.
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

// Values live here while the parser holds them, because it can only carry a
// pointer per node. Slots are reused as parents consume their children, so this
// stays the size of the live set rather than of the file.
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

    // Returns a handle that is never null, so a null value means "no value".
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

    // How many slots are free right now. Paired with `rollback` below.
    fn mark(&self) -> usize {
        self.free.len()
    }

    // Put a value back where it came from. Used when a visitor is offered a
    // hidden run and declines it: the driver then leaves the run in place, so
    // the handles it is still holding have to keep resolving.
    fn restore(&mut self, handle: *mut c_void, value: V) {
        self.items[(handle as usize) - 1] = Some(value);
    }

    // Undo the frees since `mark`.
    //
    // The slots freed by taking a run's children are the last entries in the
    // free list -- nothing else runs in between, because the visitor is only
    // handed values and never reaches the slab. So this is a truncate rather
    // than a search: looking each index up instead made declining a fold cost
    // a scan of the whole free list, which on a file that is one long list is
    // quadratic.
    fn rollback(&mut self, mark: usize) {
        self.free.truncate(mark);
    }
}

struct State<V, T: Visit<V>> {
    visit: T,
    values: Slab<V>,
    children: Vec<Child<V>>,
    // The slab handles behind `children`, kept so a declined fold can put them
    // back exactly where they were.
    handles: Vec<*mut c_void>,
    panic: Option<Box<dyn std::any::Any + Send>>,
}

// Collects a node's children out of the slab and hands them to the visitor.
// Shared by both callbacks, which differ only in what they do with the result.
unsafe fn dispatch<V, T: Visit<V>>(
    payload: *mut c_void,
    node: *const ffi::TFVisibleNode,
    fold: bool,
) -> *mut c_void {
    let state = &mut *(payload as *mut State<V, T>);
    // A visitor that panicked has already unwound as far as it may: crossing back
    // into C would be undefined. Stop doing work and re-raise once C is done.
    if state.panic.is_some() {
        return std::ptr::null_mut();
    }
    let raw = &*node;

    let result = catch_unwind(AssertUnwindSafe(|| {
        state.children.clear();
        state.handles.clear();
        let mark = state.values.mark();
        if raw.child_count > 0 {
            let slice = std::slice::from_raw_parts(raw.children, raw.child_count as usize);
            state.children.reserve(slice.len());
            for child in slice {
                if fold {
                    state.handles.push(child.value);
                }
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
        let handed = Node { raw };
        let value = if fold {
            state.visit.hidden(handed, &mut state.children)
        } else {
            Some(state.visit.node(handed, &mut state.children))
        };
        // Declining to fold means the children must go back exactly as they were,
        // so the eventual parent still sees the run.
        match value {
            Some(value) => {
                state.children.clear();
                state.values.insert(value)
            }
            None => {
                // Declined. The driver will leave the run alone, so every child
                // has to go back under the handle it arrived with.
                for (child, handle) in state.children.drain(..).zip(state.handles.drain(..)) {
                    state.values.restore(handle, child.value);
                }
                state.values.rollback(mark);
                std::ptr::null_mut()
            }
        }
    }));

    match result {
        Ok(handle) => handle,
        Err(payload) => {
            state.panic = Some(payload);
            std::ptr::null_mut()
        }
    }
}

unsafe extern "C" fn on_node<V, T: Visit<V>>(
    payload: *mut c_void,
    node: *const ffi::TFVisibleNode,
) -> *mut c_void {
    dispatch::<V, T>(payload, node, false)
}

unsafe extern "C" fn on_hidden<V, T: Visit<V>>(
    payload: *mut c_void,
    node: *const ffi::TFVisibleNode,
) -> *mut c_void {
    dispatch::<V, T>(payload, node, true)
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
    /// Loads the tables behind a grammar crate's `LANGUAGE`.
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

    /// The symbol's name, for diagnostics. `None` if `symbol` is out of range.
    pub fn symbol_name(&self, symbol: u16) -> Option<&str> {
        let name = unsafe { ffi::tf_language_symbol_name(self.raw, symbol) };
        if name.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(name) }.to_str().ok()
    }

    /// The field's name, for diagnostics. `None` if `field` is out of range.
    pub fn field_name(&self, field: u16) -> Option<&str> {
        let name = unsafe { ffi::tf_language_field_name(self.raw, field) };
        if name.is_null() {
            return None;
        }
        unsafe { CStr::from_ptr(name) }.to_str().ok()
    }

    /// Reports each node to `visitor` and returns its root value.
    ///
    /// The node stream is what a tree-sitter CST walk gives. See
    /// [`Language::parse_with`] to change that.
    ///
    /// Returns [`ParseError`] above 4 GiB, the `u32` offset limit.
    pub fn parse<V, T: Visit<V>>(&self, source: &[u8], visitor: T) -> Result<V, ParseError> {
        self.parse_with(source, Options::default(), visitor)
    }

    /// As [`Language::parse`], with the reporting rules changed.
    pub fn parse_with<V, T: Visit<V>>(
        &self,
        source: &[u8],
        options: Options,
        visitor: T,
    ) -> Result<V, ParseError> {
        let mut state = State::<V, T> {
            visit: visitor,
            values: Slab::new(),
            children: Vec::new(),
            handles: Vec::new(),
            panic: None,
        };
        let sink = ffi::TFVisibleSink {
            payload: &mut state as *mut _ as *mut c_void,
            on_node: Some(on_node::<V, T>),
            // Rust owns the values in the slab and drops them with it, so there
            // is nothing for the C side to hand back.
            on_discard: None,
            on_hidden: Some(on_hidden::<V, T>),
            named_only: options.named_only,
        };

        let mut error = ffi::TFError::default();
        let mut root: *mut c_void = std::ptr::null_mut();
        let ok = unsafe {
            ffi::tf_parse_visible(
                self.raw,
                source.as_ptr() as *const c_void,
                source.len(),
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

    /// Maps and parses a file. Fails above 4 GiB.
    pub fn parse_file<V, T: Visit<V>>(
        &self,
        path: impl AsRef<Path>,
        options: Options,
        visitor: T,
    ) -> Result<V, Box<dyn std::error::Error>> {
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
        let result = self.parse_with(source, options, visitor);
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
