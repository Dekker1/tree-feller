// The token stream tree-feller's lexer produces must equal the leaf sequence of
// the tree libtree-sitter builds -- symbols, byte spans and points, extras
// included.
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

static void collect(Subtree tree, Length offset, Leaves *out) {
  uint32_t count = ts_subtree_child_count(tree);
  if (count == 0) {
    Length start = length_add(offset, ts_subtree_padding(tree));
    Length end = length_add(start, ts_subtree_size(tree));
    if (out->len == out->cap) {
      out->cap = out->cap ? out->cap * 2 : 256;
      out->data = tf_xrealloc(out->data, out->cap * sizeof(Leaf));
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

// One grammar's tables and reference parser, loaded once and shared by every case.
typedef struct {
  const TSLanguage *ts;
  TFLanguage *lang;
  TSParser *parser;
} Grammar;

static Grammar grammar_load(const TSLanguage *ts) {
  const char *error = NULL;
  Grammar g = {.ts = ts, .lang = tf_language_load(ts, &error), .parser = ts_parser_new()};
  if (g.lang == NULL) {
    fprintf(stderr, "cannot load grammar: %s\n", error);
    exit(1);
  }
  ts_parser_set_language(g.parser, ts);
  return g;
}

static void grammar_free(Grammar *g) {
  tf_language_free(g->lang);
  ts_parser_delete(g->parser);
}

static void check(const char *label, const Grammar *g, const char *source, size_t size) {
  TSTree *tree = ts_parser_parse_string(g->parser, NULL, source, (uint32_t)size);
  // The reference must be a clean parse, or the leaf sequence contains tokens
  // lexed in ERROR_STATE, which says nothing about the lexer.
  if (ts_node_has_error(ts_tree_root_node(tree))) {
    if (tolerate_reference_errors) {
      skipped++;
    } else {
      fprintf(stderr, "  FAIL %s: reference parse has errors\n", label);
      failures++;
    }
    ts_tree_delete(tree);
    return;
  }

  Leaves leaves = {0};
  collect(tree->root, length_zero(), &leaves);

  TFLexer lexer;
  tf_lexer_init(&lexer, g->lang, source, (uint32_t)size);

  for (size_t i = 0; i < leaves.len; i++) {
    Leaf want = leaves.data[i];
    // ERROR / MISSING leaves are the recovery machinery tree-feller does not
    // have; stop comparing once the reference tree stops being a clean parse.
    if (want.end_byte == want.start_byte) {
      break;  // MISSING leaf
    }

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
              label, i, want.state, ts_language_symbol_name(g->ts, want.symbol), want.start_byte,
              want.end_byte, want.start_point.row, want.start_point.column, want.end_point.row,
              want.end_point.column, ts_language_symbol_name(g->ts, got.symbol), got.start_byte,
              got.end_byte, got.start_point.row, got.start_point.column, got.end_point.row,
              got.end_point.column);
      failures++;
      break;
    }
  }

  free(leaves.data);
  ts_tree_delete(tree);
}

static void check_file(const Grammar *g, const char *path) {
  TFFile file;
  TFError error;
  if (!tf_file_open(&file, path, &error)) {
    fprintf(stderr, "  FAIL %s\n", error.message);
    failures++;
    return;
  }
  check(path, g, file.data, file.size);
  tf_file_close(&file);
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
  Grammar datazinc = grammar_load(tree_sitter_datazinc());
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char label[32];
    snprintf(label, sizeof(label), "case %zu", i);
    check(label, &datazinc, cases[i], strlen(cases[i]));
  }
  grammar_free(&datazinc);
  printf("  %zu inline cases\n", sizeof(cases) / sizeof(cases[0]));

  // Optional bulk run: `test_lexer <grammar> <file>...`, with paths also
  // accepted on stdin so it can be fed a find(1) pipeline.
  if (argc > 1) {
    tolerate_reference_errors = true;
    const TFGrammar *g = tf_grammar_named(argv[1]);
    Grammar grammar = grammar_load(g != NULL ? g->language() : tree_sitter_datazinc());
    unsigned files = 0;
    for (int i = 2; i < argc; i++, files++) {
      check_file(&grammar, argv[i]);
    }
    if (argc == 2) {
      char path[4096];
      for (; tf_next_stdin_path(path, sizeof(path)); files++) {
        check_file(&grammar, path);
      }
    }
    grammar_free(&grammar);
    printf("  %s: %u files, %u skipped (not parseable by this grammar)\n", argv[1], files, skipped);
  }

  if (failures) {
    fprintf(stderr, "%u failures\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
