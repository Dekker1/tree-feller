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
  uint32_t start_byte;
  uint32_t end_byte;
  TFPoint start_point;
  TFPoint end_point;
} TFToken;

// A shifted token or completed reduction on the parse stack.
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
  // Grammar children exclude extras. `children` contains `node_count` grammar
  // children and intervening extras, and is valid only during the callback.
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
  bool mapped;  // internal
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
// producing the node sequence of a CST walk without building one.

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
  // Returning non-NULL folds the run into one child, preventing long lists from
  // retaining every member until their visible ancestor finishes.
  //
  // The parent sees that child under the hidden rule's symbol, so folding differs
  // from a CST walk.
  //
  // NULL declines that symbol permanently, avoiding quadratic repeated offers.
  // A NULL callback declines all folds and reproduces a CST walk.
  //
  // `node->children` is only valid for the duration of the call.
  void *(*on_hidden)(void *payload, const TFVisibleNode *node);

  // Omits anonymous leaves with no field, usually punctuation. Fielded tokens,
  // non-leaves, and named comments remain. False reproduces a full CST walk.
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
