// The visible-node filter: a raw sink that applies tree-sitter's visibility,
// alias and field rules and forwards what survives.
//
// Three rules do the work, all of them tree-sitter's:
//   * a child is visible iff its own symbol is visible or the parent's production
//     aliases it (node.c:/ts_node__is_relevant/);
//   * a hidden child contributes its own visible children in its place, so
//     `_expression` and `aux_sym_*_repeat1` disappear and their contents are
//     inlined into the nearest visible ancestor;
//   * aliases and fields are indexed by `structural_index`, which counts only
//     non-extra children (subtree.c:437).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tf_language.h"

// What the filter has to remember about a stack cell, beyond what the driver's
// own TFNode already carries (symbol, extra, byte and point spans): how many
// settled entries it owns at the top of the arena, and which production built
// it. Both fit in the pointer the driver stores on our behalf, so there is
// nothing to allocate -- on a data file that is four allocations per value
// parsed which simply do not happen.
//
typedef struct {
  uint32_t children;
  uint16_t production;
} TFVisibleCell;

#if UINTPTR_MAX >= UINT64_MAX && !defined(TF_VISIBLE_FORCE_CELL_ARENA)
// The tag bit keeps the value non-NULL, which the driver treats as "no value".
// NOLINTBEGIN(performance-no-int-to-ptr): the pointer is the storage, not a
// pointer to it. That is the whole point -- see above.
#define TF_PACK(children, production) \
  ((void *)(((uintptr_t)(children) << 17) | ((uintptr_t)(production) << 1) | 1u))
// NOLINTEND(performance-no-int-to-ptr)
#define TF_USE_PACKED_CELLS 1
#else
#define TF_USE_PACKED_CELLS 0
#endif

typedef struct {
  const TFLanguage *lang;
  const TFVisibleSink *sink;

  // Settled visible children, as a stack: a cell's entries are always the run
  // immediately below the top, in source order.
  TFVisibleChild *arena;
  uint32_t arena_len, arena_capacity;
  TFVisibleChild *scratch;
  uint32_t scratch_capacity;

#if !TF_USE_PACKED_CELLS
  // Parser values are stable, non-zero, one-based indices into this arena.
  // They remain valid when realloc moves the allocation.
  TFVisibleCell *cells;
  uint32_t cells_len, cells_capacity;
#endif

  // Symbols whose runs the consumer has already refused to fold. Offering a run
  // means handing over every child in it, and a repetition's run grows by one
  // each time it reduces -- so re-offering a symbol that was declined once costs
  // O(n^2) over the list for an answer that has not changed. One byte per
  // symbol, allocated only if a fold is declined at all.
  uint8_t *declined;

  // The most recent reduction. The last one is the root's, and the root has no
  // parent to describe it -- the driver hands back only the packed value.
  TSSymbol root_symbol;
  uint16_t root_production_id;
  uint32_t root_start_byte, root_end_byte;
  TFPoint root_start_point, root_end_point;

  bool failed;
} TFFilter;

static TFVisibleCell tf_filter__cell(const TFFilter *self, const void *value) {
#if TF_USE_PACKED_CELLS
  (void)self;
  uintptr_t packed = (uintptr_t)value;
  return (TFVisibleCell){.children = (uint32_t)(packed >> 17),
                         .production = (uint16_t)((packed >> 1) & 0xFFFFU)};
#else
  uintptr_t index = (uintptr_t)value;
  if (index == 0 || index > self->cells_len) return (TFVisibleCell){0};
  return self->cells[index - 1];
#endif
}

static void *tf_filter__store_cell(TFFilter *self, uint32_t children, uint16_t production) {
#if TF_USE_PACKED_CELLS
  (void)self;
  return TF_PACK(children, production);
#else
  if (self->cells_len == UINT32_MAX) {
    self->failed = true;
    return NULL;
  }
  if (self->cells_len == self->cells_capacity) {
    uint32_t next = self->cells_capacity ? self->cells_capacity * 2 : 64;
    if (next < self->cells_capacity) next = UINT32_MAX;
    if ((size_t)next > SIZE_MAX / sizeof(TFVisibleCell)) {
      self->failed = true;
      return NULL;
    }
    TFVisibleCell *grown = realloc(self->cells, (size_t)next * sizeof(TFVisibleCell));
    if (!grown) {
      self->failed = true;
      return NULL;
    }
    self->cells = grown;
    self->cells_capacity = next;
  }
  self->cells[self->cells_len++] = (TFVisibleCell){.children = children, .production = production};
  // The integer is a stable, one-based arena handle, not an object pointer.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return (void *)(uintptr_t)self->cells_len;
#endif
}

