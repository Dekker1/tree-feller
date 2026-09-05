// Exhaustive agreement between tree-feller's table accessors and tree-sitter's
// own, over every state and every symbol of every grammar in grammars.h.
#include <stdio.h>
#include <stdlib.h>

#include "tree_sitter/api.h"  // before parser.h, whose typedefs it guards

#include "tf_language.h"

// tree-sitter's private accessors, the reference for everything below.
#include "language.h"

#include "grammars.h"

static unsigned failures = 0;

#define CHECK(cond, ...)                      \
  do {                                        \
    if (!(cond)) {                            \
      if (failures < 20) {                    \
        fprintf(stderr, "  FAIL %s: ", name); \
        fprintf(stderr, __VA_ARGS__);         \
        fprintf(stderr, "\n");                \
      }                                       \
      failures++;                             \
    }                                         \
  } while (0)

// The count tree-feller should report: tree-sitter's, minus SHIFT_REPEAT
// actions, which the runtime skips at parser.c:1631.
static uint32_t expected_filtered_count(const TSParseAction *actions, uint32_t count) {
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!(actions[i].type == TSParseActionTypeShift && actions[i].shift.repetition)) kept++;
  }
  return kept;
}

static void check_grammar(const char *name, const TSLanguage *ts) {
  const char *error = NULL;
  TFLanguage *lang = tf_language_load(ts, &error);
  if (!lang) {
    fprintf(stderr, "  FAIL %s: load: %s\n", name, error);
    failures++;
    return;
  }

  // Census over *unique* action entries, so the numbers compare directly with a
  // hand decode of the generated tables.
  unsigned char *seen = calloc(1, 1U << 16);
  unsigned entries = 0, repeat_filtered = 0, genuine_conflicts = 0;

  for (uint32_t s = 0; s < ts->state_count; s++) {
    TSStateId state = (TSStateId)s;

    TSLexerMode want_mode = ts_language_lex_mode_for_state(ts, state);
    TSLexerMode got_mode = tf_lex_mode(lang, state);
    CHECK(want_mode.lex_state == got_mode.lex_state &&
              want_mode.external_lex_state == got_mode.external_lex_state &&
              want_mode.reserved_word_set_id == got_mode.reserved_word_set_id,
          "state %u lex mode", s);

    for (uint32_t y = 0; y < ts->symbol_count; y++) {
      TSSymbol symbol = (TSSymbol)y;

      CHECK(tf_lookup(lang, state, symbol) == ts_language_lookup(ts, state, symbol),
            "state %u symbol %u lookup", s, y);
      CHECK(tf_next_state(lang, state, symbol) == ts_language_next_state(ts, state, symbol),
            "state %u symbol %u next_state", s, y);

      if (symbol >= ts->token_count) continue;

      uint32_t want_count;
      const TSParseAction *want = ts_language_actions(ts, state, symbol, &want_count);
      uint32_t got_count;
      const TSParseAction *got = tf_actions(lang, state, symbol, &got_count);
      CHECK(want == got, "state %u symbol %u action pointer", s, y);
      CHECK(got_count == expected_filtered_count(want, want_count),
            "state %u symbol %u filtered count %u != %u", s, y, got_count,
            expected_filtered_count(want, want_count));
      uint32_t index = tf_lookup(lang, state, symbol);
      if (!seen[index]) {
        seen[index] = 1;
        entries++;
        if (want_count != got_count) repeat_filtered++;
        if (got_count > 1) genuine_conflicts++;
      }
    }
  }

  for (uint32_t p = 0; p < ts->production_id_count; p++) {
    const TSFieldMapEntry *want_start, *want_end, *got_start, *got_end;
    ts_language_field_map(ts, p, &want_start, &want_end);
    tf_field_map(lang, p, &got_start, &got_end);
    CHECK(want_start == got_start && want_end == got_end, "production %u field map", p);

    CHECK(tf_alias_sequence(lang, p) == ts_language_alias_sequence(ts, p),
          "production %u alias sequence", p);
    for (uint32_t i = 0; i < ts->max_alias_sequence_length; i++) {
      CHECK(tf_alias_at(lang, p, i) == ts_language_alias_at(ts, p, i), "production %u alias %u", p,
            i);
    }
  }

  for (uint32_t y = 0; y < ts->symbol_count; y++) {
    TSSymbolMetadata want = ts_language_symbol_metadata(ts, (TSSymbol)y);
    TSSymbolMetadata got = tf_symbol_metadata(lang, (TSSymbol)y);
    CHECK(want.visible == got.visible && want.named == got.named && want.supertype == got.supertype,
          "symbol %u metadata", y);
  }

  printf(
      "  %-9s states=%4u large=%3u symbols=%3u tokens=%3u fields=%2u aliases=%u "
      "productions=%3u | action entries=%u repeat-filtered=%u genuine-conflicts=%u\n",
      name, ts->state_count, ts->large_state_count, ts->symbol_count, ts->token_count,
      ts->field_count, ts->alias_count, ts->production_id_count, entries, repeat_filtered,
      genuine_conflicts);
  free(seen);

  tf_language_free(lang);
}

int main(void) {
  printf("table agreement:\n");
  for (size_t i = 0; i < TF_GRAMMAR_COUNT; i++) {
    check_grammar(TF_GRAMMARS[i].name, TF_GRAMMARS[i].language());
  }
  if (failures) {
    fprintf(stderr, "%u failures\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
