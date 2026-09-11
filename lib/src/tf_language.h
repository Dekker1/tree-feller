// Table access layer: the parts of tree-sitter's private `language.h` that a
// parse driver needs, ported verbatim, with no dependency on the runtime.
//
// Everything here is a pure read of the generated tables. The one thing that is
// not verbatim is `tf_actions`, which reports the SHIFT_REPEAT-filtered action
// count (see tf_language.c).
#ifndef TF_LANGUAGE_H
#define TF_LANGUAGE_H

#include <stddef.h>

#include "tree_feller.h"

// language.h:11 -- not in parser.h, but used by the table accessors.
#define tf_builtin_sym_error_repeat (ts_builtin_sym_error - 1)

struct TFLanguage {
  const TSLanguage *ts;
  // `small_parse_table` expanded to one row per state, so a lookup is an indexed
  // load rather than a linear scan of unsorted symbol groups. See tf_language.c.
  const uint16_t *dense;
  // Action counts with SHIFT_REPEAT actions removed, indexed by the same action
  // index as `ts->parse_actions`. Sized `action_entry_count`.
  const uint8_t *action_counts;
  uint32_t action_entry_count;
  // `production_id * field_at_width + child_index` -> the field that child fills
  // directly, or 0, for a `child_index` below `field_at_width`. Flattens the
  // per-production field map, which is otherwise a list to be scanned once per
  // child (tree_cursor.c:682). `inherited` entries describe fields further down
  // and are left out; a hidden child's own entries keep the field they already
  // resolved to. See tf_language.c.
  const TSFieldId *field_at;
  uint32_t field_at_width;
  // One byte per symbol: whether any production aliases it, so a hidden symbol
  // cannot be assumed to stay hidden. Sized `ts->symbol_count`.
  const uint8_t *aliasable;
  // One byte per state: whether the end of the file is accepted there. The driver
  // consults this on every reduction, which is why it is not a table lookup.
  const uint8_t *accepts_end;
};

// The same value language.h:78-92 computes, read from the expanded table instead
// of rescanning the packed one. A miss is 0 in both, because the expansion starts
// from zeroed memory.
static inline uint16_t tf_lookup(const TFLanguage *self, TSStateId state, TSSymbol symbol) {
  return self->dense[(size_t)state * self->ts->symbol_count + symbol];
}

// language.h:78-92, verbatim: the packed form, kept as the definition the
// expansion is built from and the tests check against. The emitted symbol groups
// are not sorted, so this must stay a linear scan.
static inline uint16_t tf_lookup_packed(const TFLanguage *self, TSStateId state, TSSymbol symbol) {
  const TSLanguage *ts = self->ts;
  if (state >= ts->large_state_count) {
    uint32_t index = ts->small_parse_table_map[state - ts->large_state_count];
    const uint16_t *data = &ts->small_parse_table[index];
    uint16_t group_count = *(data++);
    for (unsigned i = 0; i < group_count; i++) {
      uint16_t section_value = *(data++);
      uint16_t symbol_count = *(data++);
      for (unsigned j = 0; j < symbol_count; j++) {
        if (*(data++) == symbol) return section_value;
      }
    }
    return 0;
  } else {
    return ts->parse_table[state * ts->symbol_count + symbol];
  }
}

// language.c:66-85. `symbol` must be a terminal. The actions live immediately
// after the entry header, hence `entry + 1`. The count excludes SHIFT_REPEAT.
static inline const TSParseAction *tf_actions(const TFLanguage *self, TSStateId state,
                                              TSSymbol symbol, uint32_t *count) {
  uint32_t index = tf_lookup(self, state, symbol);
  const TSParseActionEntry *entry = &self->ts->parse_actions[index];
  *count = self->action_counts[index];
  return (const TSParseAction *)(entry + 1);
}