// The run of settled entries at `position`, as the sink sees it.
//
// A node with no visible children is reported before the arena has ever been
// allocated -- the very first reduction of a parse, if its children are all
// hidden -- and `&arena[0]` on a null arena is undefined. clang 18's
// UndefinedBehaviorSanitizer calls it "applying zero offset to null pointer";
// Apple clang and gcc do not report it, which is why it stood for so long.
//
// It stands in for an empty run rather than a null because `children` is
// documented as a pointer to `child_count` entries: a null would be a licence
// for `memcpy(dst, node->children, 0)` in a consumer to be undefined in turn,
// and Rust's `slice::from_raw_parts` requires non-null even for an empty slice.
static const TFVisibleChild tf_filter__no_children;

static const TFVisibleChild *tf_filter__run(const TFFilter *self, uint32_t position) {
  return self->arena ? &self->arena[position] : &tf_filter__no_children;
}

static bool tf_filter__reserve(TFVisibleChild **array, uint32_t *capacity, uint32_t needed) {
  if (needed <= *capacity) return true;
  uint32_t next = *capacity ? *capacity : 64;
  while (next < needed) next *= 2;
  TFVisibleChild *grown = realloc(*array, next * sizeof(TFVisibleChild));
  if (!grown) return false;
  *array = grown;
  *capacity = next;
  return true;
}

static void *tf_filter__on_shift(void *payload, const TFToken *token, bool extra) {
  TFFilter *self = payload;
  (void)token;
  (void)extra;
  // A token owns no settled entries and was built by no production.
  return tf_filter__store_cell(self, 0, 0);
}

static void *tf_filter__on_reduce(void *payload, const TFReduction *reduction) {
  TFFilter *self = payload;
  if (self->failed) return NULL;
  const TFLanguage *lang = self->lang;
  uint16_t production_id = reduction->production_id;

  uint32_t total = 0;
  for (uint32_t i = 0; i < reduction->node_count; i++) {
    total += tf_filter__cell(self, reduction->children[i].value).children;
  }
  uint32_t base = self->arena_len - total;
  uint32_t position = base;

  // A leading run of hidden children keeps its entries exactly where they are.
  // This is what stops a left-recursive repetition from being O(n^2): every
  // `repeat1 -> repeat1 x` reduction copies one entry, not the whole run.
  uint32_t index = 0, structural = 0;
  for (; index < reduction->node_count; index++) {
    const TFNode *child = &reduction->children[index];
    uint32_t owns = tf_filter__cell(self, child->value).children;
    TSSymbol alias = child->extra ? 0 : tf_alias_at(lang, production_id, structural);
    if (alias || tf_symbol_metadata(lang, child->symbol).visible) break;
    TSFieldId field = child->extra ? 0 : tf_field_at(lang, production_id, structural);
    if (field) {
      for (uint32_t i = 0; i < owns; i++) {
        TFVisibleChild *entry = &self->arena[position + i];
        if (!entry->extra && !entry->field_id) entry->field_id = field;
      }
    }
    position += owns;
    if (!child->extra) structural++;
  }
  uint32_t settled = position;

  // From the first visible child on, entries move, so build into scratch and
  // copy back over the region that has been read.
  uint32_t produced = 0;
  for (; index < reduction->node_count; index++) {
    const TFNode *child = &reduction->children[index];
    TFVisibleCell cell = tf_filter__cell(self, child->value);
    uint32_t owns = cell.children;
    TSSymbol alias = child->extra ? 0 : tf_alias_at(lang, production_id, structural);
    TSSymbolMetadata metadata = tf_symbol_metadata(lang, child->symbol);
    TSFieldId field = child->extra ? 0 : tf_field_at(lang, production_id, structural);

    if (alias || metadata.visible) {
      bool named = alias ? tf_symbol_metadata(lang, alias).named : metadata.named;
      // Punctuation the consumer said it does not want. Leaves only: a node with
      // children would take them with it.
      if (self->sink->named_only && !named && !field && owns == 0) {
        if (!child->extra) structural++;
        continue;
      }
      TFVisibleNode node = {
          .symbol = tf_public_symbol(lang, alias ? alias : child->symbol),
          .production_id = cell.production,
          .named = named,
          .extra = child->extra,
          .start_byte = child->start_byte,
          .end_byte = child->end_byte,
          .start_point = child->start_point,
          .end_point = child->end_point,
          .child_count = owns,
          .children = tf_filter__run(self, position),
      };
      if (!tf_filter__reserve(&self->scratch, &self->scratch_capacity, produced + 1)) {
        self->failed = true;
        return NULL;
      }
      self->scratch[produced++] = (TFVisibleChild){
          .symbol = node.symbol,
          .field_id = field,
          .extra = child->extra,
          .value = self->sink->on_node ? self->sink->on_node(self->sink->payload, &node) : NULL,
      };
    } else {
      if (!tf_filter__reserve(&self->scratch, &self->scratch_capacity, produced + owns)) {
        self->failed = true;
        return NULL;
      }
      for (uint32_t i = 0; i < owns; i++) {
        TFVisibleChild entry = self->arena[position + i];
        // tree_cursor.c:672 stops the walk at an extra, so an extra never picks
        // up a field from a hidden ancestor.
        if (!entry.extra && !entry.field_id) entry.field_id = field;
        self->scratch[produced++] = entry;
      }
    }

    position += owns;
    if (!child->extra) structural++;
  }

  // Most reductions produce nothing new -- their children are all hidden and
  // already sit where they belong -- so neither the growth check nor the copy is
  // worth paying for at one per reduction.
  if (produced > 0) {
    if (!tf_filter__reserve(&self->arena, &self->arena_capacity, settled + produced)) {
      self->failed = true;
      return NULL;
    }
    memcpy(&self->arena[settled], self->scratch, produced * sizeof(TFVisibleChild));
  }
  self->arena_len = settled + produced;

  // Offer a hidden run to the consumer to fold. Only worth it above one entry,
  // which also keeps supertypes out of it: those always wrap exactly one child,
  // so there is nothing to collapse.
  uint32_t owned = self->arena_len - base;
  if (owned > 1 && self->sink->on_hidden && tf_foldable(lang, reduction->symbol) &&
      !(self->declined && self->declined[reduction->symbol])) {
    TFVisibleNode node = {
        .symbol = tf_public_symbol(lang, reduction->symbol),
        .production_id = production_id,
        .named = false,
        .extra = false,
        .start_byte = reduction->start_byte,
        .end_byte = reduction->end_byte,
        .start_point = reduction->start_point,
        .end_point = reduction->end_point,
        .child_count = owned,
        .children = tf_filter__run(self, base),
    };
    void *folded = self->sink->on_hidden(self->sink->payload, &node);
    if (!folded) {
      // Declined. Take that as the answer for this symbol and stop asking.
      if (!self->declined) self->declined = calloc(lang->ts->symbol_count, sizeof(uint8_t));
      if (self->declined) self->declined[reduction->symbol] = 1;
    }
    if (folded) {
      self->arena[base] = (TFVisibleChild){
          .symbol = node.symbol,
          .field_id = 0,
          .extra = false,
          .value = folded,
      };
      self->arena_len = base + 1;
    }
  }

  // The last reduction to happen is the root's, so keeping the most recent one
  // is enough to describe it later.
  self->root_symbol = reduction->symbol;
  self->root_production_id = production_id;
  self->root_start_byte = reduction->start_byte;
  self->root_end_byte = reduction->end_byte;
  self->root_start_point = reduction->start_point;
  self->root_end_point = reduction->end_point;

  return tf_filter__store_cell(self, self->arena_len - base, production_id);
}

