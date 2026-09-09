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

// Value-free graph-structured stacks for speculative parsing. Included by
// tf_parser.c so the ordinary shift/reduce path and its sink remain private.
// The algorithms follow tree-sitter 0.27's stack.c and parser.c; error recovery,
// incremental reuse, and external scanners are deliberately absent.
#ifndef TF_PARSER_SPEC_H
#define TF_PARSER_SPEC_H

#define TF_SPEC_NONE UINT32_MAX
#define TF_SPEC_LINKS 8
#define TF_SPEC_ITERATORS 64
#define TF_SPEC_VERSIONS 6

typedef struct {
  TFToken token;
  uint32_t padding_start;
  uint32_t first_child, child_count, structural_count;
  uint16_t production_id;
  int64_t precedence;
  bool leaf, extra, inherited, opaque;
} TFSpecTree;

typedef struct {
  uint32_t node, tree;
} TFSpecLink;
typedef struct {
  TSStateId state;
  uint32_t byte;
  TFPoint point;
  int64_t precedence;
  TFSpecLink links[TF_SPEC_LINKS];
  uint32_t link_count;
} TFSpecNode;
typedef struct {
  uint32_t node;
  bool halted, errored;
} TFSpecHead;
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
  TFSpecHead *heads;
  uint32_t head_count, head_capacity;
  uint32_t *children;
  uint32_t child_count, child_capacity;
  TFSpecEdge *edges;
  uint32_t edge_count, edge_capacity;
  TFSpecSlice *slices;
  uint32_t slice_count, slice_capacity;
  uint32_t *scratch;
  uint32_t scratch_count, scratch_capacity;
  bool failed, materialized, captured;
  TFParser *owner;
  uint32_t fork_byte;
  TFToken cached;
  uint32_t cached_byte;
  TSStateId cached_state;
  bool has_cache, cached_keyword;
  uint32_t finished, finished_first, finished_count;
  uint32_t error_byte;
  TFPoint error_point;
  TSStateId error_state;
  TSSymbol error_symbol;
  bool has_error, error_is_lex;
};

static bool tf_spec__reserve(TFSpec *s, void **array, uint32_t *capacity, uint32_t needed,
                             size_t size) {
  if (s->failed) return false;
  if (needed <= *capacity) return true;
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
  if (!s) return;
  free(s->trees);
  free(s->nodes);
  free(s->heads);
  free(s->children);
  free(s->edges);
  free(s->slices);
  free(s->scratch);
  free(s);
}

static uint32_t tf_spec__tree(TFSpec *s, TFSpecTree tree) {
  // Replay reserves the high bit for its postorder marker.
  if (s->tree_count >= 0x80000000U) {
    s->failed = true;
    return TF_SPEC_NONE;
  }
  if (!TF_SPEC_RESERVE(s, trees, tree_capacity, s->tree_count + 1)) return TF_SPEC_NONE;
  s->trees[s->tree_count] = tree;
  return s->tree_count++;
}

static uint32_t tf_spec__head(TFSpec *s, uint32_t node) {
  if (!TF_SPEC_RESERVE(s, heads, head_capacity, s->head_count + 1)) return TF_SPEC_NONE;
  s->heads[s->head_count] = (TFSpecHead){.node = node};
  return s->head_count++;
}

static void tf_spec__remove_head(TFSpec *s, uint32_t v) {
  memmove(s->heads + v, s->heads + v + 1, (s->head_count - v - 1) * sizeof(*s->heads));
  s->head_count--;
}

static uint32_t tf_spec__push(TFSpec *s, uint32_t previous, uint32_t tree, TSStateId state) {
  if (!TF_SPEC_RESERVE(s, nodes, node_capacity, s->node_count + 1)) return TF_SPEC_NONE;
  TFSpecTree t = s->trees[tree];
  TFSpecNode node = {.state = state,
                     .byte = t.token.end_byte,
                     .point = t.token.end_point,
                     .precedence = s->nodes[previous].precedence + t.precedence,
                     .links = {{previous, tree}},
                     .link_count = 1};
  s->nodes[s->node_count] = node;
  return s->node_count++;
}

static bool tf_spec__materialize(TFSpec *s);

