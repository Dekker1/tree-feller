// The LR driver. Shifts and reduces straight into the sink's own values; no tree
// is built and nothing is retained below the current nesting depth.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_lexer.h"

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

  // Extras sitting above the last real child of a reduction: excluded from it,
  // then re-pushed above the new parent (parser.c:/trailing_extras/).
  TFNode *trailing;
  uint32_t trailing_capacity;

  // parser.c:/ts_parser__accept/ rebuilds the root node to absorb the trailing
  // extras above it plus the end token, so the reduction that produces the root
  // is held back until the end token arrives and can be included.
  // Extras shifted before any real content sit at the bottom of the stack and
  // stay there; the root absorbs them too.
  uint32_t leading;

  struct {
    bool pending;
    uint32_t base;
    TSSymbol symbol;
    uint16_t production_id;
    uint32_t child_count;
    uint32_t node_count;
    TFNode *children;
    uint32_t capacity;
  } root;

  TFError *error;
} TFParser;

static bool tf_parser__grow(TFParser *self, uint32_t needed) {
  if (needed <= self->capacity) return true;
  uint32_t capacity = self->capacity ? self->capacity : 64;
  while (capacity < needed) capacity *= 2;
  TSStateId *states = realloc(self->states, (capacity + 1) * sizeof(TSStateId));
  TFNode *nodes = realloc(self->nodes, capacity * sizeof(TFNode));
  if (states) self->states = states;
  if (nodes) self->nodes = nodes;
  if (!states || !nodes) return false;
  self->capacity = capacity;
  return true;
}

static bool tf_parser__push(TFParser *self, TFNode node, TSStateId state) {
  if (!tf_parser__grow(self, self->depth + 1)) return false;
  self->nodes[self->depth++] = node;
  self->states[self->depth] = state;
  return true;
}

static bool tf_parser__shift(TFParser *self, const TFToken *token, bool extra, TSStateId state) {
  TFNode node = {
      .symbol = token->symbol,
      .extra = extra,
      .start_byte = token->start_byte,
      .end_byte = token->end_byte,
      .start_point = token->start_point,
      .end_point = token->end_point,
      .value =
          self->sink->on_shift ? self->sink->on_shift(self->sink->payload, token, extra) : NULL,
  };
  if (extra && self->depth == self->leading) self->leading++;
  return tf_parser__push(self, node, state);
}

