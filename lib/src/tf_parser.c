// The LR driver. Shifts and reduces straight into the sink's own values; no tree
// is built on the ordinary path. Conflicts retain private structural alternatives
// until selection; rare ties reconstruct completed structure with a private replay.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_lexer.h"

// Forces the specialised copies below to be generated from one body each.
#if defined(__GNUC__) || defined(__clang__)
#define TF_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define TF_ALWAYS_INLINE inline
#endif

typedef struct TFSpec TFSpec;

// error_costs.h, verbatim. ERROR_STATE is state 0, which is why parsing starts
// at state 1 (stack.c:/ts_stack_new/).
#define ERROR_STATE 0
#define ERROR_COST_PER_RECOVERY 500
#define ERROR_COST_PER_MISSING_TREE 110
#define ERROR_COST_PER_SKIPPED_TREE 100
#define ERROR_COST_PER_SKIPPED_LINE 30
#define ERROR_COST_PER_SKIPPED_CHAR 1

// What tree-sitter's recovery reads off a subtree, for one stack cell: its error
// cost (subtree.h:/ts_subtree_error_cost/), whether it has children, and the
// two visible counts `ts_subtree_summarize_children` keeps (subtree.c:424).
// Recovery can reach a cell committed long before the error, and nothing can
// work these out once its children are gone, so they are kept as the stack is
// built -- beside `nodes`, and only by a parse that can recover.
//
// `stack_nodes` is the stack's node count up to and including this cell, which
// tree-sitter keeps on each stack node (stack.c:/node_count/). It is a count per
// cell, not one running total, because a merge raises only the node it lands on
// (stack.c:262): the cells below keep the count of the path that made them.
typedef struct {
  uint32_t error_cost;
  uint32_t child_count;
  uint32_t visible_child;
  uint32_t visible_descendant;
  uint32_t stack_nodes;
} TFCell;

typedef struct {
  const TFLanguage *lang;
  TFLexer lexer;
  const TFSink *sink;

  // states[0] is the start state; states[i] is the state after pushing nodes[i-1],
  // so states[depth] is always the current state.
  TSStateId *states;
  TFNode *nodes;
  uint32_t depth;
  uint32_t capacity;

  // Extras shifted before any real content sit at the bottom of the stack and
  // stay there; the root absorbs them (see tf_parser__reduce).
  uint32_t leading;
#ifndef NDEBUG
  // Whether the root went to the sink with the end token, for accept's assertion.
  bool root_emitted;
#endif

  TFError *error;
  uint32_t split_count;
  TFSpec *spec;

  // Only for a parse that can recover; NULL otherwise. `cells` runs parallel to
  // `nodes`. The totals are over the whole stack, which is where tree-sitter
  // measures a version's error cost and node count from (stack.c:/stack_node_new/).
  TFCell *cells;
  uint32_t total_error;
  // parser.c:/last_position/, which lives for the whole parse: a split that
  // recovers starts from where the last one left it, not from its fork.
  uint32_t last_position;
  // The terminals a rejecting state would have taken, for TFErrorEvent.
  TSSymbol *expected;
  // Set on a private replay, which stops at this split's fork; see tf_spec__capture.
  TFSpec *capture;
  // Set when a split accepted an ERROR root, which the tables cannot accept.
  bool accepted;
} TFParser;

// Out of line, so that the check in front of it inlines into every shift and
// reduction: grown by the cells, gcc stopped inlining the whole function, which
// was half of a 4% regression on a data file.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static bool tf_parser__grow_slow(TFParser *self, uint32_t needed) {
  // 64-bit, so doubling past 2^31 cannot wrap to 0 and loop forever.
  uint64_t capacity = self->capacity ? self->capacity : 64;
  while (capacity < needed) {
    capacity *= 2;
  }
  if (capacity > UINT32_MAX) {
    capacity = UINT32_MAX;
  }
  TSStateId *states = realloc(self->states, (capacity + 1) * sizeof(TSStateId));
  TFNode *nodes = realloc(self->nodes, capacity * sizeof(TFNode));
  if (states) {
    self->states = states;
  }
  if (nodes) {
    self->nodes = nodes;
  }
  if (!states || !nodes) {
    return false;
  }
  if (self->cells) {
    TFCell *cells = realloc(self->cells, capacity * sizeof(TFCell));
    if (!cells) {
      return false;
    }
    self->cells = cells;
  }
  self->capacity = (uint32_t)capacity;
  return true;
}