// stack.c:stack__subtree_is_equivalent. Equivalent links keep the existing
// tree on a precedence tie; this is intentionally NOT ts_subtree_compare.
static bool tf_spec__equivalent(TFSpec *s, uint32_t a, uint32_t b) {
  if (a == b) return true;
  TFSpecTree x = s->trees[a], y = s->trees[b];
  if (x.token.symbol != y.token.symbol || x.extra != y.extra ||
      x.token.start_byte - x.padding_start != y.token.start_byte - y.padding_start ||
      x.token.end_byte - x.token.start_byte != y.token.end_byte - y.token.start_byte)
    return false;
  if ((x.opaque || y.opaque) && !tf_spec__materialize(s)) {
    s->failed = true;
    return false;
  }
  return s->trees[a].child_count == s->trees[b].child_count;
}

// stack.c:stack_node_add_link. Recursively merging predecessors preserves
// alternatives below the top state instead of discarding an entire stack.
static void tf_spec__add_link(TFSpec *s, uint32_t target, TFSpecLink link) {
  if (link.node == target) return;
  TFSpecNode *node = &s->nodes[target];
  for (uint32_t i = 0; i < node->link_count; i++) {
    TFSpecLink existing = node->links[i];
    if (!tf_spec__equivalent(s, existing.tree, link.tree)) continue;
    if (existing.node == link.node) {
      if (s->trees[link.tree].precedence > s->trees[existing.tree].precedence) {
        node->links[i].tree = link.tree;
        node->precedence = s->nodes[link.node].precedence + s->trees[link.tree].precedence;
      }
      return;
    }
    if (s->nodes[existing.node].state == s->nodes[link.node].state &&
        s->nodes[existing.node].byte == s->nodes[link.node].byte) {
      TFSpecNode other = s->nodes[link.node];
      for (uint32_t j = 0; j < other.link_count; j++)
        tf_spec__add_link(s, existing.node, other.links[j]);
      int64_t prec = s->nodes[link.node].precedence + s->trees[link.tree].precedence;
      if (prec > node->precedence) node->precedence = prec;
      return;
    }
  }
  if (node->link_count == TF_SPEC_LINKS) return;  // stack.c:MAX_LINK_COUNT
  node->links[node->link_count++] = link;
  int64_t prec = s->nodes[link.node].precedence + s->trees[link.tree].precedence;
  if (prec > node->precedence) node->precedence = prec;
}

static bool tf_spec__merge(TFSpec *s, uint32_t a, uint32_t b) {
  TFSpecHead x = s->heads[a], y = s->heads[b];
  if (x.halted || y.halted || s->nodes[x.node].state != s->nodes[y.node].state ||
      s->nodes[x.node].byte != s->nodes[y.node].byte)
    return false;
  TFSpecNode other = s->nodes[y.node];
  for (uint32_t i = 0; i < other.link_count; i++) tf_spec__add_link(s, x.node, other.links[i]);
  tf_spec__remove_head(s, b);
  return true;
}

// subtree.c:ts_subtree_compare. Used only when popping different paths to the
// same predecessor (parser.c:ts_parser__select_children), or selecting a root.
static bool tf_spec__prefer(TFSpec *s, uint32_t a, uint32_t b) {
  if (a == TF_SPEC_NONE) return true;
  if (s->trees[a].precedence != s->trees[b].precedence)
    return s->trees[b].precedence > s->trees[a].precedence;
  s->scratch_count = 0;
  if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, 2)) return false;
  s->scratch[s->scratch_count++] = a;
  s->scratch[s->scratch_count++] = b;
  while (s->scratch_count) {
    uint32_t right = s->scratch[--s->scratch_count], left = s->scratch[--s->scratch_count];
    if (left == right) continue;
    TFSpecTree x = s->trees[left], y = s->trees[right];
    if (x.token.symbol != y.token.symbol) return y.token.symbol < x.token.symbol;
    if (x.opaque || y.opaque) {
      if (!tf_spec__materialize(s)) {
        s->failed = true;
        return false;
      }
      x = s->trees[left];
      y = s->trees[right];
    }
    if (x.child_count != y.child_count) return y.child_count < x.child_count;
    if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, s->scratch_count + 2 * x.child_count))
      return false;
    for (uint32_t i = x.child_count; i > 0; i--) {
      s->scratch[s->scratch_count++] = s->children[x.first_child + i - 1];
      s->scratch[s->scratch_count++] = s->children[y.first_child + i - 1];
    }
  }
  return false;
}

