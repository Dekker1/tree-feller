// tree-feller -- a streaming LR driver over tree-sitter parse tables.
//
// Reuses a generated `parser.c` as a pure table artifact and replaces the
// tree-sitter runtime with a deterministic, non-incremental parser that emits
// reduce events. No CST is built; nothing is retained beyond the current
// nesting depth.
//
// Pinned to tree-sitter ABI 15 (tree-sitter 0.26.x). See README.md.
#ifndef TREE_FELLER_H
#define TREE_FELLER_H

#include <stdbool.h>
#include <stdint.h>

// The grammar tables' own header, which defines TSLanguage, TSSymbol and the
// parse action layout. Kept under `tree_feller/` so that including this cannot
// shadow the copy that ships with a consumer's generated parser.c, or the one in
// a tree-sitter checkout. Both guard on TREE_SITTER_PARSER_H_, so whichever is
// seen first wins -- and `tf_language_load` rejects anything that is not ABI 15.
#include "tree_feller/tree_sitter/parser.h"

#ifdef __cplusplus
extern "C" {
#endif

// The ABI this library understands. `tf_language_load` rejects anything else.
#define TF_ABI_VERSION 15

// Layout-compatible with TSPoint, so a consumer can memcpy or cast. Declared
// here rather than including tree_sitter/api.h, which parser.h does not need and
// which would drag the runtime's public header into a library that does not use
// it. As in tree-sitter, only '\n' advances `row`, and `column` counts *bytes*.
typedef struct {
  uint32_t row;
  uint32_t column;
} TFPoint;

typedef struct TFLanguage TFLanguage;

// Wraps a generated `tree_sitter_<name>()` table for use by the driver.
// Returns NULL if the language is not ABI 15 or uses an external scanner.
// The TSLanguage must outlive the TFLanguage. Not thread-safe to create;
// safe to share read-only once created.
TFLanguage *tf_language_load(const TSLanguage *ts, const char **error);
void tf_language_free(TFLanguage *self);

// Names for the symbols and fields the sink reports, for diagnostics. Both
// return NULL if the id is out of range.
const char *tf_language_symbol_name(const TFLanguage *self, TSSymbol symbol);
const char *tf_language_field_name(const TFLanguage *self, TSFieldId field);

// A terminal, as the lexer produced it.
typedef struct {
  TSSymbol symbol;
  uint32_t start_byte;
  uint32_t end_byte;
  TFPoint start_point;
  TFPoint end_point;
} TFToken;

// One constituent on the parse stack: a shifted token or a completed reduction,
// carrying whatever the sink returned for it.
typedef struct {
  TSSymbol symbol;
  bool extra;  // an `extra` token: whitespace or a comment, not a real child
  uint32_t start_byte;
  uint32_t end_byte;
  TFPoint start_point;
  TFPoint end_point;
  void *value;
} TFNode;

typedef struct {
  TSSymbol symbol;
  uint16_t production_id;
  // The grammar's child count, which excludes `extra` children. `children` holds
  // `node_count` entries, being those children *plus* any extras that fell
  // between them. It is only valid for the duration of the callback.
  uint32_t child_count;
  uint32_t node_count;
  const TFNode *children;
  uint32_t start_byte;
  uint32_t end_byte;
  TFPoint start_point;
  TFPoint end_point;
} TFReduction;

// Reduce events, in the order the parse produces them: every child is reported
// before its parent. Either callback may be NULL.
typedef struct {
  void *payload;
  void *(*on_shift)(void *payload, const TFToken *token, bool extra);
  void *(*on_reduce)(void *payload, const TFReduction *reduction);

  // Optional. Called once for every value the sink returned that no parent ever
  // consumed, when a parse fails partway through.
  void (*on_discard)(void *payload, void *value);
} TFSink;

#define TF_ERROR_MESSAGE_SIZE 512

typedef struct {
  uint32_t byte;
  TFPoint point;
  char message[TF_ERROR_MESSAGE_SIZE];
} TFError;

// Parse `source` in full. On success returns true and stores the value the sink
// returned for the root in `*root`. On the first error returns false and fills
// `*error`; parsing does not continue past it. `root` and `error` may be NULL.
bool tf_parse(const TFLanguage *lang, const void *source, size_t size, const TFSink *sink,
              void **root, TFError *error);

// ---------------------------------------------------------------------------
// Input
//
// The parser reads a contiguous buffer. Everything it reports is a byte offset
// into that buffer, so the buffer has to outlive the parse -- and outlive
// whatever the sink built, if that kept offsets rather than copying text.

typedef struct {
  const void *data;
  uint32_t size;
  bool mapped;  // internal
} TFFile;

// Map a file for parsing. The file is mapped, not read, on every platform: it
// costs address space rather than committed memory, and the pages stay
// reclaimable. Fails above 4 GiB, which is what a `uint32_t` byte offset can
// address -- tree-sitter's limit too.
//
// On failure returns false and fills `error->message`; `byte` and `point` are
// not meaningful for these errors. Close it once the parse, and anything holding
// offsets into it, is done.
bool tf_file_open(TFFile *file, const char *path, TFError *error);
void tf_file_close(TFFile *self);

// ---------------------------------------------------------------------------
// Visible nodes
//
// Raw reduce events include the productions that have no counterpart in a
// tree-sitter CST: supertypes, inlined rules, and the `aux_sym_*_repeat1` nodes
// behind every repetition. This optional layer applies tree-sitter's own
// visibility rules on top of the raw stream, so what a consumer sees is the node
// sequence it would get from walking the CST -- without one being built.

typedef struct {
  TSSymbol symbol;
  TSFieldId field_id;  // the field this child fills in its parent, 0 for none
  bool extra;          // whitespace or a comment: never fills a field
  void *value;
} TFVisibleChild;

typedef struct {
  TSSymbol symbol;  // the public symbol, with any alias from the parent applied
  uint16_t production_id;
  bool named;
  bool extra;
  uint32_t start_byte;
  uint32_t end_byte;
  TFPoint start_point;
  TFPoint end_point;
  uint32_t child_count;
  const TFVisibleChild *children;  // only valid for the duration of the callback
} TFVisibleNode;

typedef struct {
  void *payload;
  void *(*on_node)(void *payload, const TFVisibleNode *node);

  // As `TFSink::on_discard`: every value no parent consumed, after a failure.
  void (*on_discard)(void *payload, void *value);

  // Optional. A hidden rule -- an inlined rule, or the `aux_sym_*_repeat1`
  // behind a repetition -- has completed with more than one visible child.
  //
  // Those children are otherwise held until the nearest *visible* ancestor
  // reduces, because only then is it known what they are children of. For a
  // repetition that means the whole list is live at once, so an array of a
  // million members costs a million entries. Returning non-NULL here collapses
  // the run to a single child carrying the returned value, which is what keeps a
  // bulk parse proportional to nesting depth rather than to the widest list.
  //
  // The parent then sees one child where it would have seen the run, under the
  // hidden rule's own symbol -- so a consumer that folds is no longer being
  // handed the same shape a CST walk would give it.
  //
  // Return NULL to decline. That answer is taken for the *symbol*, not just this
  // node, and it will not be asked again: an offer hands over every child in the
  // run, and a repetition's run grows by one each time it reduces, so re-asking
  // a symbol that has already said no costs O(n^2) across the list. Leaving this
  // callback NULL is the cheaper way to decline everything, and reproduces a CST
  // walk exactly.
  //
  // `node->children` is only valid for the duration of the call.
  void *(*on_hidden)(void *payload, const TFVisibleNode *node);

  // When set, an anonymous *leaf* that fills no field is neither reported nor
  // listed among its parent's children. These are the punctuation tokens -- the
  // commas and brackets -- and on a data file they are about half of all nodes.
  //
  // This is how a consumer reads a tree-sitter tree in practice: named children
  // and fields, as `ts_node_named_child` and `ts_node_child_by_field_id` give
  // them. Anonymous tokens that *do* fill a field, such as an `operator`, are
  // still reported, because a consumer asking for that field expects them; and
  // anything with children of its own is left alone, so nothing can be dropped
  // along with it. Comments are named, so they survive.
  //
  // Leave it false to reproduce a full CST walk.
  bool named_only;
} TFVisibleSink;

// As `tf_parse`, reporting visible nodes instead of raw reductions. Children are
// still reported before their parents.
bool tf_parse_visible(const TFLanguage *lang, const void *source, size_t size,
                      const TFVisibleSink *sink, void **root, TFError *error);

#ifdef __cplusplus
}
#endif

#endif  // TREE_FELLER_H
