// tree-feller -- a streaming LR driver over tree-sitter parse tables.
//
// Drives a generated parser's lexer and tables once, emitting reductions without
// building a CST. Only live parser state is retained.
//
// Pinned to tree-sitter ABI 15 (CLI 0.25 through 0.27). See README.md.
#ifndef TREE_FELLER_H
#define TREE_FELLER_H

#include <stdbool.h>
#include <stdint.h>

// Defines the grammar table ABI. Its nested path prevents it from shadowing a
// grammar's copy; their shared guard lets either copy be included first.
#include "tree_feller/tree_sitter/parser.h"

#ifdef __cplusplus
extern "C" {
#endif

// The ABI this library understands. `tf_language_load` rejects anything else.
#define TF_ABI_VERSION 15

// Layout-compatible with TSPoint. Only '\n' advances `row`; `column` counts bytes.
typedef struct {
  uint32_t row;
  uint32_t column;
} TFPoint;

typedef struct TFLanguage TFLanguage;

// Loads generated grammar tables. Returns NULL for a non-ABI-15 grammar or one
// with an external scanner. `error` may be NULL. `ts` must outlive the result.
// Loading is not thread-safe; a loaded language is immutable and shareable.
TFLanguage *tf_language_load(const TSLanguage *ts, const char **error);
// Safe to call with NULL, like free().
void tf_language_free(TFLanguage *self);

// Diagnostic names. Return NULL for an out-of-range id.
const char *tf_language_symbol_name(const TFLanguage *self, TSSymbol symbol);
const char *tf_language_field_name(const TFLanguage *self, TSFieldId field);

// A terminal, as the lexer produced it.
typedef struct {
  TSSymbol symbol;
  // Inserted by error recovery rather than read from the input, so the span is
  // empty. What `ts_node_is_missing` reports. Always false without recovery.
  bool missing;
  uint32_t start_byte;
  uint32_t end_byte;
  TFPoint start_point;
  TFPoint end_point;
} TFToken;

// A shifted token or completed reduction on the parse stack.
typedef struct {
  TSSymbol symbol;
  bool extra;    // an `extra` token: whitespace or a comment, not a real child
  bool missing;  // as `TFToken::missing`; never set on a reduction
  uint32_t start_byte;
  uint32_t end_byte;
  TFPoint start_point;
  TFPoint end_point;
  void *value;
} TFNode;

typedef struct {
  TSSymbol symbol;
  uint16_t production_id;
  // Grammar children exclude extras. `children` contains `node_count` grammar
  // children and intervening extras, and is valid only during the callback.
  uint32_t child_count;
  uint32_t node_count;
  // Extras that were above the last child, which the parent does not take and
  // which stay on the stack above it (parser.c:/trailing_extras/). They are
  // `children[node_count]` onwards, valid only during the callback. Only a
  // consumer that mirrors the stack needs them: they are not the parent's.
  uint32_t trailing_count;
  const TFNode *children;
  uint32_t start_byte;
  uint32_t end_byte;
  TFPoint start_point;
  TFPoint end_point;
} TFReduction;

// Why the parser stopped, handed to `TFSink::on_error`.
typedef struct {
  // The token the state has no action for. For a run of bytes that does not lex
  // at all, this is a leaf with symbol `ts_builtin_sym_error` spanning them,
  // which is not a terminal the grammar knows and has no name.
  TFToken token;
  TSStateId state;  // the parse state that rejected it
  // The terminals with an action in `state`, ascending. Only valid during the
  // call. Empty when the state accepts nothing, which only the error state does.
  const TSSymbol *expected;
  uint32_t expected_count;
} TFErrorEvent;

// Shift and reduce events, with children reported before parents, but not in
// tree-walk order: extras can precede reductions of earlier nodes.
// Any callback may be NULL.
typedef struct {
  void *payload;
  void *(*on_shift)(void *payload, const TFToken *token, bool extra);
  void *(*on_reduce)(void *payload, const TFReduction *reduction);

  // Optional. Called once for every value the sink returned that no parent ever
  // consumed, when a parse fails partway through.
  void (*on_discard)(void *payload, void *value);

  // Optional. Enables tree-sitter's error recovery. Called once each time the
  // parser enters recovery, never once per skipped token.
  //
  // Returning true recovers, and the parse then reports what it did as ordinary
  // events: an `ERROR` reduction over what it skipped, shifts inside it for the
  // skipped tokens, and shifts with `TFToken::missing` for what it inserted.
  //
  // Returning false stops the parse exactly as a NULL callback would.
  //
  // A NULL callback means no recovery: the parse stops at the first error.
  bool (*on_error)(void *payload, const TFErrorEvent *event);
} TFSink;

#define TF_ERROR_MESSAGE_SIZE 512

typedef struct {
  uint32_t byte;
  TFPoint point;
  char message[TF_ERROR_MESSAGE_SIZE];
} TFError;

// Parses all of `source`. On success, stores the root value in `*root`. On the
// first error, returns false and fills `*error`. `sink`, `root`, and `error` may
// be NULL. Inputs above 4 GiB fail instead of being truncated.
bool tf_parse(const TFLanguage *lang, const void *source, size_t size, const TFSink *sink,
              void **root, TFError *error);

// ---------------------------------------------------------------------------
// Input
//
// Reported offsets refer to the contiguous input buffer. Keep it alive while
// parsing and while any consumer-built value still refers to it.

typedef struct {
  const void *data;
  uint32_t size;
} TFFile;

// Maps a file without committing it all to memory. Fails above the 4 GiB
// `uint32_t` offset limit.
//
// On failure, fills `error->message`; `byte` and `point` are undefined. `error`
// may be NULL. Close after all references to the mapping are gone.
bool tf_file_open(TFFile *file, const char *path, TFError *error);
// Safe to call on a `TFFile` that failed to open or was already closed.
void tf_file_close(TFFile *self);

// ---------------------------------------------------------------------------
// Visible nodes
//
// Applies tree-sitter visibility, alias, and field rules to raw reductions,
// reporting the nodes a CST walk would visit, with the children and fields it
// would see, without building a tree.

typedef struct {
  TSSymbol symbol;
  TSFieldId field_id;  // the field this child fills in its parent, 0 for none
  bool extra;          // whitespace or a comment: never fills a field
  bool missing;        // inserted by recovery, so the span is empty
  void *value;
} TFVisibleChild;

typedef struct {
  TSSymbol symbol;  // the public symbol, with any alias from the parent applied
  uint16_t production_id;
  bool named;
  bool extra;
  bool missing;  // inserted by recovery, so the span is empty
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
  // Returning non-NULL folds the run into one child, preventing long lists from
  // retaining every member until their visible ancestor finishes.
  //
  // The parent sees that child under the hidden rule's symbol, so folding differs
  // from a CST walk.
  //
  // NULL declines that symbol permanently, avoiding quadratic repeated offers.
  // A NULL callback declines all folds, leaving the nodes of a full CST walk.
  //
  // `node->children` is only valid for the duration of the call.
  void *(*on_hidden)(void *payload, const TFVisibleNode *node);

  // Omits anonymous leaves with no field, usually punctuation. Fielded tokens,
  // non-leaves, and named comments remain. False keeps every visible node.
  bool named_only;

  // As `TFSink::on_error`, and forwarded to it unchanged. A recovered parse
  // reports `ERROR` as a visible named node, hides the `error_repeat` nodes
  // that group skipped tokens, and marks inserted tokens with `missing`, which
  // is what a `TSTreeCursor` walk of tree-sitter's own tree would show.
  bool (*on_error)(void *payload, const TFErrorEvent *event);
} TFVisibleSink;

// As `tf_parse`, reporting visible nodes instead of raw reductions. Children
// precede parents, but callbacks are not in tree-walk order: nodes wait for their
// parent's reduction. To obtain a walk order, reassemble and walk the child values.
bool tf_parse_visible(const TFLanguage *lang, const void *source, size_t size,
                      const TFVisibleSink *sink, void **root, TFError *error);

#ifdef __cplusplus
}
#endif

#endif  // TREE_FELLER_H