bool tf_parse_visible(const TFLanguage *lang, const void *source, size_t size,
                      const TFVisibleSink *sink, void **root, TFError *error) {
  static const TFVisibleSink no_sink = {0};
  TFFilter self = {.lang = lang, .sink = sink ? sink : &no_sink};
  TFSink raw = {
      .payload = &self, .on_shift = tf_filter__on_shift, .on_reduce = tf_filter__on_reduce};

  void *raw_root = NULL;
  bool ok = tf_parse(lang, source, size, &raw, &raw_root, error);

  if (ok && self.failed) {
    ok = false;
    if (error) snprintf(error->message, TF_ERROR_MESSAGE_SIZE, "out of memory");
  }

  // The root has no parent to judge it, so it is emitted on its own terms.
  if (ok && raw_root) {
    uint32_t owns = tf_filter__cell(&self, raw_root).children;
    TSSymbolMetadata metadata = tf_symbol_metadata(lang, self.root_symbol);
    void *value = NULL;
    if (metadata.visible && self.sink->on_node) {
      TFVisibleNode node = {
          .symbol = tf_public_symbol(lang, self.root_symbol),
          .production_id = self.root_production_id,
          .named = metadata.named,
          .start_byte = self.root_start_byte,
          .end_byte = self.root_end_byte,
          .start_point = self.root_start_point,
          .end_point = self.root_end_point,
          .child_count = owns,
          .children = tf_filter__run(&self, self.arena_len - owns),
      };
      value = self.sink->on_node(self.sink->payload, &node);
    }
    if (root) *root = value;
  } else if (root) {
    *root = NULL;
  }

  // Same as the driver: on failure the consumer never sees a root, so hand back
  // everything still sitting in the arena.
  if (!ok && self.sink->on_discard) {
    for (uint32_t i = 0; i < self.arena_len; i++) {
      if (self.arena[i].value) self.sink->on_discard(self.sink->payload, self.arena[i].value);
    }
  }
  free(self.arena);
  free(self.scratch);
  free(self.declined);
#if !TF_USE_PACKED_CELLS
  free(self.cells);
#endif
  return ok;
}
