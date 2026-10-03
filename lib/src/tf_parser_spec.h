/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Max Brunsfeld
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// Value-free graph-structured stacks for speculative parsing, and the error
// recovery that runs on them. Included by tf_parser.c so the ordinary
// shift/reduce path and its sink remain private.
// The algorithms follow tree-sitter 0.27's stack.c and parser.c; incremental
// reuse and external scanners are deliberately absent.
#ifndef TF_PARSER_SPEC_H
#define TF_PARSER_SPEC_H

#define TF_SPEC_NONE UINT32_MAX
#define TF_SPEC_LINKS 8
#define TF_SPEC_ITERATORS 64
#define TF_SPEC_VERSIONS 6

// error_costs.h, verbatim. ERROR_STATE is state 0, which tf_parser.c already
// relies on when it seeds the stack at state 1.
#define ERROR_STATE 0
#define ERROR_COST_PER_RECOVERY 500
#define ERROR_COST_PER_MISSING_TREE 110
#define ERROR_COST_PER_SKIPPED_TREE 100
#define ERROR_COST_PER_SKIPPED_LINE 30
#define ERROR_COST_PER_SKIPPED_CHAR 1

// parser.c:79-80. A version is dropped outright rather than merely ranked
// below another once the cost gap, scaled by how much it has parsed, passes
// this. MAX_SUMMARY_DEPTH bounds the states recovery will rewind to.
#define TF_SPEC_MAX_COST_DIFFERENCE (18 * ERROR_COST_PER_SKIPPED_TREE)
#define TF_SPEC_MAX_SUMMARY_DEPTH 16

// For tf_spec__replay; see there.
#if defined(__GNUC__) || defined(__clang__)
#define TF_NOINLINE __attribute__((noinline))
#else
#define TF_NOINLINE
#endif

// Ordered so the small fields fill the hole ahead of `precedence`: 56 bytes
// rather than 64, over an arena that is one entry per speculative shift and
// reduction. `structural_count` is a production's child count, which the ABI
// itself stores in a uint8_t. Error bookkeeping is deliberately NOT here: see
// TFSpecTreeCost below.
typedef struct {
  TFToken token;
  uint32_t padding_start;
  uint32_t first_child, child_count;
  uint16_t production_id;
  uint8_t structural_count;
  bool leaf, extra, inherited, opaque;
  // tree-sitter's NULL subtree: the link handle_error pushes to mark where
  // recovery began (parser.c:/NULL_SUBTREE, false, ERROR_STATE/). It counts
  // toward a pop's goal like any non-extra tree (stack.c:/next_iterator->subtree_count++/)
  // but is never anyone's child; the recovery code that pops across it drops it.
  bool discontinuity;
  int64_t precedence;
} TFSpecTree;

// The arena holds one of these per speculative shift and reduction, so its size
// is the memory a split costs. Asserted because the packing above is deliberate
// and a stray field would undo it silently.
_Static_assert(sizeof(TFSpecTree) == 56, "TFSpecTree grew past its 56-byte layout");

typedef struct {
  uint32_t node, tree;
} TFSpecLink;
typedef struct {
  TSStateId state;
  uint32_t byte;
  TFPoint point;
  int64_t precedence;
  // Links live in a shared arena: a node almost always has exactly one, and an
  // inline array of eight would triple the size of every node in the graph.
  uint32_t first_link;
  uint8_t link_count, link_capacity;
} TFSpecNode;
// stack.c:/StackStatus/. A paused version is one that could not advance and is
// waiting for recovery; an active one is neither paused nor halted.
typedef enum {
  TF_SPEC_ACTIVE,
  TF_SPEC_PAUSED,
  TF_SPEC_HALTED,
} TFSpecStatus;

_Static_assert(sizeof(TFSpecNode) == 32, "TFSpecNode grew past its 32-byte layout");

// Kept small. tf_spec__pop makes a head per slice, so a head is written once per
// speculative reduction: a 44-byte head cost 1.5% on a conflict-heavy corpus,
// and even 16 bytes measured 1%. What only recovery needs is in TFSpecRecord.
typedef struct {
  uint32_t node;
  uint8_t status;  // a TFSpecStatus, narrowed so the head stays small
  bool errored;
} TFSpecHead;

_Static_assert(sizeof(TFSpecHead) == 8, "TFSpecHead grew past its 8-byte layout");

// The rest of stack.c:/StackHead/, in TFSpec::records beside `heads` and moved
// with it, only in a split that can recover.
//
// The summary is the states recovery may return to: a run of `summary_count`
// entries from `summary_first` in TFSpec::summaries. A paused head has none
// until it resumes, and `summary_first` then indexes TFSpec::paused instead,
// the lookahead tree-sitter keeps on a paused head (stack.c:/ts_stack_pause/).
// Lexing it again on resume is not the same: a keyword is judged against the
// state that lexes it, and the head has reduced since.
typedef struct {
  // stack.c:/node_count_at_last_error/, the baseline for
  // `ts_stack_node_count_since_error`.
  uint32_t node_count_at_error;
  uint32_t summary_first;
  uint32_t summary_count;
} TFSpecRecord;

// Per graph node, what tree-sitter keeps on a StackNode for error recovery
// (stack.c:/struct StackNode/). Held beside the nodes rather than in them: a
// TFSpecNode is a deliberate 32 bytes with only four to spare, and a parse that
// never recovers should not carry this at all.
typedef struct {
  uint32_t error_cost;
  uint32_t node_count;
} TFSpecCost;

// What a speculative tree needs for error recovery: its own cost
// (subtree.h:/ts_subtree_error_cost/) and the two counts `TFNode` carries for a
// real stack cell. Held beside the arena rather than in it. TFSpecTree is a
// deliberate 56 bytes, and packing these into it made `tf_spec__pop` read a
// masked bitfield per link, in the innermost speculative loop, for fields that
// only matter once a split recovers.
typedef struct {
  uint32_t error_cost;
  uint32_t visible_child;
  uint32_t visible_descendant;
  // Whether the tree has children; an inherited tree's child list is unknown.
  bool has_children;
} TFSpecTreeCost;

// stack.c:/StackSummaryEntry/.
typedef struct {
  uint32_t byte, row, depth;
  TSStateId state;
} TFSpecSummary;
typedef struct {
  uint32_t tree, previous;
} TFSpecEdge;
typedef struct {
  uint32_t node, edge, count, length;
} TFSpecIterator;
typedef struct {
  uint32_t version, first, count;
} TFSpecSlice;

struct TFSpec {
  TFSpecTree *trees;
  uint32_t tree_count, tree_capacity;
  TFSpecNode *nodes;
  uint32_t node_count, node_capacity;
  TFSpecLink *links;
  uint32_t link_count, link_capacity;
  TFSpecHead *heads;
  uint32_t head_count, head_capacity;
  TFSpecRecord *records;  // capacity with `capture_capacity`, below
  uint32_t *children;
  uint32_t child_count, child_capacity;
  TFSpecEdge *edges;
  uint32_t edge_count, edge_capacity;
  TFSpecSlice *slices;
  uint32_t slice_count, slice_capacity;
  uint32_t *scratch;
  uint32_t scratch_count, scratch_capacity;
  // The inherited prefix, materialized on demand: prefix_tree[k] is the tree for
  // real stack cell prefix_depth-1-k, and capture_id[i] is cell i's shape once a
  // replay has recovered it.
  uint32_t *prefix_tree;
  uint32_t prefix_count, prefix_tree_capacity;
  uint32_t *capture_id;
  uint32_t capture_capacity, record_capacity;
  // Parallel to `nodes` and to `trees`, both allocated the first time a split
  // recovers. NULL means every cost and count is zero, which is exactly true
  // until then.
  TFSpecCost *node_cost;
  TFSpecTreeCost *tree_cost;
  uint32_t node_cost_capacity, tree_cost_capacity;
  TFSpecSummary *summaries;
  uint32_t summary_count, summary_capacity;
  // The lookahead each paused head stopped on, by TFSpecRecord::summary_first.
  TFToken *paused;
  uint32_t paused_count, paused_capacity;
  // Set for the whole of a split in a parse that can recover: every tree and
  // node then carries its cost. `recovering` is set once this split has pushed
  // a discontinuity, and `stopped` once the consumer declined to recover.
  bool recover, recovering, stopped;
  // parser.c:/accept_count/, which caps how many trees recovery may finish.
  uint32_t accept_count;
  // The rejected token and state of a head that paused, for the message a
  // declined recovery reports: the one a NULL `on_error` would have given.
  TFToken stop_token;
  TSStateId stop_state;
  uint32_t stop_byte;
  TFPoint stop_point;
  bool stop_lex;
  uint32_t frontier, prefix_left, prefix_depth;
  bool failed, materialized, captured;
  TFParser *owner;
  uint32_t fork_byte;
  TFToken cached;
  uint32_t cached_byte;
  TSStateId cached_state;
  bool has_cache, cached_keyword;
  // The first head starts with the token the ordinary parser already holds, as
  // a tree-sitter version keeps its lookahead across the reductions before it.
  bool in_hand;
  uint32_t finished;
  uint32_t error_byte;
  TFPoint error_point;
  TSStateId error_state;
  TSSymbol error_symbol;
  bool has_error, error_is_lex;
};

static bool tf_spec__reserve(TFSpec *s, void **array, uint32_t *capacity, uint32_t needed,
                             size_t size) {
  if (s->failed) {
    return false;
  }
  if (needed <= *capacity) {
    return true;
  }
  uint32_t next = *capacity ? *capacity : 64;
  while (next < needed) {
    if (next > UINT32_MAX / 2) {
      s->failed = true;
      return false;
    }
    next *= 2;
  }
  if (next > SIZE_MAX / size) {
    s->failed = true;
    return false;
  }
  void *p = realloc(*array, next * size);
  if (!p) {
    s->failed = true;
    return false;
  }
  *array = p;
  *capacity = next;
  return true;
}
#define TF_SPEC_RESERVE(s, array, cap, n) \
  tf_spec__reserve(s, (void **)&(s)->array, &(s)->cap, n, sizeof(*(s)->array))

static void tf_spec__free(TFSpec *s) {
  if (!s) {
    return;
  }
  free(s->trees);
  free(s->nodes);
  free(s->links);
  free(s->heads);
  free(s->records);
  free(s->children);
  free(s->edges);
  free(s->slices);
  free(s->scratch);
  free(s->prefix_tree);
  free(s->capture_id);
  free(s->node_cost);
  free(s->tree_cost);
  free(s->summaries);
  free(s->paused);
  free(s);
}

// ---------------------------------------------------------------------------
// Error bookkeeping. Every reader below answers zero while `node_cost` is NULL,
// which is the whole of an error-free parse, so the comparisons that use them
// decide exactly what they decided before recovery existed.

// stack.c:/ts_stack_error_cost/. A version waiting for recovery is charged for
// the recovery it is about to need.
static uint32_t tf_spec__error_cost(const TFSpec *s, uint32_t version) {
  const TFSpecHead *head = &s->heads[version];
  if (!s->node_cost) {
    return head->status == TF_SPEC_PAUSED ? ERROR_COST_PER_RECOVERY : 0;
  }
  const TFSpecNode *node = &s->nodes[head->node];
  uint32_t cost = s->node_cost[head->node].error_cost;
  // Also charged while the head sits right on the discontinuity handle_error
  // pushed, before recovery has done anything about it.
  if (head->status == TF_SPEC_PAUSED || (node->state == ERROR_STATE && node->link_count &&
                                         s->trees[s->links[node->first_link].tree].discontinuity)) {
    cost += ERROR_COST_PER_RECOVERY;
  }
  return cost;
}

// stack.c:/ts_stack_node_count_since_error/, including its clamp: a pop can
// leave the head below the baseline it recorded.
static uint32_t tf_spec__nodes_since_error(TFSpec *s, uint32_t version) {
  if (!s->recover) {
    return 0;
  }
  TFSpecRecord *record = &s->records[version];
  uint32_t count = s->node_cost[s->heads[version].node].node_count;
  if (count < record->node_count_at_error) {
    record->node_count_at_error = count;
  }
  return count - record->node_count_at_error;
}

// parser.c:/ErrorComparison/ and /ts_parser__version_status/.
typedef enum {
  TF_SPEC_TAKE_LEFT,
  TF_SPEC_PREFER_LEFT,
  TF_SPEC_EQUAL,
  TF_SPEC_PREFER_RIGHT,
  TF_SPEC_TAKE_RIGHT,
} TFSpecOrder;

typedef struct {
  uint32_t cost;
  uint32_t node_count;
  int64_t precedence;
  bool in_error;
} TFSpecVersion;

