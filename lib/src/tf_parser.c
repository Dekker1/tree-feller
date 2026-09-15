// The LR driver. Shifts and reduces straight into the sink's own values; no tree
// is built on the ordinary path. Conflicts retain private structural alternatives
// until selection; rare ties reconstruct completed structure with a private replay.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_lexer.h"

typedef struct TFSpec TFSpec;

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
  // Set on a private replay, which stops at this split's fork; see tf_spec__capture.
  TFSpec *capture;
} TFParser;

static bool tf_parser__grow(TFParser *self, uint32_t needed) {
  if (needed <= self->capacity) {
    return true;
  }
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
  self->capacity = (uint32_t)capacity;
  return true;
}

static bool tf_parser__push(TFParser *self, TFNode node, TSStateId state) {
  if (!tf_parser__grow(self, self->depth + 1)) {
    return false;
  }
  self->nodes[self->depth++] = node;
  self->states[self->depth] = state;
  return true;
}

static inline void *tf_parser__emit_shift(const TFParser *self, const TFToken *token, bool extra) {
  return self->sink->on_shift ? self->sink->on_shift(self->sink->payload, token, extra) : NULL;
}

static inline void *tf_parser__emit_reduce(const TFParser *self, const TFReduction *reduction) {
  return self->sink->on_reduce ? self->sink->on_reduce(self->sink->payload, reduction) : NULL;
}

static bool tf_parser__shift(TFParser *self, const TFToken *token, bool extra, TSStateId state) {
  TFNode node = {
      .symbol = token->symbol,
      .extra = extra,
      .start_byte = token->start_byte,
      .end_byte = token->end_byte,
      .start_point = token->start_point,
      .end_point = token->end_point,
      .value = tf_parser__emit_shift(self, token, extra),
  };
  if (extra && self->depth == self->leading) {
    self->leading++;
  }
  return tf_parser__push(self, node, state);
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
static bool tf_parser__reduce(TFParser *self, TSSymbol symbol, uint32_t child_count,
                              uint16_t production_id, const TFToken *lookahead) {
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

  TFReduction reduction = {
      .symbol = symbol,
      .production_id = production_id,
      .child_count = child_count,
      .node_count = end - base,
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

  // parser.c:/ts_parser__accept/ rebuilds the root to absorb the extras around it
  // and the end token. The root is only reduced with the end token as lookahead,
  // after which accept is the only action, so the stack already holds exactly
  // those children in order: leading extras, the root's own and trailing extras.
  // Nothing checks the tables for that; accept asserts it held.
  if (lookahead && lookahead->symbol == 0 && base == self->leading &&
      self->lang->accepts_end[state]) {
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
  }
  self->nodes[base] = parent;
  self->depth = base + 1 + trailing_count;
  for (uint32_t i = base + 1; i <= self->depth; i++) {
    self->states[i] = state;
  }
  return true;
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

  if (!tf_parser__grow(&self, 64)) {
    goto oom;
  }
  // State 0 is ERROR_STATE (error_costs.h:4); parsing starts at state 1
  // (stack.c:/ts_stack_new/, which seeds the base node with state 1).
  self.states[0] = 1;

  for (;;) {
    TSStateId state = self.states[self.depth];
    if (!tf_lexer_next(&self.lexer, state, &token)) {
      tf_parser__fail(&self, self.lexer.byte, self.lexer.point, "unexpected character");
      goto done;
    }

    // Reduce until the token can be shifted, or the parse ends.
    for (;;) {
      uint32_t count;
      const TSParseAction *actions = tf_actions(lang, state, token.symbol, &count);
      if (count == 0) {
        if (tf_parser__demote_keyword(lang, state, &token, self.lexer.token_is_keyword)) {
          continue;
        }
        tf_parser__fail_unexpected(&self, state, &token);
        goto done;
      }
      if (count > 1) {
        // The tables cannot decide here; work it out speculatively and replay.
        if (!tf_parser__split(&self, token, &token)) {
          goto done;
        }
        state = self.states[self.depth];
        continue;
      }

      TSParseAction action = actions[0];
      if (action.type == TSParseActionTypeShift) {
        // An extra does not change the state (parser.c:1633).
        if (!tf_parser__shift(&self, &token, action.shift.extra,
                              action.shift.extra ? state : action.shift.state)) {
          goto oom;
        }
        break;
      }

      if (action.type == TSParseActionTypeReduce) {
        if (!tf_parser__reduce(&self, action.reduce.symbol, action.reduce.child_count,
                               action.reduce.production_id, &token)) {
          goto oom;
        }
        state = self.states[self.depth];
        continue;
      }

      if (action.type == TSParseActionTypeAccept) {
        // The root reduction already went to the sink, end token included. If a
        // grammar ever reduces its root on another lookahead and still accepts,
        // the root would be missing its trailing extras and end token.
        assert(self.root_emitted);
        if (root && self.leading < self.depth) {
          *root = self.nodes[self.leading].value;
        }
        ok = true;
        goto done;
      }

      // TSParseActionTypeRecover: error recovery, which tree-feller does not do.
      tf_parser__fail_unexpected(&self, state, &token);
      goto done;
    }
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
  tf_spec__free(self.spec);
  return ok;
}

// Recover the structure of completed stack cells only when an ambiguity's
// structural tie-break reaches inside them. Replay stops at the exact fork;
// it calls only this private collector, never the consumer's callbacks.
static void *tf_capture__shift(void *payload, const TFToken *token, bool extra) {
  TFSpec *s = payload;
  uint32_t id = tf_spec__tree(s, (TFSpecTree){.token = *token, .leaf = true, .extra = extra});
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
  uint32_t id = tf_spec__tree(s, (TFSpecTree){.token = {.symbol = reduction->symbol,
                                                        .start_byte = reduction->start_byte,
                                                        .end_byte = reduction->end_byte,
                                                        .start_point = reduction->start_point,
                                                        .end_point = reduction->end_point},
                                              .first_child = first,
                                              .child_count = reduction->node_count});
  // Stable arena handle carried in the sink value; never dereferenced.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return s->failed ? NULL : (void *)(uintptr_t)(id + 1);
}

// Sets `failed` if the replay cannot recover the shapes.
static bool tf_spec__materialize(TFSpec *s) {
  if (!s->materialized) {
    s->materialized = true;
    TFSink sink = {.payload = s, .on_shift = tf_capture__shift, .on_reduce = tf_capture__reduce};
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
