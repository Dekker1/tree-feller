// Lexer adapter: runs a grammar's generated `lex_fn` over an in-memory buffer.
//
// A stripped-down `Lexer` (lexer.c) for the one case tree-feller cares about: a
// single contiguous UTF-8 buffer, no included ranges, no external scanner, no
// incremental reuse. What is kept is exactly the observable behaviour the
// generated lexers and the byte/point arithmetic depend on.
#ifndef TF_LEXER_H
#define TF_LEXER_H

#include "tf_language.h"

typedef struct {
  // First member: the generated lex functions are handed this pointer and cast
  // it back to the enclosing struct (parser.c:345).
  TSLexer data;

  const TFLanguage *lang;
  const uint8_t *source;
  uint32_t size;

  uint32_t byte;
  TFPoint point;  // like TFPoint everywhere else, `column` counts bytes
  uint32_t lookahead_size;

  uint32_t token_start_byte;
  TFPoint token_start_point;
  uint32_t token_end_byte;  // TF_NO_END until mark_end
  TFPoint token_end_point;
} TFLexer;

// ponytail: the source is one contiguous buffer, indexed directly. A pull source
// -- a ring buffer over a stream -- was designed for but not built, because
// nothing yet needs one and an indirection on every byte is not free. What it
// would have to guarantee, measured rather than guessed:
//
//   * Lookbehind of one whole token. The keyword re-lex returns to the token's
//     first byte (parser.c:645), and `tf_lexer_next` repositions to its end.
//     Longest token seen: 11 KB over 20,668 .dzn, 67 KB over 7,633 .mzn -- both
//     block comments, so the bound is "longest comment", not "longest literal".
//   * During split mode, cover from the earliest live branch's position to the
//     furthest. Branches advance in lockstep on the earliest token, so that
//     spread is a token or two, not a region.
//   * Byte offsets stay absolute. Everything the sink is handed is an offset
//     into the whole input, and a consumer that keeps offsets rather than
//     copying text needs them to keep meaning something.
//
// None of this touches the parser: its stack is bounded by nesting depth, not by
// input size -- 860 cells at the deepest across those same 20,668 files, the
// largest of which is 61.7 MB.
void tf_lexer_init(TFLexer *self, const TFLanguage *lang, const void *source, uint32_t size);

// Lex the token that follows, in the given parse state, skipping any leading
// `extra` characters. Returns false if no token matches, leaving the position at
// the offending character for the caller to report.
bool tf_lexer_next(TFLexer *self, TSStateId state, TFToken *out);

#endif  // TF_LEXER_H