static TFSpecVersion tf_spec__version_status(TFSpec *s, uint32_t version) {
  bool paused = s->heads[version].status == TF_SPEC_PAUSED;
  uint32_t cost = tf_spec__error_cost(s, version);
  if (paused) {
    cost += ERROR_COST_PER_SKIPPED_TREE;
  }
  return (TFSpecVersion){
      .cost = cost,
      .node_count = tf_spec__nodes_since_error(s, version),
      .precedence = s->nodes[s->heads[version].node].precedence,
      .in_error = paused || s->nodes[s->heads[version].node].state == ERROR_STATE,
  };
}

// parser.c:/ts_parser__compare_versions/, verbatim. With every cost zero it
// falls straight through to the precedence test, which is what tf_spec__condense
// compared on its own before recovery.
static TFSpecOrder tf_spec__compare_versions(TFSpecVersion a, TFSpecVersion b) {
  if (!a.in_error && b.in_error) {
    return a.cost < b.cost ? TF_SPEC_TAKE_LEFT : TF_SPEC_PREFER_LEFT;
  }
  if (a.in_error && !b.in_error) {
    return b.cost < a.cost ? TF_SPEC_TAKE_RIGHT : TF_SPEC_PREFER_RIGHT;
  }
  if (a.cost < b.cost) {
    return (b.cost - a.cost) * (1 + a.node_count) > TF_SPEC_MAX_COST_DIFFERENCE
               ? TF_SPEC_TAKE_LEFT
               : TF_SPEC_PREFER_LEFT;
  }
  if (b.cost < a.cost) {
    return (a.cost - b.cost) * (1 + b.node_count) > TF_SPEC_MAX_COST_DIFFERENCE
               ? TF_SPEC_TAKE_RIGHT
               : TF_SPEC_PREFER_RIGHT;
  }
  if (a.precedence > b.precedence) {
    return TF_SPEC_PREFER_LEFT;
  }
  if (b.precedence > a.precedence) {
    return TF_SPEC_PREFER_RIGHT;
  }
  return TF_SPEC_EQUAL;
}

// The recovery record of the tree about to be `s->tree_count`. Every leaf costs
// nothing, except one recovery inserted.
TF_NOINLINE static bool tf_spec__tree_cost(TFSpec *s, bool missing) {
  if (!TF_SPEC_RESERVE(s, tree_cost, tree_cost_capacity, s->tree_count + 1)) {
    return false;
  }
  s->tree_cost[s->tree_count] = (TFSpecTreeCost){
      .error_cost = missing ? ERROR_COST_PER_MISSING_TREE + ERROR_COST_PER_RECOVERY : 0};
  return true;
}

// This and the other TF_ALWAYS_INLINE helpers here run for every speculative
// shift and reduction, so they are forced inline, and what only recovery needs
// is kept out of line. Grown by recovery, they stopped inlining, which cost 2-4%
// on the conflict-heavy corpora with recovery off.
static TF_ALWAYS_INLINE uint32_t tf_spec__tree(TFSpec *s, TFSpecTree tree, bool recover) {
  // Replay reserves the high bit for its postorder marker.
  if (s->tree_count >= 0x80000000U) {
    s->failed = true;
    return TF_SPEC_NONE;
  }
  if (!TF_SPEC_RESERVE(s, trees, tree_capacity, s->tree_count + 1)) {
    return TF_SPEC_NONE;
  }
  if (recover && !tf_spec__tree_cost(s, tree.token.missing)) {
    return TF_SPEC_NONE;
  }
  s->trees[s->tree_count] = tree;
  return s->tree_count++;
}

// What a tree adds to a version's node count; a discontinuity adds nothing,
// as a NULL subtree does not (stack.c:/if (subtree.ptr)/).
static uint32_t tf_spec__tree_nodes(const TFSpec *s, uint32_t tree) {
  const TFSpecTree *t = &s->trees[tree];
  if (t->discontinuity) {
    return 0;
  }
  return tf_parser__node_count(s->owner->lang, t->token.symbol,
                               s->tree_cost[tree].visible_descendant);
}

// stack.c:/ts_stack__add_version/: a new active head on `node`, taking the
// error baseline of version `from`, but not its summary.
static TF_ALWAYS_INLINE uint32_t tf_spec__head(TFSpec *s, uint32_t node, uint32_t from,
                                               bool recover) {
  if (!TF_SPEC_RESERVE(s, heads, head_capacity, s->head_count + 1)) {
    return TF_SPEC_NONE;
  }
  if (recover) {
    if (!TF_SPEC_RESERVE(s, records, record_capacity, s->head_count + 1)) {
      return TF_SPEC_NONE;
    }
    s->records[s->head_count] = (TFSpecRecord){
        .node_count_at_error = from == TF_SPEC_NONE ? 0 : s->records[from].node_count_at_error};
  }
  s->heads[s->head_count] = (TFSpecHead){.node = node};
  return s->head_count++;
}

// Usually the last head, and an unguarded memmove of nothing is still a call.
static void tf_spec__remove_head(TFSpec *s, uint32_t v) {
  if (v + 1 < s->head_count) {
    memmove(s->heads + v, s->heads + v + 1, (s->head_count - v - 1) * sizeof(*s->heads));
    if (s->recover) {
      memmove(s->records + v, s->records + v + 1, (s->head_count - v - 1) * sizeof(*s->records));
    }
  }
  s->head_count--;
}

// Heads, and their records, trade places.
static void tf_spec__swap_heads(TFSpec *s, uint32_t a, uint32_t b) {
  TFSpecHead head = s->heads[a];
  s->heads[a] = s->heads[b];
  s->heads[b] = head;
  if (s->recover) {
    TFSpecRecord record = s->records[a];
    s->records[a] = s->records[b];
    s->records[b] = record;
  }
}

// stack.c:/ts_stack_renumber_version/: version `from` takes slot `to`, keeping
// the summary `to` had if it has none of its own.
static void tf_spec__renumber(TFSpec *s, uint32_t from, uint32_t to) {
  if (from == to) {
    return;
  }
  s->heads[to] = s->heads[from];
  if (s->recover) {
    TFSpecRecord record = s->records[from];
    if (s->records[to].summary_count && !record.summary_count &&
        s->heads[from].status != TF_SPEC_PAUSED) {
      record.summary_first = s->records[to].summary_first;
      record.summary_count = s->records[to].summary_count;
    }
    s->records[to] = record;
  }
  tf_spec__remove_head(s, from);
}

// A run of link slots. Links are never released, and a node that outgrows its
// run takes a longer one -- at most eight, so the waste is bounded.
static uint32_t tf_spec__links(TFSpec *s, uint32_t count) {
  if (!TF_SPEC_RESERVE(s, links, link_capacity, s->link_count + count)) {
    return TF_SPEC_NONE;
  }
  uint32_t at = s->link_count;
  s->link_count += count;
  return at;
}

// The running cost and count of node `s->node_count`, pushed over `tree`.
TF_NOINLINE static bool tf_spec__push_cost(TFSpec *s, uint32_t previous, uint32_t tree) {
  if (!TF_SPEC_RESERVE(s, node_cost, node_cost_capacity, s->node_count + 1)) {
    return false;
  }
  s->node_cost[s->node_count] =
      (TFSpecCost){.error_cost = s->node_cost[previous].error_cost + s->tree_cost[tree].error_cost,
                   .node_count = s->node_cost[previous].node_count + tf_spec__tree_nodes(s, tree)};
  return true;
}

static TF_ALWAYS_INLINE uint32_t tf_spec__push(TFSpec *s, uint32_t previous, uint32_t tree,
                                               TSStateId state, bool recover) {
  if (!TF_SPEC_RESERVE(s, nodes, node_capacity, s->node_count + 1)) {
    return TF_SPEC_NONE;
  }
  uint32_t at = tf_spec__links(s, 1);
  if (s->failed) {
    return TF_SPEC_NONE;
  }
  s->links[at] = (TFSpecLink){previous, tree};
  const TFSpecTree *t = &s->trees[tree];
  s->nodes[s->node_count] =
      (TFSpecNode){.state = state,
                   .byte = t->token.end_byte,
                   .point = t->token.end_point,
                   .precedence = s->nodes[previous].precedence + t->precedence,
                   .first_link = at,
                   .link_count = 1,
                   .link_capacity = 1};
  if (recover && !tf_spec__push_cost(s, previous, tree)) {
    return TF_SPEC_NONE;
  }
  return s->node_count++;
}

static bool tf_spec__materialize(TFSpec *s);

// A prefix cell whose shape a replay has already recovered.
static void tf_spec__resolve(TFSpec *s, uint32_t k) {
  uint32_t slot = s->prefix_tree[k], id = s->capture_id[s->prefix_depth - 1 - k];
  s->trees[slot].first_child = s->trees[id].first_child;
  s->trees[slot].child_count = s->trees[id].child_count;
  s->trees[slot].opaque = false;
}

// The real stack below the fork is one chain, so it only has to become graph
// nodes where a reduction reaches into it -- 6% to 16% of its cells, measured.
// Everything above stays speculative, and a pop that is only collecting the
// speculative region stops here instead of walking the whole stack.
static void tf_spec__extend(TFSpec *s) {
  const TFParser *p = s->owner;
  uint32_t i = s->prefix_left - 1;
  TFNode n = p->nodes[i];
  uint32_t byte = i ? p->nodes[i - 1].end_byte : 0;
  TFPoint point = i ? p->nodes[i - 1].end_point : (TFPoint){0, 0};
  uint32_t tree = tf_spec__tree(s,
                                (TFSpecTree){.token = {.symbol = n.symbol,
                                                       .start_byte = n.start_byte,
                                                       .end_byte = n.end_byte,
                                                       .start_point = n.start_point,
                                                       .end_point = n.end_point},
                                             .padding_start = byte,
                                             .child_count = TF_SPEC_NONE,
                                             .extra = n.extra,
                                             .inherited = true,
                                             .opaque = true},
                                s->recover);
  if (s->failed) {
    return;
  }
  if (!TF_SPEC_RESERVE(s, prefix_tree, prefix_tree_capacity, s->prefix_count + 1)) {
    return;
  }
  if (!TF_SPEC_RESERVE(s, nodes, node_capacity, s->node_count + 1)) {
    return;
  }
  s->prefix_tree[s->prefix_count++] = tree;
  uint32_t at = tf_spec__links(s, 1);
  if (s->failed) {
    return;
  }
  uint32_t below = s->node_count++;
  s->nodes[below] = (TFSpecNode){.state = p->states[i], .byte = byte, .point = point};
  if (s->recover) {
    if (!TF_SPEC_RESERVE(s, node_cost, node_cost_capacity, s->node_count)) {
      return;
    }
    const TFCell *cell = &p->cells[i];
    s->tree_cost[tree] = (TFSpecTreeCost){.error_cost = cell->error_cost,
                                          .visible_child = cell->visible_child,
                                          .visible_descendant = cell->visible_descendant,
                                          .has_children = cell->child_count > 0};
    // The frontier's error cost includes this cell's, so the node below has it
    // without, which keeps it exact down to the bottom of the stack.
    s->node_cost[below] =
        (TFSpecCost){.error_cost = s->node_cost[s->frontier].error_cost - cell->error_cost,
                     .node_count = i ? p->cells[i - 1].stack_nodes : 0};
  }
  s->links[at] = (TFSpecLink){below, tree};
  s->nodes[s->frontier].first_link = at;
  s->nodes[s->frontier].link_count = 1;
  s->nodes[s->frontier].link_capacity = 1;
  s->frontier = below;
  s->prefix_left = i;
  // A replay that already ran cannot be asked again for this cell's shape.
  if (s->materialized && s->captured) {
    tf_spec__resolve(s, s->prefix_count - 1);
  }
}

// The frontier has no links of its own until the cell below it is unrolled, so
// anything about to read or add its links unrolls first. False on failure.
static inline bool tf_spec__unroll(TFSpec *s, uint32_t node) {
  if (node == s->frontier && s->prefix_left) {
    tf_spec__extend(s);
  }
  return !s->failed;
}

// stack.c:stack__subtree_is_equivalent. Equivalent links keep the existing
// tree on a precedence tie; this is intentionally NOT ts_subtree_compare.
static bool tf_spec__equivalent(TFSpec *s, uint32_t a, uint32_t b) {
  if (a == b) {
    return true;
  }
  TFSpecTree x = s->trees[a], y = s->trees[b];
  // stack.c:184: two NULL subtrees are the same, and a NULL one is like nothing.
  if (x.discontinuity || y.discontinuity) {
    return x.discontinuity && y.discontinuity;
  }
  if (x.token.symbol != y.token.symbol) {
    return false;
  }
  // stack.c:189, after the symbols: two trees of the same symbol that both carry
  // errors are not worth keeping apart, whatever else differs about them.
  if (s->tree_cost && s->tree_cost[a].error_cost > 0 && s->tree_cost[b].error_cost > 0) {
    return true;
  }
  if (x.extra != y.extra ||
      x.token.start_byte - x.padding_start != y.token.start_byte - y.padding_start ||
      x.token.end_byte - x.token.start_byte != y.token.end_byte - y.token.start_byte) {
    return false;
  }
  if ((x.opaque || y.opaque) && !tf_spec__materialize(s)) {
    return false;
  }
  return s->trees[a].child_count == s->trees[b].child_count;
}

