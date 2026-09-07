// Builds a TFLanguage from a TSLanguage: checks the grammar is one this driver
// can run (ABI 15, no external scanner, no empty table), then expands its packed
// tables into the arrays tf_language.h reads directly -- see there for what each
// expansion buys and why it is safe to skip at lookup time.
#include "tf_language.h"

#include <stdlib.h>
#include <string.h>

// tree-sitter emits a state's actions as a run of TSParseActionEntry, headed by
// a {count, reusable} entry. SHIFT_REPEAT actions (`shift.repetition`) exist for
// incremental reuse and error recovery; the runtime skips them outright
// (parser.c:1631). Filtering them once here keeps them off the hot path, and
// makes `count > 1` mean "genuine conflict" rather than "maybe a repeat shift".
static bool tf_language__is_repeat_shift(TSParseAction action) {
  return action.type == TSParseActionTypeShift && action.shift.repetition;
}

// Walk every (state, terminal) pair to find the extent of `parse_actions`, which
// the TSLanguage does not carry a length for.
static uint32_t tf_language__action_table_extent(const TSLanguage *ts, const TFLanguage *probe) {
  uint32_t extent = 1;  // index 0 is the shared empty entry
  for (uint32_t state = 0; state < ts->state_count; state++) {
    for (uint32_t symbol = 0; symbol < ts->token_count; symbol++) {
      uint32_t index = tf_lookup(probe, (TSStateId)state, symbol);
      if (index == 0) continue;
      uint32_t end = index + 1 + ts->parse_actions[index].entry.count;
      if (end > extent) extent = end;
    }
  }
  return extent;
}