static uint32_t tf_spec__parent(TFSpec *s, TSSymbol symbol, uint16_t production,
                                uint32_t structural, uint32_t first, uint32_t count,
                                uint32_t base) {
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
    TFSpecTree left = s->trees[s->children[first]],
               right = s->trees[s->children[first + count - 1]];
    tree.token.start_byte = left.token.start_byte;
    tree.token.start_point = left.token.start_point;
    tree.token.end_byte = right.token.end_byte;
    tree.token.end_point = right.token.end_point;
    tree.padding_start = left.padding_start;
    for (uint32_t i = 0; i < count; i++)
      tree.precedence += s->trees[s->children[first + i]].precedence;
  }
  return tf_spec__tree(s, tree);
}

// stack.c:stack__iter and ts_stack__add_slice. The breadth-first visitation
// order matters when equivalent alternatives have equal precedence.
static void tf_spec__pop(TFSpec *s, uint32_t version, uint32_t goal) {
  TFSpecIterator it[TF_SPEC_ITERATORS] = {{s->heads[version].node, TF_SPEC_NONE, 0, 0}};
  uint32_t length = 1;
  s->slice_count = 0;
  s->edge_count = 0;
  while (length && !s->failed) {
    for (uint32_t i = 0, size = length; i < size; i++) {
      TFSpecIterator current = it[i];
      TFSpecNode node = s->nodes[current.node];
      bool pop = goal == TF_SPEC_NONE ? node.link_count == 0 : current.count == goal;
      if (pop) {
        if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + current.length) ||
            !TF_SPEC_RESERVE(s, slices, slice_capacity, s->slice_count + 1))
          return;
        uint32_t first = s->child_count;
        for (uint32_t edge = current.edge; edge != TF_SPEC_NONE; edge = s->edges[edge].previous)
          s->children[s->child_count++] = s->edges[edge].tree;
        uint32_t v = TF_SPEC_NONE, slot = s->slice_count;
        for (uint32_t j = s->slice_count; j > 0; j--) {
          if (s->heads[s->slices[j - 1].version].node == current.node) {
            v = s->slices[j - 1].version;
            slot = j;
            break;
          }
        }
        if (v == TF_SPEC_NONE) v = tf_spec__head(s, current.node);
        if (s->failed) return;
        memmove(s->slices + slot + 1, s->slices + slot,
                (s->slice_count - slot) * sizeof(*s->slices));
        s->slices[slot] = (TFSpecSlice){v, first, current.length};
        s->slice_count++;
      }
      if (pop || node.link_count == 0) {
        memmove(it + i, it + i + 1, (length - i - 1) * sizeof(*it));
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
          link = node.links[0];
        } else {
          if (length == TF_SPEC_ITERATORS) continue;
          slot = length++;
          link = node.links[j];
        }
        if (!TF_SPEC_RESERVE(s, edges, edge_capacity, s->edge_count + 1)) return;
        s->edges[s->edge_count] = (TFSpecEdge){link.tree, current.edge};
        it[slot] = (TFSpecIterator){link.node, s->edge_count++,
                                    current.count + !s->trees[link.tree].extra, current.length + 1};
      }
    }
  }
}