static void tf_spec__add_link(TFSpec *s, uint32_t target, TFSpecLink link);

// Add every link of `from` to `into`. Copied out: the recursion can grow the link
// arena and move it. glibc declares memcpy non-null, so an empty run must not
// reach it.
static void tf_spec__absorb(TFSpec *s, uint32_t into, uint32_t from) {
  TFSpecLink copy[TF_SPEC_LINKS];
  uint32_t n = s->nodes[from].link_count;
  if (n) {
    memcpy(copy, s->links + s->nodes[from].first_link, n * sizeof(*copy));
  }
  for (uint32_t i = 0; i < n; i++) {
    tf_spec__add_link(s, into, copy[i]);
  }
}

// stack.c:stack_node_add_link. Recursively merging predecessors preserves
// alternatives below the top state instead of discarding an entire stack.
static void tf_spec__add_link(TFSpec *s, uint32_t target, TFSpecLink link) {
  if (link.node == target) {
    return;
  }
  // links[0] of the frontier belongs to the prefix; claim it before adding here.
  if (!tf_spec__unroll(s, target)) {
    return;
  }
  TFSpecNode *node = &s->nodes[target];
  for (uint32_t i = 0; i < node->link_count; i++) {
    TFSpecLink existing = s->links[node->first_link + i];
    if (!tf_spec__equivalent(s, existing.tree, link.tree)) {
      continue;
    }
    if (existing.node == link.node) {
      if (s->trees[link.tree].precedence > s->trees[existing.tree].precedence) {
        s->links[node->first_link + i].tree = link.tree;
        node->precedence = s->nodes[link.node].precedence + s->trees[link.tree].precedence;
      }
      return;
    }
    // stack.c:232 also requires the two predecessors to carry the same error
    // cost, so a cheaper path is never folded into a dearer one.
    if (s->nodes[existing.node].state == s->nodes[link.node].state &&
        s->nodes[existing.node].byte == s->nodes[link.node].byte &&
        (!s->node_cost ||
         s->node_cost[existing.node].error_cost == s->node_cost[link.node].error_cost)) {
      tf_spec__absorb(s, existing.node, link.node);
      if (s->failed) {
        return;
      }
      // The recursion can unroll another prefix cell, which moves the node array.
      int64_t prec = s->nodes[link.node].precedence + s->trees[link.tree].precedence;
      if (prec > s->nodes[target].precedence) {
        s->nodes[target].precedence = prec;
      }
      return;
    }
  }
  if (node->link_count == TF_SPEC_LINKS) {
    return;  // stack.c:MAX_LINK_COUNT
  }
  if (node->link_count == node->link_capacity) {
    uint32_t want = node->link_capacity ? (uint32_t)node->link_capacity * 2 : 1;
    if (want > TF_SPEC_LINKS) {
      want = TF_SPEC_LINKS;
    }
    uint32_t at = tf_spec__links(s, want);
    if (s->failed) {
      return;
    }
    if (node->link_count) {
      memcpy(s->links + at, s->links + node->first_link, node->link_count * sizeof(*s->links));
    }
    node->first_link = at;
    node->link_capacity = (uint8_t)want;
  }
  s->links[node->first_link + node->link_count++] = link;
  int64_t prec = s->nodes[link.node].precedence + s->trees[link.tree].precedence;
  if (prec > node->precedence) {
    node->precedence = prec;
  }
  // stack.c:262: the node counts as many nodes as its longest path.
  if (s->node_cost) {
    uint32_t count = s->node_cost[link.node].node_count + tf_spec__tree_nodes(s, link.tree);
    if (count > s->node_cost[target].node_count) {
      s->node_cost[target].node_count = count;
    }
  }
}

// stack.c:/ts_stack_can_merge/, which also requires the two to have gone wrong
// to the same degree.
static TF_ALWAYS_INLINE bool tf_spec__merge(TFSpec *s, uint32_t a, uint32_t b) {
  uint32_t x = s->heads[a].node, y = s->heads[b].node;
  if (s->heads[a].status != TF_SPEC_ACTIVE || s->heads[b].status != TF_SPEC_ACTIVE ||
      s->nodes[x].state != s->nodes[y].state || s->nodes[x].byte != s->nodes[y].byte ||
      tf_spec__error_cost(s, a) != tf_spec__error_cost(s, b)) {
    return false;
  }
  tf_spec__absorb(s, x, y);
  if (s->node_cost && s->nodes[x].state == ERROR_STATE) {
    s->records[a].node_count_at_error = s->node_cost[x].node_count;
  }
  tf_spec__remove_head(s, b);
  return true;
}

// parser.c:/ts_parser__select_tree/, which ends in subtree.c:ts_subtree_compare.
// Used only when popping different paths to the same predecessor
// (parser.c:ts_parser__select_children), or selecting a root.
static bool tf_spec__prefer(TFSpec *s, uint32_t a, uint32_t b, bool recover) {
  if (a == TF_SPEC_NONE) {
    return true;
  }
  // The cheaper parse wins before anything else is looked at. Both costs are
  // zero until a split recovers, so this decides nothing before then.
  uint32_t left = recover ? s->tree_cost[a].error_cost : 0;
  uint32_t right = recover ? s->tree_cost[b].error_cost : 0;
  if (left != right) {
    return right < left;
  }
  if (s->trees[a].precedence != s->trees[b].precedence) {
    return s->trees[b].precedence > s->trees[a].precedence;
  }
  // Two trees that both went wrong are not worth comparing node by node.
  if (left > 0) {
    return true;
  }
  s->scratch_count = 0;
  if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, 2)) {
    return false;
  }
  s->scratch[s->scratch_count++] = a;
  s->scratch[s->scratch_count++] = b;
  while (s->scratch_count) {
    uint32_t right = s->scratch[--s->scratch_count], left = s->scratch[--s->scratch_count];
    if (left == right) {
      continue;
    }
    TFSpecTree x = s->trees[left], y = s->trees[right];
    if (x.token.symbol != y.token.symbol) {
      return y.token.symbol < x.token.symbol;
    }
    if (x.opaque || y.opaque) {
      if (!tf_spec__materialize(s)) {
        return false;
      }
      x = s->trees[left];
      y = s->trees[right];
    }
    if (x.child_count != y.child_count) {
      return y.child_count < x.child_count;
    }
    if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, s->scratch_count + 2 * x.child_count)) {
      return false;
    }
    for (uint32_t i = x.child_count; i > 0; i--) {
      s->scratch[s->scratch_count++] = s->children[x.first_child + i - 1];
      s->scratch[s->scratch_count++] = s->children[y.first_child + i - 1];
    }
  }
  return false;
}

// subtree.c:/ts_subtree_summarize_children/, into the recovery record of tree
// `id` over its `count` children at `first`: its error cost and the two visible
// counts. The same arithmetic as tf_parser__reduce_as does for a stack cell.
TF_NOINLINE static void tf_spec__summarize(TFSpec *s, uint32_t id, uint32_t first, uint32_t count) {
  const TFLanguage *lang = s->owner->lang;
  const TFSpecTree *parent = &s->trees[id];
  const TSSymbol *alias_row = tf_alias_sequence(lang, parent->production_id);
  bool error_parent = tf_parser__is_error(parent->token.symbol);
  TFSpecTreeCost cost = {.has_children = count > 0};
  uint32_t structural = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t child = s->children[first + i];
    const TFSpecTree *c = &s->trees[child];
    const TFSpecTreeCost *cc = &s->tree_cost[child];
    TSSymbol alias = (alias_row && !c->extra && c->token.symbol != 0) ? alias_row[structural] : 0;
    cost.visible_descendant += cc->visible_descendant;
    if (alias || tf_symbol_metadata(lang, c->token.symbol).visible) {
      cost.visible_descendant++;
      cost.visible_child++;
    } else {
      cost.visible_child += cc->visible_child;
    }
    cost.error_cost += tf_parser__child_cost(lang, error_parent, c->token.symbol, c->extra,
                                             cc->error_cost, cc->has_children, cc->visible_child,
                                             c->token.end_byte - c->token.start_byte,
                                             c->token.end_point.row - c->token.start_point.row);
    structural += !c->extra;
  }
  if (error_parent) {
    cost.error_cost +=
        tf_parser__extent_cost(parent->token.end_byte - parent->token.start_byte,
                               parent->token.end_point.row - parent->token.start_point.row);
  }
  s->tree_cost[id] = cost;
}

static TF_ALWAYS_INLINE uint32_t tf_spec__parent(TFSpec *s, TSSymbol symbol, uint16_t production,
                                                 uint8_t structural, uint32_t first, uint32_t count,
                                                 uint32_t base, bool recover) {
  TFSpecTree tree = {.token = {.symbol = symbol,
                               .start_byte = s->nodes[base].byte,
                               .end_byte = s->nodes[base].byte,
                               .start_point = s->nodes[base].point,
                               .end_point = s->nodes[base].point},
                     .padding_start = s->nodes[base].byte,
                     .first_child = first,
                     .child_count = count,
                     .structural_count = structural,
                     .production_id = production};
  if (count) {
    const TFSpecTree *left = &s->trees[s->children[first]];
    const TFSpecTree *right = &s->trees[s->children[first + count - 1]];
    tree.token.start_byte = left->token.start_byte;
    tree.token.start_point = left->token.start_point;
    tree.token.end_byte = right->token.end_byte;
    tree.token.end_point = right->token.end_point;
    tree.padding_start = left->padding_start;
    for (uint32_t i = 0; i < count; i++) {
      tree.precedence += s->trees[s->children[first + i]].precedence;
    }
  }
  uint32_t id = tf_spec__tree(s, tree, recover);
  if (recover && id != TF_SPEC_NONE) {
    tf_spec__summarize(s, id, first, count);
  }
  return id;
}

// stack.c:stack__iter and ts_stack__add_slice. The breadth-first visitation
// order matters when equivalent alternatives have equal precedence.
static TF_ALWAYS_INLINE void tf_spec__pop_as(TFSpec *s, uint32_t version, uint32_t goal,
                                             bool recover) {
  s->slice_count = 0;
  s->edge_count = 0;

  // A chain with no fork has exactly one path, so it yields exactly one slice.
  // Most pops are that: the breadth-first walk below, with its edge list and its
  // per-iterator bookkeeping, only earns its keep at a real fork.
  {
    uint32_t node = s->heads[version].node, count = 0, walked = 0;
    for (;;) {
      if (goal == TF_SPEC_NONE ? s->nodes[node].link_count == 0 : count == goal) {
        if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + walked) ||
            !TF_SPEC_RESERVE(s, slices, slice_capacity, 1)) {
          return;
        }
        // Deepest first, as the edge chain would have produced them.
        uint32_t first = s->child_count, n = s->heads[version].node;
        for (uint32_t i = walked; i > 0; i--) {
          TFSpecLink step = s->links[s->nodes[n].first_link];
          s->children[first + i - 1] = step.tree;
          n = step.node;
        }
        s->child_count += walked;
        uint32_t v = tf_spec__head(s, node, version, recover);
        if (s->failed) {
          return;
        }
        s->slices[0] = (TFSpecSlice){v, first, walked};
        s->slice_count = 1;
        return;
      }
      if (goal != TF_SPEC_NONE && !tf_spec__unroll(s, node)) {
        return;
      }
      if (s->nodes[node].link_count != 1) {
        break;
      }
      TFSpecLink link = s->links[s->nodes[node].first_link];
      count += !s->trees[link.tree].extra;
      walked++;
      node = link.node;
    }
  }

  // Only element 0 is live on entry; zeroing the other 63 costs a kilobyte of
  // memset on every reduction, and a pop is the innermost speculative loop.
  TFSpecIterator it[TF_SPEC_ITERATORS];
  it[0] = (TFSpecIterator){s->heads[version].node, TF_SPEC_NONE, 0, 0};
  uint32_t length = 1;
  while (length && !s->failed) {
    for (uint32_t i = 0, size = length; i < size; i++) {
      TFSpecIterator current = it[i];
      if (goal != TF_SPEC_NONE && current.count != goal && !tf_spec__unroll(s, current.node)) {
        return;
      }
      TFSpecNode node = s->nodes[current.node];
      bool pop = goal == TF_SPEC_NONE ? node.link_count == 0 : current.count == goal;
      if (pop) {
        if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + current.length) ||
            !TF_SPEC_RESERVE(s, slices, slice_capacity, s->slice_count + 1)) {
          return;
        }
        uint32_t first = s->child_count;
        for (uint32_t edge = current.edge; edge != TF_SPEC_NONE; edge = s->edges[edge].previous) {
          s->children[s->child_count++] = s->edges[edge].tree;
        }
        uint32_t v = TF_SPEC_NONE, slot = s->slice_count;
        for (uint32_t j = s->slice_count; j > 0; j--) {
          if (s->heads[s->slices[j - 1].version].node == current.node) {
            v = s->slices[j - 1].version;
            slot = j;
            break;
          }
        }
        if (v == TF_SPEC_NONE) {
          v = tf_spec__head(s, current.node, version, recover);
        }
        if (s->failed) {
          return;
        }
        if (slot < s->slice_count) {
          memmove(s->slices + slot + 1, s->slices + slot,
                  (s->slice_count - slot) * sizeof(*s->slices));
        }
        s->slices[slot] = (TFSpecSlice){v, first, current.length};
        s->slice_count++;
      }
      if (pop || node.link_count == 0) {
        if (i + 1 < length) {
          memmove(it + i, it + i + 1, (length - i - 1) * sizeof(*it));
        }
        length--;
        i--;
        size--;
        continue;
      }
      for (uint32_t j = 1; j <= node.link_count; j++) {
        uint32_t slot;
        TFSpecLink link;
        if (j == node.link_count) {
          slot = i;
          link = s->links[node.first_link];
        } else {
          if (length == TF_SPEC_ITERATORS) {
            continue;
          }
          slot = length++;
          link = s->links[node.first_link + j];
        }
        if (!TF_SPEC_RESERVE(s, edges, edge_capacity, s->edge_count + 1)) {
          return;
        }
        s->edges[s->edge_count] = (TFSpecEdge){link.tree, current.edge};
        it[slot] = (TFSpecIterator){link.node, s->edge_count++,
                                    current.count + !s->trees[link.tree].extra, current.length + 1};
      }
    }
  }
}

