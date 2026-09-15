// Differential harness: tree-feller's visible node stream against a post-order
// walk of the tree libtree-sitter builds for the same input.
//
//   tf_diff [--grammar <name>] [--corpus] [--only] [--expect-failures N] [-v]
//           <path>...
//
// The grammar is any name in tests/grammars.h, defaulting to datazinc.
// Directories are walked for files matching the grammar's extension; paths are
// also read from stdin when none are given. `--corpus` reads tree-sitter's own
// corpus format instead, and `--only` parses without a reference.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tree_sitter/api.h"  // before parser.h, whose typedefs it guards

#include "tree_feller.h"

#include "subtree.h"
#include "tree.h"

#include "grammars.h"

// ---------------------------------------------------------------------------
// The reference: every visible node of the CST, in post-order.

typedef struct {
  TSSymbol symbol;
  uint16_t production_id;
  TSFieldId field_id;
  bool named;
  bool extra;
  uint32_t start_byte, end_byte;
  uint32_t start_row, start_column, end_row, end_column;
  uint32_t child_count;
} Node;

typedef struct {
  Node *data;
  size_t len, capacity;
} Nodes;

static void nodes_push(Nodes *self, Node node) {
  if (self->len == self->capacity) {
    self->capacity = self->capacity ? self->capacity * 2 : 1024;
    self->data = tf_xrealloc(self->data, self->capacity * sizeof(Node));
  }
  self->data[self->len++] = node;
}

// A cursor visits exactly the visible nodes, and reports each one's field.
static void walk(TSTreeCursor *cursor, Nodes *out) {
  TSNode node = ts_tree_cursor_current_node(cursor);
  uint32_t child_count = ts_node_child_count(node);
  if (ts_tree_cursor_goto_first_child(cursor)) {
    do {
      walk(cursor, out);
    } while (ts_tree_cursor_goto_next_sibling(cursor));
    ts_tree_cursor_goto_parent(cursor);
  }
  TSPoint start = ts_node_start_point(node), end = ts_node_end_point(node);
  Subtree subtree = *(const Subtree *)node.id;  // node.c:/ts_node__subtree/
  nodes_push(out,
             (Node){
                 .symbol = ts_node_symbol(node),
                 .production_id = ts_subtree_child_count(subtree) ? subtree.ptr->production_id : 0,
                 .field_id = ts_tree_cursor_current_field_id(cursor),
                 .named = ts_node_is_named(node),
                 .extra = ts_node_is_extra(node),
                 .start_byte = ts_node_start_byte(node),
                 .end_byte = ts_node_end_byte(node),
                 .start_row = start.row,
                 .start_column = start.column,
                 .end_row = end.row,
                 .end_column = end.column,
                 .child_count = child_count,
             });
}

// ---------------------------------------------------------------------------
// The subject: the same sequence, from tree-feller.

// tree-feller reports a node when its *parent* reduces, so a node nested under a
// hidden one is reported before an earlier sibling that is not. The guarantee is
// only that children come before parents, so the events are reassembled into a
// tree and walked in post-order to line up with the cursor.
typedef struct {
  Nodes nodes;
  size_t *links;  // child indices, `child_count` of them from each node's `base`
  size_t *base;   // grown with `nodes`, to `base_capacity`
  size_t link_count, link_capacity, base_capacity;
} Collector;

static void *on_node(void *payload, const TFVisibleNode *node) {
  Collector *self = payload;
  if (self->link_count + node->child_count > self->link_capacity) {
    self->link_capacity = (self->link_count + node->child_count) * 2;
    self->links = tf_xrealloc(self->links, self->link_capacity * sizeof(size_t));
  }
  size_t base = self->link_count;
  for (uint32_t i = 0; i < node->child_count; i++) {
    size_t index = (size_t)node->children[i].value - 1;
    // A node's field belongs to the edge from its parent, so it is only known now.
    self->nodes.data[index].field_id = node->children[i].field_id;
    self->links[self->link_count++] = index;
  }
  nodes_push(&self->nodes, (Node){
                               .symbol = node->symbol,
                               .production_id = node->production_id,
                               .named = node->named,
                               .extra = node->extra,
                               .start_byte = node->start_byte,
                               .end_byte = node->end_byte,
                               .start_row = node->start_point.row,
                               .start_column = node->start_point.column,
                               .end_row = node->end_point.row,
                               .end_column = node->end_point.column,
                               .child_count = node->child_count,
                           });
  if (self->base_capacity < self->nodes.capacity) {
    self->base_capacity = self->nodes.capacity;
    self->base = tf_xrealloc(self->base, self->base_capacity * sizeof(size_t));
  }
  self->base[self->nodes.len - 1] = base;
  return (void *)self->nodes.len;  // 1-based, so NULL is never a valid handle
}

