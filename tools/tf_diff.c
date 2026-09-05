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

// This is a test tool, so running out of memory is fatal rather than handled --
// but `p = realloc(p, n)` still loses the original buffer when it fails, so the
// result goes through here instead.
static void *xrealloc(void *ptr, size_t size) {
  void *grown = realloc(ptr, size);
  if (grown == NULL) {
    free(ptr);
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  return grown;
}

static void nodes_push(Nodes *self, Node node) {
  if (self->len == self->capacity) {
    self->capacity = self->capacity ? self->capacity * 2 : 1024;
    self->data = xrealloc(self->data, self->capacity * sizeof(Node));
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
  size_t *base;
  size_t link_count, link_capacity;
} Collector;

static void *on_node(void *payload, const TFVisibleNode *node) {
  Collector *self = payload;
  if (self->link_count + node->child_count > self->link_capacity) {
    self->link_capacity = (self->link_count + node->child_count) * 2;
    self->links = xrealloc(self->links, self->link_capacity * sizeof(size_t));
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
  self->base = xrealloc(self->base, self->nodes.len * sizeof(size_t));
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

static const char *name_of(const TSLanguage *ts, TSSymbol symbol) {
  return ts_language_symbol_name(ts, symbol);
}

static void report_one(const TSLanguage *ts, size_t index, const Node *node) {
  fprintf(stderr, "    %3zu %-24s prod=%-4u field=%-3u named=%d extra=%d [%u,%u) kids=%u\n", index,
          name_of(ts, node->symbol), node->production_id, node->field_id, node->named, node->extra,
          node->start_byte, node->end_byte, node->child_count);
}

static void report(const char *path, const TSLanguage *ts, size_t index, const Node *want,
                   const Node *got) {
  fprintf(stderr, "  FAIL %s: node %zu\n", path, index);
  if (want) {
    fprintf(stderr,
            "    want %s prod=%u field=%u named=%d extra=%d [%u,%u) (%u,%u)-(%u,%u) kids=%u\n",
            name_of(ts, want->symbol), want->production_id, want->field_id, want->named,
            want->extra, want->start_byte, want->end_byte, want->start_row, want->start_column,
            want->end_row, want->end_column, want->child_count);
  }
  if (got) {
    fprintf(stderr,
            "    got  %s prod=%u field=%u named=%d extra=%d [%u,%u) (%u,%u)-(%u,%u) kids=%u\n",
            name_of(ts, got->symbol), got->production_id, got->field_id, got->named, got->extra,
            got->start_byte, got->end_byte, got->start_row, got->start_column, got->end_row,
            got->end_column, got->child_count);
  }
}

static bool same(const Node *a, const Node *b) { return memcmp(a, b, sizeof(Node)) == 0; }

// The end of the first token beginning at or after `from`: how far tree-feller
// may legitimately get past the reference's error region.
static uint32_t error_limit(TSNode node, uint32_t from) {
  uint32_t count = ts_node_child_count(node);
  if (count == 0) return ts_node_start_byte(node) >= from ? ts_node_end_byte(node) : from;
  for (uint32_t i = 0; i < count; i++) {
    TSNode child = ts_node_child(node, i);
    if (ts_node_end_byte(child) < from) continue;
    uint32_t limit = error_limit(child, from);
    if (limit > from) return limit;
  }
  return from;
}

static void check(const char *path, const TSLanguage *ts, const void *bytes, uint32_t size) {
  const char *source = bytes;
  if (only_mode) {
    const char *load_error = NULL;
    TFLanguage *lang = tf_language_load(ts, &load_error);
    TFError error;
    if (tf_parse(lang, source, size, NULL, NULL, &error)) {
      checked++;
    } else {
      fprintf(stderr, "  FAIL %s: %u:%u: %s\n", path, error.point.row + 1, error.point.column,
              error.message);
      failed++;
    }
    tf_language_free(lang);
    return;
  }

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, ts);
  TSTree *tree = ts_parser_parse_string(parser, NULL, source, size);
  TSNode root = ts_tree_root_node(tree);

  const char *load_error = NULL;
  TFLanguage *lang = tf_language_load(ts, &load_error);
  Collector collector = {0};
  TFVisibleSink sink = {.payload = &collector, .on_node = on_node};
  TFError error;
  bool ok = tf_parse_visible(lang, source, size, &sink, NULL, &error);

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
      if (!found) break;
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
    fprintf(stderr, "  FAIL %s: %u:%u: %s\n", path, error.point.row + 1, error.point.column,
            error.message);
    failed++;
    goto done;
  }

  Nodes want = {0};
  TSTreeCursor cursor = ts_tree_cursor_new(root);
  walk(&cursor, &want);
  ts_tree_cursor_delete(&cursor);

  Nodes got = {0};
  if (collector.nodes.len) flatten(&collector, collector.nodes.len - 1, &got);

  size_t limit = want.len < got.len ? want.len : got.len;
  size_t mismatch = limit;
  for (size_t i = 0; i < limit; i++) {
    if (!same(&want.data[i], &got.data[i])) {
      mismatch = i;
      break;
    }
  }
  if ((mismatch < limit || want.len != got.len) && verbose) {
    fprintf(stderr, "  reference:\n");
    for (size_t i = 0; i < want.len; i++) report_one(ts, i, &want.data[i]);
    fprintf(stderr, "  tree-feller:\n");
    for (size_t i = 0; i < got.len; i++) report_one(ts, i, &got.data[i]);
  }
  if (mismatch < limit || want.len != got.len) {
    report(path, ts, mismatch, mismatch < want.len ? &want.data[mismatch] : NULL,
           mismatch < got.len ? &got.data[mismatch] : NULL);
    if (want.len != got.len) {
      fprintf(stderr, "    %zu reference nodes, %zu from tree-feller\n", want.len, got.len);
    }
    failed++;
  } else {
    checked++;
    if (verbose) printf("  ok %s (%zu nodes)\n", path, want.len);
  }
  free(want.data);
  free(got.data);

done:
  free(collector.nodes.data);
  free(collector.links);
  free(collector.base);
  tf_language_free(lang);
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

static void check_file(const TSLanguage *ts, const char *path) {
  TFFile file;
  TFError error;
  if (!tf_file_open(&file, path, &error)) {
    fprintf(stderr, "  FAIL %s\n", error.message);
    failed++;
    return;
  }
  check(path, ts, file.data, file.size);
  tf_file_close(&file);
}

// tree-sitter's own corpus format: a name between two rules of '=', the source,
// then '---' and the expected s-expression -- which is ignored here, since the
// comparison is against the parser itself rather than against a recorded tree.
static bool rule_of(const char *line, char character) {
  if (*line != character) return false;
  while (*line == character) line++;
  return *line == '\n' || *line == '\0';
}

static void check_corpus(const TSLanguage *ts, const char *path) {
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
        while (length > 0 && source[length - 1] == '\n') length--;
        char label[512];
        snprintf(label, sizeof(label), "%s:%u %s", path, ++index, name);
        source[length] = '\0';
        check(label, ts, source, (uint32_t)length);
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

static void check_path(const TSLanguage *ts, const char *path, const char *extension) {
  struct stat info;
  if (stat(path, &info) != 0) {
    fprintf(stderr, "  FAIL %s: cannot stat\n", path);
    failed++;
    return;
  }
  if (!S_ISDIR(info.st_mode)) {
    if (corpus_mode)
      check_corpus(ts, path);
    else
      check_file(ts, path);
    return;
  }
  DIR *dir = opendir(path);
  if (!dir) return;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (entry->d_name[0] == '.') continue;
    char child[4096];
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    struct stat child_info;
    if (stat(child, &child_info) != 0) continue;
    if (S_ISDIR(child_info.st_mode)) {
      check_path(ts, child, extension);
    } else {
      const char *dot = strrchr(entry->d_name, '.');
      if (dot && strcmp(dot, extension) == 0)
        check_file(ts, child);
      else if (corpus_mode && dot && strcmp(dot, ".txt") == 0)
        check_corpus(ts, child);
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

  if (first < argc) {
    for (int i = first; i < argc; i++) check_path(ts, argv[i], extension);
  } else {
    char path[4096];
    while (fgets(path, sizeof(path), stdin)) {
      path[strcspn(path, "\n")] = '\0';
      if (*path) check_file(ts, path);
    }
  }

  printf("%u matched, %u not parseable by this grammar, %u failed\n", checked, skipped, failed);
  if (failed != expected_failures) {
    fprintf(stderr, "expected %u failures, got %u\n", expected_failures, failed);
    return 1;
  }
  return 0;
}