// The speculative reduction's copy is picked at compile time; the rest pick here.
static void tf_spec__pop_plain(TFSpec *s, uint32_t version, uint32_t goal) {
  tf_spec__pop_as(s, version, goal, false);
}

static void tf_spec__pop_recover(TFSpec *s, uint32_t version, uint32_t goal) {
  tf_spec__pop_as(s, version, goal, true);
}

static void tf_spec__pop(TFSpec *s, uint32_t version, uint32_t goal) {
  if (s->recover) {
    tf_spec__pop_recover(s, version, goal);
  } else {
    tf_spec__pop_plain(s, version, goal);
  }
}

static TF_ALWAYS_INLINE uint32_t tf_spec__reduce(TFSpec *s, const TFLanguage *lang,
                                                 uint32_t version, TSParseAction action,
                                                 bool recover) {
  uint32_t initial = s->head_count, removed = 0, halted = 0;
  for (uint32_t i = 0; i < s->head_count; i++) {
    halted += s->heads[i].status == TF_SPEC_HALTED && !s->heads[i].errored;
  }
  if (recover) {
    tf_spec__pop_recover(s, version, action.reduce.child_count);
  } else {
    tf_spec__pop_plain(s, version, action.reduce.child_count);
  }
  for (uint32_t i = 0; i < s->slice_count && !s->failed; i++) {
    TFSpecSlice slice = s->slices[i];
    uint32_t v = slice.version - removed;
    if (v > TF_SPEC_VERSIONS + 4 + halted) {
      tf_spec__remove_head(s, v);
      removed++;
      while (i + 1 < s->slice_count && s->slices[i + 1].version == slice.version) {
        i++;
      }
      continue;
    }
    uint32_t base = s->heads[v].node;
    // Every slice that reached the same predecessor proposes a parent; the
    // trailing extras are not its children.
    uint32_t parent = TF_SPEC_NONE, trailing_first = 0, trailing_count = 0;
    for (;; i++) {
      TFSpecSlice other = s->slices[i];
      uint32_t count = other.count;
      while (count && s->trees[s->children[other.first + count - 1]].extra) {
        count--;
      }
      uint32_t candidate =
          tf_spec__parent(s, action.reduce.symbol, action.reduce.production_id,
                          action.reduce.child_count, other.first, count, base, recover);
      if (s->failed) {
        return TF_SPEC_NONE;
      }
      if (tf_spec__prefer(s, parent, candidate, recover)) {
        parent = candidate;
        trailing_first = other.first + count;
        trailing_count = other.count - count;
      }
      if (i + 1 == s->slice_count || s->slices[i + 1].version != slice.version) {
        break;
      }
    }
    s->trees[parent].precedence += action.reduce.dynamic_precedence;
    TSStateId next = tf_next_state(lang, s->nodes[base].state, action.reduce.symbol);
    uint32_t top = tf_spec__push(s, base, parent, next, recover);
    for (uint32_t j = 0; j < trailing_count && !s->failed; j++) {
      top = tf_spec__push(s, top, s->children[trailing_first + j], next, recover);
    }
    if (s->failed) {
      return TF_SPEC_NONE;
    }
    s->heads[v].node = top;
    for (uint32_t j = 0; j < v; j++) {
      if (j != version && tf_spec__merge(s, j, v)) {
        removed++;
        break;
      }
    }
  }
  return s->head_count > initial ? initial : TF_SPEC_NONE;
}

static void tf_spec__error(TFSpec *s, uint32_t byte, TFPoint point, TSStateId state,
                           TSSymbol symbol, bool lex) {
  if (s->has_error && byte <= s->error_byte) {
    return;
  }
  s->has_error = true;
  s->error_byte = byte;
  s->error_point = point;
  s->error_state = state;
  s->error_symbol = symbol;
  s->error_is_lex = lex;
}

// parser.c:/ts_parser__lex/, for when the state's own lex mode finds nothing:
// lex again in the error state's mode, then skip characters until something
// lexes, and report what was skipped as one ERROR leaf. That leaf runs from
// where the failed attempt's token would have started to where the attempt
// that succeeded began; the token that attempt found is not used
// (parser.c:627-632). `n` is the node lexing began at.
TF_NOINLINE static void tf_spec__lex_error(TFParser *p, TFSpecNode n, TFToken *token,
                                           bool *keyword) {
  TFLexer *lexer = &p->lexer;
  *keyword = false;
  // parser.c:519: in the error state the first attempt already used its mode.
  if (n.state != ERROR_STATE) {
    tf_lexer_seek(lexer, n.byte, n.point);
    if (tf_lexer_next_in_error_mode(lexer, n.state, token)) {
      *keyword = lexer->token_is_keyword;
      return;
    }
  }
  uint32_t start = lexer->token_start_byte, end = start;
  TFPoint start_point = lexer->token_start_point, end_point = start_point;
  for (;;) {
    if (lexer->byte == end) {
      if (lexer->byte >= lexer->size) {
        break;
      }
      lexer->data.advance(&lexer->data, false);
    }
    end = lexer->byte;
    end_point = lexer->point;
    TFToken found;
    if (tf_lexer_next_in_error_mode(lexer, n.state, &found)) {
      break;
    }
  }
  *token = (TFToken){.symbol = ts_builtin_sym_error,
                     .start_byte = start,
                     .end_byte = end,
                     .start_point = start_point,
                     .end_point = end_point};
}

// parser.c:ts_parser__can_reuse_first_leaf and ts_parser__get_cached_token.
static TF_ALWAYS_INLINE bool tf_spec__lex(TFSpec *s, TFParser *p, uint32_t node, TFToken *token,
                                          bool *keyword) {
  TFSpecNode n = s->nodes[node];
  // An ERROR leaf has no table entry, so it is never reused (parser.c:/can_reuse_first_leaf/
  // finds no actions and no reusable entry for it); it is lexed again instead.
  if (s->has_cache && s->cached_byte == n.byte && !tf_parser__is_error(s->cached.symbol)) {
    uint32_t count;
    tf_actions(p->lang, n.state, s->cached.symbol, &count);
    TSLexerMode a = tf_lex_mode(p->lang, n.state), b = tf_lex_mode(p->lang, s->cached_state);
    bool reusable = count && memcmp(&a, &b, sizeof(a)) == 0 &&
                    (s->cached.symbol != p->lang->ts->keyword_capture_token ||
                     (!s->cached_keyword && s->cached_state == n.state));
    if (!reusable && (s->cached.end_byte > s->cached.start_byte || s->cached.symbol == 0)) {
      reusable =
          p->lang->ts->parse_actions[tf_lookup(p->lang, n.state, s->cached.symbol)].entry.reusable;
    }
    if (reusable) {
      *token = s->cached;
      *keyword = s->cached_keyword;
      return true;
    }
  }
  tf_lexer_seek(&p->lexer, n.byte, n.point);
  if (!tf_lexer_next(&p->lexer, n.state, token)) {
    tf_spec__error(s, p->lexer.byte, p->lexer.point, n.state, 0, true);
    if (!s->recover) {
      return false;
    }
    tf_spec__lex_error(p, n, token, keyword);
  } else {
    *keyword = p->lexer.token_is_keyword;
  }
  // The lexer does not write it: only recovery makes a token missing.
  token->missing = false;
  s->cached = *token;
  s->cached_byte = n.byte;
  s->cached_state = n.state;
  s->cached_keyword = *keyword;
  s->has_cache = true;
  return true;
}

static uint32_t tf_spec__collect(TFSpec *s, uint32_t first, uint32_t count, uint32_t *out);

static void tf_spec__accept(TFSpec *s, uint32_t version, TFToken eof) {
  // The root absorbs everything below it, and roots proposed at different times
  // are compared against each other, so the whole prefix has to be real here.
  while (s->prefix_left && !s->failed) {
    tf_spec__extend(s);
  }
  if (s->failed) {
    return;
  }
  uint32_t top = s->heads[version].node;
  uint32_t leaf = tf_spec__tree(
      s,
      (TFSpecTree){.token = eof, .padding_start = s->nodes[top].byte, .extra = true, .leaf = true},
      s->recover);
  if (s->failed) {
    return;
  }
  s->heads[version].node = tf_spec__push(s, top, leaf, 1, s->recover);
  if (s->failed) {
    return;
  }
  tf_spec__pop(s, version, TF_SPEC_NONE);
  for (uint32_t i = 0; i < s->slice_count && !s->failed; i++) {
    TFSpecSlice slice = s->slices[i];
    if (s->recovering) {
      // A pop never collects a NULL subtree (stack.c:/if (link.subtree.ptr)/).
      uint32_t first;
      slice.count = tf_spec__collect(s, slice.first, slice.count, &first);
      slice.first = first;
      if (s->failed) {
        return;
      }
    }
    s->accept_count++;
    for (uint32_t j = slice.count; j > 0; j--) {
      TFSpecTree root = s->trees[s->children[slice.first + j - 1]];
      if (root.extra) {
        continue;
      }
      if (root.opaque) {
        if (!tf_spec__materialize(s)) {
          return;
        }
        root = s->trees[s->children[slice.first + j - 1]];
      }
      uint32_t count = slice.count - 1 + root.child_count, first = s->child_count;
      if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + count)) {
        return;
      }
      for (uint32_t k = 0; k < slice.count; k++) {
        if (k == j - 1) {
          for (uint32_t c = 0; c < root.child_count; c++) {
            s->children[s->child_count++] = s->children[root.first_child + c];
          }
        } else {
          s->children[s->child_count++] = s->children[slice.first + k];
        }
      }
      uint32_t candidate = tf_spec__parent(s, root.token.symbol, root.production_id,
                                           root.structural_count, first, count, 0, s->recover);
      if (s->failed) {
        return;
      }
      if (tf_parser__is_error(root.token.symbol)) {
        // An ERROR root is recovery's (parser.c:/recover_eof/), and takes the
        // whole stack as its children, not a production's worth.
        uint32_t structural = 0;
        for (uint32_t k = 0; k < count; k++) {
          structural += !s->trees[s->children[first + k]].extra;
        }
        s->trees[candidate].structural_count = structural > 255 ? 255 : (uint8_t)structural;
      }
      if (tf_spec__prefer(s, s->finished, candidate, s->recover)) {
        s->finished = candidate;
      }
      break;
    }
  }
  if (s->slice_count) {
    tf_spec__remove_head(s, s->slices[0].version);
  }
  s->heads[version].status = TF_SPEC_HALTED;
}

static void tf_spec__recover(TFSpec *s, uint32_t version, TFToken lookahead);