static void flatten(const Collector *self, size_t index, Nodes *out) {
  const Node *node = &self->nodes.data[index];
  for (uint32_t i = 0; i < node->child_count; i++) {
    flatten(self, self->links[self->base[index] + i], out);
  }
  nodes_push(out, *node);
}

// ---------------------------------------------------------------------------

static unsigned checked, skipped, failed;
static bool verbose;
static bool corpus_mode;
// Parse with tree-feller alone, with no sink and no reference: what the library
// costs on its own, without the node lists this tool keeps for comparison.
static bool only_mode;
// Number of failures that are known and accounted for. Anything else is a
// regression, and anything fewer means a limitation was fixed without the note
// being removed.
static unsigned expected_failures;

// Everything the comparison holds on to for one grammar, loaded once.
typedef struct {
  const TSLanguage *ts;
  TFLanguage *lang;
  TSParser *parser;
} Grammar;

static void print_node(const char *label, const TSLanguage *ts, const Node *node) {
  fprintf(stderr, "    %s %s prod=%u field=%u named=%d extra=%d [%u,%u) (%u,%u)-(%u,%u) kids=%u\n",
          label, ts_language_symbol_name(ts, node->symbol), node->production_id, node->field_id,
          node->named, node->extra, node->start_byte, node->end_byte, node->start_row,
          node->start_column, node->end_row, node->end_column, node->child_count);
}

static void print_nodes(const char *heading, const TSLanguage *ts, const Nodes *nodes) {
  fprintf(stderr, "  %s:\n", heading);
  for (size_t i = 0; i < nodes->len; i++) {
    char label[24];
    snprintf(label, sizeof(label), "%3zu", i);
    print_node(label, ts, &nodes->data[i]);
  }
}

static void parse_failed(const char *path, const TFError *error) {
  fprintf(stderr, "  FAIL %s: %u:%u: %s\n", path, error->point.row + 1, error->point.column,
          error->message);
  failed++;
}

static bool same(const Node *a, const Node *b) { return memcmp(a, b, sizeof(Node)) == 0; }

// The end of the first token beginning at or after `from`: how far tree-feller
// may legitimately get past the reference's error region.
static uint32_t next_token_end(TSNode node, uint32_t from) {
  uint32_t count = ts_node_child_count(node);
  if (count == 0) {
    return ts_node_start_byte(node) >= from ? ts_node_end_byte(node) : from;
  }
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(node, i);
    if (ts_node_end_byte(child) < from) {
      continue;
    }
    uint32_t limit = next_token_end(child, from);
    if (limit > from) {
      return limit;
    }
  }
  return from;
}

static uint32_t error_limit(TSNode root, uint32_t from) {
  uint32_t limit = next_token_end(root, from);
  // EOF is a lookahead too, but has no visible leaf in a cursor walk. If no
  // later leaf exists, a parser may consume trailing whitespace before finding
  // the missing token at EOF (for example an unfinished compiler directive).
  return limit > from ? limit : ts_node_end_byte(root);
}