static uint32_t tf_spec__reduce(TFSpec *s, const TFLanguage *lang, uint32_t version,
                                TSParseAction action) {
  uint32_t initial = s->head_count, removed = 0, halted = 0;
  for (uint32_t i = 0; i < s->head_count; i++) halted += s->heads[i].halted && !s->heads[i].errored;
  tf_spec__pop(s, version, action.reduce.child_count);
  for (uint32_t i = 0; i < s->slice_count && !s->failed; i++) {
    TFSpecSlice slice = s->slices[i];
    uint32_t v = slice.version - removed;
    if (v > TF_SPEC_VERSIONS + 4 + halted) {
      tf_spec__remove_head(s, v);
      removed++;
      while (i + 1 < s->slice_count && s->slices[i + 1].version == slice.version) i++;
      continue;
    }
    uint32_t base = s->heads[v].node;
    uint32_t count = slice.count;
    while (count && s->trees[s->children[slice.first + count - 1]].extra) count--;
    uint32_t parent = tf_spec__parent(s, action.reduce.symbol, action.reduce.production_id,
                                      action.reduce.child_count, slice.first, count, base);
    if (s->failed) break;
    uint32_t trailing_first = slice.first + count, trailing_count = slice.count - count;
    while (i + 1 < s->slice_count && s->slices[i + 1].version == slice.version) {
      TFSpecSlice other = s->slices[++i];
      count = other.count;
      while (count && s->trees[s->children[other.first + count - 1]].extra) count--;
      uint32_t candidate = tf_spec__parent(s, action.reduce.symbol, action.reduce.production_id,
                                           action.reduce.child_count, other.first, count, base);
      if (s->failed) return TF_SPEC_NONE;
      if (tf_spec__prefer(s, parent, candidate)) {
        parent = candidate;
        trailing_first = other.first + count;
        trailing_count = other.count - count;
      }
    }
    s->trees[parent].precedence += action.reduce.dynamic_precedence;
    TSStateId next = tf_next_state(lang, s->nodes[base].state, action.reduce.symbol);
    uint32_t top = tf_spec__push(s, base, parent, next);
    for (uint32_t j = 0; j < trailing_count && !s->failed; j++)
      top = tf_spec__push(s, top, s->children[trailing_first + j], next);
    if (s->failed) return TF_SPEC_NONE;
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
  if (s->has_error && byte <= s->error_byte) return;
  s->has_error = true;
  s->error_byte = byte;
  s->error_point = point;
  s->error_state = state;
  s->error_symbol = symbol;
  s->error_is_lex = lex;
}

// parser.c:ts_parser__can_reuse_first_leaf and ts_parser__get_cached_token.
static bool tf_spec__lex(TFSpec *s, TFParser *p, uint32_t node, TFToken *token, bool *keyword) {
  TFSpecNode n = s->nodes[node];
  if (s->has_cache && s->cached_byte == n.byte) {
    uint32_t count;
    tf_actions(p->lang, n.state, s->cached.symbol, &count);
    TSLexerMode a = tf_lex_mode(p->lang, n.state), b = tf_lex_mode(p->lang, s->cached_state);
    bool reusable = count && memcmp(&a, &b, sizeof(a)) == 0 &&
                    (s->cached.symbol != p->lang->ts->keyword_capture_token ||
                     (!s->cached_keyword && s->cached_state == n.state));
    if (!reusable && (s->cached.end_byte > s->cached.start_byte || s->cached.symbol == 0))
      reusable =
          p->lang->ts->parse_actions[tf_lookup(p->lang, n.state, s->cached.symbol)].entry.reusable;
    if (reusable) {
      *token = s->cached;
      *keyword = s->cached_keyword;
      return true;
    }
  }
  tf_lexer_seek(&p->lexer, n.byte, n.point);
  if (!tf_lexer_next(&p->lexer, n.state, token)) {
    tf_spec__error(s, p->lexer.byte, p->lexer.point, n.state, 0, true);
    return false;
  }
  *keyword = p->lexer.token_is_keyword;
  s->cached = *token;
  s->cached_byte = n.byte;
  s->cached_state = n.state;
  s->cached_keyword = *keyword;
  s->has_cache = true;
  return true;
}

static void tf_spec__accept(TFSpec *s, uint32_t version, TFToken eof) {
  uint32_t top = s->heads[version].node;
  uint32_t leaf = tf_spec__tree(
      s,
      (TFSpecTree){.token = eof, .padding_start = s->nodes[top].byte, .extra = true, .leaf = true});
  if (s->failed) return;
  s->heads[version].node = tf_spec__push(s, top, leaf, 1);
  if (s->failed) return;
  tf_spec__pop(s, version, TF_SPEC_NONE);
  for (uint32_t i = 0; i < s->slice_count && !s->failed; i++) {
    TFSpecSlice slice = s->slices[i];
    for (uint32_t j = slice.count; j > 0; j--) {
      TFSpecTree root = s->trees[s->children[slice.first + j - 1]];
      if (root.extra) continue;
      if (root.opaque) {
        if (!tf_spec__materialize(s)) {
          s->failed = true;
          return;
        }
        root = s->trees[s->children[slice.first + j - 1]];
      }
      uint32_t count = slice.count - 1 + root.child_count, first = s->child_count;
      if (!TF_SPEC_RESERVE(s, children, child_capacity, s->child_count + count)) return;
      for (uint32_t k = 0; k < slice.count; k++) {
        if (k == j - 1) {
          for (uint32_t c = 0; c < root.child_count; c++)
            s->children[s->child_count++] = s->children[root.first_child + c];
        } else
          s->children[s->child_count++] = s->children[slice.first + k];
      }
      uint32_t candidate = tf_spec__parent(s, root.token.symbol, root.production_id,
                                           root.structural_count, first, count, 0);
      if (s->failed) return;
      if (tf_spec__prefer(s, s->finished, candidate)) {
        s->finished = candidate;
        s->finished_first = slice.first;
        s->finished_count = slice.count - 1;
      }
      break;
    }
  }
  if (s->slice_count) tf_spec__remove_head(s, s->slices[0].version);
  s->heads[version].halted = true;
}

// parser.c:ts_parser__advance, with failed heads halted instead of recovered.
static void tf_spec__advance(TFSpec *s, TFParser *p, uint32_t version) {
  TFToken token;
  bool keyword;
  if (!tf_spec__lex(s, p, s->heads[version].node, &token, &keyword)) {
    s->heads[version].halted = true;
    s->heads[version].errored = true;
    return;
  }
  for (;;) {
    TSStateId state = s->nodes[s->heads[version].node].state;
    uint32_t count, last = TF_SPEC_NONE;
    const TSParseAction *actions = tf_actions(p->lang, state, token.symbol, &count);
    bool reduced = false;
    for (uint32_t i = 0; i < count; i++) {
      TSParseAction action = actions[i];
      if (action.type == TSParseActionTypeShift) {
        uint32_t top = s->heads[version].node;
        uint32_t leaf = tf_spec__tree(s, (TFSpecTree){.token = token,
                                                      .padding_start = s->nodes[top].byte,
                                                      .extra = action.shift.extra,
                                                      .leaf = true});
        if (s->failed) return;
        s->heads[version].node =
            tf_spec__push(s, top, leaf, action.shift.extra ? state : action.shift.state);
        return;
      }
      if (action.type == TSParseActionTypeReduce) {
        uint32_t v = tf_spec__reduce(s, p->lang, version, action);
        if (s->failed) return;
        reduced = true;
        if (v != TF_SPEC_NONE) last = v;
      } else if (action.type == TSParseActionTypeAccept) {
        tf_spec__accept(s, version, token);
        return;
      }
    }
    if (last != TF_SPEC_NONE) {
      s->heads[version] = s->heads[last];
      tf_spec__remove_head(s, last);
      continue;
    }
    if (reduced) {
      s->heads[version].halted = true;
      return;
    }
    if (tf_parser__demote_keyword(p->lang, state, &token, keyword)) continue;
    tf_spec__error(s, token.start_byte, token.start_point, state, token.symbol, false);
    s->heads[version].halted = true;
    s->heads[version].errored = true;
    return;
  }
}

// parser.c:ts_parser__condense_stack, restricted to error-free candidates.
static void tf_spec__condense(TFSpec *s) {
  for (uint32_t i = 0; i < s->head_count; i++) {
    if (s->heads[i].halted) {
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
        TFSpecHead swap = s->heads[i];
        s->heads[i] = s->heads[j];
        s->heads[j] = swap;
      }
    }
  }
  if (s->head_count > TF_SPEC_VERSIONS) s->head_count = TF_SPEC_VERSIONS;
}