// parser.c:ts_parser__advance. A head with nothing to do halts, unless the parse
// can recover, when it pauses for tf_spec__condense to decide about.
static TF_ALWAYS_INLINE void tf_spec__advance(TFSpec *s, TFParser *p, uint32_t version,
                                              bool recover) {
  TFToken token;
  bool keyword;
  if (s->in_hand) {
    s->in_hand = false;
    token = s->cached;
    keyword = s->cached_keyword;
  } else if (!tf_spec__lex(s, p, s->heads[version].node, &token, &keyword)) {
    s->heads[version].status = TF_SPEC_HALTED;
    s->heads[version].errored = true;
    return;
  }
  for (;;) {
    TSStateId state = s->nodes[s->heads[version].node].state;
    uint32_t count, last = TF_SPEC_NONE;
    // Only a parse that can recover lexes an ERROR leaf, which has no table
    // entry; tf_actions would read past the end of the table for it.
    const TSParseAction *actions = (recover && tf_parser__is_error(token.symbol))
                                       ? (count = 0, NULL)
                                       : tf_actions(p->lang, state, token.symbol, &count);
    bool reduced = false;
    for (uint32_t i = 0; i < count; i++) {
      TSParseAction action = actions[i];
      if (action.type == TSParseActionTypeShift) {
        uint32_t top = s->heads[version].node;
        uint32_t leaf = tf_spec__tree(s,
                                      (TFSpecTree){.token = token,
                                                   .padding_start = s->nodes[top].byte,
                                                   .extra = action.shift.extra,
                                                   .leaf = true},
                                      recover);
        if (s->failed) {
          return;
        }
        s->heads[version].node =
            tf_spec__push(s, top, leaf, action.shift.extra ? state : action.shift.state, recover);
        return;
      }
      if (action.type == TSParseActionTypeReduce) {
        uint32_t v = tf_spec__reduce(s, p->lang, version, action, recover);
        if (s->failed) {
          return;
        }
        reduced = true;
        if (v != TF_SPEC_NONE) {
          last = v;
        }
      } else if (action.type == TSParseActionTypeAccept) {
        tf_spec__accept(s, version, token);
        return;
      } else if (action.type == TSParseActionTypeRecover && recover) {
        tf_spec__recover(s, version, token);
        return;
      }
    }
    if (last != TF_SPEC_NONE) {
      tf_spec__renumber(s, last, version);
      continue;
    }
    if (reduced) {
      s->heads[version].status = TF_SPEC_HALTED;
      return;
    }
    if (tf_parser__demote_keyword(p->lang, state, &token, keyword)) {
      continue;
    }
    // Recorded as a parse that cannot recover would record it, so that declining
    // to recover reports the very same error.
    if (!tf_parser__is_error(token.symbol)) {
      tf_spec__error(s, token.start_byte, token.start_point, state, token.symbol, false);
    }
    if (recover) {
      if (state == ERROR_STATE) {
        tf_spec__recover(s, version, token);
        return;
      }
      // stack.c:/ts_stack_pause/.
      if (!TF_SPEC_RESERVE(s, paused, paused_capacity, s->paused_count + 1)) {
        return;
      }
      s->paused[s->paused_count] = token;
      s->records[version].summary_first = s->paused_count++;
      s->records[version].summary_count = 0;
      s->heads[version].status = TF_SPEC_PAUSED;
      s->records[version].node_count_at_error = s->node_cost[s->heads[version].node].node_count;
      return;
    }
    s->heads[version].status = TF_SPEC_HALTED;
    s->heads[version].errored = true;
    return;
  }
}

static void tf_spec__handle_error(TFSpec *s, TFParser *p, uint32_t version, TFToken lookahead);

// parser.c:ts_parser__condense_stack. Returns the lowest error cost of any
// version not in error, which ends the split once a finished tree beats it.
static TF_ALWAYS_INLINE uint32_t tf_spec__condense(TFSpec *s, TFParser *p, bool recover) {
  // Without recovery every cost is zero and nothing is ever paused, so the
  // comparison below comes down to precedence. This is it without building a
  // status per pair, which is what a conflict-heavy parse runs every round.
  if (!recover) {
    for (uint32_t i = 0; i < s->head_count; i++) {
      if (s->heads[i].status == TF_SPEC_HALTED) {
        tf_spec__remove_head(s, i--);
        continue;
      }
      int64_t precedence = s->nodes[s->heads[i].node].precedence;
      for (uint32_t j = 0; j < i; j++) {
        bool prefer = precedence > s->nodes[s->heads[j].node].precedence;
        if (tf_spec__merge(s, j, i)) {
          i--;
          break;
        }
        if (prefer) {
          tf_spec__swap_heads(s, i, j);
        }
      }
    }
    if (s->head_count > TF_SPEC_VERSIONS) {
      s->head_count = TF_SPEC_VERSIONS;
    }
    return s->head_count ? 0 : UINT32_MAX;
  }
  uint32_t min_error_cost = UINT32_MAX;
  for (uint32_t i = 0; i < s->head_count; i++) {
    if (s->heads[i].status == TF_SPEC_HALTED) {
      tf_spec__remove_head(s, i--);
      continue;
    }
    TFSpecVersion right = tf_spec__version_status(s, i);
    if (!right.in_error && right.cost < min_error_cost) {
      min_error_cost = right.cost;
    }
    for (uint32_t j = 0; j < i; j++) {
      switch (tf_spec__compare_versions(tf_spec__version_status(s, j), right)) {
        case TF_SPEC_TAKE_LEFT:
          tf_spec__remove_head(s, i--);
          j = i;
          break;
        case TF_SPEC_PREFER_LEFT:
        case TF_SPEC_EQUAL:
          if (tf_spec__merge(s, j, i)) {
            i--;
            j = i;
          }
          break;
        case TF_SPEC_PREFER_RIGHT:
          if (tf_spec__merge(s, j, i)) {
            i--;
            j = i;
          } else {
            tf_spec__swap_heads(s, i, j);
          }
          break;
        case TF_SPEC_TAKE_RIGHT:
          tf_spec__remove_head(s, j);
          i--;
          j--;
          break;
      }
    }
  }
  if (s->head_count > TF_SPEC_VERSIONS) {
    s->head_count = TF_SPEC_VERSIONS;
  }
  // If the best version is paused, or every one is, resume the best and recover
  // it; the rest of the paused ones go.
  if (recover) {
    bool unpaused = false;
    for (uint32_t i = 0, n = s->head_count; i < n && !s->failed && !s->stopped; i++) {
      if (s->heads[i].status != TF_SPEC_PAUSED) {
        unpaused = true;
        continue;
      }
      if (unpaused || s->accept_count >= TF_SPEC_VERSIONS) {
        tf_spec__remove_head(s, i--);
        n--;
        continue;
      }
      min_error_cost = tf_spec__error_cost(s, i);
      s->heads[i].status = TF_SPEC_ACTIVE;
      tf_spec__handle_error(s, p, i, s->paused[s->records[i].summary_first]);
      unpaused = true;
    }
  }
  return min_error_cost;
}

static TF_ALWAYS_INLINE bool tf_spec__unique(const TFSpec *s, uint32_t node) {
  if (!s->recovering) {
    while (s->nodes[node].link_count) {
      if (s->nodes[node].link_count != 1) {
        return false;
      }
      node = s->links[s->nodes[node].first_link].node;
    }
    return true;
  }
  // A discontinuity has no real stack cell to become, so a chain still holding
  // one cannot be handed to the ordinary parser, and neither can a head that is
  // still in the error state: the summary recovery would resume from lives on
  // the speculative head. Nor can an ERROR that is still a link: a later
  // recovery may pop it and regroup its children under a new ERROR
  // (parser.c:/ts_stack_pop_error/), which a consumer that has seen it cannot
  // undo. Once a reduction absorbs it, nothing can reach it again.
  if (s->nodes[node].state == ERROR_STATE) {
    return false;
  }
  while (s->nodes[node].link_count) {
    if (s->nodes[node].link_count != 1) {
      return false;
    }
    TFSpecLink link = s->links[s->nodes[node].first_link];
    if (s->trees[link.tree].discontinuity ||
        s->trees[link.tree].token.symbol == ts_builtin_sym_error) {
      return false;
    }
    node = link.node;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Error recovery: parser.c:/ts_parser__handle_error/ and what it calls, on the
// speculative stack. Only a split in a parse that can recover gets here, so
// none of it is on the path of a parse that cannot.

// A guarded table read: tf_actions indexes the table by symbol, and the two
// error symbols lie past its end (language.c:168 guards the same way).
static const TSParseAction *tf_spec__raw_actions(const TFLanguage *lang, TSStateId state,
                                                 TSSymbol symbol, uint32_t *count) {
  if (tf_parser__is_error(symbol)) {
    *count = 0;
    return NULL;
  }
  const TSParseActionEntry *entry = &lang->ts->parse_actions[tf_lookup(lang, state, symbol)];
  *count = entry->entry.count;
  return (const TSParseAction *)(entry + 1);
}

// language.h:/ts_language_has_actions/.
static bool tf_spec__has_actions(const TFLanguage *lang, TSStateId state, TSSymbol symbol) {
  return !tf_parser__is_error(symbol) && tf_lookup(lang, state, symbol) != 0;
}

// language.h:/ts_language_has_reduce_action/: the first unfiltered action.
static bool tf_spec__has_reduce_action(const TFLanguage *lang, TSStateId state, TSSymbol symbol) {
  uint32_t count;
  const TSParseAction *actions = tf_spec__raw_actions(lang, state, symbol, &count);
  return count > 0 && actions[0].type == TSParseActionTypeReduce;
}

// stack.c:/ts_stack_can_merge/, on the nodes' own costs; a pause or a
// discontinuity does not come into it.
static bool tf_spec__can_merge(const TFSpec *s, uint32_t a, uint32_t b) {
  uint32_t x = s->heads[a].node, y = s->heads[b].node;
  return s->heads[a].status == TF_SPEC_ACTIVE && s->heads[b].status == TF_SPEC_ACTIVE &&
         s->nodes[x].state == s->nodes[y].state && s->nodes[x].byte == s->nodes[y].byte &&
         (!s->node_cost || s->node_cost[x].error_cost == s->node_cost[y].error_cost);
}

// stack.c:/ts_stack_copy_version/: the same head again, without its summary.
static uint32_t tf_spec__copy(TFSpec *s, uint32_t version) {
  if (!TF_SPEC_RESERVE(s, heads, head_capacity, s->head_count + 1)) {
    return TF_SPEC_NONE;
  }
  if (!TF_SPEC_RESERVE(s, records, record_capacity, s->head_count + 1)) {
    return TF_SPEC_NONE;
  }
  s->heads[s->head_count] = s->heads[version];
  s->records[s->head_count] = s->records[version];
  s->records[s->head_count].summary_count = 0;
  return s->head_count++;
}

// A tree over `count` children at `first`, for recovery. ERROR and error_repeat
// charge for what they skip; the structural count is worked out because an
// ERROR's children are not a production's.
static uint32_t tf_spec__error_tree(TFSpec *s, TSSymbol symbol, uint32_t first, uint32_t count,
                                    uint32_t base, bool extra) {
  uint32_t structural = 0;
  for (uint32_t i = 0; i < count; i++) {
    structural += !s->trees[s->children[first + i]].extra;
  }
  uint32_t id = tf_spec__parent(s, symbol, 0, structural > 255 ? 255 : (uint8_t)structural, first,
                                count, base, true);
  if (id != TF_SPEC_NONE) {
    s->trees[id].extra = extra;
  }
  return id;
}

// Copy a slice's trees to a fresh run of `children`, dropping discontinuities:
// tree-sitter's pops never collect a NULL subtree (stack.c:/if (link.subtree.ptr)/).
static uint32_t tf_spec__collect(TFSpec *s, uint32_t first, uint32_t count, uint32_t *out) {
  if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + count)) {
    return 0;
  }
  *out = s->child_count;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t tree = s->children[first + i];
    if (!s->trees[tree].discontinuity) {
      s->children[s->child_count++] = tree;
    }
  }
  return s->child_count - *out;
}