static void check(const char *path, const Grammar *g, const void *bytes, uint32_t size) {
  const char *source = bytes;
  TFError error;
  if (only_mode) {
    if (tf_parse(g->lang, source, size, NULL, NULL, &error)) {
      checked++;
    } else {
      parse_failed(path, &error);
    }
    return;
  }

  TSTree *tree = ts_parser_parse_string(g->parser, NULL, source, size);
  TSNode root = ts_tree_root_node(tree);

  Collector collector = {0};
  TFVisibleSink sink = {.payload = &collector, .on_node = on_node};
  bool ok = tf_parse_visible(g->lang, source, size, &sink, NULL, &error);

  if (ts_node_has_error(root)) {
    // Invalid input: tree-feller must refuse it, at or before the point where
    // the reference parser first had to recover.
    TSNode first = root;
    while (ts_node_child_count(first) > 0) {
      TSNode next = ts_node_child(first, 0);
      bool found = false;
      for (uint32_t i = 0; i < ts_node_child_count(first); i++) {
        TSNode child = ts_node_child(first, i);
        if (ts_node_has_error(child) || ts_node_is_missing(child)) {
          next = child;
          found = true;
          break;
        }
      }
      if (!found) {
        break;
      }
      first = next;
    }
    skipped++;
    if (ok) {
      fprintf(stderr, "  FAIL %s: accepted input the reference parser could not parse\n", path);
      failed++;
    } else if (error.byte > error_limit(root, ts_node_end_byte(first))) {
      // An LR parser only notices at the first token with no valid action, while
      // tree-sitter's ERROR node covers whatever it chose to recover over --
      // which can start earlier and end a token sooner. What must hold is that
      // tree-feller stops within that region or on the token right after it,
      // never running on into the rest of the file.
      fprintf(stderr, "  FAIL %s: failed at byte %u, past the first error node [%u,%u) (%s)\n",
              path, error.byte, ts_node_start_byte(first), ts_node_end_byte(first), error.message);
      failed++;
    }
    goto done;
  }

  if (!ok) {
    parse_failed(path, &error);
    goto done;
  }

  Nodes want = {0};
  TSTreeCursor cursor = ts_tree_cursor_new(root);
  walk(&cursor, &want);
  ts_tree_cursor_delete(&cursor);

  Nodes got = {0};
  if (collector.nodes.len) {
    flatten(&collector, collector.nodes.len - 1, &got);
  }

  size_t limit = want.len < got.len ? want.len : got.len;
  size_t mismatch = limit;
  for (size_t i = 0; i < limit; i++) {
    if (!same(&want.data[i], &got.data[i])) {
      mismatch = i;
      break;
    }
  }
  if (mismatch < limit || want.len != got.len) {
    if (verbose) {
      print_nodes("reference", g->ts, &want);
      print_nodes("tree-feller", g->ts, &got);
    }
    fprintf(stderr, "  FAIL %s: node %zu\n", path, mismatch);
    if (mismatch < want.len) {
      print_node("want", g->ts, &want.data[mismatch]);
    }
    if (mismatch < got.len) {
      print_node("got ", g->ts, &got.data[mismatch]);
    }
    if (want.len != got.len) {
      fprintf(stderr, "    %zu reference nodes, %zu from tree-feller\n", want.len, got.len);
    }
    failed++;
  } else {
    checked++;
    if (verbose) {
      printf("  ok %s (%zu nodes)\n", path, want.len);
    }
  }
  free(want.data);
  free(got.data);

done:
  free(collector.nodes.data);
  free(collector.links);
  free(collector.base);
  ts_tree_delete(tree);
}

static void check_file(const Grammar *g, const char *path) {
  TFFile file;
  TFError error;
  if (!tf_file_open(&file, path, &error)) {
    fprintf(stderr, "  FAIL %s\n", error.message);
    failed++;
    return;
  }
  check(path, g, file.data, file.size);
  tf_file_close(&file);
}

// tree-sitter's own corpus format: a name between two rules of '=', the source,
// then '---' and the expected s-expression -- which is ignored here, since the
// comparison is against the parser itself rather than against a recorded tree.
static bool rule_of(const char *line, char character) {
  if (*line != character) {
    return false;
  }
  while (*line == character) {
    line++;
  }
  return *line == '\n' || *line == '\0';
}