static bool tf_spec__unique(const TFSpec *s, uint32_t node) {
  while (s->nodes[node].link_count) {
    if (s->nodes[node].link_count != 1) return false;
    node = s->nodes[node].links[0].node;
  }
  return true;
}

// A selected forest is replayed in postorder. Inherited cells are already on
// the real stack, so only speculative shifts and reductions reach the sink.
static bool tf_spec__replay(TFSpec *s, TFParser *p, uint32_t first, uint32_t count) {
  s->scratch_count = 0;
  if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, count)) return false;
  for (uint32_t i = count; i > 0; i--) s->scratch[s->scratch_count++] = s->children[first + i - 1];
  while (s->scratch_count) {
    uint32_t id = s->scratch[--s->scratch_count];
    bool finish = (id & 0x80000000U) != 0;
    id &= 0x7fffffffU;
    TFSpecTree tree = s->trees[id];
    if (tree.inherited) continue;
    if (finish) {
      if (!tf_parser__reduce(p, tree.token.symbol, tree.structural_count, tree.production_id))
        return false;
    } else if (tree.leaf) {
      TSStateId state = p->states[p->depth];
      if (!tf_parser__shift(p, &tree.token, tree.extra,
                            tree.extra ? state : tf_next_state(p->lang, state, tree.token.symbol)))
        return false;
    } else {
      if (!TF_SPEC_RESERVE(s, scratch, scratch_capacity, s->scratch_count + tree.child_count + 1))
        return false;
      s->scratch[s->scratch_count++] = id | 0x80000000U;
      for (uint32_t i = tree.child_count; i > 0; i--)
        s->scratch[s->scratch_count++] = s->children[tree.first_child + i - 1];
    }
  }
  return true;
}