// stack.c:/ts_stack_record_summary/: the states within MAX_SUMMARY_DEPTH
// non-extra trees of the head, down every path, in the breadth-first order
// stack__iter visits them. A discontinuity counts as a tree, as a NULL subtree
// does.
TF_NOINLINE static void tf_spec__record_summary(TFSpec *s, uint32_t version) {
  typedef struct {
    uint32_t node, depth;
  } TFWalk;
  uint32_t first = s->summary_count;
  TFWalk it[TF_SPEC_ITERATORS];
  it[0] = (TFWalk){s->heads[version].node, 0};
  uint32_t length = 1;
  while (length && !s->failed) {
    for (uint32_t i = 0, size = length; i < size; i++) {
      TFWalk current = it[i];
      bool stop = current.depth > TF_SPEC_MAX_SUMMARY_DEPTH;
      if (!stop) {
        TSStateId state = s->nodes[current.node].state;
        bool seen = false;
        for (uint32_t k = s->summary_count; k > first; k--) {
          const TFSpecSummary *entry = &s->summaries[k - 1];
          if (entry->depth < current.depth) {
            break;
          }
          if (entry->depth == current.depth && entry->state == state) {
            seen = true;
            break;
          }
        }
        if (!seen) {
          if (!TF_SPEC_RESERVE(s, summaries, summary_capacity, s->summary_count + 1)) {
            return;
          }
          s->summaries[s->summary_count++] =
              (TFSpecSummary){.byte = s->nodes[current.node].byte,
                              .row = s->nodes[current.node].point.row,
                              .depth = current.depth,
                              .state = state};
        }
      }
      if (!stop && !tf_spec__unroll(s, current.node)) {
        return;
      }
      TFSpecNode node = s->nodes[current.node];
      if (stop || node.link_count == 0) {
        if (i + 1 < length) {
          memmove(it + i, it + i + 1, (length - i - 1) * sizeof(*it));
        }
        length--;
        i--;
        size--;
        continue;
      }
      for (uint32_t j = 1; j <= node.link_count; j++) {
        uint32_t slot;
        TFSpecLink link;
        if (j == node.link_count) {
          slot = i;
          link = s->links[node.first_link];
        } else {
          if (length == TF_SPEC_ITERATORS) {
            continue;
          }
          slot = length++;
          link = s->links[node.first_link + j];
        }
        it[slot] = (TFWalk){link.node, current.depth + !s->trees[link.tree].extra};
      }
    }
  }
  s->records[version].summary_first = first;
  s->records[version].summary_count = s->summary_count - first;
}

// parser.c:/ts_parser__better_version_exists/.
static bool tf_spec__better_version_exists(TFSpec *s, uint32_t version, bool in_error,
                                           uint32_t cost) {
  if (s->finished != TF_SPEC_NONE && s->tree_cost[s->finished].error_cost <= cost) {
    return true;
  }
  uint32_t node = s->heads[version].node;
  TFSpecVersion status = {.cost = cost,
                          .in_error = in_error,
                          .precedence = s->nodes[node].precedence,
                          .node_count = tf_spec__nodes_since_error(s, version)};
  for (uint32_t i = 0; i < s->head_count; i++) {
    if (i == version || s->heads[i].status != TF_SPEC_ACTIVE ||
        s->nodes[s->heads[i].node].byte < s->nodes[node].byte) {
      continue;
    }
    switch (tf_spec__compare_versions(status, tf_spec__version_status(s, i))) {
      case TF_SPEC_TAKE_RIGHT:
        return true;
      case TF_SPEC_PREFER_RIGHT:
        if (tf_spec__can_merge(s, i, version)) {
          return true;
        }
        break;
      default:
        break;
    }
  }
  return false;
}

// stack.c:/ts_stack_pop_error/: an ERROR sitting directly on top of `version`,
// popped, or TF_SPEC_NONE.
//
// An ERROR the ordinary parser already committed is left where it is. Its
// children went to the consumer inside it, and a streaming sink cannot take
// them back to regroup them under a new one, as tree-sitter does here.
static uint32_t tf_spec__pop_error(TFSpec *s, uint32_t version) {
  uint32_t node = s->heads[version].node;
  if (!tf_spec__unroll(s, node)) {
    return TF_SPEC_NONE;
  }
  for (uint32_t i = 0; i < s->nodes[node].link_count; i++) {
    TFSpecLink link = s->links[s->nodes[node].first_link + i];
    const TFSpecTree *tree = &s->trees[link.tree];
    if (tree->token.symbol == ts_builtin_sym_error && !tree->discontinuity) {
      if (tree->inherited) {
        return TF_SPEC_NONE;
      }
      s->heads[version].node = link.node;
      return link.tree;
    }
  }
  return TF_SPEC_NONE;
}

// parser.c:/ts_parser__process_candidate_recovery_actions/, into a reduce set
// that ignores a repeat of the same symbol and child count
// (reduce_action.h:/ts_reduce_action_set_add/).
typedef struct {
  TSSymbol symbol;
  uint16_t production_id;
  uint32_t count;
  int16_t dynamic_precedence;
} TFSpecReduce;

static bool tf_spec__candidates(const TSParseAction *actions, uint32_t count, TFSpecReduce *set,
                                uint32_t *size, uint32_t capacity) {
  bool shift = false;
  for (uint32_t i = 0; i < count; i++) {
    TSParseAction action = actions[i];
    if (action.type == TSParseActionTypeShift || action.type == TSParseActionTypeRecover) {
      if (!action.shift.extra && !action.shift.repetition) {
        shift = true;
      }
    } else if (action.type == TSParseActionTypeReduce && action.reduce.child_count > 0) {
      bool seen = false;
      for (uint32_t j = 0; j < *size; j++) {
        if (set[j].symbol == action.reduce.symbol && set[j].count == action.reduce.child_count) {
          seen = true;
          break;
        }
      }
      if (!seen && *size < capacity) {
        set[(*size)++] = (TFSpecReduce){.symbol = action.reduce.symbol,
                                        .production_id = action.reduce.production_id,
                                        .count = action.reduce.child_count,
                                        .dynamic_precedence = action.reduce.dynamic_precedence};
      }
    }
  }
  return shift;
}

// parser.c:/ts_parser__do_all_potential_reductions/. With `lookahead` 0, every
// reduction any terminal allows here, whatever comes next.
TF_NOINLINE static bool tf_spec__all_reductions(TFSpec *s, uint32_t starting, TSSymbol lookahead) {
  const TFLanguage *lang = s->owner->lang;
  uint32_t initial = s->head_count;
  bool can_shift = false;
  uint32_t version = starting;
  enum { TF_SPEC_REDUCE_SET = 256 };
  TFSpecReduce set[TF_SPEC_REDUCE_SET];
  for (uint32_t i = 0;; i++) {
    uint32_t version_count = s->head_count;
    if (version >= version_count || s->failed) {
      break;
    }
    bool merged = false;
    for (uint32_t j = initial; j < version; j++) {
      if (tf_spec__merge(s, j, version)) {
        merged = true;
        break;
      }
    }
    if (merged) {
      continue;
    }
    TSStateId state = s->nodes[s->heads[version].node].state;
    uint32_t size = 0;
    bool shift = false;
    if (lookahead != 0) {
      uint32_t count;
      const TSParseAction *actions = tf_spec__raw_actions(lang, state, lookahead, &count);
      shift = tf_spec__candidates(actions, count, set, &size, TF_SPEC_REDUCE_SET);
    } else {
      // Every terminal but the end token, as the lookahead iterator visits them,
      // then by symbol, descending: parser.c sorts for exactly this reason.
      for (uint32_t symbol = 1; symbol < lang->ts->token_count; symbol++) {
        uint32_t count;
        const TSParseAction *actions = tf_spec__raw_actions(lang, state, (TSSymbol)symbol, &count);
        if (count && tf_spec__candidates(actions, count, set, &size, TF_SPEC_REDUCE_SET)) {
          shift = true;
        }
      }
      for (uint32_t j = 1; j < size; j++) {
        TFSpecReduce key = set[j];
        int32_t k = (int32_t)j - 1;
        while (k >= 0 && set[k].symbol < key.symbol) {
          set[k + 1] = set[k];
          k--;
        }
        set[k + 1] = key;
      }
    }
    uint32_t reduced = TF_SPEC_NONE;
    for (uint32_t j = 0; j < size && !s->failed; j++) {
      TSParseAction action = {.reduce = {.type = TSParseActionTypeReduce,
                                         .child_count = (uint8_t)set[j].count,
                                         .symbol = set[j].symbol,
                                         .dynamic_precedence = set[j].dynamic_precedence,
                                         .production_id = set[j].production_id}};
      reduced = tf_spec__reduce(s, lang, version, action, true);
    }
    if (shift) {
      can_shift = true;
    } else if (reduced != TF_SPEC_NONE && i < TF_SPEC_VERSIONS) {
      tf_spec__renumber(s, reduced, version);
      continue;
    } else if (lookahead != 0) {
      tf_spec__remove_head(s, version);
    }
    version = version == starting ? version_count : version + 1;
  }
  return can_shift;
}

// parser.c:/ts_parser__recover_to_state/: pop `depth` trees, and if the version
// reached is in `goal`, wrap them in an ERROR there. The ERROR is an extra, so
// it does not count as a child of what follows.
TF_NOINLINE static bool tf_spec__recover_to_state(TFSpec *s, uint32_t version, uint32_t depth,
                                                  TSStateId goal) {
  tf_spec__pop(s, version, depth);
  if (s->failed) {
    return false;
  }
  // Later pops and pushes reuse the slice array, so take a copy.
  TFSpecSlice slices[TF_SPEC_ITERATORS];
  uint32_t slice_count = s->slice_count < TF_SPEC_ITERATORS ? s->slice_count : TF_SPEC_ITERATORS;
  memcpy(slices, s->slices, slice_count * sizeof(*slices));
  uint32_t previous = TF_SPEC_NONE;
  for (uint32_t i = 0; i < slice_count && !s->failed; i++) {
    TFSpecSlice slice = slices[i];
    if (slice.version == previous) {
      continue;
    }
    if (s->nodes[s->heads[slice.version].node].state != goal) {
      s->heads[slice.version].status = TF_SPEC_HALTED;
      continue;
    }
    uint32_t error = tf_spec__pop_error(s, slice.version);
    uint32_t first = s->child_count, count = 0;
    if (error != TF_SPEC_NONE && s->trees[error].child_count > 0) {
      // The old ERROR's contents go in first, grouped, as they lay below.
      uint32_t nested =
          tf_spec__error_tree(s, tf_builtin_sym_error_repeat, s->trees[error].first_child,
                              s->trees[error].child_count, s->heads[slice.version].node, false);
      if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + 1 + slice.count)) {
        return false;
      }
      first = s->child_count;
      s->children[s->child_count++] = nested;
      uint32_t rest;
      count = 1 + tf_spec__collect(s, slice.first, slice.count, &rest);
    } else {
      count = tf_spec__collect(s, slice.first, slice.count, &first);
    }
    // ts_subtree_array_remove_trailing_extras: they go back on top, outside it.
    uint32_t kept = count;
    while (kept && s->trees[s->children[first + kept - 1]].extra) {
      kept--;
    }
    uint32_t top = s->heads[slice.version].node;
    if (kept > 0) {
      uint32_t tree = tf_spec__error_tree(s, ts_builtin_sym_error, first, kept, top, true);
      top = tf_spec__push(s, top, tree, goal, true);
    }
    for (uint32_t j = kept; j < count && !s->failed; j++) {
      top = tf_spec__push(s, top, s->children[first + j], goal, true);
    }
    if (s->failed) {
      return false;
    }
    s->heads[slice.version].node = top;
    previous = slice.version;
  }
  return previous != TF_SPEC_NONE;
}