static void check_corpus(const Grammar *g, const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "  FAIL %s: cannot open\n", path);
    failed++;
    return;
  }
  char line[8192], name[256] = "", source[1 << 16];
  size_t length = 0;
  unsigned state = 0;  // 0: outside, 1: read name, 2: skip closing rule, 3: source
  unsigned index = 0;
  while (fgets(line, sizeof(line), file)) {
    if (state == 0 && rule_of(line, '=')) {
      state = 1;
      continue;
    }
    if (state == 1) {
      snprintf(name, sizeof(name), "%s", line);
      // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): snprintf terminates,
      // so strcspn cannot run past the buffer it was given.
      name[strcspn(name, "\n")] = '\0';
      state = 2;
      continue;
    }
    if (state == 2) {
      if (rule_of(line, '=')) {
        state = 3;
        length = 0;
      }
      continue;
    }
    if (state == 3) {
      if (rule_of(line, '-')) {
        while (length > 0 && source[length - 1] == '\n') {
          length--;
        }
        char label[512];
        snprintf(label, sizeof(label), "%s:%u %s", path, ++index, name);
        source[length] = '\0';
        check(label, g, source, (uint32_t)length);
        state = 0;
        continue;
      }
      size_t chunk = strlen(line);
      if (length + chunk < sizeof(source)) {
        memcpy(source + length, line, chunk);
        length += chunk;
      }
    }
  }
  fclose(file);
}

static void check_path(const Grammar *g, const char *path, const char *extension) {
  struct stat info;
  if (stat(path, &info) != 0) {
    fprintf(stderr, "  FAIL %s: cannot stat\n", path);
    failed++;
    return;
  }
  if (!S_ISDIR(info.st_mode)) {
    if (corpus_mode) {
      check_corpus(g, path);
    } else {
      check_file(g, path);
    }
    return;
  }
  DIR *dir = opendir(path);
  if (!dir) {
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char child[4096];
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    bool is_dir = entry->d_type == DT_DIR;
    // Symlinks are followed, and some filesystems do not fill in the type.
    if (entry->d_type != DT_DIR && entry->d_type != DT_REG) {
      struct stat child_info;
      if (stat(child, &child_info) != 0) {
        continue;
      }
      is_dir = S_ISDIR(child_info.st_mode);
    }
    if (is_dir) {
      check_path(g, child, extension);
    } else {
      const char *dot = strrchr(entry->d_name, '.');
      if (dot && strcmp(dot, extension) == 0) {
        check_file(g, child);
      } else if (corpus_mode && dot && strcmp(dot, ".txt") == 0) {
        check_corpus(g, child);
      }
    }
  }
  closedir(dir);
}

int main(int argc, char **argv) {
  const TSLanguage *ts = tree_sitter_datazinc();
  const char *extension = ".dzn";
  int first = 1;
  for (; first < argc; first++) {
    if (strcmp(argv[first], "-v") == 0) {
      verbose = true;
    } else if (strcmp(argv[first], "--expect-failures") == 0 && first + 1 < argc) {
      expected_failures = (unsigned)atoi(argv[++first]);
    } else if (strcmp(argv[first], "--only") == 0) {
      only_mode = true;
    } else if (strcmp(argv[first], "--corpus") == 0) {
      corpus_mode = true;
    } else if (strcmp(argv[first], "--grammar") == 0 && first + 1 < argc) {
      const TFGrammar *g = tf_grammar_named(argv[++first]);
      if (g == NULL) {
        fprintf(stderr, "unknown grammar '%s'\n", argv[first]);
        return 2;
      }
      ts = g->language();
      extension = g->extension;
    } else {
      break;
    }
  }

  const char *load_error = NULL;
  Grammar g = {.ts = ts, .lang = tf_language_load(ts, &load_error), .parser = ts_parser_new()};
  if (g.lang == NULL) {
    fprintf(stderr, "cannot load grammar: %s\n", load_error);
    ts_parser_delete(g.parser);
    return 2;
  }
  ts_parser_set_language(g.parser, ts);

  if (first < argc) {
    for (int i = first; i < argc; i++) {
      check_path(&g, argv[i], extension);
    }
  } else {
    char path[4096];
    while (tf_next_stdin_path(path, sizeof(path))) {
      check_file(&g, path);
    }
  }
  tf_language_free(g.lang);
  ts_parser_delete(g.parser);

  printf("%u matched, %u not parseable by this grammar, %u failed\n", checked, skipped, failed);
  if (failed != expected_failures) {
    fprintf(stderr, "expected %u failures, got %u\n", expected_failures, failed);
    return 1;
  }
  return 0;
}