static inline bool tf_parser__grow(TFParser *self, uint32_t needed) {
  return needed <= self->capacity || tf_parser__grow_slow(self, needed);
}

static bool tf_parser__push(TFParser *self, TFNode node, TSStateId state) {
  if (!tf_parser__grow(self, self->depth + 1)) {
    return false;
  }
  self->nodes[self->depth++] = node;
  self->states[self->depth] = state;
  return true;
}

// stack.c:/stack__subtree_node_count/: what one stack cell adds to a version's
// node count. An intermediate error node counts though it is hidden, because the
// count exists to tell whether a version has made progress since its last error.
static inline uint32_t tf_parser__node_count(const TFLanguage *lang, TSSymbol symbol,
                                             uint32_t visible_descendants) {
  return visible_descendants + tf_symbol_metadata(lang, symbol).visible +
         (symbol == tf_builtin_sym_error_repeat);
}

// subtree.c:/ts_subtree__error_extent_cost/.
static inline uint32_t tf_parser__extent_cost(uint32_t bytes, uint32_t rows) {
  return ERROR_COST_PER_RECOVERY + ERROR_COST_PER_SKIPPED_CHAR * bytes +
         ERROR_COST_PER_SKIPPED_LINE * rows;
}

// subtree.c:/ts_subtree_summarize_children/, the error cost of one child as its
// parent charges it. `error_parent` is whether the parent is ERROR or
// error_repeat, the only parents that charge for skipped trees.
static inline uint32_t tf_parser__child_cost(const TFLanguage *lang, bool error_parent,
                                             TSSymbol symbol, bool extra, uint32_t error_cost,
                                             bool has_children, uint32_t visible_children,
                                             uint32_t bytes, uint32_t rows) {
  if (symbol == tf_builtin_sym_error_repeat) {
    // Refund the child's own extent charge, which the parent re-charges as part
    // of its extent, so that grouping is cost-neutral.
    return error_cost - tf_parser__extent_cost(bytes, rows);
  }
  if (error_parent && !extra && !(symbol == ts_builtin_sym_error && !has_children)) {
    if (tf_symbol_metadata(lang, symbol).visible) {
      return error_cost + ERROR_COST_PER_SKIPPED_TREE;
    }
    if (has_children) {
      return error_cost + ERROR_COST_PER_SKIPPED_TREE * visible_children;
    }
  }
  return error_cost;
}

static inline bool tf_parser__is_error(TSSymbol symbol) {
  return symbol == ts_builtin_sym_error || symbol == tf_builtin_sym_error_repeat;
}

static inline void *tf_parser__emit_shift(const TFParser *self, const TFToken *token, bool extra) {
  return self->sink->on_shift ? self->sink->on_shift(self->sink->payload, token, extra) : NULL;
}

static inline void *tf_parser__emit_reduce(const TFParser *self, const TFReduction *reduction) {
  return self->sink->on_reduce ? self->sink->on_reduce(self->sink->payload, reduction) : NULL;
}

// stack.c:/stack_node_new/: a cell counts the nodes below it and its own.
static inline void tf_parser__count_nodes(TFParser *self, uint32_t i) {
  TFCell *cell = &self->cells[i];
  cell->stack_nodes =
      (i ? self->cells[i - 1].stack_nodes : 0) +
      tf_parser__node_count(self->lang, self->nodes[i].symbol, cell->visible_descendant);
}

// Every hot function below comes in two copies, chosen once per parse by
// whether the sink asked to recover. Without recovery, `recover` is a constant
// false and everything recovery keeps on the stack compiles away, so a parse
// that cannot recover runs exactly the instructions it ran before recovery
// existed. Testing `sink->on_error` per call instead measured 1.2% on a
// reduction-heavy file.
static TF_ALWAYS_INLINE bool tf_parser__shift_as(TFParser *self, const TFToken *token, bool extra,
                                                 TSStateId state, bool recover) {
  TFNode node = {
      .symbol = token->symbol,
      .extra = extra,
      // Only recovery inserts missing tokens.
      .missing = recover && token->missing,
      .start_byte = token->start_byte,
      .end_byte = token->end_byte,
      .start_point = token->start_point,
      .end_point = token->end_point,
      .value = tf_parser__emit_shift(self, token, extra),
  };
  if (extra && self->depth == self->leading) {
    self->leading++;
  }
  if (!tf_parser__push(self, node, state)) {
    return false;
  }
  if (recover) {
    // Only a token recovery inserted costs anything (subtree.h:/ts_subtree_error_cost/).
    uint32_t cost = token->missing ? ERROR_COST_PER_MISSING_TREE + ERROR_COST_PER_RECOVERY : 0;
    self->cells[self->depth - 1] = (TFCell){.error_cost = cost};
    self->total_error += cost;
    tf_parser__count_nodes(self, self->depth - 1);
  }
  return true;
}