// parser.c:/ts_parser__recover/: either return to a state the summary says the
// lookahead is valid in, wrapping what lies between in an ERROR, or skip the
// lookahead into an error_repeat and stay in the error state. Both are tried,
// as separate versions, unless one is clearly worse than what already exists.
TF_NOINLINE static void tf_spec__recover(TFSpec *s, uint32_t version, TFToken lookahead) {
  const TFLanguage *lang = s->owner->lang;
  bool did_recover = false;
  uint32_t previous_count = s->head_count;
  uint32_t byte = s->nodes[s->heads[version].node].byte;
  TFPoint point = s->nodes[s->heads[version].node].point;
  uint32_t since_error = tf_spec__nodes_since_error(s, version);
  uint32_t current_cost = tf_spec__error_cost(s, version);

  if (s->records[version].summary_count && lookahead.symbol != ts_builtin_sym_error) {
    uint32_t first = s->records[version].summary_first, count = s->records[version].summary_count;
    for (uint32_t i = 0; i < count; i++) {
      TFSpecSummary entry = s->summaries[first + i];
      if (entry.state == ERROR_STATE || entry.byte == byte) {
        continue;
      }
      uint32_t depth = entry.depth + (since_error > 0);
      // Do not recover in ways that create redundant stack versions.
      bool would_merge = false;
      for (uint32_t j = 0; j < previous_count; j++) {
        if (s->nodes[s->heads[j].node].state == entry.state &&
            s->nodes[s->heads[j].node].byte == byte) {
          would_merge = true;
          break;
        }
      }
      if (would_merge) {
        continue;
      }
      uint32_t cost = current_cost + entry.depth * ERROR_COST_PER_SKIPPED_TREE +
                      (byte - entry.byte) * ERROR_COST_PER_SKIPPED_CHAR +
                      (point.row - entry.row) * ERROR_COST_PER_SKIPPED_LINE;
      if (tf_spec__better_version_exists(s, version, false, cost)) {
        break;
      }
      if (tf_spec__has_actions(lang, entry.state, lookahead.symbol) &&
          tf_spec__recover_to_state(s, version, depth, entry.state)) {
        did_recover = true;
        break;
      }
    }
  }
  if (s->failed) {
    return;
  }
  for (uint32_t i = previous_count; i < s->head_count; i++) {
    if (s->heads[i].status != TF_SPEC_ACTIVE) {
      tf_spec__remove_head(s, i--);
    }
  }

  // Still in error at the end of the file: an empty ERROR, and accept.
  if (lookahead.symbol == ts_builtin_sym_end) {
    uint32_t top = s->heads[version].node;
    uint32_t error = tf_spec__error_tree(s, ts_builtin_sym_error, s->child_count, 0, top, false);
    if (s->failed) {
      return;
    }
    s->heads[version].node = tf_spec__push(s, top, error, 1, true);
    if (!s->failed) {
      tf_spec__accept(s, version, lookahead);
    }
    return;
  }
  if (did_recover && s->head_count > TF_SPEC_VERSIONS) {
    s->heads[version].status = TF_SPEC_HALTED;
    return;
  }
  uint32_t cost = current_cost + ERROR_COST_PER_SKIPPED_TREE +
                  (lookahead.end_byte - byte) * ERROR_COST_PER_SKIPPED_CHAR +
                  (lookahead.end_point.row - point.row) * ERROR_COST_PER_SKIPPED_LINE;
  if (tf_spec__better_version_exists(s, version, false, cost)) {
    s->heads[version].status = TF_SPEC_HALTED;
    return;
  }

  // A token that is an extra where the parse started stays one (parser.c:1391:
  // the last action, unfiltered, in state 1).
  uint32_t count;
  const TSParseAction *actions = tf_spec__raw_actions(lang, 1, lookahead.symbol, &count);
  bool extra = count > 0 && actions[count - 1].type == TSParseActionTypeShift &&
               actions[count - 1].shift.extra;
  uint32_t leaf = tf_spec__tree(
      s, (TFSpecTree){.token = lookahead, .padding_start = byte, .leaf = true, .extra = extra},
      true);
  if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + 1)) {
    return;
  }
  uint32_t first = s->child_count;
  s->children[s->child_count++] = leaf;
  uint32_t top = s->heads[version].node;
  uint32_t repeat = tf_spec__error_tree(s, tf_builtin_sym_error_repeat, first, 1, top, false);

  // Anything skipped already sits on top as an error_repeat; take it back off and
  // group the two, rather than stacking them.
  if (since_error > 0) {
    tf_spec__pop(s, version, 1);
    if (s->failed || !s->slice_count) {
      return;
    }
    TFSpecSlice slice = s->slices[0];
    while (s->head_count > slice.version + 1) {
      tf_spec__remove_head(s, slice.version + 1);
    }
    tf_spec__renumber(s, slice.version, version);
    uint32_t start, kept = tf_spec__collect(s, slice.first, slice.count, &start);
    if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + 1)) {
      return;
    }
    s->children[s->child_count++] = repeat;
    top = s->heads[version].node;
    repeat = tf_spec__error_tree(s, tf_builtin_sym_error_repeat, start, kept + 1, top, false);
  }
  if (!s->failed) {
    s->heads[version].node = tf_spec__push(s, top, repeat, ERROR_STATE, true);
  }
}

// The consumer's say in the matter, TFSink::on_error, once per recovery: the
// rejected token, the state that rejected it, and what that state would take.
static bool tf_spec__report(TFSpec *s, TFParser *p, uint32_t version, const TFToken *lookahead) {
  const TFLanguage *lang = p->lang;
  TSStateId state = s->nodes[s->heads[version].node].state;
  if (!p->expected) {
    p->expected = malloc(lang->ts->token_count * sizeof(TSSymbol));
    if (!p->expected) {
      s->failed = true;
      return false;
    }
  }
  uint32_t count = 0;
  for (uint32_t symbol = 0; symbol < lang->ts->token_count; symbol++) {
    uint32_t actions;
    tf_actions(lang, state, (TSSymbol)symbol, &actions);
    if (actions) {
      p->expected[count++] = (TSSymbol)symbol;
    }
  }
  TFErrorEvent event = {
      .token = *lookahead, .state = state, .expected = p->expected, .expected_count = count};
  return p->sink->on_error(p->sink->payload, &event);
}

// parser.c:/ts_parser__handle_error/: try every reduction the state allows, and
// on each result a missing token that would let the lookahead through; then mark
// where recovery began, record where it could return to, and recover.
TF_NOINLINE static void tf_spec__handle_error(TFSpec *s, TFParser *p, uint32_t version,
                                              TFToken lookahead) {
  if (!tf_spec__report(s, p, version, &lookahead)) {
    s->stopped = true;
    return;
  }
  const TFLanguage *lang = p->lang;
  uint32_t previous_count = s->head_count;
  tf_spec__all_reductions(s, version, 0);
  uint32_t version_count = s->head_count;
  uint32_t byte = s->nodes[s->heads[version].node].byte;
  TFPoint point = s->nodes[s->heads[version].node].point;

  bool did_insert_missing = false;
  for (uint32_t v = version; v < version_count && !s->failed;) {
    if (!did_insert_missing) {
      TSStateId state = s->nodes[s->heads[v].node].state;
      for (TSSymbol missing = 1; missing < lang->ts->token_count && !s->failed; missing++) {
        TSStateId after = tf_next_state(lang, state, missing);
        if (after == 0 || after == state ||
            !tf_spec__has_reduce_action(lang, after, lookahead.symbol)) {
          continue;
        }
        uint32_t copy = tf_spec__copy(s, v);
        uint32_t leaf = tf_spec__tree(s,
                                      (TFSpecTree){.token = {.symbol = missing,
                                                             .missing = true,
                                                             .start_byte = byte,
                                                             .end_byte = byte,
                                                             .start_point = point,
                                                             .end_point = point},
                                                   .padding_start = byte,
                                                   .leaf = true},
                                      true);
        if (s->failed) {
          return;
        }
        s->heads[copy].node = tf_spec__push(s, s->heads[copy].node, leaf, after, true);
        if (tf_spec__all_reductions(s, copy, lookahead.symbol)) {
          did_insert_missing = true;
          break;
        }
      }
    }
    uint32_t mark = tf_spec__tree(s,
                                  (TFSpecTree){.token = {.start_byte = byte,
                                                         .end_byte = byte,
                                                         .start_point = point,
                                                         .end_point = point},
                                               .padding_start = byte,
                                               .discontinuity = true},
                                  true);
    if (s->failed) {
      return;
    }
    s->heads[v].node = tf_spec__push(s, s->heads[v].node, mark, ERROR_STATE, true);
    s->recovering = true;
    // stack.c:/ts_stack_push/: pushing a NULL subtree resets the baseline.
    if (!s->failed) {
      s->records[v].node_count_at_error = s->node_cost[s->heads[v].node].node_count;
    }
    v = v == version ? previous_count : v + 1;
  }
  for (uint32_t i = previous_count; i < version_count && !s->failed; i++) {
    tf_spec__merge(s, version, previous_count);
  }
  tf_spec__record_summary(s, version);
  if (!s->failed) {
    tf_spec__recover(s, version, lookahead);
  }
}

// The number of non-extra children a reduction of `tree` pops. For a grammar
// tree that is its production's; an error tree's is counted, since an ERROR root
// can hold more than the 255 a production can.
static uint32_t tf_spec__structural(const TFSpec *s, const TFSpecTree *tree) {
  if (!tf_parser__is_error(tree->token.symbol)) {
    return tree->structural_count;
  }
  uint32_t count = 0;
  for (uint32_t i = 0; i < tree->child_count; i++) {
    count += !s->trees[s->children[tree->first_child + i]].extra;
  }
  return count;
}

// The real stack cell an inherited tree stands for.
static uint32_t tf_spec__cell(const TFSpec *s, uint32_t tree) {
  for (uint32_t k = 0; k < s->prefix_count; k++) {
    if (s->prefix_tree[k] == tree) {
      return s->prefix_depth - 1 - k;
    }
  }
  return TF_SPEC_NONE;
}

// Mark the real stack cells an extra ERROR wraps as extras, or put them back.
//
// Postorder replay is the order an LR parser reduces in, which is right for
// anything the tables produced. Recovery can wrap cells already on the real
// stack in an ERROR that is an extra, which later reductions to its left carry
// along as a trailing extra. In postorder those reductions come first, and would
// pop the ERROR's cells as their own children; marked, they step over them as
// the speculative ones did over the ERROR. Replay unmarks them on reaching the
// ERROR, before anything reduces them into it.
static bool tf_spec__wraps(const TFSpecTree *tree) {
  return tree->extra && tree->token.symbol == ts_builtin_sym_error && !tree->leaf;
}

// Unmarking stops at an ERROR nested inside: its cells stay extras until the
// replay reaches that one, since what reduces before it has to step over them.
static void tf_spec__mark_wrapped(TFSpec *s, TFParser *p, uint32_t tree, bool marked) {
  uint32_t base = s->scratch_count;
  if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, s->scratch_count + 1)) {
    return;
  }
  s->scratch[s->scratch_count++] = tree;
  while (s->scratch_count > base) {
    uint32_t id = s->scratch[--s->scratch_count];
    const TFSpecTree *t = &s->trees[id];
    if (!marked && id != tree && tf_spec__wraps(t)) {
      continue;
    }
    if (t->inherited) {
      // Marking happens before replay moves anything, so the prefix map finds
      // the cell. By the time it is unmarked, reductions to its left will have
      // moved it down as a trailing extra, so it is found by what it is: near
      // the top, since it is what the ERROR reduces next.
      uint32_t cell = TF_SPEC_NONE;
      if (marked) {
        cell = tf_spec__cell(s, (uint32_t)(t - s->trees));
      } else {
        for (uint32_t i = p->depth; i > 0; i--) {
          const TFNode *n = &p->nodes[i - 1];
          if (n->start_byte == t->token.start_byte && n->end_byte == t->token.end_byte &&
              n->symbol == t->token.symbol) {
            cell = i - 1;
            break;
          }
        }
      }
      if (cell != TF_SPEC_NONE) {
        p->nodes[cell].extra = marked || t->extra;
      }
      continue;
    }
    if (t->leaf ||
        !TF_SPEC_RESERVE(s, scratch, scratch_capacity, s->scratch_count + t->child_count)) {
      continue;
    }
    for (uint32_t i = 0; i < t->child_count; i++) {
      s->scratch[s->scratch_count++] = s->children[t->first_child + i];
    }
  }
}

