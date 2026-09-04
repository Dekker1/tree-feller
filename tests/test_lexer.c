// Phase 2: the token stream tree-feller's lexer produces must equal the leaf
// sequence of the tree libtree-sitter builds -- symbols, byte spans and points,
// extras included.
//
// Each leaf records the parse state it was lexed in (ts_subtree_parse_state), so
// the reference tree supplies the state sequence and the lexer can be checked on
// its own, before there is a driver to produce those states.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tree_sitter/api.h"  // before parser.h, whose typedefs it guards

#include "tf_lexer.h"

#include "subtree.h"
#include "tree.h"

#include "grammars.h"

typedef struct {
  TSSymbol symbol;
  TSStateId state;
  uint32_t start_byte, end_byte;
  TSPoint start_point, end_point;
} Leaf;

typedef struct {
  Leaf *data;
  size_t len, cap;
} Leaves;

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

static void collect(Subtree tree, Length offset, Leaves *out) {
  uint32_t count = ts_subtree_child_count(tree);
  if (count == 0) {
    Length start = length_add(offset, ts_subtree_padding(tree));
    Length end = length_add(start, ts_subtree_size(tree));
    if (out->len == out->cap) {
      out->cap = out->cap ? out->cap * 2 : 256;
      out->data = xrealloc(out->data, out->cap * sizeof(Leaf));
    }
    out->data[out->len++] = (Leaf){
        .symbol = ts_subtree_symbol(tree),
        .state = ts_subtree_parse_state(tree),
        .start_byte = start.bytes,
        .end_byte = end.bytes,
        .start_point = start.extent,
        .end_point = end.extent,
    };
    return;
  }
  const Subtree *children = ts_subtree_children(tree);
  for (uint32_t i = 0; i < count; i++) {
    collect(children[i], offset, out);
    offset = length_add(offset, ts_subtree_total_size(children[i]));
  }
}

static unsigned failures = 0;
// Files the reference parser cannot parse cleanly are not a lexer result either
// way. In bulk runs they are counted; among the curated cases they are a failure.
static unsigned skipped = 0;
static bool tolerate_reference_errors = false;

static void check(const char *label, const TSLanguage *ts, const char *source, size_t size) {
  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, ts);
  TSTree *tree = ts_parser_parse_string(parser, NULL, source, (uint32_t)size);
  // Error recovery is Phase 3's problem; here the reference must be a clean parse
  // or the leaf sequence contains tokens lexed in ERROR_STATE.
  if (ts_node_has_error(ts_tree_root_node(tree))) {
    if (tolerate_reference_errors) {
      skipped++;
    } else {
      fprintf(stderr, "  FAIL %s: reference parse has errors\n", label);
      failures++;
    }
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return;
  }

  Leaves leaves = {0};
  collect(tree->root, length_zero(), &leaves);

  const char *error = NULL;
  TFLanguage *lang = tf_language_load(ts, &error);
  TFLexer lexer;
  tf_lexer_init(&lexer, lang, source, (uint32_t)size);

  for (size_t i = 0; i < leaves.len; i++) {
    Leaf want = leaves.data[i];
    // ERROR / MISSING leaves are the recovery machinery tree-feller does not
    // have; stop comparing once the reference tree stops being a clean parse.
    if (want.end_byte == want.start_byte) break;  // MISSING leaf

    TFToken got;
    if (!tf_lexer_next(&lexer, want.state, &got)) {
      fprintf(stderr, "  FAIL %s: leaf %zu: no token in state %u at byte %u\n", label, i,
              want.state, want.start_byte);
      failures++;
      break;
    }
    if (got.symbol != want.symbol || got.start_byte != want.start_byte ||
        got.end_byte != want.end_byte || got.start_point.row != want.start_point.row ||
        got.start_point.column != want.start_point.column ||
        got.end_point.row != want.end_point.row || got.end_point.column != want.end_point.column) {
      fprintf(stderr,
              "  FAIL %s: leaf %zu state %u: want %s [%u,%u) (%u,%u)-(%u,%u), "
              "got %s [%u,%u) (%u,%u)-(%u,%u)\n",
              label, i, want.state, ts_language_symbol_name(ts, want.symbol), want.start_byte,
              want.end_byte, want.start_point.row, want.start_point.column, want.end_point.row,
              want.end_point.column, ts_language_symbol_name(ts, got.symbol), got.start_byte,
              got.end_byte, got.start_point.row, got.start_point.column, got.end_point.row,
              got.end_point.column);
      failures++;
      break;
    }
  }

  free(leaves.data);
  tf_language_free(lang);
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

static void check_file(const TSLanguage *ts, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "  FAIL: cannot open %s\n", path);
    failures++;
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) {
    fprintf(stderr, "  FAIL: cannot size %s\n", path);
    failures++;
    fclose(f);
    return;
  }
  char *source = malloc((size_t)size + 1);
  if (source == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  size_t read = fread(source, 1, (size_t)size, f);
  source[read] = '\0';
  fclose(f);
  check(path, ts, source, read);
  free(source);
}

int main(int argc, char **argv) {
  static const char *const cases[] = {
      "",
      "x = 1;",
      "x = 1;\ny = 2;\n",
      "% a comment\nx = [1, 2, 3];\n",
      "x = [| 1, 2 | 3, 4 |];\n",
      "x = [| a: b: | c: 1, 2 |];\n",
      "s = \"a string with \\\" escapes and \xc3\xa9 and \xf0\x9f\x8c\xb2\";\n",
      "x = <>;\ny = {1, 2, 3};\nz = 1..10;\n",
      "\xef\xbb\xbfx = 1;\n",  // byte order mark
      "/* block\n   comment */ x = true;\n",
      "e = 1e10;\nf = 1.5;\ng = 0x1f;\nh = 0b101;\no = 0o17;\n",
      "r = 'quoted ident';\nn = -infinity;\n",
      "a = [1, 2, 3] ++ [4];\nx = (1, 2);\ny = (a: 1, b: 2);\n",
      "c = foo(1, bar(2, 3));\nu = {1} union {2};\nv = 1..3 ++ 5..7;\n",
      "x = \n\n\n  1;  % trailing\n",
  };

  printf("lexer token streams:\n");
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char label[32];
    snprintf(label, sizeof(label), "case %zu", i);
    check(label, tree_sitter_datazinc(), cases[i], strlen(cases[i]));
  }
  printf("  %zu inline cases\n", sizeof(cases) / sizeof(cases[0]));

  // Optional bulk run: `test_lexer <grammar> <file>...`, with paths also
  // accepted on stdin so it can be fed a find(1) pipeline.
  if (argc > 1) {
    tolerate_reference_errors = true;
    const TFGrammar *g = tf_grammar_named(argv[1]);
    const TSLanguage *ts = g != NULL ? g->language() : tree_sitter_datazinc();
    unsigned files = 0;
    for (int i = 2; i < argc; i++, files++) check_file(ts, argv[i]);
    if (argc == 2) {
      char path[4096];
      while (fgets(path, sizeof(path), stdin)) {
        path[strcspn(path, "\n")] = '\0';
        if (*path) {
          check_file(ts, path);
          files++;
        }
      }
    }
    printf("  %s: %u files, %u skipped (not parseable by this grammar)\n", argv[1], files, skipped);
  }

  if (failures) {
    fprintf(stderr, "%u failures\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