static bool tf_parser__shift_plain(TFParser *self, const TFToken *token, bool extra,
                                   TSStateId state) {
  return tf_parser__shift_as(self, token, extra, state, false);
}

static bool tf_parser__shift_recover(TFParser *self, const TFToken *token, bool extra,
                                     TSStateId state) {
  return tf_parser__shift_as(self, token, extra, state, true);
}

static void tf_parser__fail(TFParser *self, uint32_t byte, TFPoint point, const char *format, ...) {
  if (!self->error) {
    return;
  }
  self->error->byte = byte;
  self->error->point = point;
  va_list args;
  va_start(args, format);
  vsnprintf(self->error->message, TF_ERROR_MESSAGE_SIZE, format, args);
  va_end(args);
}

static const char *tf_parser__symbol_name(const TFLanguage *lang, TSSymbol symbol) {
  const char *name = tf_language_symbol_name(lang, symbol);
  return name ? name : "?";
}

// The tokens that would have been accepted here. tree-sitter has a lookahead
// iterator (language.h:109-178) that exploits the small table's structure; this
// runs once per parse, at the point where it is about to stop, so a plain scan
// over the terminals is enough and is a tenth of the code.
static void tf_parser__describe_expected(const TFLanguage *lang, TSStateId state, char *out,
                                         size_t size) {
  size_t used = 0;
  unsigned found = 0;
  for (uint32_t symbol = 0; symbol < lang->ts->token_count; symbol++) {
    uint32_t count;
    tf_actions(lang, state, symbol, &count);
    if (count == 0) {
      continue;
    }
    const char *name = tf_parser__symbol_name(lang, symbol);
    int written = snprintf(out + used, size - used, "%s%s", found++ ? ", " : "", name);
    if (written < 0 || (size_t)written >= size - used) {
      snprintf(out + (used > 4 ? used - 4 : 0), size - (used > 4 ? used - 4 : 0), ", ...");
      return;
    }
    used += (size_t)written;
  }
}

static void tf_parser__fail_unexpected(TFParser *self, TSStateId state, const TFToken *token) {
  char expected[TF_ERROR_MESSAGE_SIZE / 2];
  tf_parser__describe_expected(self->lang, state, expected, sizeof(expected));
  tf_parser__fail(self, token->start_byte, token->start_point, "expected one of {%s}, found %s",
                  expected, tf_parser__symbol_name(self->lang, token->symbol));
}

