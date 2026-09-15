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

  // Whether the last token `tf_lexer_next` produced was reclassified by the
  // keyword lexer; the parser may have to undo that later, see
  // `tf_parser__demote_keyword`. Last on purpose: everything above is touched
  // per input byte, and putting a field in the middle of that moved the hot
  // ones across a cache line for a measured 10% loss.
  bool token_is_keyword;
  TSStateId token_lex_state;
} TFLexer;

// The source is one contiguous buffer, indexed directly.
void tf_lexer_init(TFLexer *self, const TFLanguage *lang, const void *source, uint32_t size);

void tf_lexer_seek(TFLexer *self, uint32_t byte, TFPoint point);

// Lex the token that follows, in the given parse state, skipping any leading
// `extra` characters. Returns false if no token matches, leaving the position at
// the offending character for the caller to report.
bool tf_lexer_next(TFLexer *self, TSStateId state, TFToken *out);

#endif  // TF_LEXER_H