static void tf_parser__fail(TFParser *self, uint32_t byte, TFPoint point, const char *format, ...) {
  if (!self->error) return;
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
    if (count == 0) continue;
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

// Hand the held-back root reduction to the sink after all, because something
// other than the end token turned out to follow it.
static void *tf_parser__flush_root(TFParser *self) {
  self->root.pending = false;
  const TFNode *node = &self->nodes[self->root.base];
  TFReduction reduction = {
      .symbol = self->root.symbol,
      .production_id = self->root.production_id,
      .child_count = self->root.child_count,
      .node_count = self->root.node_count,
      .children = self->root.children,
      .start_byte = node->start_byte,
      .end_byte = node->end_byte,
      .start_point = node->start_point,
      .end_point = node->end_point,
  };
  return self->sink->on_reduce ? self->sink->on_reduce(self->sink->payload, &reduction) : NULL;
}

// parser.c:/ts_parser__reduce/, with the GLR bookkeeping removed. Pops
// `child_count` non-extra cells -- carrying along any extras between them --
// hands them to the sink, and pushes the result in their place.
static bool tf_parser__reduce(TFParser *self, TSSymbol symbol, uint32_t child_count,
                              uint16_t production_id) {
  uint32_t popped = 0;
  for (uint32_t structural = 0; structural < child_count;) {
    popped++;
    if (!self->nodes[self->depth - popped].extra) structural++;
  }
  uint32_t base = self->depth - popped;

  // A held-back root turns out not to be the root after all if something reaches
  // down to it.
  if (self->root.pending && base <= self->root.base) {
    self->nodes[self->root.base].value = tf_parser__flush_root(self);
  }

  uint32_t end = self->depth;
  while (end > base && self->nodes[end - 1].extra) end--;
  uint32_t trailing_count = self->depth - end;

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

  // If nothing follows this node but extras and the end of the file, it is the
  // root: keep its children so they can be handed over in one piece once the end
  // token is in hand.
  bool is_root = base == self->leading && self->lang->accepts_end[state];
  if (is_root) {
    if (reduction.node_count > self->root.capacity) {
      TFNode *children = realloc(self->root.children, reduction.node_count * sizeof(TFNode));
      if (!children) return false;
      self->root.children = children;
      self->root.capacity = reduction.node_count;
    }
    // An empty root production has no children and nothing allocated to hold
    // them; glibc declares memcpy non-null, so even a zero-length copy is UB.
    if (reduction.node_count > 0) {
      memcpy(self->root.children, reduction.children, reduction.node_count * sizeof(TFNode));
    }
    self->root.pending = true;
    self->root.base = base;
    self->root.symbol = symbol;
    self->root.production_id = production_id;
    self->root.child_count = child_count;
    self->root.node_count = reduction.node_count;
  }

  TFNode parent = {
      .symbol = symbol,
      .start_byte = reduction.start_byte,
      .end_byte = reduction.end_byte,
      .start_point = reduction.start_point,
      .end_point = reduction.end_point,
      .value = is_root                 ? NULL
               : self->sink->on_reduce ? self->sink->on_reduce(self->sink->payload, &reduction)
                                       : NULL,
  };

  if (trailing_count > 0 && trailing_count > self->trailing_capacity) {
    TFNode *trailing = realloc(self->trailing, trailing_count * sizeof(TFNode));
    if (!trailing) return false;
    self->trailing = trailing;
    self->trailing_capacity = trailing_count;
  }
  // Almost always zero -- a data file is mostly not comments -- and the call is
  // not free at one per reduction.
  if (trailing_count > 0) {
    memcpy(self->trailing, &self->nodes[end], trailing_count * sizeof(TFNode));
  }

  self->depth = base;
  if (!tf_parser__push(self, parent, state)) return false;
  for (uint32_t i = 0; i < trailing_count; i++) {
    if (!tf_parser__push(self, self->trailing[i], state)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Split mode
//
// A genuine multi-action entry means the tables cannot decide with one token of
// lookahead. Rather than call the sink speculatively, the driver forks a set of
// branches that carry parse states and an action log but no values, runs them
// until one survives, and then replays the survivor's log against the real stack
// -- so the sink sees exactly one, correct, sequence of events.
//
// Choosing between branches follows tree-sitter exactly
// (parser.c:/ts_parser__select_tree/): higher dynamic precedence first, then the
// structural comparison below, then the branch that came first.
//
// CONSIDERATION: branches are whole stacks that merge only on a full match,
// where tree-sitter's heads merge on top state and position alone (stack.c:
// /ts_stack_can_merge/) and then share their predecessors through added links.
// One tree-sitter version therefore stands for many stacks, which is the entire
// reason its own limit can be six (parser.c:/Enforce a hard upper bound/, where
// it discards the least promising beyond that). Measured peak here, over 26,594
// files and all three grammars' corpora: 2 for DataZinc's `[| e :`; for MiniZinc
// a ladder of 19, 37, 75, 149, 299 as generator calls nest. Six explicit
// branches parses none of those, so the limit is 4096 -- 13x the worst seen --
// and reaching it is reported as an error, never as a wrong parse.
//
// Two ways to close the gap, deferred until the benchmarks say whether split
// mode costs anything at all on real data: make the stacks and logs persistent
// so clones share their common tail, which turns memory from O(branches x depth)
// into O(divergence); or build the real GSS and inherit the bound directly.
#define TF_MAX_BRANCHES 4096

typedef struct {
  bool is_shift;
  union {
    struct {
      TFToken token;
      bool extra;
      TSStateId state;
    } shift;
    struct {
      TSSymbol symbol;
      uint16_t production_id;
      uint32_t child_count;
      // Enough to rebuild the shape of the tree from the log alone: how many
      // cells the reduction actually took (its children, extras between them
      // included) and how many extras were left above it.
      uint32_t node_count;
      uint32_t trailing;
    } reduce;
  };
} TFLogEntry;

typedef struct {
  // States and extra flags only: a branch needs to know how far a pop reaches,
  // not what anything means. states[0] is the state at the fork's base;
  // extra[i] says whether the cell that produced states[i] was an extra.
  TSStateId *states;
  uint8_t *extra;
  uint32_t depth, capacity;
  TFLexer lexer;
  TFToken token;  // the token this branch is working on
  TFLogEntry *log;
  uint32_t log_length, log_capacity;
  // subtree.c:407 plus parser.c:/parent.ptr->dynamic_precedence/: a tree's dynamic
  // precedence is the sum over its reductions. Branches share everything below
  // the fork, so the sum since the fork is what distinguishes them.
  int64_t precedence;
  uint32_t forced;  // action index to take at the next conflict, then cleared
  bool has_forced;
  bool alive, accepted;
  uint32_t error_byte;
  TFPoint error_point;
  TSStateId error_state;
  TSSymbol error_symbol;
  bool error_is_lex;
} TFBranch;

static bool tf_branch__reserve(TFBranch *self, uint32_t needed) {
  // `capacity` starts at zero with nothing allocated, and a branch forked at the
  // very bottom of the stack needs zero *more* than that -- but still needs the
  // one cell. Checking the pointer as well as the capacity keeps that case from
  // returning success with nothing to write into.
  if (self->states != NULL && needed <= self->capacity) return true;
  uint32_t capacity = self->capacity ? self->capacity : 64;
  while (capacity < needed) capacity *= 2;
  TSStateId *states = realloc(self->states, (capacity + 1) * sizeof(TSStateId));
  if (states) self->states = states;
  uint8_t *extra = realloc(self->extra, (capacity + 1) * sizeof(uint8_t));
  if (extra) self->extra = extra;
  if (!states || !extra) return false;
  self->capacity = capacity;
  return true;
}

static bool tf_branch__log(TFBranch *self, TFLogEntry entry) {
  if (self->log_length == self->log_capacity) {
    uint32_t capacity = self->log_capacity ? self->log_capacity * 2 : 64;
    TFLogEntry *log = realloc(self->log, capacity * sizeof(TFLogEntry));
    if (!log) return false;
    self->log = log;
    self->log_capacity = capacity;
  }
  self->log[self->log_length++] = entry;
  return true;
}

static void tf_branch__die(TFBranch *self, bool is_lex) {
  self->alive = false;
  self->error_is_lex = is_lex;
  self->error_byte = is_lex ? self->lexer.byte : self->token.start_byte;
  self->error_point = is_lex ? self->lexer.point : self->token.start_point;
  self->error_state = self->states[self->depth];
  self->error_symbol = self->token.symbol;
}

static void tf_branch__free(TFBranch *self) {
  free(self->states);
  free(self->extra);
  free(self->log);
}

// Release a branch's memory and free its slot for reuse. Dead branches must not
// hold slots: the branch limit is meant to bound how many possibilities are live
// at once, not how many have been tried since the fork.
static void tf_branch__retire(TFBranch *self) {
  tf_branch__free(self);
  *self = (TFBranch){0};
}

static bool tf_branch__clone(TFBranch *out, const TFBranch *self) {
  *out = *self;
  out->states = NULL;
  out->extra = NULL;
  out->capacity = 0;
  out->log = NULL;
  out->log_capacity = 0;
  if (!tf_branch__reserve(out, self->depth)) return false;
  memcpy(out->states, self->states, (self->depth + 1) * sizeof(TSStateId));
  memcpy(out->extra, self->extra, (self->depth + 1) * sizeof(uint8_t));
  if (self->log_length) {
    out->log = malloc(self->log_length * sizeof(TFLogEntry));
    if (!out->log) return false;
    memcpy(out->log, self->log, self->log_length * sizeof(TFLogEntry));
    out->log_capacity = self->log_length;
  }
  return true;
}

typedef enum { TFStepShifted, TFStepDead, TFStepAccepted, TFStepSplit, TFStepFailed } TFStep;

// parser.c:1722-1747. A word the keyword lexer reclassified can still turn out
// to be an identifier after all.
//
// The substitution was judged against the state the token was *lexed* in, using
// that state's larger token set. Reductions since then may have arrived at a
// state where the keyword is not valid and is not reserved, but the word token
// is -- a field or a label named after a keyword, in a grammar that allows it.
// tree-sitter demotes the token there rather than failing, so this does too.
//
// Reached only where the parse was otherwise about to stop, so it costs nothing
// on the way through.
static bool tf_parser__demote_keyword(const TFLanguage *lang, TSStateId state, TFToken *token,
                                      bool was_keyword) {
  const TSLanguage *ts = lang->ts;
  if (!was_keyword || token->symbol == ts->keyword_capture_token) return false;
  if (tf_is_reserved_word(lang, state, token->symbol)) return false;
  uint32_t count;
  tf_actions(lang, state, ts->keyword_capture_token, &count);
  if (count == 0) return false;
  token->symbol = ts->keyword_capture_token;
  return true;
}

// Run one branch through its current token, up to and including the shift that
// consumes it. Returns TFStepSplit with the branch untouched when the token
// hits a conflict and the caller must clone.
static TFStep tf_branch__step(TFBranch *self, const TFLanguage *lang, uint32_t *action_count) {
  for (;;) {
    TSStateId state = self->states[self->depth];
    uint32_t count;
    const TSParseAction *actions = tf_actions(lang, state, self->token.symbol, &count);
    if (count == 0) {
      if (tf_parser__demote_keyword(lang, state, &self->token, self->lexer.token_is_keyword)) {
        continue;
      }
      tf_branch__die(self, false);
      return TFStepDead;
    }
    uint32_t index = 0;
    if (count > 1) {
      if (!self->has_forced) {
        *action_count = count;
        return TFStepSplit;
      }
      index = self->forced;
      self->has_forced = false;
    }

    TSParseAction action = actions[index];
    if (action.type == TSParseActionTypeShift) {
      TSStateId next = action.shift.extra ? state : action.shift.state;
      if (!tf_branch__reserve(self, self->depth + 1) ||
          !tf_branch__log(self, (TFLogEntry){.is_shift = true,
                                             .shift = {self->token, action.shift.extra, next}})) {
        return TFStepFailed;
      }
      self->depth++;
      self->states[self->depth] = next;
      self->extra[self->depth] = action.shift.extra;
      if (!tf_lexer_next(&self->lexer, next, &self->token)) {
        tf_branch__die(self, true);
        return TFStepDead;
      }
      return TFStepShifted;
    }

    if (action.type == TSParseActionTypeReduce) {
      // The same pop as the real reduce: `child_count` non-extra cells, plus any
      // extras between them, with the extras above the last child left on top.
      uint32_t popped = 0;
      for (uint32_t structural = 0; structural < action.reduce.child_count;) {
        popped++;
        if (popped > self->depth) {
          tf_branch__die(self, false);
          return TFStepDead;
        }
        if (!self->extra[self->depth - popped + 1]) structural++;
      }
      uint32_t base = self->depth - popped;
      uint32_t end = self->depth;
      while (end > base && self->extra[end]) end--;
      uint32_t trailing = self->depth - end;

      self->precedence += action.reduce.dynamic_precedence;
      if (!tf_branch__reserve(self, base + 1 + trailing) ||
          !tf_branch__log(self,
                          (TFLogEntry){.reduce = {.symbol = action.reduce.symbol,
                                                  .production_id = action.reduce.production_id,
                                                  .child_count = action.reduce.child_count,
                                                  .node_count = end - base,
                                                  .trailing = trailing}})) {
        return TFStepFailed;
      }
      TSStateId next = tf_next_state(lang, self->states[base], action.reduce.symbol);
      self->depth = base + 1;
      self->states[self->depth] = next;
      self->extra[self->depth] = false;
      for (uint32_t i = 0; i < trailing; i++) {
        self->depth++;
        self->states[self->depth] = next;
        self->extra[self->depth] = true;
      }
      continue;
    }

    if (action.type == TSParseActionTypeAccept) {
      self->accepted = true;
      return TFStepAccepted;
    }

    tf_branch__die(self, false);
    return TFStepDead;
  }
}

// ---------------------------------------------------------------------------
// Structural comparison
//
// When two converged branches have equal dynamic precedence, tree-sitter falls
// through to comparing the two candidate subtrees outright
// (parser.c:/ts_subtree_compare/): a preorder walk taking the smaller symbol,
// then the smaller child count, first difference wins. Reproducing that needs
// the shape of what each branch built, which its log is enough to rebuild -- so
// it is rebuilt here, on the rare occasions it is asked for, rather than
// maintained on every step.
#define TF_SHAPE_INHERITED 0x80000000u

typedef struct {
  TSSymbol symbol;
  uint32_t child_count;
  uint32_t first_child;
} TFShape;

typedef struct {
  TFShape *shapes;
  uint32_t shape_count, shape_capacity;
  uint32_t *links;  // children, in source order, run per shape
  uint32_t link_count, link_capacity;
  uint32_t *cells;                     // shape per stack cell; TF_SHAPE_INHERITED marks one from
  uint32_t cell_count, cell_capacity;  // before the fork, which both branches share
  bool failed;
} TFForest;

static uint32_t *tf_forest__grow(uint32_t **array, uint32_t *capacity, uint32_t needed,
                                 size_t item) {
  if (needed <= *capacity) return *array;
  uint32_t next = *capacity ? *capacity : 64;
  while (next < needed) next *= 2;
  void *grown = realloc(*array, next * item);
  if (!grown) return NULL;
  *array = grown;
  *capacity = next;
  return *array;
}

static void tf_forest__free(TFForest *self) {
  free(self->shapes);
  free(self->links);
  free(self->cells);
}

static void tf_forest__push_cell(TFForest *self, uint32_t shape) {
  if (!tf_forest__grow(&self->cells, &self->cell_capacity, self->cell_count + 1,
                       sizeof(uint32_t))) {
    self->failed = true;
    return;
  }
  self->cells[self->cell_count++] = shape;
}

// Replay a branch's log to recover what it built. Cells below the fork are the
// same objects in both branches, so they are left opaque.
static void tf_forest__build(TFForest *self, const TFBranch *branch, uint32_t base_depth) {
  for (uint32_t i = 0; i < base_depth; i++) tf_forest__push_cell(self, TF_SHAPE_INHERITED | i);

  for (uint32_t i = 0; i < branch->log_length && !self->failed; i++) {
    TFLogEntry entry = branch->log[i];
    if (!tf_forest__grow((uint32_t **)&self->shapes, &self->shape_capacity, self->shape_count + 1,
                         sizeof(TFShape))) {
      self->failed = true;
      return;
    }
    uint32_t id = self->shape_count;

    if (entry.is_shift) {
      self->shapes[self->shape_count++] = (TFShape){entry.shift.token.symbol, 0, 0};
      tf_forest__push_cell(self, id);
      continue;
    }

    uint32_t trailing = entry.reduce.trailing, taken = entry.reduce.node_count;
    if (trailing + taken > self->cell_count) {
      self->failed = true;
      return;
    }
    self->cell_count -= trailing;
    uint32_t saved = self->cell_count;  // trailing cells sit just above, untouched

    if (!tf_forest__grow(&self->links, &self->link_capacity, self->link_count + taken,
                         sizeof(uint32_t))) {
      self->failed = true;
      return;
    }
    uint32_t first_child = self->link_count;
    // An empty production takes nothing, and `cells` may not be allocated at all
    // yet; memcpy from a null pointer is undefined even for zero bytes. Taking
    // anything at all means `cell_count` was incremented, which only happens
    // after `tf_forest__push_cell` allocates -- but the analyser cannot carry
    // that here. Which of the two checks fires depends on the platform: glibc
    // declares memcpy non-null and other libcs do not, so this is reported on
    // Linux and not on macOS.
    if (taken > 0) {
      // NOLINTNEXTLINE(clang-analyzer-unix.cstring.NullArg,clang-analyzer-core.NonNullParamChecker)
      memcpy(&self->links[first_child], &self->cells[saved - taken], taken * sizeof(uint32_t));
    }
    self->link_count += taken;
    self->cell_count = saved - taken;

    self->shapes[self->shape_count++] = (TFShape){entry.reduce.symbol, taken, first_child};
    tf_forest__push_cell(self, id);
    for (uint32_t t = 0; t < trailing; t++) tf_forest__push_cell(self, self->cells[saved + t]);
  }
}

// subtree.c:/ts_subtree_compare/, over the cells each branch has on its stack.
// Returns -1 if `a` sorts first, 1 if `b` does, 0 if they are indistinguishable.
// The symbol of a cell, whether it was built after the fork or inherited from
// before it. `nodes` is the parser's own stack, which the branches have not
// touched, so an inherited index still names a real node.
static TSSymbol tf_forest__symbol(const TFForest *f, uint32_t cell, const TFNode *nodes) {
  return cell >= TF_SHAPE_INHERITED ? nodes[cell & ~TF_SHAPE_INHERITED].symbol
                                    : f->shapes[cell].symbol;
}

static int tf_forest__compare(const TFForest *a, const TFForest *b, const TFNode *nodes) {
  if (a->failed || b->failed) return 0;
  uint32_t count = a->cell_count < b->cell_count ? a->cell_count : b->cell_count;
  uint32_t *stack = NULL, length = 0, capacity = 0;
  int result = 0;

  for (uint32_t cell = 0; cell < count && result == 0; cell++) {
    length = 0;
    if (!tf_forest__grow(&stack, &capacity, 2, sizeof(uint32_t))) break;
    stack[length++] = a->cells[cell];
    stack[length++] = b->cells[cell];

    while (length > 0) {
      uint32_t right = stack[--length], left = stack[--length];
      bool left_old = left >= TF_SHAPE_INHERITED, right_old = right >= TF_SHAPE_INHERITED;

      // The same cell from before the fork on both sides: literally the same
      // node, nothing to tell apart.
      if (left_old && right_old && left == right) continue;

      // Otherwise they still have to be compared. One branch having built a node
      // *around* a cell the other left alone is exactly the difference that
      // decides which derivation wins, and skipping it here reported the two as
      // indistinguishable -- which handed the choice to whichever branch
      // happened to come first.
      TSSymbol ls = tf_forest__symbol(a, left, nodes);
      TSSymbol rs = tf_forest__symbol(b, right, nodes);
      if (ls != rs) {
        result = ls < rs ? -1 : 1;
        break;
      }
      // Same symbol, but at least one side is opaque: there is no recorded shape
      // to recurse into, and both sides agree so far.
      if (left_old || right_old) continue;

      const TFShape *x = &a->shapes[left], *y = &b->shapes[right];
      if (x->child_count != y->child_count) {
        result = x->child_count < y->child_count ? -1 : 1;
        break;
      }
      if (!tf_forest__grow(&stack, &capacity, length + 2 * x->child_count, sizeof(uint32_t))) {
        result = 0;
        break;
      }
      // Pushed last child first, so the first child is examined first.
      for (uint32_t i = x->child_count; i > 0; i--) {
        stack[length++] = a->links[x->first_child + i - 1];
        stack[length++] = b->links[y->first_child + i - 1];
      }
    }
  }

  free(stack);
  return result;
}

// Which of two converged branches tree-sitter would keep: higher dynamic
// precedence, then the structural comparison, then the one that came first
// (parser.c:/ts_parser__select_tree/, whose final case is "select_existing").
static bool tf_branch__prefer_second(const TFBranch *first, const TFBranch *second,
                                     uint32_t base_depth, const TFNode *nodes) {
  if (second->precedence != first->precedence) return second->precedence > first->precedence;
  TFForest a = {0}, b = {0};
  tf_forest__build(&a, first, base_depth);
  tf_forest__build(&b, second, base_depth);
  bool prefer = tf_forest__compare(&a, &b, nodes) > 0;
  tf_forest__free(&a);
  tf_forest__free(&b);
  return prefer;
}

// Fork on `token`, run the branches until one is left, and replay its actions
// against the real stack. On return the parser has consumed at least `token` and
// `*next` holds the token to carry on with.
// Two branches that reached the same state stack at the same position behave
// identically from here on, so only the better one need continue. This is what
// stops an ambiguity that is never resolved -- a `.mzn` generator call, say --
// from running to the end of the file with the whole action log in memory.
//
// CONSIDERATION: the comparison is a memcmp over the whole state stack, on the
// theory that split mode is rare. If a grammar forks deep inside a large file
// often enough for that to show, hash the stack incrementally instead.
static bool tf_branch__same(const TFBranch *a, const TFBranch *b) {
  return a->depth == b->depth && a->lexer.byte == b->lexer.byte &&
         a->token.symbol == b->token.symbol && a->token.start_byte == b->token.start_byte &&
         memcmp(a->states, b->states, (a->depth + 1) * sizeof(TSStateId)) == 0 &&
         memcmp(a->extra, b->extra, (a->depth + 1) * sizeof(uint8_t)) == 0;
}

typedef struct {
  bool present;
  uint32_t byte;
  TFPoint point;
  TSStateId state;
  TSSymbol symbol;
  bool is_lex;
} TFDeath;

// Keep the death that got furthest through the input: that branch made sense of
// the most of it, so its complaint is the useful one.
static void tf_death__keep_furthest(TFDeath *self, const TFBranch *branch) {
  if (self->present && branch->error_byte <= self->byte) return;
  *self = (TFDeath){true,
                    branch->error_byte,
                    branch->error_point,
                    branch->error_state,
                    branch->error_symbol,
                    branch->error_is_lex};
}

static bool tf_parser__split(TFParser *self, TFToken token, TFToken *next) {
  uint32_t capacity = 8;
  TFBranch *branches = calloc(capacity, sizeof(TFBranch));
  if (!branches) {
    tf_parser__fail(self, token.start_byte, token.start_point, "out of memory");
    return false;
  }
  uint32_t count = 1;
  uint32_t live = 1;
  uint32_t peak = 1;
  uint32_t base_depth = self->depth;
  TFDeath death = {0};
  bool ok = false;

  branches[0] = (TFBranch){.lexer = self->lexer, .token = token, .alive = true};
  // Both of the branch's arrays have to have come through: one can be reallocated
  // while the other fails, which leaves `states` looking fine and `extra` null.
  if (!tf_branch__reserve(&branches[0], self->depth)) {
    tf_parser__fail(self, token.start_byte, token.start_point, "out of memory");
    free(branches);
    return false;
  }
  memcpy(branches[0].states, self->states, (self->depth + 1) * sizeof(TSStateId));
  for (uint32_t i = 0; i <= self->depth; i++) {
    branches[0].extra[i] = i > 0 && self->nodes[i - 1].extra;
  }
  branches[0].depth = self->depth;

  uint32_t running = 1;
  while (running > 0) {
    // Advance in lockstep: only the branches sitting on the earliest token move.
    // Branches at different positions can never be seen to converge, and it is
    // convergence that keeps nested forks from multiplying.
    uint32_t front = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
      if (branches[i].alive && !branches[i].accepted && branches[i].token.start_byte < front) {
        front = branches[i].token.start_byte;
      }
    }

    for (uint32_t i = 0; i < count; i++) {
      if (!branches[i].alive || branches[i].accepted) continue;
      if (branches[i].token.start_byte != front) continue;
      uint32_t actions = 0;
      TFStep step = tf_branch__step(&branches[i], self->lang, &actions);

      if (step == TFStepSplit) {
        if (live + actions - 1 > TF_MAX_BRANCHES) {
          tf_parser__fail(self, branches[i].token.start_byte, branches[i].token.start_point,
                          "conflict needs more than %u branches to resolve",
                          (unsigned)TF_MAX_BRANCHES);
          goto cleanup;
        }
        while (count + actions - 1 > capacity) {
          uint32_t next = capacity * 2;
          TFBranch *grown = realloc(branches, next * sizeof(TFBranch));
          if (!grown) {
            tf_parser__fail(self, token.start_byte, token.start_point, "out of memory");
            goto cleanup;
          }
          memset(grown + capacity, 0, (next - capacity) * sizeof(TFBranch));
          branches = grown;
          capacity = next;
        }
        for (uint32_t a = 1; a < actions; a++) {
          uint32_t slot = 0;
          while (slot < count && branches[slot].alive) slot++;
          if (slot == count) count++;
          if (!tf_branch__clone(&branches[slot], &branches[i])) {
            tf_parser__fail(self, token.start_byte, token.start_point, "out of memory");
            goto cleanup;
          }
          branches[slot].forced = a;
          branches[slot].has_forced = true;
          running++;
          live++;
        }
        if (live > peak) peak = live;
        branches[i].forced = 0;
        branches[i].has_forced = true;
        i--;  // re-run this branch, now with its action chosen
        continue;
      }

      if (step == TFStepFailed) {
        tf_parser__fail(self, token.start_byte, token.start_point, "out of memory");
        goto cleanup;
      }
      if (step == TFStepDead) {
        running--;
        live--;
        tf_death__keep_furthest(&death, &branches[i]);
        tf_branch__retire(&branches[i]);
      } else if (step == TFStepAccepted) {
        running--;
      }
    }

    // Collapse branches that have converged, keeping the higher precedence.
    for (uint32_t i = 0; i < count; i++) {
      if (!branches[i].alive || branches[i].accepted) continue;
      for (uint32_t j = i + 1; j < count; j++) {
        if (!branches[j].alive || branches[j].accepted) continue;
        if (!tf_branch__same(&branches[i], &branches[j])) continue;
        if (tf_branch__prefer_second(&branches[i], &branches[j], base_depth, self->nodes)) {
          TFBranch swap = branches[i];
          branches[i] = branches[j];
          branches[j] = swap;
        }
        tf_branch__retire(&branches[j]);
        running--;
        live--;
      }
    }

    // One possibility left and nothing already finished to compare it against:
    // it must be the answer, so stop rather than run it to the end of the file.
    if (running == 1) {
      unsigned finished = 0;
      for (uint32_t i = 0; i < count; i++) finished += branches[i].alive && branches[i].accepted;
      if (finished == 0) break;
    }
  }

  TFBranch *winner = NULL;
  for (uint32_t i = 0; i < count; i++) {
    if (!branches[i].alive) continue;
    // parser.c:/ts_parser__select_tree/: with no errors on either side, the
    // higher dynamic precedence wins.
    if (!winner || branches[i].precedence > winner->precedence) winner = &branches[i];
  }

  if (!winner) {
    if (death.is_lex || !death.present) {
      tf_parser__fail(self, death.byte, death.point, "unexpected character");
    } else {
      TFToken bad = {.symbol = death.symbol, .start_byte = death.byte, .start_point = death.point};
      tf_parser__fail_unexpected(self, death.state, &bad);
    }
    goto cleanup;
  }

  // Opt-in diagnostics: split mode is meant to be invisible, and this is how to
  // check that on a new grammar or corpus.
  if (getenv("TF_SPLIT_STATS")) {
    fprintf(stderr, "split: byte=%u peak-live=%u slots=%u log=%u precedence=%lld\n",
            token.start_byte, peak, count, winner->log_length, (long long)winner->precedence);
  }

  // Replay. The states the branch computed are rebuilt from scratch by the real
  // shift and reduce, so only the actions need to be repeated.
  for (uint32_t i = 0; i < winner->log_length; i++) {
    TFLogEntry entry = winner->log[i];
    bool applied =
        entry.is_shift
            ? tf_parser__shift(self, &entry.shift.token, entry.shift.extra, entry.shift.state)
            : tf_parser__reduce(self, entry.reduce.symbol, entry.reduce.child_count,
                                entry.reduce.production_id);
    if (!applied) {
      tf_parser__fail(self, token.start_byte, token.start_point, "out of memory");
      goto cleanup;
    }
  }
  self->lexer = winner->lexer;
  *next = winner->token;
  ok = true;

cleanup:
  for (uint32_t i = 0; i < count; i++) tf_branch__free(&branches[i]);
  free(branches);
  return ok;
}

bool tf_parse(const TFLanguage *lang, const void *source, uint32_t size, const TFSink *sink,
              void **root, TFError *error) {
  static const TFSink no_sink = {0};
  TFParser self = {.lang = lang, .sink = sink ? sink : &no_sink, .error = error};
  if (error) *error = (TFError){0};
  tf_lexer_init(&self.lexer, lang, source, size);
  bool ok = false;

  if (!tf_parser__grow(&self, 64)) {
    tf_parser__fail(&self, 0, (TFPoint){0, 0}, "out of memory");
    goto done;
  }
  // State 0 is ERROR_STATE (error_costs.h:4); parsing starts at state 1
  // (stack.c:/ts_stack_new/, which seeds the base node with state 1).
  self.states[0] = 1;

  for (;;) {
    TSStateId state = self.states[self.depth];
    TFToken token;
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
        if (!tf_parser__split(&self, token, &token)) goto done;
        state = self.states[self.depth];
        continue;
      }

      TSParseAction action = actions[0];
      if (action.type == TSParseActionTypeShift) {
        // An extra does not change the state (parser.c:1633).
        if (!tf_parser__shift(&self, &token, action.shift.extra,
                              action.shift.extra ? state : action.shift.state)) {
          tf_parser__fail(&self, token.start_byte, token.start_point, "out of memory");
          goto done;
        }
        break;
      }

      if (action.type == TSParseActionTypeReduce) {
        if (!tf_parser__reduce(&self, action.reduce.symbol, action.reduce.child_count,
                               action.reduce.production_id)) {
          tf_parser__fail(&self, token.start_byte, token.start_point, "out of memory");
          goto done;
        }
        state = self.states[self.depth];
        continue;
      }

      if (action.type == TSParseActionTypeAccept) {
        void *value = self.depth ? self.nodes[self.root.base].value : NULL;
        if (self.root.pending) {
          // The end token joins the root as a trailing extra, along with any
          // extras that were sitting above it (parser.c:/ts_parser__accept/).
          uint32_t below = self.root.base;
          uint32_t above = self.depth - self.root.base - 1;
          uint32_t total = below + self.root.node_count + above + 1;
          TFNode *children = malloc(total * sizeof(TFNode));
          if (!children) {
            tf_parser__fail(&self, token.start_byte, token.start_point, "out of memory");
            goto done;
          }
          if (below > 0) memcpy(children, self.nodes, below * sizeof(TFNode));
          // A root with an empty production never allocated a child array.
          if (self.root.node_count > 0) {
            memcpy(children + below, self.root.children, self.root.node_count * sizeof(TFNode));
          }
          memcpy(children + below + self.root.node_count, &self.nodes[self.root.base + 1],
                 above * sizeof(TFNode));
          children[total - 1] = (TFNode){
              .symbol = token.symbol,
              .extra = true,
              .start_byte = token.start_byte,
              .end_byte = token.end_byte,
              .start_point = token.start_point,
              .end_point = token.end_point,
              .value = self.sink->on_shift ? self.sink->on_shift(self.sink->payload, &token, true)
                                           : NULL,
          };
          TFReduction reduction = {
              .symbol = self.root.symbol,
              .production_id = self.root.production_id,
              .child_count = self.root.child_count,
              .node_count = total,
              .children = children,
              .start_byte = total > 1 ? children[0].start_byte : token.start_byte,
              .start_point = total > 1 ? children[0].start_point : token.start_point,
              .end_byte = token.end_byte,
              .end_point = token.end_point,
          };
          value =
              self.sink->on_reduce ? self.sink->on_reduce(self.sink->payload, &reduction) : NULL;
          free(children);
        }
        if (root) *root = value;
        ok = true;
        goto done;
      }

      // TSParseActionTypeRecover: error recovery, which tree-feller does not do.
      tf_parser__fail_unexpected(&self, state, &token);
      goto done;
    }
  }

done:
  free(self.states);
  free(self.nodes);
  free(self.trailing);
  free(self.root.children);
  return ok;
}