static bool tf_parser__split(TFParser *p, TFToken token, TFToken *next) {
  if (!p->spec) {
    p->spec = calloc(1, sizeof(TFSpec));
    if (!p->spec) goto oom;
  }
  TFSpec *s = p->spec;
  s->tree_count = 0;
  s->node_count = 0;
  s->head_count = 0;
  s->child_count = 0;
  s->slice_count = 0;
  s->edge_count = 0;
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
  s->has_cache = true;
  s->cached_byte = p->depth ? p->nodes[p->depth - 1].end_byte : 0;
  if (!TF_SPEC_RESERVE(s, nodes, node_capacity, 1)) goto oom;
  s->nodes[s->node_count++] = (TFSpecNode){.state = 1};
  uint32_t top = 0;
  for (uint32_t i = 0; i < p->depth; i++) {
    TFNode n = p->nodes[i];
    uint32_t tree = tf_spec__tree(s, (TFSpecTree){.token = {.symbol = n.symbol,
                                                            .start_byte = n.start_byte,
                                                            .end_byte = n.end_byte,
                                                            .start_point = n.start_point,
                                                            .end_point = n.end_point},
                                                  .padding_start = s->nodes[top].byte,
                                                  .child_count = TF_SPEC_NONE,
                                                  .extra = n.extra,
                                                  .inherited = true,
                                                  .opaque = true});
    if (s->failed) goto oom;
    top = tf_spec__push(s, top, tree, p->states[i + 1]);
    if (s->failed) goto oom;
  }
  tf_spec__head(s, top);
  if (s->failed) goto oom;
  uint32_t last_position = 0, peak = 1, first = 0, count = 0;
  bool accepted = false;
  for (;;) {
    for (uint32_t v = 0; v < s->head_count; v++) {
      while (!s->heads[v].halted) {
        tf_spec__advance(s, p, v);
        if (s->failed) goto oom;
        if (s->head_count > peak) peak = s->head_count;
        uint32_t pos = s->nodes[s->heads[v].node].byte;
        if (pos > last_position || (v > 0 && pos == last_position)) {
          last_position = pos;
          break;
        }
      }
    }
    tf_spec__condense(s);
    if (s->finished != TF_SPEC_NONE && s->head_count == 0) {
      first = s->finished_first;
      count = s->finished_count;
      accepted = true;
      break;
    }
    if (!s->head_count) {
      if (s->error_is_lex)
        tf_parser__fail(p, s->error_byte, s->error_point, "unexpected character");
      else {
        TFToken bad = {
            .symbol = s->error_symbol, .start_byte = s->error_byte, .start_point = s->error_point};
        tf_parser__fail_unexpected(p, s->error_state, &bad);
      }
      return false;
    }
    if (s->head_count == 1 && s->finished == TF_SPEC_NONE && tf_spec__unique(s, s->heads[0].node)) {
      top = s->heads[0].node;
      tf_spec__pop(s, 0, TF_SPEC_NONE);
      if (s->failed) goto oom;
      first = s->slices[0].first;
      count = s->slices[0].count;
      break;
    }
  }
  if (!tf_spec__replay(s, p, first, count)) goto oom;
  if (p->report_splits)
    fprintf(stderr, "split: byte=%u peak-heads=%u nodes=%u trees=%u\n", token.start_byte, peak,
            s->node_count, s->tree_count);
  if (accepted) {
    *next = s->cached;
    next->symbol = 0;
    next->start_byte = next->end_byte = p->lexer.size;
    // EOF's point is retained by the accepted candidate, including whitespace.
    next->start_point = next->end_point = s->trees[s->finished].token.end_point;
    tf_lexer_seek(&p->lexer, next->end_byte, next->end_point);
  } else {
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
#undef TF_SPEC_RESERVE
#endif