// parser.c:/ts_parser__reduce/, with the GLR bookkeeping removed. Pops
// `child_count` non-extra cells -- carrying along any extras between them --
// hands them to the sink, and pushes the result in their place. `lookahead` is
// the token that will follow, where the caller knows it.
static TF_ALWAYS_INLINE bool tf_parser__reduce_as(TFParser *self, TSSymbol symbol,
                                                  uint32_t child_count, uint16_t production_id,
                                                  const TFToken *lookahead, bool recover) {
  // The extras above the last real child are exactly the run this scan crosses
  // before it reaches one, so counting them here saves walking the top of the
  // stack a second time.
  uint32_t popped = 0, trailing_count = 0;
  for (uint32_t structural = 0; structural < child_count;) {
    popped++;
    if (!self->nodes[self->depth - popped].extra) {
      structural++;
    } else if (structural == 0) {
      trailing_count++;
    }
  }
  uint32_t base = self->depth - popped;
  uint32_t end = self->depth - trailing_count;
  // Replay marks cells an ERROR wraps as extras (tf_spec__mark_wrapped), so they
  // can be counted into the leading run and then reduced into the ERROR.
  if (recover && self->leading > base) {
    self->leading = base;
  }

  // tree-sitter's summary of the cell that will replace these children
  // (subtree.c:/ts_subtree_summarize_children/), for recovery to weigh it by.
  // Only a parse that can recover keeps it, which `recover` settles at compile
  // time for each copy of this function. It has to run here, before the
  // trailing extras are moved down over these cells further on; folding it into
  // the scan above cost 5.9% on a reduction-heavy file.
  TFCell cell = {.child_count = end - base};
  uint32_t popped_error = 0;
  if (recover) {
    const TSSymbol *alias_row = tf_alias_sequence(self->lang, production_id);
    bool error_parent = tf_parser__is_error(symbol);
    uint32_t structural = 0;
    for (uint32_t i = base; i < end; i++) {
      const TFNode *child = &self->nodes[i];
      const TFCell *c = &self->cells[i];
      bool visible = tf_symbol_metadata(self->lang, child->symbol).visible;
      TSSymbol alias =
          (alias_row && !child->extra && child->symbol != 0) ? alias_row[structural] : 0;
      cell.visible_descendant += c->visible_descendant;
      if (alias || visible) {
        cell.visible_descendant++;
        cell.visible_child++;
      } else {
        cell.visible_child += c->visible_child;
      }
      // Beyond the sum of the children's costs, only an ERROR or error_repeat
      // parent charges anything, and only an error_repeat child is refunded.
      if (error_parent || child->symbol == tf_builtin_sym_error_repeat) {
        cell.error_cost += tf_parser__child_cost(
            self->lang, error_parent, child->symbol, child->extra, c->error_cost,
            c->child_count > 0, c->visible_child, child->end_byte - child->start_byte,
            child->end_point.row - child->start_point.row);
      } else {
        cell.error_cost += c->error_cost;
      }
      popped_error += c->error_cost;
      structural += !child->extra;
    }
  }

  TFReduction reduction = {
      .symbol = symbol,
      .production_id = production_id,
      .child_count = child_count,
      .node_count = end - base,
      .trailing_count = trailing_count,
      .children = &self->nodes[base],
  };
  if (reduction.node_count > 0) {
    reduction.start_byte = self->nodes[base].start_byte;
    reduction.start_point = self->nodes[base].start_point;
    reduction.end_byte = self->nodes[end - 1].end_byte;
    reduction.end_point = self->nodes[end - 1].end_point;
  } else {
    // An empty production sits at the current position, above any extras.
    reduction.start_byte = reduction.end_byte =
        self->depth ? self->nodes[self->depth - 1].end_byte : 0;
    reduction.start_point = reduction.end_point =
        self->depth ? self->nodes[self->depth - 1].end_point : (TFPoint){0, 0};
  }

  TSStateId state = tf_next_state(self->lang, self->states[base], symbol);

  TFNode parent = {
      .symbol = symbol,
      .start_byte = reduction.start_byte,
      .end_byte = reduction.end_byte,
      .start_point = reduction.start_point,
      .end_point = reduction.end_point,
  };
  if (recover && tf_parser__is_error(symbol)) {
    cell.error_cost += tf_parser__extent_cost(reduction.end_byte - reduction.start_byte,
                                              reduction.end_point.row - reduction.start_point.row);
  }

  // parser.c:/ts_parser__accept/ rebuilds the root to absorb the extras around it
  // and the end token. The root is only reduced with the end token as lookahead,
  // after which accept is the only action, so the stack already holds exactly
  // those children in order: leading extras, the root's own and trailing extras.
  // Nothing checks the tables for that; accept asserts it held.
  // A file still in error at its end has an ERROR node for a root
  // (parser.c:/recover_eof/), and tf_next_state puts that in state 0, which no
  // grammar accepts in. The condition is true once per parse, so the extra
  // comparison costs nothing.
  if (lookahead && lookahead->symbol == 0 && base == self->leading &&
      (self->lang->accepts_end[state] || symbol == ts_builtin_sym_error)) {
    if (!tf_parser__grow(self, self->depth + 1)) {
      return false;
    }
#ifndef NDEBUG
    self->root_emitted = true;
#endif
    TFNode *children = self->nodes;
    uint32_t total = self->depth + 1;
    children[self->depth] = (TFNode){
        .symbol = lookahead->symbol,
        .extra = true,
        .start_byte = lookahead->start_byte,
        .end_byte = lookahead->end_byte,
        .start_point = lookahead->start_point,
        .end_point = lookahead->end_point,
        .value = tf_parser__emit_shift(self, lookahead, true),
    };
    reduction.node_count = total;
    reduction.trailing_count = 0;
    reduction.children = children;
    reduction.start_byte = total > 1 ? children[0].start_byte : lookahead->start_byte;
    reduction.start_point = total > 1 ? children[0].start_point : lookahead->start_point;
    reduction.end_byte = lookahead->end_byte;
    reduction.end_point = lookahead->end_point;
  }
  parent.value = tf_parser__emit_reduce(self, &reduction);

  // The parent takes the cell at `base`, and the trailing extras excluded from it
  // sit directly on top in the new state (parser.c:/trailing_extras/). They move
  // down, or up by one above an empty production, so the regions can overlap.
  if (!tf_parser__grow(self, base + 1 + trailing_count)) {
    return false;
  }
  // Almost always zero -- a data file is mostly not comments -- and the call is
  // not free at one per reduction.
  if (trailing_count > 0) {
    memmove(&self->nodes[base + 1], &self->nodes[end], trailing_count * sizeof(TFNode));
    if (recover) {
      memmove(&self->cells[base + 1], &self->cells[end], trailing_count * sizeof(TFCell));
    }
  }
  self->nodes[base] = parent;
  if (recover) {
    self->cells[base] = cell;
    // Zero unless an error node was involved.
    self->total_error += cell.error_cost - popped_error;
    // The trailing extras are pushed again above the parent (parser.c:/trailing_extras/).
    for (uint32_t i = base; i <= base + trailing_count; i++) {
      tf_parser__count_nodes(self, i);
    }
  }
  self->depth = base + 1 + trailing_count;
  for (uint32_t i = base + 1; i <= self->depth; i++) {
    self->states[i] = state;
  }
  return true;
}

