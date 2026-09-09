// Implements the TSLexer callbacks tf_lexer.h declares, and tf_lexer_next's dispatch
// of one token: the generated lex_fn, then the keyword re-lex tree-sitter itself runs
// before accepting a word token. Ported from tree-sitter's lexer.c and parser.c; see
// tf_lexer.h for what the single-buffer, no-included-ranges case leaves out.
#include "tf_lexer.h"

#include <stdarg.h>

#include "tf_utf8.h"

#define TF_BOM 0xFEFF
#define TF_NO_END UINT32_MAX

static bool tf_lexer__eof(const TSLexer *lexer) {
  const TFLexer *self = (const TFLexer *)lexer;
  return self->byte >= self->size;
}

// lexer.c:102-136, minus the "chunk ended mid-character" retry, which cannot
// happen when the whole input is one chunk.
static void tf_lexer__get_lookahead(TFLexer *self) {
  if (self->byte >= self->size) {
    self->lookahead_size = 1;
    self->data.lookahead = '\0';
    return;
  }
  // ASCII, which is nearly every byte, without the call: the decoder is too
  // large for the compiler to inline here, and it was showing up as its own
  // frame in a profile of this loop.
  uint8_t lead = self->source[self->byte];
  if (lead < 0x80) {
    self->lookahead_size = 1;
    self->data.lookahead = lead;
    return;
  }
  uint32_t i = 1;
  self->data.lookahead = tf_utf8_next(self->source + self->byte, self->size - self->byte, &i);
  // A malformed sequence advances one byte, as tree-sitter does (lexer.c:130).
  self->lookahead_size = (self->data.lookahead == TF_DECODE_ERROR) ? 1 : i;
}

void tf_lexer_seek(TFLexer *self, uint32_t byte, TFPoint point) {
  self->byte = byte;
  self->point = point;
  tf_lexer__get_lookahead(self);
}

// lexer.c:194-247. The included-range walk collapses to nothing with one range;
// what remains is the position arithmetic, which must match exactly: only '\n'
// advances the row, and `column` counts bytes.
static void tf_lexer__do_advance(TFLexer *self, bool skip) {
  if (self->lookahead_size) {
    if (self->data.lookahead == '\n') {
      self->point.row++;
      self->point.column = 0;
    } else {
      self->point.column += self->lookahead_size;
    }
    self->byte += self->lookahead_size;
  }
  if (skip) {
    self->token_start_byte = self->byte;
    self->token_start_point = self->point;
  }
  tf_lexer__get_lookahead(self);
}

static void tf_lexer__advance(TSLexer *lexer, bool skip) {
  TFLexer *self = (TFLexer *)lexer;
  if (self->byte >= self->size) return;  // lexer.c:250, `if (!self->chunk) return`
  tf_lexer__do_advance(self, skip);
}

static void tf_lexer__mark_end(TSLexer *lexer) {
  TFLexer *self = (TFLexer *)lexer;
  self->token_end_byte = self->byte;
  self->token_end_point = self->point;
}

// lexer.c:285-322. Unlike TFPoint.column this counts codepoints, and skips a
// byte order mark at offset 0.
//
// CONSIDERATION: rescans the line on every call, where tree-sitter caches the
// running count. No grammar in scope calls get_column at all; cache it if one
// does and the O(line length) per call shows up.
static uint32_t tf_lexer__get_column(TSLexer *lexer) {
  TFLexer *self = (TFLexer *)lexer;
  uint32_t column = 0;
  for (uint32_t i = self->byte - self->point.column; i < self->byte;) {
    uint32_t next = 1;
    int32_t code_point = tf_utf8_next(self->source + i, self->size - i, &next);
    if (code_point == TF_DECODE_ERROR) next = 1;
    if (i != 0 || code_point != TF_BOM) column++;
    i += next;
  }
  return column;
}

// lexer.c:325-333. True only at the very start of the one included range.
static bool tf_lexer__is_at_included_range_start(const TSLexer *lexer) {
  const TFLexer *self = (const TFLexer *)lexer;
  return self->byte == 0 && self->size > 0;
}

static void tf_lexer__log(const TSLexer *lexer, const char *format, ...) {
  (void)lexer;
  (void)format;
}

void tf_lexer_init(TFLexer *self, const TFLanguage *lang, const void *source, uint32_t size) {
  *self = (TFLexer){
      .data =
          {
              .advance = tf_lexer__advance,
              .mark_end = tf_lexer__mark_end,
              .get_column = tf_lexer__get_column,
              .is_at_included_range_start = tf_lexer__is_at_included_range_start,
              .eof = tf_lexer__eof,
              .log = tf_lexer__log,
          },
      .lang = lang,
      .source = source,
      .size = size,
  };
  tf_lexer__get_lookahead(self);
}

// lexer.c:/ts_lexer_start/. tree-sitter decodes only when its lookahead was
// invalidated by moving between input chunks or included ranges. Neither exists
// here: only `tf_lexer_seek` and `tf_lexer__do_advance` move the position, and
// both refresh the lookahead. Avoids one decode per token on the main path and
// another on every keyword re-lex.
static void tf_lexer__start(TFLexer *self) {
  self->token_start_byte = self->byte;
  self->token_start_point = self->point;
  self->token_end_byte = TF_NO_END;
  self->data.result_symbol = 0;
  if (self->byte == 0 && self->size > 0 && self->data.lookahead == TF_BOM) {
    tf_lexer__advance(&self->data, true);
  }
}

static void tf_lexer__finish(TFLexer *self) {
  if (self->token_end_byte == TF_NO_END) tf_lexer__mark_end(&self->data);
}

bool tf_lexer_next(TFLexer *self, TSStateId state, TFToken *out) {
  const TSLanguage *ts = self->lang->ts;

  self->token_is_keyword = false;
  self->token_lex_state = state;
  tf_lexer__start(self);
  bool found = ts->lex_fn(&self->data, tf_lex_mode(self->lang, state).lex_state);
  tf_lexer__finish(self);
  if (!found) return false;

  out->symbol = self->data.result_symbol;
  out->start_byte = self->token_start_byte;
  out->start_point = self->token_start_point;
  out->end_byte = self->token_end_byte;
  out->end_point = self->token_end_point;

  // parser.c:645-670. A token that lexed as the word token is re-lexed with the
  // keyword lexer, and only reclassified if that lexer accepted, consumed exactly
  // the same bytes, and the resulting symbol is usable in this state -- either it
  // has actions, or the state reserves it. The keyword lexer is always called
  // with state 0.
  if (out->symbol == ts->keyword_capture_token && out->symbol != 0) {
    tf_lexer_seek(self, out->start_byte, out->start_point);
    tf_lexer__start(self);
    bool is_keyword = ts->keyword_lex_fn(&self->data, 0);
    tf_lexer__finish(self);
    if (is_keyword && self->token_end_byte == out->end_byte &&
        (tf_lookup(self->lang, state, self->data.result_symbol) != 0 ||
         tf_is_reserved_word(self->lang, state, self->data.result_symbol))) {
      out->symbol = self->data.result_symbol;
      // Recorded because the substitution was judged against *this* state, and
      // the parser may end up dispatching the token in a later one.
      self->token_is_keyword = true;
    }
  }

  // The keyword lexer may have stopped past the token, and the main lexer may
  // have read lookahead beyond it, so reposition explicitly. tree-sitter does the
  // same, from the parse stack's position (parser.c:531).
  tf_lexer_seek(self, out->end_byte, out->end_point);
  return true;
}