// language.c:142-162, verbatim -- including the fact that it does *not* skip
// SHIFT_REPEAT actions, so it uses the unfiltered count.
static inline TSStateId tf_next_state(const TFLanguage *self, TSStateId state, TSSymbol symbol) {
  const TSLanguage *ts = self->ts;
  if (symbol == ts_builtin_sym_error || symbol == tf_builtin_sym_error_repeat) {
    return 0;
  } else if (symbol < ts->token_count) {
    uint32_t index = tf_lookup(self, state, symbol);
    const TSParseActionEntry *entry = &ts->parse_actions[index];
    uint32_t count = entry->entry.count;
    if (count > 0) {
      TSParseAction action = ((const TSParseAction *)(entry + 1))[count - 1];
      if (action.type == TSParseActionTypeShift) {
        return action.shift.extra ? state : action.shift.state;
      }
    }
    return 0;
  } else {
    return tf_lookup(self, state, symbol);
  }
}

// language.h:196-208, verbatim.
static inline const TSSymbol *tf_alias_sequence(const TFLanguage *self, uint32_t production_id) {
  return production_id
             ? &self->ts
                    ->alias_sequences[(size_t)production_id * self->ts->max_alias_sequence_length]
             : NULL;
}

static inline TSSymbol tf_alias_at(const TFLanguage *self, uint32_t production_id,
                                   uint32_t child_index) {
  return production_id
             ? self->ts->alias_sequences[production_id * self->ts->max_alias_sequence_length +
                                         child_index]
             : 0;
}

// language.h:210-226, verbatim.
static inline void tf_field_map(const TFLanguage *self, uint32_t production_id,
                                const TSFieldMapEntry **start, const TSFieldMapEntry **end) {
  if (self->ts->field_count == 0) {
    *start = NULL;
    *end = NULL;
    return;
  }
  TSMapSlice slice = self->ts->field_map_slices[production_id];
  *start = &self->ts->field_map_entries[slice.index];
  *end = *start + slice.length;
}

// language.c:120-131, verbatim.
static inline TSSymbolMetadata tf_symbol_metadata(const TFLanguage *self, TSSymbol symbol) {
  if (symbol == ts_builtin_sym_error) {
    return (TSSymbolMetadata){.visible = true, .named = true, .supertype = false};
  } else if (symbol == tf_builtin_sym_error_repeat) {
    return (TSSymbolMetadata){.visible = false, .named = false, .supertype = false};
  } else {
    return self->ts->symbol_metadata[symbol];
  }
}

// language.c:134-140, verbatim.
static inline TSSymbol tf_public_symbol(const TFLanguage *self, TSSymbol symbol) {
  if (symbol == ts_builtin_sym_error) return symbol;
  return self->ts->public_symbol_map[symbol];
}

// language.c:87-102. ABI 15 always stores the 3-field TSLexerMode, so the
// pre-15 TSLexMode branch is dropped along with support for older ABIs.
static inline TSLexerMode tf_lex_mode(const TFLanguage *self, TSStateId state) {
  return self->ts->lex_modes[state];
}

// language.c:104-119, verbatim. `reserved_words` is a flat array of
// `max_reserved_word_set_size` columns, one row per set, each row terminated
// early by a 0. Set id 0 means the state reserves nothing, which is every state
// in a grammar that does not use `reserved`.
static inline bool tf_is_reserved_word(const TFLanguage *self, TSStateId state, TSSymbol symbol) {
  const TSLanguage *ts = self->ts;
  uint16_t set_id = ts->lex_modes[state].reserved_word_set_id;
  if (set_id == 0) return false;
  unsigned start = set_id * ts->max_reserved_word_set_size;
  unsigned end = start + ts->max_reserved_word_set_size;
  for (unsigned i = start; i < end; i++) {
    if (ts->reserved_words[i] == symbol) return true;
    if (ts->reserved_words[i] == 0) break;
  }
  return false;
}

// A hidden rule whose children a consumer may fold: hidden by its own metadata,
// and not something a parent production can alias into visibility.
static inline bool tf_foldable(const TFLanguage *self, TSSymbol symbol) {
  return !self->aliasable[symbol] && !tf_symbol_metadata(self, symbol).visible;
}

#endif  // TF_LANGUAGE_H