static bool tf_parser__reduce_plain(TFParser *self, TSSymbol symbol, uint32_t child_count,
                                    uint16_t production_id, const TFToken *lookahead) {
  return tf_parser__reduce_as(self, symbol, child_count, production_id, lookahead, false);
}

static bool tf_parser__reduce_recover(TFParser *self, TSSymbol symbol, uint32_t child_count,
                                      uint16_t production_id, const TFToken *lookahead) {
  return tf_parser__reduce_as(self, symbol, child_count, production_id, lookahead, true);
}

static bool tf_parser__reduce(TFParser *self, TSSymbol symbol, uint32_t child_count,
                              uint16_t production_id, const TFToken *lookahead) {
  return self->sink->on_error
             ? tf_parser__reduce_recover(self, symbol, child_count, production_id, lookahead)
             : tf_parser__reduce_plain(self, symbol, child_count, production_id, lookahead);
}

static bool tf_parser__demote_keyword(const TFLanguage *lang, TSStateId state, TFToken *token,
                                      bool was_keyword) {
  const TSLanguage *ts = lang->ts;
  if (!was_keyword || token->symbol == ts->keyword_capture_token) {
    return false;
  }
  if (tf_is_reserved_word(lang, state, token->symbol)) {
    return false;
  }
  uint32_t count;
  tf_actions(lang, state, ts->keyword_capture_token, &count);
  if (count == 0) {
    return false;
  }
  token->symbol = ts->keyword_capture_token;
  return true;
}

#include "tf_parser_spec.h"

typedef enum { TF_LOOP_OK, TF_LOOP_FAIL, TF_LOOP_OOM } TFLoopResult;

// The root reduction already went to the sink, end token included. If a grammar
// ever reduces its root on another lookahead and still accepts, the root would
// be missing its trailing extras and end token.
static TFLoopResult tf_parser__accept(TFParser *self, void **root) {
  assert(self->root_emitted);
  if (root && self->leading < self->depth) {
    *root = self->nodes[self->leading].value;
  }
  return TF_LOOP_OK;
}