// A selected forest is replayed in postorder. Inherited cells are already on
// the real stack, so only speculative shifts and reductions reach the sink.
// Out of line on purpose. It runs once per split, so the call costs nothing, and
// inlining it grows the function that also holds the ordinary dispatch loop:
// keeping it out is worth -11% on SystemVerilog by itself. `noinline` on
// `tf_parser__split`, which contains it, measures nothing -- the placement is
// what matters.
//
// Specialised on `recover` like tf_parser.c's dispatch loop, and for the same
// reason: choosing the shift and reduce copy per replayed node read
// `sink->on_error` once per node, and a conflict-heavy file replays constantly.
static TF_ALWAYS_INLINE bool tf_spec__replay_as(TFSpec *s, TFParser *p, uint32_t first,
                                                uint32_t count, bool recover) {
  s->scratch_count = 0;
  if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, count)) {
    return false;
  }
  for (uint32_t i = count; i > 0; i--) {
    s->scratch[s->scratch_count++] = s->children[first + i - 1];
  }
  while (s->scratch_count) {
    uint32_t item = s->scratch[--s->scratch_count];
    // Descend into the leftmost child directly instead of pushing it and popping
    // it straight back: a left-recursive repetition is one such step per element.
    for (;;) {
      if (item & 0x80000000U) {
        const TFSpecTree *done = &s->trees[item & 0x7fffffffU];
        if (recover && tf_parser__is_error(done->token.symbol)) {
          if (!tf_parser__reduce_recover(p, done->token.symbol, tf_spec__structural(s, done), 0,
                                         NULL)) {
            return false;
          }
          // tf_next_state has no entry for either error symbol, so the reduction
          // lands in state 0, which is right for an error_repeat skipping tokens
          // in the error state. The ERROR recovery wraps what it returned over is
          // an extra (parser.c:/ts_subtree_new_error_node(&slice.subtrees, true/),
          // and like any extra it leaves the state as it found it.
          if (done->extra) {
            uint32_t at = p->depth - 1;
            while (at > 0 && p->nodes[at].extra) {
              at--;
            }
            p->nodes[at].extra = true;
            // Extras the reduction left above it were shifted over a real cell,
            // so they only join the leading run now.
            if (at == p->leading) {
              while (p->leading < p->depth && p->nodes[p->leading].extra) {
                p->leading++;
              }
            }
            for (uint32_t k = at + 1; k <= p->depth; k++) {
              p->states[k] = p->states[at];
            }
          }
          break;
        }
        if (!(recover ? tf_parser__reduce_recover : tf_parser__reduce_plain)(
                p, done->token.symbol, done->structural_count, done->production_id, NULL)) {
          return false;
        }
        break;
      }
      // Read through the arena rather than copying the entry: each branch below
      // wants a different handful of its fields.
      const TFSpecTree *tree = &s->trees[item];
      if (tree->inherited) {
        break;
      }
      if (recover && s->recovering && tf_spec__wraps(tree)) {
        // Its cells are the next thing to reduce; see tf_spec__mark_wrapped.
        uint32_t saved = s->scratch_count;
        tf_spec__mark_wrapped(s, p, item, false);
        s->scratch_count = saved;
        tree = &s->trees[item];
      }
      if (tree->leaf) {
        TSStateId state = p->states[p->depth];
        TFToken token = tree->token;
        bool extra = tree->extra;
        if (!(recover ? tf_parser__shift_recover : tf_parser__shift_plain)(
                p, &token, extra, extra ? state : tf_next_state(p->lang, state, token.symbol))) {
          return false;
        }
        break;
      }
      uint32_t children = tree->child_count, at = tree->first_child;
      if (children == 0) {
        item |= 0x80000000U;
        continue;
      }
      if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, s->scratch_count + children)) {
        return false;
      }
      s->scratch[s->scratch_count++] = item | 0x80000000U;
      for (uint32_t i = children; i > 1; i--) {
        s->scratch[s->scratch_count++] = s->children[at + i - 1];
      }
      item = s->children[at];
    }
  }
  return true;
}

TF_NOINLINE static bool tf_spec__replay(TFSpec *s, TFParser *p, uint32_t first, uint32_t count) {
  if (s->recovering) {
    // Every extra ERROR in the forest that wraps cells already on the real stack.
    s->scratch_count = 0;
    for (uint32_t i = 0; i < count && !s->failed; i++) {
      if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, s->scratch_count + 1)) {
        return false;
      }
      s->scratch[s->scratch_count++] = s->children[first + i];
    }
    while (s->scratch_count && !s->failed) {
      uint32_t id = s->scratch[--s->scratch_count];
      const TFSpecTree *t = &s->trees[id];
      if (t->inherited || t->leaf) {
        continue;
      }
      if (tf_spec__wraps(t)) {
        tf_spec__mark_wrapped(s, p, id, true);
        continue;
      }
      if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, s->scratch_count + t->child_count)) {
        return false;
      }
      for (uint32_t i = 0; i < t->child_count; i++) {
        s->scratch[s->scratch_count++] = s->children[t->first_child + i];
      }
    }
    if (s->failed) {
      return false;
    }
  }
  return p->sink->on_error ? tf_spec__replay_as(s, p, first, count, true)
                           : tf_spec__replay_as(s, p, first, count, false);
}

// A private replay reaching the owner's fork: record the shapes of the cells
// below it and stop. True if this is the fork, whether or not recording worked.
static bool tf_spec__capture(TFSpec *c, TFParser *p, const TFToken *token) {
  const TFParser *owner = c->owner;
  if (p->split_count != owner->split_count || token->start_byte != c->fork_byte ||
      p->depth != owner->depth ||
      memcmp(p->states, owner->states, (p->depth + 1) * sizeof(TSStateId)) != 0) {
    return false;
  }
  if (!TF_SPEC_RESERVE(c, capture_id, capture_capacity, p->depth)) {
    return true;
  }
  for (uint32_t i = 0; i < p->depth; i++) {
    c->capture_id[i] = (uint32_t)(uintptr_t)p->nodes[i].value - 1;
  }
  c->captured = true;
  // Only the cells the fork has already unrolled need a shape; a later
  // tf_spec__extend takes its own from capture_id.
  for (uint32_t k = 0; k < c->prefix_count; k++) {
    tf_spec__resolve(c, k);
  }
  return true;
}

// Compiled twice on `recover`, like the dispatch loop: the checks recovery added,
// read at run time, cost 2% more instructions on SystemVerilog with recovery
// off. Both copies are out of line, since tf_parser.c's two dispatch loops
// would otherwise each inline one; that measured 1-2% slower.
//
// `lexed` is false when the ordinary lexer found nothing at all, and recovery
// has to start by lexing in error mode; `token` then only carries the position.
static TF_ALWAYS_INLINE bool tf_parser__split_as(TFParser *p, TFToken token, TFToken *next,
                                                 bool lexed, bool recover) {
  p->split_count++;
  // A replay whose collector ran out of memory has nothing left to find.
  if (p->capture && (p->capture->failed || tf_spec__capture(p->capture, p, &token))) {
    return false;
  }
  if (!p->spec) {
    p->spec = calloc(1, sizeof(TFSpec));
    if (!p->spec) {
      goto oom;
    }
  }
  TFSpec *s = p->spec;
  s->tree_count = 0;
  s->node_count = 0;
  s->link_count = 0;
  s->head_count = 0;
  s->child_count = 0;
  s->failed = false;
  s->has_error = false;
  s->finished = TF_SPEC_NONE;
  s->owner = p;
  s->fork_byte = token.start_byte;
  s->materialized = false;
  s->captured = false;
  s->cached = token;
  s->cached_state = p->lexer.token_lex_state;
  s->cached_keyword = p->lexer.token_is_keyword;
  s->has_cache = lexed;
  s->in_hand = lexed;
  s->paused_count = 0;
  s->cached_byte = p->depth ? p->nodes[p->depth - 1].end_byte : 0;
  s->recover = recover;
  s->recovering = false;
  s->stopped = false;
  s->accept_count = 0;
  s->summary_count = 0;
  s->prefix_count = 0;
  s->prefix_depth = p->depth;
  s->prefix_left = p->depth;
  if (!TF_SPEC_RESERVE(s, nodes, node_capacity, 1)) {
    goto oom;
  }
  // One node standing for the whole real stack; tf_spec__extend unrolls it.
  s->nodes[0] =
      (TFSpecNode){.state = p->states[p->depth],
                   .byte = s->cached_byte,
                   .point = p->depth ? p->nodes[p->depth - 1].end_point : (TFPoint){0, 0}};
  s->node_count = 1;
  s->frontier = 0;
  if (recover) {
    // Tree-sitter measures a version's cost and node count from the bottom of
    // the stack, and the real stack below the fork has its totals already.
    if (!TF_SPEC_RESERVE(s, node_cost, node_cost_capacity, 1)) {
      goto oom;
    }
    s->node_cost[0] = (TFSpecCost){.error_cost = p->total_error,
                                   .node_count = p->depth ? p->cells[p->depth - 1].stack_nodes : 0};
  }
  uint32_t top = 0;
  tf_spec__head(s, top, TF_SPEC_NONE, recover);
  if (s->failed) {
    goto oom;
  }
  uint32_t last_position = 0, first = 0, count = 0;
  if (recover) {
    last_position = p->last_position > s->cached_byte ? p->last_position : s->cached_byte;
  }
  bool accepted = false;
  for (;;) {
    for (uint32_t v = 0; v < s->head_count; v++) {
      while (s->heads[v].status == TF_SPEC_ACTIVE) {
        tf_spec__advance(s, p, v, recover);
        if (s->failed) {
          goto oom;
        }
        uint32_t pos = s->nodes[s->heads[v].node].byte;
        if (pos > last_position || (v > 0 && pos == last_position)) {
          last_position = pos;
          break;
        }
      }
    }
    uint32_t min_error_cost = tf_spec__condense(s, p, recover);
    if (s->failed) {
      goto oom;
    }
    if (s->stopped) {
      goto report;
    }
    // parser.c: a finished tree that no version still going can beat ends it.
    // Before recovery every cost is zero, so that is when no version is left.
    uint32_t finished_cost =
        recover && s->finished != TF_SPEC_NONE ? s->tree_cost[s->finished].error_cost : 0;
    if (s->finished != TF_SPEC_NONE && finished_cost < min_error_cost) {
      // The root's children and the extras around it, ending in the end token.
      first = s->trees[s->finished].first_child;
      count = s->trees[s->finished].child_count - 1;
      accepted = true;
      break;
    }
    if (!s->head_count) {
    report:
      if (s->error_is_lex) {
        tf_parser__fail(p, s->error_byte, s->error_point, "unexpected character");
      } else {
        TFToken bad = {
            .symbol = s->error_symbol, .start_byte = s->error_byte, .start_point = s->error_point};
        tf_parser__fail_unexpected(p, s->error_state, &bad);
      }
      return false;
    }
    if (s->head_count == 1 && s->finished == TF_SPEC_NONE && tf_spec__unique(s, s->heads[0].node)) {
      top = s->heads[0].node;
      if (recover) {
        // The ordinary parser cannot take an ERROR leaf as its lookahead, so
        // stay speculative until the next token is a real one.
        bool keyword;
        tf_spec__lex(s, p, top, next, &keyword);
        if (tf_parser__is_error(next->symbol)) {
          goto keep_going;
        }
      }
      tf_spec__pop(s, 0, TF_SPEC_NONE);
      if (s->failed) {
        goto oom;
      }
      first = s->slices[0].first;
      count = s->slices[0].count;
      break;
    }
  keep_going:;
  }
  if (accepted) {
    *next = s->cached;
    next->symbol = 0;
    next->missing = false;
    next->start_byte = next->end_byte = p->lexer.size;
    // EOF's point is retained by the accepted candidate, including whitespace.
    next->start_point = next->end_point = s->trees[s->finished].token.end_point;
  }
  p->last_position = last_position;
  if (!tf_spec__replay(s, p, first, count)) {
    goto oom;
  }
  if (accepted) {
    // Only now are the trailing extras on the stack, so the root is reduced here,
    // with the end token as lookahead, as the ordinary loop would.
    const TFSpecTree *root = &s->trees[s->finished];
    // An ERROR root's count excludes the end token, which is an extra anyway.
    if (!tf_parser__reduce(p, root->token.symbol, tf_spec__structural(s, root), root->production_id,
                           next)) {
      goto oom;
    }
    tf_lexer_seek(&p->lexer, next->end_byte, next->end_point);
    // An ERROR root lands in state 0, which has no Accept action for the end
    // token; tree-sitter accepts it outright (parser.c:/recover_eof/), and so
    // does the loop when it sees this.
    p->accepted = tf_parser__is_error(root->token.symbol);
  } else {
    // The replay counted the cells it pushed along the tree it chose. tree-sitter's
    // counts are the stack nodes', which a merge can have raised (stack.c:262).
    if (recover) {
      uint32_t node = top;
      for (uint32_t i = p->depth; i > 0 && s->nodes[node].link_count; i--) {
        p->cells[i - 1].stack_nodes = s->node_cost[node].node_count;
        node = s->links[s->nodes[node].first_link].node;
      }
    }
    bool keyword;
    if (!tf_spec__lex(s, p, top, next, &keyword)) {
      tf_parser__fail(p, s->error_byte, s->error_point, "unexpected character");
      return false;
    }
    tf_lexer_seek(&p->lexer, next->end_byte, next->end_point);
    p->lexer.token_is_keyword = keyword;
    p->lexer.token_lex_state = s->cached_state;
  }
  return true;
oom:
  tf_parser__fail(p, token.start_byte, token.start_point, "out of memory");
  return false;
}

TF_NOINLINE static bool tf_parser__split_plain(TFParser *p, TFToken token, TFToken *next,
                                               bool lexed) {
  return tf_parser__split_as(p, token, next, lexed, false);
}

TF_NOINLINE static bool tf_parser__split_recover(TFParser *p, TFToken token, TFToken *next,
                                                 bool lexed) {
  return tf_parser__split_as(p, token, next, lexed, true);
}

static bool tf_parser__split(TFParser *p, TFToken token, TFToken *next, bool lexed) {
  return p->sink->on_error ? tf_parser__split_recover(p, token, next, lexed)
                           : tf_parser__split_plain(p, token, next, lexed);
}
#endif