TFLanguage *tf_language_load(const TSLanguage *ts, const char **error) {
  const char *ignored = NULL;
  if (!error) error = &ignored;
  *error = NULL;

  if (!ts) {
    *error = "language is NULL";
    return NULL;
  }
  if (ts->abi_version != TF_ABI_VERSION) {
    *error = "language ABI version is not 15";
    return NULL;
  }
  // A language with no symbols or no states is not one; the allocations below
  // would all be zero-sized, and every table read would be out of bounds.
  if (ts->symbol_count == 0 || ts->state_count == 0 || ts->token_count == 0 ||
      ts->production_id_count == 0) {
    *error = "language has an empty symbol, state or production table";
    return NULL;
  }
  if (ts->external_token_count != 0) {
    *error = "grammars with an external scanner are not supported";
    return NULL;
  }
  TFLanguage *self = calloc(1, sizeof(TFLanguage));
  if (!self) {
    *error = "out of memory";
    return NULL;
  }
  self->ts = ts;

  // A 0xFFFF lex state marks a non-terminal extra rule, where the parser takes a
  // fixed reduction from the EOF entry instead of lexing (parser.c:1605). None of
  // the grammars this targets use one, and the driver does not implement it.
  for (uint32_t state = 0; state < ts->state_count; state++) {
    if (ts->lex_modes[state].lex_state == UINT16_MAX) {
      *error = "grammars with non-terminal extras are not supported";
      tf_language_free(self);
      return NULL;
    }
  }

  // Expand the parse table to one row per state.
  //
  // tree-sitter packs states with few entries into `small_parse_table` as
  // unsorted (value, symbols...) groups, so a lookup there is a linear scan.
  // That is the whole hot path for a small grammar: the CLI only emits a dense
  // row for a state with many entries, so the *stricter* the grammar, the fewer
  // of those there are. DataZinc has 2 dense rows out of 163. Expanding costs one
  // pass over the packed table and `state_count * symbol_count` 16-bit cells --
  // 25 KB for DataZinc, 574 KB for MiniZinc -- and turns the scan into an
  // indexed load.
  //
  // CONSIDERATION: a flat expansion, so a grammar with very many states pays for cells
  // that are mostly zero (18k states x 450 symbols would be 16 MB). If that ever
  // matters, expand per row on first use, or keep the packed scan above a size.
  size_t cells = (size_t)ts->state_count * ts->symbol_count;
  if (cells > (size_t)64 * 1024 * 1024) {
    *error = "parse table is too large to expand";
    tf_language_free(self);
    return NULL;
  }
  // Zeroed, because 0 is what a miss returns in the packed table too.
  uint16_t *dense = calloc(cells, sizeof(uint16_t));
  if (!dense) {
    *error = "out of memory";
    tf_language_free(self);
    return NULL;
  }
  self->dense = dense;
  // Dense states are already in this layout.
  memcpy(dense, ts->parse_table,
         (size_t)ts->large_state_count * ts->symbol_count * sizeof(uint16_t));
  for (uint32_t state = ts->large_state_count; state < ts->state_count; state++) {
    const uint16_t *data =
        &ts->small_parse_table[ts->small_parse_table_map[state - ts->large_state_count]];
    uint16_t *row = dense + (size_t)state * ts->symbol_count;
    uint16_t group_count = *(data++);
    for (unsigned i = 0; i < group_count; i++) {
      uint16_t section_value = *(data++);
      uint16_t symbol_count = *(data++);
      for (unsigned j = 0; j < symbol_count; j++) row[*(data++)] = section_value;
    }
  }

  // Flatten the field maps. `tf_field_map` hands back a list per production, and
  // resolving one child means scanning it; on a data file that is once per value
  // parsed. A production has at most `max_alias_sequence_length` structural
  // children, so the whole thing fits in a rectangle -- 492 bytes for DataZinc.
  uint32_t width = ts->max_alias_sequence_length;
  {
    const TSFieldMapEntry *entry, *end;
    for (uint32_t production = 0; production < ts->production_id_count; production++) {
      tf_field_map(self, production, &entry, &end);
      for (; entry != end; entry++) {
        if (!entry->inherited && entry->child_index + 1U > width) width = entry->child_index + 1U;
      }
    }
  }
  self->field_at_width = width;
  if (width > 0) {
    TSFieldId *field_at = calloc((size_t)ts->production_id_count * width, sizeof(TSFieldId));
    if (!field_at) {
      *error = "out of memory";
      tf_language_free(self);
      return NULL;
    }
    self->field_at = field_at;
    const TSFieldMapEntry *entry, *end;
    for (uint32_t production = 0; production < ts->production_id_count; production++) {
      tf_field_map(self, production, &entry, &end);
      for (; entry != end; entry++) {
        // First match wins, as the scan it replaces returned the first hit.
        TSFieldId *slot = &field_at[(size_t)production * width + entry->child_index];
        if (!entry->inherited && *slot == 0) *slot = entry->field_id;
      }
    }
  }

  // language.h:240-261. `alias_map` is a run of {symbol, count, aliases...},
  // ordered by symbol and terminated by a 0 symbol. A symbol listed there can be
  // renamed by its parent's production, so it cannot be treated as reliably
  // hidden.
  uint8_t *aliasable = calloc(ts->symbol_count, sizeof(uint8_t));
  if (!aliasable) {
    *error = "out of memory";
    tf_language_free(self);
    return NULL;
  }
  self->aliasable = aliasable;
  for (unsigned idx = 0;;) {
    TSSymbol symbol = ts->alias_map[idx++];
    if (symbol == 0) break;
    uint16_t count = ts->alias_map[idx++];
    if (symbol < ts->symbol_count) aliasable[symbol] = 1;
    idx += count;
  }

  self->action_entry_count = tf_language__action_table_extent(ts, self);
  uint8_t *counts = calloc(self->action_entry_count, sizeof(uint8_t));
  if (!counts) {
    *error = "out of memory";
    tf_language_free(self);
    return NULL;
  }
  self->action_counts = counts;

  // Fill in the filtered counts. Truncating the count only works if the kept
  // actions form a prefix of the entry, which holds for every grammar seen so
  // far; refuse to guess if it ever stops holding.
  for (uint32_t state = 0; state < ts->state_count; state++) {
    for (uint32_t symbol = 0; symbol < ts->token_count; symbol++) {
      uint32_t index = tf_lookup(self, (TSStateId)state, symbol);
      if (index == 0) continue;
      // The table said an action lives past the end of the action table. Nothing
      // this library does can make sense of that, so do not read it.
      if (index >= self->action_entry_count) {
        *error = "parse table refers to an action index that does not exist";
        tf_language_free(self);
        return NULL;
      }
      if (counts[index] != 0) continue;
      uint32_t count = ts->parse_actions[index].entry.count;
      const TSParseAction *actions = (const TSParseAction *)(&ts->parse_actions[index] + 1);
      uint32_t kept = 0;
      while (kept < count && !tf_language__is_repeat_shift(actions[kept])) kept++;
      for (uint32_t i = kept; i < count; i++) {
        if (!tf_language__is_repeat_shift(actions[i])) {
          *error = "SHIFT_REPEAT actions are not trailing in an action entry";
          tf_language_free(self);
          return NULL;
        }
      }
      counts[index] = (uint8_t)kept;
    }
  }

  // `state_count` is rejected above if it is zero, but the analyser loses that
  // across the loop in between.
  // NOLINTNEXTLINE(clang-analyzer-optin.portability.UnixAPI)
  uint8_t *accepts_end = calloc(ts->state_count, sizeof(uint8_t));
  if (!accepts_end) {
    *error = "out of memory";
    tf_language_free(self);
    return NULL;
  }
  self->accepts_end = accepts_end;
  for (uint32_t state = 0; state < ts->state_count; state++) {
    uint32_t count;
    const TSParseAction *actions = tf_actions(self, (TSStateId)state, ts_builtin_sym_end, &count);
    accepts_end[state] = count == 1 && actions[0].type == TSParseActionTypeAccept;
  }

  return self;
}

const char *tf_language_symbol_name(const TFLanguage *self, TSSymbol symbol) {
  if (symbol == ts_builtin_sym_end) return "end of file";
  if (symbol >= self->ts->symbol_count + self->ts->alias_count) return NULL;
  return self->ts->symbol_names[symbol];
}

const char *tf_language_field_name(const TFLanguage *self, TSFieldId field) {
  if (field == 0 || field > self->ts->field_count) return NULL;
  return self->ts->field_names[field];
}

void tf_language_free(TFLanguage *self) {
  if (!self) return;
  free((void *)self->dense);
  free((void *)self->aliasable);
  free((void *)self->field_at);
  free((void *)self->action_counts);
  free((void *)self->accepts_end);
  free(self);
}