// The dispatch loop, specialised on `recover` like the helpers it calls.
// `lang` is passed rather than read from `self`: `self` escapes into every
// shift and reduce, so a field read would be reloaded after each call.
static TF_ALWAYS_INLINE TFLoopResult tf_parser__loop(TFParser *self, const TFLanguage *lang,
                                                     void **root, TFToken *token, bool recover) {
  for (;;) {
    TSStateId state = self->states[self->depth];
    if (!tf_lexer_next(&self->lexer, state, token)) {
      if (!recover) {
        tf_parser__fail(self, self->lexer.byte, self->lexer.point, "unexpected character");
        return TF_LOOP_FAIL;
      }
      // Nothing lexed at all: recovery starts by lexing in error mode. The
      // token only carries where, which a private replay compares its fork by.
      *token = (TFToken){.start_byte = self->depth ? self->nodes[self->depth - 1].end_byte : 0};
      if (!tf_parser__split(self, *token, token, false)) {
        return TF_LOOP_FAIL;
      }
      if (self->accepted) {
        return tf_parser__accept(self, root);
      }
      state = self->states[self->depth];
    }

    // Reduce until the token can be shifted, or the parse ends.
    for (;;) {
      uint32_t count;
      const TSParseAction *actions = tf_actions(lang, state, token->symbol, &count);
      if (count == 0) {
        if (tf_parser__demote_keyword(lang, state, token, self->lexer.token_is_keyword)) {
          continue;
        }
        if (recover) {
          // The one place a parse that can recover differs: speculate, and let
          // the split recover as tree-sitter would.
          if (!tf_parser__split(self, *token, token, true)) {
            return TF_LOOP_FAIL;
          }
          if (self->accepted) {
            return tf_parser__accept(self, root);
          }
          state = self->states[self->depth];
          continue;
        }
        tf_parser__fail_unexpected(self, state, token);
        return TF_LOOP_FAIL;
      }
      if (count > 1) {
        // The tables cannot decide here; work it out speculatively and replay.
        if (!tf_parser__split(self, *token, token, true)) {
          return TF_LOOP_FAIL;
        }
        if (recover && self->accepted) {
          return tf_parser__accept(self, root);
        }
        state = self->states[self->depth];
        continue;
      }

      TSParseAction action = actions[0];
      if (action.type == TSParseActionTypeShift) {
        // An extra does not change the state (parser.c:1633).
        if (!(recover ? tf_parser__shift_recover : tf_parser__shift_plain)(
                self, token, action.shift.extra, action.shift.extra ? state : action.shift.state)) {
          return TF_LOOP_OOM;
        }
        break;
      }

      if (action.type == TSParseActionTypeReduce) {
        if (!(recover ? tf_parser__reduce_recover : tf_parser__reduce_plain)(
                self, action.reduce.symbol, action.reduce.child_count, action.reduce.production_id,
                token)) {
          return TF_LOOP_OOM;
        }
        state = self->states[self->depth];
        continue;
      }

      if (action.type == TSParseActionTypeAccept) {
        return tf_parser__accept(self, root);
      }

      // TSParseActionTypeRecover, which only the error state's row holds. The
      // ordinary parser is never left in that state, but if a parse that can
      // recover ever got here, the split would recover as tree-sitter does.
      if (recover) {
        if (!tf_parser__split(self, *token, token, true)) {
          return TF_LOOP_FAIL;
        }
        if (self->accepted) {
          return tf_parser__accept(self, root);
        }
        state = self->states[self->depth];
        continue;
      }
      tf_parser__fail_unexpected(self, state, token);
      return TF_LOOP_FAIL;
    }
  }
}

