// Folding a hidden run changes what a *parent* is handed, and nothing else: the
// set of visible nodes, and their spans, must be identical either way. If that
// ever stops holding, `on_hidden` is dropping nodes rather than collapsing
// children, and a consumer using it would silently lose data.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tree_feller.h"

const TSLanguage *tree_sitter_datazinc(void);
const TSLanguage *tree_sitter_eprime(void);
const TSLanguage *tree_sitter_minizinc(void);

typedef struct {
  TSSymbol symbol;
  uint32_t start_byte, end_byte;
} Record;

typedef struct {
  Record *items;
  size_t len, capacity;
  size_t folds;
} Log;

// A failed `realloc` leaves the original buffer alive; `p = realloc(p, n)` drops
// it. Tests may die on allocation failure, but not silently lose memory first.
static void *xrealloc(void *ptr, size_t size) {
  void *grown = realloc(ptr, size);
  if (grown == NULL) {
    free(ptr);
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  return grown;
}

static void *record(void *payload, const TFVisibleNode *node) {
  Log *log = payload;
  if (log->len == log->capacity) {
    log->capacity = log->capacity ? log->capacity * 2 : 256;
    log->items = xrealloc(log->items, log->capacity * sizeof(Record));
    assert(log->items);
  }
  log->items[log->len++] =
      (Record){.symbol = node->symbol, .start_byte = node->start_byte, .end_byte = node->end_byte};
  return NULL;
}

static void *fold(void *payload, const TFVisibleNode *node) {
  Log *log = payload;
  log->folds++;
  // Any non-NULL collapses the run; the value is the consumer's business.
  return (void *)(uintptr_t)node->child_count;
}

static void check(const TSLanguage *ts, const char *name, const char *source) {
  const char *error_message = NULL;
  TFLanguage *lang = tf_language_load(ts, &error_message);
  assert(lang);

  Log plain = {0}, folded = {0};
  TFError error;
  TFVisibleSink a = {.payload = &plain, .on_node = record, .on_hidden = NULL};
  TFVisibleSink b = {.payload = &folded, .on_node = record, .on_hidden = fold};

  uint32_t size = (uint32_t)strlen(source);
  bool ok_a = tf_parse_visible(lang, source, size, &a, NULL, &error);
  bool ok_b = tf_parse_visible(lang, source, size, &b, NULL, &error);
  assert(ok_a == ok_b);

  if (ok_a) {
    if (plain.len != folded.len) {
      fprintf(stderr, "%s: %zu nodes without folding, %zu with\n", name, plain.len, folded.len);
      exit(1);
    }
    for (size_t i = 0; i < plain.len; i++) {
      const Record *a = &plain.items[i];
      const Record *b = &folded.items[i];
      if (a->symbol != b->symbol || a->start_byte != b->start_byte || a->end_byte != b->end_byte) {
        fprintf(stderr, "%s: node %zu differs (symbol %u/%u, bytes %u-%u / %u-%u)\n", name, i,
                a->symbol, b->symbol, a->start_byte, a->end_byte, b->start_byte, b->end_byte);
        exit(1);
      }
    }
  }

  free(plain.items);
  free(folded.items);
  tf_language_free(lang);
  printf("  %-28s %5zu nodes, %4zu runs folded\n", name, plain.len, folded.folds);
}

int main(void) {
  // A long repetition, which is the case folding exists for.
  size_t n = 20000;
  char *big = malloc(n * 8 + 32);
  int at = sprintf(big, "x = [");
  for (size_t i = 0; i < n; i++) at += sprintf(big + at, "%zu,", i % 97);
  sprintf(big + at, "];\n");

  printf("datazinc\n");
  check(tree_sitter_datazinc(), "empty", "");
  check(tree_sitter_datazinc(), "scalar", "x = 1;\n");
  check(tree_sitter_datazinc(), "array", "x = [1, 2, 3];\n");
  check(tree_sitter_datazinc(), "nested", "x = [[1, 2], [3, 4]];\n");
  check(tree_sitter_datazinc(), "2d literal", "x = [| 1, 2 | 3, 4 |];\n");
  check(tree_sitter_datazinc(), "2d column header", "x = [| a: b: | 1, 2 |];\n");
  check(tree_sitter_datazinc(), "set and record", "s = {1, 2};\nr = (a: 1, b: 2);\n");
  check(tree_sitter_datazinc(), "comments", "% one\nx = 1; /* two */\ny = 2;\n");
  check(tree_sitter_datazinc(), "20k members", big);
  free(big);

  // Grammars that actually use aliases, where `tf_foldable` has to say no for
  // any symbol a production can rename.
  printf("eprime\n");
  check(tree_sitter_eprime(), "find", "language ESSENCE' 1.0\nfind x : int(1..10)\n");
  printf("minizinc\n");
  check(tree_sitter_minizinc(), "array decl", "array[1..3] of int: a = [1, 2, 3];\n");
  check(tree_sitter_minizinc(), "comprehension", "constraint forall(i in 1..3)(a[i] > 0);\n");
  check(tree_sitter_minizinc(), "predicate", "predicate p(int: x) = x > 0;\n");

  printf("ok\n");
  return 0;
}