static bool tf_parser__run(const TFLanguage *lang, const void *source, size_t size,
                           const TFSink *sink, void **root, TFError *error, TFSpec *capture) {
  static const TFSink no_sink = {0};
  TFParser self = {
      .lang = lang, .sink = sink ? sink : &no_sink, .error = error, .capture = capture};
  if (error) {
    *error = (TFError){0};
  }
  if (root) {
    *root = NULL;
  }
  if (size > UINT32_MAX) {
    tf_parser__fail(&self, 0, (TFPoint){0, 0}, "input is larger than 4 GiB");
    return false;
  }
  tf_lexer_init(&self.lexer, lang, source, (uint32_t)size);
  bool ok = false;
  // Zeroed, so running out of memory before the first token reports byte 0.
  TFToken token = {0};

  if (self.sink->on_error) {
    // Before the first grow, which then keeps it the size of `nodes`.
    self.cells = malloc(64 * sizeof(TFCell));
    if (!self.cells) {
      goto oom;
    }
  }
  if (!tf_parser__grow(&self, 64)) {
    goto oom;
  }
  // State 0 is ERROR_STATE (error_costs.h:4); parsing starts at state 1
  // (stack.c:/ts_stack_new/, which seeds the base node with state 1).
  self.states[0] = 1;

  // Picked once: see tf_parser__shift_as.
  switch (self.sink->on_error ? tf_parser__loop(&self, lang, root, &token, true)
                              : tf_parser__loop(&self, lang, root, &token, false)) {
    case TF_LOOP_OK:
      ok = true;
      goto done;
    case TF_LOOP_FAIL:
      goto done;
    case TF_LOOP_OOM:
    default:
      break;
  }

oom:
  tf_parser__fail(&self, token.start_byte, token.start_point, "out of memory");
done:
  // A failed parse has no root to hand the consumer, so anything it built is
  // otherwise dropped on the floor. Give it back before the stack goes away.
  if (!ok && self.sink->on_discard) {
    for (uint32_t i = 0; i < self.depth; i++) {
      if (self.nodes[i].value) {
        self.sink->on_discard(self.sink->payload, self.nodes[i].value);
      }
    }
  }
  free(self.states);
  free(self.nodes);
  free(self.cells);
  free(self.expected);
  tf_spec__free(self.spec);
  return ok;
}

// Recover the structure of completed stack cells only when an ambiguity's
// structural tie-break reaches inside them. Replay stops at the exact fork;
// it calls only this private collector, never the consumer's callbacks.
static void *tf_capture__shift(void *payload, const TFToken *token, bool extra) {
  TFSpec *s = payload;
  uint32_t id =
      tf_spec__tree(s, (TFSpecTree){.token = *token, .leaf = true, .extra = extra}, s->recover);
  // Stable arena handle carried in the sink value; never dereferenced.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return s->failed ? NULL : (void *)(uintptr_t)(id + 1);
}

static void *tf_capture__reduce(void *payload, const TFReduction *reduction) {
  TFSpec *s = payload;
  if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + reduction->node_count)) {
    return NULL;
  }
  uint32_t first = s->child_count;
  for (uint32_t i = 0; i < reduction->node_count; i++) {
    s->children[s->child_count++] = (uint32_t)(uintptr_t)reduction->children[i].value - 1;
  }
  uint32_t id = tf_spec__tree(s,
                              (TFSpecTree){.token = {.symbol = reduction->symbol,
                                                     .start_byte = reduction->start_byte,
                                                     .end_byte = reduction->end_byte,
                                                     .start_point = reduction->start_point,
                                                     .end_point = reduction->end_point},
                                           .first_child = first,
                                           .child_count = reduction->node_count,
                                           .production_id = reduction->production_id},
                              s->recover);
  if (s->recover && id != TF_SPEC_NONE) {
    tf_spec__summarize(s, id, first, reduction->node_count);
  }
  // Stable arena handle carried in the sink value; never dereferenced.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return s->failed ? NULL : (void *)(uintptr_t)(id + 1);
}

// The consumer already agreed to every recovery before the fork, and the replay
// has to take the same path to reach it.
static bool tf_capture__error(void *payload, const TFErrorEvent *event) {
  (void)payload;
  (void)event;
  return true;
}

// Sets `failed` if the replay cannot recover the shapes.
static bool tf_spec__materialize(TFSpec *s) {
  if (!s->materialized) {
    s->materialized = true;
    TFSink sink = {.payload = s,
                   .on_shift = tf_capture__shift,
                   .on_reduce = tf_capture__reduce,
                   .on_error = s->recover ? tf_capture__error : NULL};
    // The replay stops by failing once it has captured, so only `captured` counts.
    (void)tf_parser__run(s->owner->lang, s->owner->lexer.source, s->owner->lexer.size, &sink, NULL,
                         NULL, s);
    if (!s->captured) {
      s->failed = true;
    }
  }
  return !s->failed;
}

bool tf_parse(const TFLanguage *lang, const void *source, size_t size, const TFSink *sink,
              void **root, TFError *error) {
  return tf_parser__run(lang, source, size, sink, root, error, NULL);
}

#undef TF_SPEC_RESERVE
